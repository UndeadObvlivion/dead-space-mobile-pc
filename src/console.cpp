// In-game console. ` (or ~) opens / closes it; Enter runs the line; Up / Down recall earlier lines;
// Esc closes. Commands:
//   /spawn_NAME[_COUNT]    NAME = slasher lurker brute exploder pregnant swarm boss; COUNT 1-20
//   /give_all              give every weapon
//   /god                   toggle god mode
//   /bandana               toggle infinite ammo
//   /level_MAP             load a level (e.g. /level_01_gov_02)
//   /kill                  kill the player
// Words are joined with '_' (spaces also work).
//   /help, /clear
// While typing a /command, matching commands are suggested above the input line; Tab completes
// the first one.
#include "console.h"
#include "debugview.h"
#include "render/native.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "cpu.h"
#include "elf_loader.h"
#include "guest_util.h"
#include "hle/gles.h"
#include "hle/hle.h"
#include "input.h"
#include "log.h"
#include "objects.h"

namespace console {

namespace {

bool g_open = false;
std::string g_line;
std::vector<std::string> g_history;
int g_hist_pos = 0;
std::mutex g_lock;
std::deque<std::string> g_log;  // output lines (newest last)
constexpr size_t kMaxLog = 12;
constexpr int kMaxCount = 20;

void print(const std::string& s) {
    std::lock_guard lk(g_lock);
    g_log.push_back(s);
    while (g_log.size() > kMaxLog) g_log.pop_front();
    LOGI("console: %s", s.c_str());
}

const char* kEnemies[] = {"slasher", "lurker", "brute", "exploder", "pregnant", "swarm", "boss"};

void spawn_enemies(const std::string& name, int count) {
    int type = objs::enemy_type(name);
    input::run_on_game_thread([name, type, count](unsigned player) {
        objs::Vec3 p = objs::pos(player), f = objs::facing(player);
        int made = 0;
        for (int i = 0; i < count; i++) {
            // rows of up to 5, spread across the view, 4 m ahead and further back per row
            int row = i / 5, col = i % 5, in_row = std::min(5, count - row * 5);
            float side = (col - (in_row - 1) * 0.5f) * 1.6f, ahead = 4.0f + row * 1.8f;
            objs::Vec3 r{f.z, 0, -f.x};
            objs::Vec3 at{p.x + f.x * ahead + r.x * side, p.y - 1.0f, p.z + f.z * ahead + r.z * side};
            if (objs::spawn(input::world(), type, at, {-f.x, 0, -f.z})) made++;
        }
        print(made == count ? "spawned " + std::to_string(made) + " " + name
                            : "spawned " + std::to_string(made) + " of " + std::to_string(count) + " " + name);
    });
}

void kill_player() {
    input::run_on_game_thread([](unsigned player) {
        static u32 fn_health = elf::find("_ZN18GameObjectPlayable9setHealthEfa"),
                   fn_die = elf::find("_ZN18GameObjectPlayable3dieENS_11DeathMethodE");
        if (!fn_health || !fn_die) return print("cannot kill: game function not found");
        cpu::call(fn_health, {player, 0, 1});  // health 0 (immediate)
        cpu::call(fn_die, {player, 0});        // the game's normal death (as from a lethal hit)
        print("player killed");
    });
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

void set_open(bool open);

struct Command {
    const char* name;
    const char* args;
    const char* help;
};
const Command kCommands[] = {
    {"/spawn_", "NAME[_COUNT]", "spawn enemies in front of you"},
    {"/give_all", "", "give every weapon"},
    {"/god", "", "toggle god mode (no damage)"},
    {"/bandana", "", "toggle infinite ammo"},
    {"/level_", "MAP", "load a level, e.g. /level_01_gov_02"},
    {"/kill", "", "kill the player"},
    {"/debug_lights", "", "toggle the light placement view (F6)"},
    {"/debug_hitboxes", "", "toggle the hitbox view (F7)"},
    {"/debug_interact", "", "toggle the interaction box view (F10)"},
    {"/debug_vertexlight", "", "cycle the level's painted vertex lighting: normal / hidden / alone (F5)"},
    {"/debug_off", "", "turn all debug views off"},
    {"/help", "", "list commands"},
    {"/clear", "", "clear the console"},
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Lower case, words joined with '_' (spaces are accepted and treated as '_').
std::string normalize(const std::string& raw) {
    std::string t = lower(trim(raw)), out;
    for (char ch : t) {
        char c = ch == ' ' ? '_' : ch;
        if (c == '_' && !out.empty() && out.back() == '_') continue;
        out += c;
    }
    return out;
}

std::atomic<bool> g_god{false};
// GameObjectPlayable::isInvincible(HitEvent const&): true while god mode is on
void on_is_invincible(cpu::Ctx& c) {
    if (g_god) cpu::skip_function(c, 1);
}

std::atomic<bool> g_bandana{false};
// Weapon::reduceAmmo(): every weapon (stasis too) spends a round through it; skipped while on
void on_reduce_ammo(cpu::Ctx& c) {
    if (g_bandana) cpu::skip_function(c, 0);
}

// Playable level ids: the maps in published/maps (00_* and test maps excluded)
const std::vector<std::string>& level_ids() {
    static std::vector<std::string> v = [] {
        std::vector<std::string> r;
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(hle::g_assets_dir + "/published/maps", ec)) {
            std::string n = e.path().filename().string();
            auto dot = n.find(".scene.sb");
            if (dot == std::string::npos || n.rfind("00_", 0) == 0 || n.find("test") != std::string::npos) continue;
            r.push_back(n.substr(0, dot));
        }
        std::sort(r.begin(), r.end());
        return r;
    }();
    return v;
}

void load_level(const std::string& id) {
    // Application::loadLevelCheated: the game's developer level select (main thread, between frames)
    static u32 fn_app = elf::find("_ZN11Application11getInstanceEv"),
               fn_load = elf::find("_ZN11Application16loadLevelCheatedEN5eastl12basic_stringIwN2im20StringEASTLAllocatorEEE");
    if (!fn_app || !fn_load) return print("cannot load levels: game function not found");
    print("loading " + id);
    cpu::call(fn_load, {cpu::call(fn_app, {}), guest::wstring(id)});
}

void give_all() {
    input::run_on_game_thread([](unsigned player) {
        static u32 fn = elf::find("_ZN18GameObjectPlayable19debugGiveAllWeaponsEv");
        if (!fn) return print("cannot give weapons: game function not found");
        cpu::call(fn, {player});
        print("all weapons given");
    });
}

// After "/spawn_": "slasher", "slasher_4", "slasher4", "slasher_x4". false + message if malformed.
bool parse_spawn(const std::string& a, std::string& name, int& count, std::string& err) {
    count = 1;
    for (const char* e : kEnemies) {
        std::string n = e;
        if (a.rfind(n, 0) != 0) continue;
        std::string rest = a.substr(n.size());
        if (!rest.empty() && rest[0] == '_') rest.erase(0, 1);
        if (!rest.empty() && rest[0] == 'x') rest.erase(0, 1);
        if (!rest.empty()) {
            char* end = nullptr;
            long v = std::strtol(rest.c_str(), &end, 10);
            if (!end || *end || v < 1) {
                err = "count must be 1 to " + std::to_string(kMaxCount) + ", e.g. /spawn_" + n + "_4";
                return false;
            }
            count = int(std::min<long>(v, kMaxCount));
        }
        name = n;
        return true;
    }
    err = a.empty() ? "which enemy? /spawn_NAME_COUNT, NAME: slasher lurker brute exploder pregnant swarm boss"
                    : "unknown enemy '" + a + "' (slasher lurker brute exploder pregnant swarm boss)";
    return false;
}

bool g_failed = false;  // set by fail() while running a command
void fail(const std::string& msg) {
    print(msg);
    g_failed = true;
}

void play_ui_sound(const char* name) {
    static u32 fn = elf::find("_ZN2im12SoundManager9playSoundERKN5eastl12basic_stringIwNS_20StringEASTLAllocatorEEE");
    if (fn) cpu::call(fn, {guest::wstring(name)});
}

void run(const std::string& raw) {
    std::string line = normalize(raw);
    if (line.empty()) return;
    print("> " + raw);
    if (line == "/help") {
        for (auto& c : kCommands) print(std::string(c.name) + c.args + "  - " + c.help);
        print("  NAME: slasher lurker brute exploder pregnant swarm boss; COUNT 1-20, e.g. /spawn_slasher_4");
        print("  Tab completes a suggestion; ~ or Esc closes the console");
        return;
    }
    if (line == "/clear") {
        std::lock_guard lk(g_lock);
        g_log.clear();
        return;
    }
    if (line == "/kill") {
        if (!input::world()) return fail("no level loaded");
        return kill_player();
    }
    if (line == "/debug_lights") {
        debugview::set_lights(!debugview::lights());
        return print(debugview::lights() ? "light view on: crosses = lights, rings = radius, dim = not lit now" : "light view off");
    }
    if (line == "/debug_hitboxes") {
        debugview::set_hitboxes(!debugview::hitboxes());
        return print(debugview::hitboxes() ? "hitbox view on: green player, red enemies, pink limbs, yellow other" : "hitbox view off");
    }
    if (line == "/debug_interact") {
        input::set_interact_debug(!input::interact_debug());
        return print(input::interact_debug() ? "interaction view on: cyan = your reach, white = usable, green = what E picks" : "interaction view off");
    }
    if (line == "/debug_vertexlight") {
        render::set_vertex_color_debug((render::vertex_color_debug() + 1) % 3);
        static const char* names[3] = {"vertex lighting: normal", "vertex lighting: hidden (textures and added lights only)", "vertex lighting: alone (no textures)"};
        return print(names[render::vertex_color_debug()]);
    }
    if (line == "/debug_off") {
        render::set_vertex_color_debug(0);
        debugview::set_lights(false);
        debugview::set_hitboxes(false);
        input::set_interact_debug(false);
        return print("debug views off");
    }
    if (line == "/god") {
        g_god = !g_god;
        return print(g_god ? "god mode on" : "god mode off");
    }
    if (line == "/bandana") {
        g_bandana = !g_bandana;
        return print(g_bandana ? "infinite ammo on" : "infinite ammo off");
    }
    if (line == "/level" || line.rfind("/level_", 0) == 0) {
        std::string id = line.size() > 7 ? line.substr(7) : "";
        auto& ids = level_ids();
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            std::string all;
            for (auto& l : ids) all += " " + l;
            return fail((id.empty() ? std::string("which level?") : "unknown level '" + id + "'.") + " levels:" + all);
        }
        set_open(false);
        return load_level(id);
    }
    if (line == "/give_all") {
        if (!input::world()) return fail("no level loaded");
        return give_all();
    }
    if (line == "/spawn" || line.rfind("/spawn_", 0) == 0) {
        std::string name, err;
        int count;
        if (!parse_spawn(line.size() > 7 ? line.substr(7) : "", name, count, err)) return fail(err);
        if (!input::world()) return fail("no level loaded");
        return spawn_enemies(name, count);
    }
    fail("unknown command '" + raw + "' (type /help)");
}

// Suggestions for the current input: completion text and a description.
struct Suggestion {
    std::string text, help;
};
std::vector<Suggestion> suggestions(const std::string& raw) {
    std::vector<Suggestion> out;
    std::string line = normalize(raw);
    if (line.empty() || line[0] != '/') return out;
    if (line.rfind("/level_", 0) == 0) {  // level ids
        std::string arg = line.substr(7);
        for (auto& l : level_ids())
            if (l.rfind(arg, 0) == 0) out.push_back({"/level_" + l, "/level_" + l});
        return out;
    }
    if (line.rfind("/spawn_", 0) == 0) {  // enemy names
        std::string arg = line.substr(7);
        for (const char* e : kEnemies) {
            std::string n = e;
            if (n.rfind(arg, 0) == 0 || arg.rfind(n, 0) == 0)
                out.push_back({"/spawn_" + n + "_", "/spawn_" + n + "_COUNT"});
        }
        return out;
    }
    for (auto& c : kCommands)
        if (std::string(c.name).rfind(line, 0) == 0)
            out.push_back({c.name, std::string(c.name) + c.args + "  - " + c.help});
    return out;
}

void set_open(bool open) {
    if (open == g_open) return;
    g_open = open;
    if (open) {
        static bool first = true;
        if (first) print("Type /help to learn the commands.");
        first = false;
        input::release_all();  // keys held while the console opens must not stay down
        SDL_StartTextInput();
    } else {
        SDL_StopTextInput();
    }
}

bool is_toggle(const SDL_Event& e) {
    return e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_GRAVE;
}

}  // namespace

bool is_open() { return g_open; }
void set_god(bool on) { g_god = on; }

void install() {
    if (u32 f = elf::find("_ZNK18GameObjectPlayable12isInvincibleERK8HitEvent"))
        cpu::hook_entry(f, "GameObjectPlayable::isInvincible", on_is_invincible);
    if (u32 f = elf::find("_ZN6Weapon10reduceAmmoEv")) cpu::hook_entry(f, "Weapon::reduceAmmo", on_reduce_ammo);
}

bool event(const SDL_Event& e) {
    if (is_toggle(e)) {
        set_open(!g_open);
        return true;
    }
    if (!g_open) return false;
    switch (e.type) {
    case SDL_TEXTINPUT:
        for (const char* p = e.text.text; *p; p++)
            if (*p >= 32 && *p < 127 && *p != '`' && *p != '~' && g_line.size() < 80) g_line += *p;
        return true;
    case SDL_KEYDOWN:
        switch (e.key.keysym.scancode) {
        case SDL_SCANCODE_ESCAPE: set_open(false); break;
        case SDL_SCANCODE_TAB: {  // complete the first suggestion
            auto sug = suggestions(g_line);
            if (!sug.empty() && normalize(g_line) != normalize(sug[0].text)) g_line = sug[0].text;
            break;
        }
        case SDL_SCANCODE_BACKSPACE:
            if (!g_line.empty()) g_line.pop_back();
            break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            if (!g_line.empty()) {
                g_history.push_back(g_line);
                g_failed = false;
                run(g_line);
                if (!g_failed) play_ui_sound("deadspace/ui/confirm");
            }
            g_line.clear();
            g_hist_pos = int(g_history.size());
            break;
        case SDL_SCANCODE_UP:
            if (g_hist_pos > 0) g_line = g_history[size_t(--g_hist_pos)];
            break;
        case SDL_SCANCODE_DOWN:
            if (g_hist_pos < int(g_history.size())) g_hist_pos++;
            g_line = g_hist_pos < int(g_history.size()) ? g_history[size_t(g_hist_pos)] : "";
            break;
        default: break;
        }
        return true;
    case SDL_KEYUP:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL: return true;
    default: return false;
    }
}

void draw(int w, int h) {
    if (!g_open) return;
    float px = std::max(2.0f, std::floor(h / 360.0f));  // font pixel
    float line_h = 10 * px, pad = 3 * px;
    std::vector<std::string> lines;
    {
        std::lock_guard lk(g_lock);
        lines.assign(g_log.begin(), g_log.end());
    }
    float box_h = pad * 2 + line_h * float(kMaxLog + 1);
    hle::overlay_rect(0, 0, float(w), box_h, 0x000000c0u);
    hle::overlay_rect(0, box_h, float(w), px / 2, 0x40c0ffffu);
    float y = pad + line_h * float(kMaxLog - lines.size());
    for (auto& l : lines) {
        hle::overlay_text(pad, y, px, l.c_str(), 0xd0d0d0ffu);
        y += line_h;
    }
    bool caret = (SDL_GetTicks() / 500) % 2 == 0;
    std::string prompt = "] " + g_line + (caret ? "_" : " ");
    float prompt_y = pad + line_h * float(kMaxLog);
    hle::overlay_text(pad, prompt_y, px, prompt.c_str(), 0x80e0ffffu);

    // suggestions: a box just above the input line, first entry highlighted (Tab completes it)
    auto sug = suggestions(g_line);
    if (sug.empty()) return;
    size_t widest = 0;
    for (auto& sg : sug) widest = std::max(widest, sg.help.size());
    float bw = float(widest + 2) * 6 * px + pad, bh = line_h * float(sug.size()) + pad;
    float bx = pad + 2 * 6 * px - pad, by = prompt_y - bh - px;
    hle::overlay_rect(bx, by, bw, bh, 0x101820f0u);
    hle::overlay_rect(bx, by, bw, px / 2, 0x40c0ffffu);
    for (size_t i = 0; i < sug.size(); i++) {
        float ly = by + pad * 0.5f + line_h * float(i);
        if (i == 0) hle::overlay_rect(bx, ly - px, bw, line_h, 0x2a4a60ffu);
        hle::overlay_text(bx + pad, ly, px, sug[i].help.c_str(), i == 0 ? 0xffffffffu : 0xa0b8c8ffu);
    }
}

}  // namespace console
