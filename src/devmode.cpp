#include <algorithm>
// Dev mode command processor. Commands (one per line, '#' comments, ';' also separates):
//   wait N                 let N frames pass
//   key NAME 1|0           SDL key name (W, Left Shift, E...)    press NAME [frames]
//   lmb|rmb|mmb 1|0        mouse buttons                         click lmb|rmb|mmb [frames]
//   mouse DX DY            relative mouse motion (look)
//   tap X Y [hold]         touch at game screen coordinates
//   shot NAME              screenshot to <dir>/NAME.png (quarter size)
//   sync ID                writes "done ID" once everything before it has run
//   quit
// Game thread (need a player):
//   pos                    player position / facing
//   tp X Y Z               teleport (~ keeps a coordinate, ~N adds N)
//   turn DEG               turn the player       face X Z   turn the player toward a point
//   dump [RADIUS] [all]    interactive objects (all = every object) near the player
//   goto CLASS [N] [DIST] [ANGLE]   stand DIST m from the Nth object of CLASS (substring),
//                          ANGLE degrees around it (0 = in front of its facing), facing it
//   god [1|0]              player takes no damage
//   spawn TYPE [DIST]      enemy in front of the player (slasher lurker brute exploder pregnant swarm
//                          boss, or a GameObject::Type number)
//   kill [RADIUS]          hit every necromorph within RADIUS m with lethal damage
//   scripts                encounter scripts, their state and spawn zones
//   look                   report what E would use (every nearby candidate)
//   call OBJ SYMBOL        call a game method on object OBJ (hex address)
//   audit [DIST]           stand DIST m in front of every usable object; report where E picks wrong
#include "devmode.h"

#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#include "console.h"
#include "cpu.h"
#include "elf_loader.h"
#include "guest_util.h"
#include "input.h"
#include "lights.h"
#include "debugview.h"
#include "settings.h"
#include "menu_nav.h"
#include "render/native.h"
#include "interact.h"
#include "log.h"
#include "memory.h"
#include "objects.h"

namespace dev {

namespace {
namespace fs = std::filesystem;
std::string g_dir;
Host g_host;
bool g_active = false;
std::mutex g_out_lock;
std::deque<std::string> g_cmds;
int g_wait = 0;
std::atomic<int> g_job_pending{0};
int g_job_frames = 0;
struct Release {
    int frames;
    SDL_Event e;
};
std::vector<Release> g_releases;


std::vector<std::string> split(const std::string& s) {
    std::istringstream is(s);
    std::vector<std::string> v;
    for (std::string w; is >> w;) v.push_back(w);
    return v;
}

bool mouse_button(const std::string& n, Uint8& b) {
    if (n == "lmb") b = SDL_BUTTON_LEFT;
    else if (n == "rmb") b = SDL_BUTTON_RIGHT;
    else if (n == "mmb") b = SDL_BUTTON_MIDDLE;
    else return false;
    return true;
}
SDL_Event key_event(const std::string& name, bool down) {
    SDL_Event e{};
    Uint8 b;
    if (mouse_button(name, b)) {
        e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        e.button.button = b;
    } else {
        e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        e.key.keysym.scancode = SDL_GetScancodeFromName(name.c_str());
        e.key.keysym.sym = SDL_GetKeyFromScancode(e.key.keysym.scancode);
    }
    return e;
}

float coord(const std::string& s, float cur) {
    if (s.empty() || s[0] != '~') return std::stof(s);
    return s.size() > 1 ? cur + std::stof(s.substr(1)) : cur;
}

const float kPi = 3.14159265f;

void game_job(std::function<void(u32)> fn) {
    g_job_pending = 1;
    g_job_frames = 0;
    input::run_on_game_thread([fn](unsigned player) {
        try {
            fn(player);
        } catch (const std::exception& ex) {
            out(std::string("error: ") + ex.what());
        }
        g_job_pending = 0;
    });
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

void face_point(u32 player, objs::Vec3 t) {
    objs::Vec3 p = objs::pos(player), f = objs::facing(player);
    objs::Vec3 d{t.x - p.x, 0, t.z - p.z};
    float delta = std::remainder(objs::yaw_of(d) - objs::yaw_of(f), 2 * kPi);
    objs::turn_by(player, delta);
}

void dump(u32 player, float radius, bool all) {
    objs::Vec3 p = objs::pos(player);
    int n = 0;
    for (u32 o : objs::world_objects(input::world())) {
        bool ic = objs::is_interactive_class(o);
        if (!ic && !all) continue;
        objs::Vec3 q = objs::pos(o);
        float d = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) + (q.z - p.z) * (q.z - p.z));
        if (radius > 0 && d > radius) continue;
        objs::Box b = objs::bounds(o);
        std::string line = fmt("%08x %-28s d=%6.2f pos %.2f %.2f %.2f box %.2f %.2f %.2f .. %.2f %.2f %.2f flags=%x", o,
                               objs::class_name(o).c_str(), d, q.x, q.y, q.z, b.lo.x, b.lo.y, b.lo.z, b.hi.x, b.hi.y,
                               b.hi.z, objs::flags(o));
        if (ic)
            for (auto& q : objs::region_points(o)) line += fmt(" region %.2f %.2f %.2f", q.x, q.y, q.z);
        if (ic)
            line += fmt(" int=%d plus=%d tk=%d use=%d reach=%.2f chk=%d%d%d", objs::interactive(o), objs::interactive_plus(o),
                        objs::telekinesisable(o), objs::use_enabled(o), objs::reach(o), mem::r8(o + 0x17c),
                        mem::r8(o + 0x17d), mem::r8(o + 0x17e));
        out(line);
        n++;
    }
    out(fmt("%d objects", n));
}

void run(const std::string& line) {
    auto a = split(line);
    if (a.empty() || a[0][0] == '#') return;
    const std::string& c = a[0];
    auto arg = [&](size_t i, const char* def = "") { return i < a.size() ? a[i] : std::string(def); };
    if (c == "wait") {
        g_wait = std::stoi(arg(1, "1"));
    } else if (c == "key" || c == "lmb" || c == "rmb" || c == "mmb") {
        bool mb = c != "key";
        std::string name = mb ? c : arg(1);
        SDL_Event e = key_event(name, arg(mb ? 1 : 2, "1") == "1");
        SDL_PushEvent(&e);
    } else if (c == "press" || c == "click") {
        SDL_Event e = key_event(arg(1), true);
        SDL_PushEvent(&e);
        g_releases.push_back({std::stoi(arg(2, "3")), key_event(arg(1), false)});
    } else if (c == "type") {  // type TEXT: typed characters (text input), rest of the line
        SDL_Event e{};
        e.type = SDL_TEXTINPUT;
        std::string t = line.substr(std::min(line.size(), line.find("type") + 5));
        std::snprintf(e.text.text, sizeof e.text.text, "%s", t.c_str());
        SDL_PushEvent(&e);
    } else if (c == "buttons") {  // menu buttons on screen
        for (auto& b : menu_nav::buttons())
            out(fmt("%08x %-28s %d,%d %dx%d en=%d sub=%08x vis=%d st=%d a=%08x", b.obj, b.name.c_str(), b.x, b.y, b.w, b.h,
                    mem::r8(b.obj + 0x24), mem::r32(b.obj + 0x38), mem::r32(b.obj + 0x38) ? mem::r8(mem::r32(b.obj + 0x38) + 0x54) : 9,
                    mem::r32(b.obj + 0x64), mem::r32(b.obj + 0x5c)));
    } else if (c == "aniso") {  // aniso N: anisotropic filtering level on the native renderer (1 = off)
        render::set_anisotropy(std::stoi(arg(1, "16")));
        out("anisotropy " + std::to_string(render::anisotropy()));
    } else if (c == "bloom") {  // bloom 0-3
        render::set_bloom(std::stoi(arg(1, "1")));
    } else if (c == "ao") {  // ao 0-3 [debug|x [GAIN [RADIUS]]]: ambient occlusion level; "debug" shows the term alone
        render::set_ao(std::stoi(arg(1, "1")));
        render::set_ao_debug(arg(2, "") == "debug");
        if (a.size() > 3) render::set_ao_tuning(std::stof(arg(3)), std::stof(arg(4, "0")));
    } else if (c == "lighttrace") {  // lighttrace 1|0: log light source events
        lights::set_trace(arg(1, "1") == "1");
    } else if (c == "fogveil") {  // fogveil 1|0: keep the added lighting off the fog banks (default 1)
        out(fmt("fog veil %s, %d fog draws", arg(1, "1").c_str(), render::set_fog_veil(std::stoi(arg(1, "1")))));
    } else if (c == "rich") {  // rich [CREDITS] [NODES]: fill the wallet (Inventory at player+0x530: nodes +0x14, credits +0x18)
        u32 credits = u32(std::stoul(arg(1, "9999999"))), nodes = u32(std::stoul(arg(2, "99")));
        game_job([credits, nodes](u32 pl) {
            u32 inv = mem::r32(pl + 0x530);
            if (inv < 0x10000) return out("no inventory");
            mem::w32(inv + 0x14, nodes);
            mem::w32(inv + 0x18, credits);
            out(fmt("credits %u, power nodes %u", credits, nodes));
        });
    } else if (c == "health") {  // health N: set the player's health
        float v = std::stof(arg(1, "100"));
        game_job([v](u32 pl) {
            u32 bits;
            std::memcpy(&bits, &v, 4);
            cpu::call(elf::find("_ZN18GameObjectPlayable9setHealthEfa"), {pl, bits, 0});
            out("health set");
        });
    } else if (c == "shadows") {  // shadows 0-3 [debug]: shadows of moving objects; "debug" tints them red and logs caster counts
        render::set_shadows(std::stoi(arg(1, "2")));
        render::set_shadow_debug(arg(2, "") == "debug");
    } else if (c == "lightshadows") {  // lightshadows 1|0: dynamic lights cast shadows
        render::set_light_shadows(arg(1, "1") != "0");
        render::set_tile_debug(std::stoi(arg(2, "0")));  // lightshadows 1 N: show light N's shadow map result
    } else if (c == "tint") {  // tint RRGGBB [AMOUNT]: GameWorld::setTintOverride (the alarm-light tint), e.g. tint ff2020 1
        if (arg(1) == "off") {
            game_job([](u32) {
                cpu::call(elf::find("_ZN9GameWorld17clearTintOverrideEv"), {input::world()});
                out("tint cleared");
            });
            return;
        }
        u32 col = u32(std::stoul(arg(1, "ff2020"), nullptr, 16));
        float amt = std::stof(arg(2, "1"));
        game_job([col, amt](u32) {
            u32 bits;
            std::memcpy(&bits, &amt, 4);
            cpu::call(elf::find("_ZN9GameWorld15setTintOverrideEif"), {input::world(), col, bits});
            out("tint set");
        });
    } else if (c == "gltrace") {  // gltrace 1|0: log every GL call (use: gltrace 1; wait 1; gltrace 0)
        bool on = arg(1, "1") == "1";
        cpu::set_gl_trace(on);
        if (on) render::trace_next_frame();
    } else if (c == "pad") {  // pad BUTTON 1|0 / pad AXIS VALUE: controller input (a b x y back start l3 r3 lb rb up down left right lt rt; lx ly rx ry -1..1)
        static const char* btn[] = {"a", "b", "x", "y", "back", "guide", "start", "l3", "r3", "lb", "rb", "up", "down", "left", "right"};
        static const char* axes[] = {"lx", "ly", "rx", "ry", "lt", "rt"};
        std::string n = arg(1);
        SDL_Event e{};
        for (int i = 0; i < 15; i++)
            if (n == btn[i]) {
                e.type = arg(2, "1") == "1" ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
                e.cbutton.button = Uint8(i);
            }
        for (int i = 0; i < 6; i++)
            if (n == axes[i]) {
                e.type = SDL_CONTROLLERAXISMOTION;
                e.caxis.axis = Uint8(i);
                e.caxis.value = Sint16(std::clamp(std::stof(arg(2, "1")), -1.0f, 1.0f) * 32767.0f);
            }
        if (e.type) SDL_PushEvent(&e);
        else out("error: unknown controller input " + n);
    } else if (c == "usingpad") {
        out(input::using_pad() ? "controller" : "keyboard");
    } else if (c == "grab") {  // grab: the player's grab state
        out("grab state " + std::to_string(input::grab()));
    } else if (c == "fadenodes") {  // nearest proximity-fade effects (fog) and their state
        game_job([](u32 pl) {
            objs::Vec3 p = objs::pos(pl);
            out(lights::debug_fade_nodes(p.x, p.y, p.z));
        });
    } else if (c == "shownodes") {  // shownodes X Z [RADIUS]: un-hide animated level nodes there (e.g. alarm beacons)
        float x = std::stof(arg(1)), z = std::stof(arg(2)), r = std::stof(arg(3, "1"));
        game_job([=](u32) { out("shown " + std::to_string(lights::debug_show_nodes(x, z, r))); });
    } else if (c == "fpslimit") {  // fpslimit N: frame rate limit (0 = unlimited)
        settings::set_int("fps_limit", std::stoi(arg(1, "60")));
    } else if (c == "vertexlight") {  // vertexlight 0|1|2: painted vertex lighting normal / hidden / alone
        render::set_vertex_color_debug(std::stoi(arg(1, "1")));
    } else if (c == "glowgain") {  // glowgain N: brightness of door / screen glow lights (0 = off)
        render::set_glow_gain(std::stof(arg(1, "1.3")));
    } else if (c == "vignette") {  // vignette 1|0
        render::set_vignette(arg(1, "1") != "0");
    } else if (c == "envshadows") {  // envshadows 1|0: level geometry casts shadows from the fixtures
        render::set_env_shadows(arg(1, "1") != "0");
    } else if (c == "probe") {  // probe X Y Z R: log meshes and lights near a point (R 0 = stop)
        render::probe(std::stof(arg(1, "0")), std::stof(arg(2, "0")), std::stof(arg(3, "0")), std::stof(arg(4, "0")));
    } else if (c == "envlight") {  // envlight 0-3: light contrast (how far the painted lighting is turned down)
        render::set_env_light(std::stoi(arg(1, "2")));
    } else if (c == "lightshade") {  // lightshade 0-1: darkness of the shadows fixtures cast
        render::set_lshade(std::stof(arg(1, "0.65")));
    } else if (c == "fanshade") {  // fanshade 0-1: darkness of fan blade shadows on characters
        render::set_fanshade(std::stof(arg(1, "0.7")));
    } else if (c == "specular") {  // specular 0-3
        render::set_specular(std::stoi(arg(1, "2")));
    } else if (c == "dlights") {  // dlights 0-3: dynamic light level
        render::set_lights(std::stoi(arg(1, "2")));
    } else if (c == "light") {  // light [R G B [RADIUS [MS [FWD UP SIDE]]]]: test light near the player
        float r = std::stof(arg(1, "1")), g = std::stof(arg(2, "1")), b = std::stof(arg(3, "1"));
        float rad = std::stof(arg(4, "4")), ms = std::stof(arg(5, "3000")), fwd = std::stof(arg(6, "0")), up = std::stof(arg(7, "0")), side = std::stof(arg(8, "0"));
        game_job([=](u32 pl) {
            objs::Vec3 p = objs::pos(pl), f = objs::facing(pl);
            objs::Vec3 q{p.x + f.x * fwd - f.z * side, p.y + up, p.z + f.z * fwd + f.x * side};
            lights::add(q.x, q.y, q.z, r, g, b, rad, ms);
            float sx = 0, sy = 0;
            bool vis = objs::to_screen(input::world(), q, sx, sy);
            out("light at " + std::to_string(q.x) + " " + std::to_string(q.y) + " " + std::to_string(q.z) + (vis ? " screen " + std::to_string(int(sx)) + "," + std::to_string(int(sy)) : " (behind camera)"));
        });
    } else if (c == "normals") {  // normals 1|0: show the reconstructed surface normals (native renderer)
        render::set_normals_debug(arg(1, "1") == "1");
    } else if (c == "sharpen") {  // sharpen 0-3
        render::set_sharpen(std::stoi(arg(1, "1")));
    } else if (c == "revz") {  // revz 1|0: reversed-Z depth on the native renderer
        render::set_reversed_z(arg(1, "1") == "1");
    } else if (c == "hudmagenta") {  // debug: HUD draws flat magenta on the native renderer
        render::set_debug_hud(arg(1, "1") == "1");
    } else if (c == "renderer") {  // renderer native|legacy
        render::set_enabled(arg(1) == "native");
        out(std::string("renderer: ") + (render::enabled() ? "native" : "legacy"));
    } else if (c == "wheel") {  // wheel N: mouse wheel (positive = up)
        SDL_Event e{};
        e.type = SDL_MOUSEWHEEL;
        e.wheel.y = std::stoi(arg(1, "1"));
        SDL_PushEvent(&e);
    } else if (c == "mouse") {
        SDL_Event e{};
        e.type = SDL_MOUSEMOTION;
        e.motion.xrel = std::stoi(arg(1, "0"));
        e.motion.yrel = std::stoi(arg(2, "0"));
        SDL_PushEvent(&e);
    } else if (c == "tap") {
        g_host.tap(std::stof(arg(1)), std::stof(arg(2)), std::stoi(arg(3, "4")));
    } else if (c == "shot") {  // shot NAME: quarter-size PNG (NAME ending in _full: full resolution)
        g_host.screenshot(g_dir + "/" + arg(1, "shot") + ".png");
        out("shot " + g_dir + "/" + arg(1, "shot") + ".png");
    } else if (c == "sync") {
        out("done " + arg(1));
    } else if (c == "quit") {
        g_host.quit();
    } else if (c == "pos") {
        game_job([](u32 pl) {
            objs::Vec3 p = objs::pos(pl), f = objs::facing(pl);
            out(fmt("player %.2f %.2f %.2f facing %.2f %.2f %.2f yaw %.1f", p.x, p.y, p.z, f.x, f.y, f.z,
                    objs::yaw_of(f) * 180 / kPi));
        });
    } else if (c == "tp") {
        std::string x = arg(1, "~"), y = arg(2, "~"), z = arg(3, "~");
        game_job([x, y, z](u32 pl) {
            objs::Vec3 p = objs::pos(pl);
            objs::teleport(pl, {coord(x, p.x), coord(y, p.y), coord(z, p.z)});
            p = objs::pos(pl);
            out(fmt("player %.2f %.2f %.2f", p.x, p.y, p.z));
        });
    } else if (c == "turn") {
        float deg = std::stof(arg(1, "0"));
        game_job([deg](u32 pl) { objs::turn_by(pl, deg * kPi / 180); });
    } else if (c == "face") {
        float x = std::stof(arg(1)), z = std::stof(arg(2));
        game_job([x, z](u32 pl) { face_point(pl, {x, 0, z}); });
    } else if (c == "dump") {
        float r = std::stof(arg(1, "0"));
        bool all = arg(2) == "all" || arg(1) == "all";
        if (arg(1) == "all") r = 0;
        game_job([r, all](u32 pl) { dump(pl, r, all); });
    } else if (c == "call") {  // call OBJ SYMBOL: run a game method on an object (debug)
        u32 obj = u32(std::stoul(arg(1), nullptr, 16));
        std::string sym = arg(2);
        game_job([obj, sym](u32) {
            u32 fn = elf::find(sym);
            if (!fn) return out("error: no symbol " + sym);
            out(fmt("%s -> %08x", sym.c_str(), cpu::call(fn, {obj})));
        });
    } else if (c == "audit") {  // audit [DIST]: stand in front of every usable object, check E's pick
        float dist = std::stof(arg(1, "1.2"));
        game_job([dist](u32 pl) {
            objs::Vec3 home = objs::pos(pl);
            int ok = 0, wrong = 0, none = 0, away_bad = 0;
            for (u32 o : objs::world_objects(input::world())) {
                if (!objs::is_interactive_class(o)) continue;
                std::string cls = objs::class_name(o);
                if (cls == "GameObjectShootSwitch") continue;
                if (!objs::interactive(o) && !objs::interactive_plus(o)) continue;
                auto approach = interact::approach_point(o, dist);
                objs::teleport(pl, approach.stand);
                face_point(pl, approach.target);
                std::string pick = interact::pick_name(pl, input::world(), 0);
                bool hit = pick.find(fmt("%08x", o)) != std::string::npos;
                objs::turn_by(pl, kPi);
                std::string away = interact::pick_name(pl, input::world(), 0);
                bool away_hit = away.find(fmt("%08x", o)) != std::string::npos;
                if (hit) ok++;
                else if (pick.empty()) none++;
                else wrong++;
                if (away_hit) away_bad++;
                if (!hit || away_hit)
                    out(fmt("%s %08x at %.1f %.1f %.1f: facing -> %s; away -> %s", cls.c_str(), o, approach.target.x,
                            approach.target.y, approach.target.z, pick.empty() ? "nothing" : pick.c_str(),
                            away.empty() ? "nothing" : away.c_str()));
            }
            objs::teleport(pl, home);
            out(fmt("audit: %d picked correctly, %d picked something else, %d nothing, %d still picked facing away", ok,
                    wrong, none, away_bad));
        });
    } else if (c == "peek") {  // peek OBJ OFFSET [f]: read a word / float of an object (hex)
        u32 obj = u32(std::stoul(arg(1), nullptr, 16)), off = u32(std::stoul(arg(2), nullptr, 16));
        bool fl = arg(3) == "f";
        game_job([obj, off, fl](u32 pl) {
            u32 o = obj ? obj : pl;
            out(fl ? fmt("%08x+%x = %g", o, off, objs::rf(o + off)) : fmt("%08x+%x = %08x", o, off, mem::r32(o + off)));
        });
    } else if (c == "god") {  // god [1|0]: player takes no damage
        console::set_god(arg(1, "1") == "1");
    } else if (c == "scripts") {  // encounter scripts: state and the spawn zones keeping them active
        game_job([](u32 pl) {
            static u32 fn_member = elf::find(
                "_ZN10GameObject14isScriptMemberERKN5eastl12basic_stringIwN2im20StringEASTLAllocatorEEE");
            auto objsv = objs::world_objects(input::world());
            objs::Vec3 p = objs::pos(pl);
            for (u32 sc : objsv) {
                std::string cls = objs::class_name(sc);
                if (cls.rfind("Script", 0) != 0) continue;
                u32 info = mem::r32(sc + 0x2c);
                if (info < 0x10000) continue;
                u32 name = mem::r32(info + 0xc4);  // script name (wstring*)
                if (name < 0x10000) continue;
                std::string n;
                for (u32 q = mem::r32(name), e = mem::r32(name + 4); q && q < e && n.size() < 60; q += 2) n += char(mem::r16(q));
                out(fmt("%08x %-18s %-24s state=%d", sc, cls.c_str(), n.c_str(), s32(mem::r32(sc + 0x1a4))));
                for (u32 o : objsv) {
                    if (!(guest::vcall(o, 0x40) & 0xff)) continue;
                    if (!(cpu::call(fn_member, {o, name}) & 0xff)) continue;
                    objs::Vec3 q = objs::pos(o);
                    float d = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.z - p.z) * (q.z - p.z));
                    out(fmt("    %08x %-22s active=%d d=%.1f pos %.1f %.1f %.1f", o, objs::class_name(o).c_str(),
                            s32(mem::r32(o + 0x174)), d, q.x, q.y, q.z));
                }
            }
        });
    } else if (c == "kill") {  // DISABLED: it crashes the game (the HitEvent it builds is incomplete)
        out("error: the kill command is disabled (it crashes the game); use the console /kill for the player");
    } else if (c == "kill_broken") {
        float r = std::stof(arg(1, "40"));
        game_job([r](u32 pl) {
            static u32 fn_ctor = elf::find("_ZN8HitEventC1Ev"),
                       fn_init = elf::find("_ZN8HitEvent20initGenericWithBloodEP10GameObjectfRK9btVector3S4_");
            static u32 hit = mem::heap_alloc(0x100, 16), vecs = mem::heap_alloc(32, 16);
            objs::Vec3 p = objs::pos(pl);
            int n = 0;
            for (u32 o : objs::world_objects(input::world())) {
                std::string cls = objs::class_name(o);
                if (o == pl || mem::r32(o + 0x58) == 0) continue;
                // necromorphs derive from GameObjectNecromorph (its onDamageReceived is at slot 0x108)
                if (cpu::symbolize(mem::r32(mem::r32(o) + 0x108)).find("GameObjectNecromorph") == std::string::npos &&
                    cls.find("Necromorph") == std::string::npos)
                    continue;
                objs::Vec3 q = objs::pos(o);
                if ((q.x - p.x) * (q.x - p.x) + (q.z - p.z) * (q.z - p.z) > r * r) continue;
                std::memset(mem::host(hit), 0, 0x100);
                cpu::call(fn_ctor, {hit});
                float dmg = 10000;
                u32 bits;
                std::memcpy(&bits, &dmg, 4);
                // hit point = the enemy's position, direction = from the player to it
                float dx = q.x - p.x, dz = q.z - p.z, len = std::sqrt(dx * dx + dz * dz) + 1e-3f;
                float v[8] = {q.x, q.y + 1.0f, q.z, 0, dx / len, 0, dz / len, 0};
                std::memcpy(mem::host(vecs), v, sizeof v);
                cpu::call(fn_init, {hit, pl, bits, vecs, vecs + 16});
                guest::vcall(o, 0x104, {hit});  // onHit
                out(fmt("hit %s %08x", cls.c_str(), o));
                n++;
            }
            out(fmt("%d enemies hit", n));
        });
    } else if (c == "aimat") {  // turn toward the nearest living necromorph
        game_job([](u32 pl) {
            objs::Vec3 p = objs::pos(pl);
            u32 best = 0;
            float bd = 1e9f;
            for (u32 o : objs::world_objects(input::world())) {
                if (o == pl || mem::r32(o + 0x58) == 0) continue;
                if (cpu::symbolize(mem::r32(mem::r32(o) + 0x108)).find("GameObjectNecromorph") == std::string::npos) continue;
                if (!(guest::vcall(o, 0x110) & 0xff)) continue;  // alive
                objs::Vec3 q = objs::pos(o);
                float d = (q.x - p.x) * (q.x - p.x) + (q.z - p.z) * (q.z - p.z);
                if (d < bd) bd = d, best = o;
            }
            if (!best) return out("no enemy");
            objs::Vec3 q = objs::pos(best);
            face_point(pl, q);
            face_point(pl, q);
            out(fmt("aiming at %s %08x %.1f m", objs::class_name(best).c_str(), best, std::sqrt(bd)));
        });
    } else if (c == "spawn") {  // spawn TYPE [DIST]: an enemy DIST m in front of the player
        std::string t = arg(1, "slasher");
        int type = objs::enemy_type(t);
        if (!type) type = std::atoi(t.c_str());
        float dist = std::stof(arg(2, "4"));
        game_job([type, dist](u32 pl) {
            objs::Vec3 p = objs::pos(pl), f = objs::facing(pl);
            u32 obj = objs::spawn(input::world(), type, {p.x + f.x * dist, p.y - 1.0f, p.z + f.z * dist}, {-f.x, 0, -f.z});
            if (!obj) return out(fmt("error: type %d not created", type));
            objs::Vec3 q = objs::pos(obj);
            out(fmt("spawned %s %08x (type %d) at %.1f %.1f %.1f", objs::class_name(obj).c_str(), obj, type, q.x, q.y, q.z));
        });
    } else if (c == "debugview") {  // debugview 1|0 [lights|hitboxes|interact]
        std::string which = arg(2, "interact");
        bool on = arg(1, "1") == "1";
        if (which == "lights") debugview::set_lights(on);
        else if (which == "hitboxes") debugview::set_hitboxes(on);
        else input::set_interact_debug(on);
    } else if (c == "look") {  // what E would pick (no action)
        game_job([](u32 pl) { out(interact::preview(pl, input::world(), input::free_look_yaw())); });
    } else if (c == "goto") {
        std::string cls = arg(1);
        int idx = std::stoi(arg(2, "0"));
        float dist = std::stof(arg(3, "1.5")), ang = std::stof(arg(4, "0")) * kPi / 180;
        game_job([cls, idx, dist, ang](u32 pl) {
            int k = 0;
            for (u32 o : objs::world_objects(input::world())) {
                if (objs::class_name(o).find(cls) == std::string::npos || k++ != idx) continue;
                objs::Vec3 q = objs::pos(o), f = objs::facing(o);
                objs::Box b = objs::bounds(o);
                float ca = std::cos(ang), sa = std::sin(ang);
                objs::Vec3 d{f.x * ca + f.z * sa, 0, -f.x * sa + f.z * ca};
                objs::Vec3 p = objs::pos(pl);
                // keep the player's height relative to the floor under the object (box bottom)
                float y = b.lo.y < b.hi.y ? b.lo.y + 1.0f : p.y;
                objs::teleport(pl, {q.x + d.x * dist, y, q.z + d.z * dist});
                face_point(pl, q);
                p = objs::pos(pl);
                out(fmt("at %s #%d (%08x): object %.2f %.2f %.2f, player %.2f %.2f %.2f", objs::class_name(o).c_str(), idx,
                        o, q.x, q.y, q.z, p.x, p.y, p.z));
                return;
            }
            out("error: no " + cls + " #" + std::to_string(idx));
        });
    } else {
        out("error: unknown command " + c);
    }
}

void read_commands() {
    std::string path = g_dir + "/cmd.txt";
    std::error_code ec;
    if (!fs::exists(path, ec)) return;
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    f.close();
    fs::remove(path, ec);
    std::string all = ss.str();
    for (char& ch : all)
        if (ch == ';') ch = '\n';
    std::istringstream is(all);
    for (std::string l; std::getline(is, l);) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty()) g_cmds.push_back(l);
    }
}
}  // namespace

void init(const std::string& dir, Host host) {
    g_dir = dir;
    g_host = std::move(host);
    g_active = true;
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::remove(dir + "/cmd.txt", ec);
    std::ofstream(dir + "/out.txt", std::ios::trunc) << "";
    input::set_test_mode(true);

    LOGI("dev mode: commands from %s/cmd.txt", dir.c_str());
    out("ready");
}

bool active() { return g_active; }

void out(const std::string& line) {
    std::lock_guard lk(g_out_lock);
    std::ofstream f(g_dir + "/out.txt", std::ios::app);
    f << line << "\n";
}

void frame() {
    if (!g_active) return;
    for (size_t i = 0; i < g_releases.size();) {
        if (--g_releases[i].frames <= 0) {
            SDL_PushEvent(&g_releases[i].e);
            g_releases.erase(g_releases.begin() + long(i));
        } else {
            i++;
        }
    }
    read_commands();
    if (g_job_pending) {
        if (++g_job_frames < 600) return;
        out("error: game thread did not run the command (no player?)");
        g_job_pending = 0;
    }
    if (g_wait > 0) {
        g_wait--;
        return;
    }
    while (!g_cmds.empty() && g_wait == 0) {
        std::string c = g_cmds.front();
        g_cmds.pop_front();
        try {
            run(c);
        } catch (const std::exception& ex) {
            out("error: " + c + ": " + ex.what());
        }
        if (g_job_pending) return;  // next command after the game thread ran this one
    }
}

}  // namespace dev
