// Keyboard and mouse controls.
//
// The mobile game turns touches into a small set of player events, which we generate directly:
//   Vector2Event (type 1006): sub 1 = move stick (x, y in -1..1, y < 0 forward, flags bit0 = run),
//                             sub 3 / sub 5 = look deltas in screen pixels (normal / aiming)
//   ActionEvent  (type 1008): action ids handled by GameObjectPlayable::onEvent:
//       6 tap (raise weapon / fire / launch kinesis object), 7 switch fire mode, 8 quick turn,
//       9 lower weapon, 10 reload, 11 stasis, 12 zero-G jump, 13 stomp, 14 melee
//   Interactive objects (doors, power boxes, kinesis targets, benches...) receive action 16.
//   HUD buttons are StringIdEvents (type 1009) such as BTN_RIG.
// Events are queued here and delivered from the player's own update (game thread).
#include "input.h"

#include "devmode.h"
#include "interact.h"
#include "debugview.h"
#include "render/native.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "cpu.h"
#include "elf_loader.h"
#include "guest_util.h"
#include "hle/timing.h"
#include "jni/jni.h"
#include "log.h"
#include "memory.h"
#include "settings.h"

namespace input {

namespace {

enum EventKind { EV_MOVE, EV_LOOK, EV_ACTION, EV_INTERACT, EV_HUD, EV_WEAPON };
struct Event {
    EventKind kind;
    int a = 0;            // action id / weapon index
    float x = 0, y = 0;   // move vector / look delta
    int flags = 0;
    std::string id;       // HUD string id
};

std::mutex g_lock;
std::deque<Event> g_queue;

// game objects (written on the game thread)
std::atomic<u32> g_player{0}, g_hud{0}, g_world{0};
std::atomic<u32> g_player_tick{0};
std::atomic<bool> g_aiming{false}, g_paused{true};
std::atomic<int> g_state{0};

// game functions / vtables
u32 fn_player_event, fn_hud_event, fn_is_paused, fn_in_rig, fn_in_pause_menu;
u32 fn_change_weapon, fn_can_change_weapon;
u32 vt_vector2, vt_action;

bool g_trace = false;

// Cinematics (GameObjectCinematic, e.g. the opening fly-by): state +0x174, 1 = starting, 2 = playing,
// 3 = finished. The game ends one by calling stateTransition(3) when its animation is over
// (onAnimEnd); skipping (Space / Enter / click, A / Start) makes that same call at once, from the
// cinematic's own update, so the camera is handed back and the scripts waiting for it go on exactly as
// at its natural end. Should a cinematic still be playing half a second later, the old way takes over:
// the game clock is fast-forwarded until it has finished by itself.
std::atomic<bool> g_cine_skip{false};
std::atomic<int> g_cine_skip_frames{0};  // frames since a skip was asked for (0 = none pending)
u32 fn_cine_transition = 0;
std::atomic<u32> g_cinematic{0};        // cinematic currently playing
std::atomic<u32> g_cinematic_tick{0};
bool g_fast_forward = false;
constexpr double kSkipSpeed = 20.0;
u32 g_cine_tick_seen = 0;
int g_frames_without_cine = 1000;
bool g_test = false;  // scripted input: ignore window focus
bool g_give_weapons = false;  // debug: give all weapons on the first gameplay frame
float f32(u32 a);
void wf32(u32 a, float f);
std::string g_teleport_class;   // debug: move the player to the first object of this class
float g_teleport_off[3] = {0, 0, 1.5f};
u32 g_ticks_this_player = 0;

void log_transform(const char* what, u32 t) {
    LOGI("debug: %s origin %.2f %.2f %.2f  rows (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f)", what, f32(t + 48),
         f32(t + 52), f32(t + 56), f32(t), f32(t + 4), f32(t + 8), f32(t + 16), f32(t + 20), f32(t + 24), f32(t + 32),
         f32(t + 36), f32(t + 40));
}

void debug_teleport(u32 player, u32 world) {
    static u32 fn_get = elf::find("_ZNK10GameObject17getWorldTransformER11btTransform");
    u32 list = world + 0xb8;
    for (u32 node = mem::r32(list); node && node != list; node = mem::r32(node)) {
        u32 obj = mem::r32(node + 8);
        if (!obj || cpu::symbolize(mem::r32(obj)).find(g_teleport_class) == std::string::npos) continue;
        u32 t = mem::heap_alloc(64), pt = mem::heap_alloc(64);
        cpu::call(fn_get, {obj, t});
        cpu::call(fn_get, {player, pt});
        log_transform("target", t);
        log_transform("player", pt);
        // keep the player's orientation, stand g_teleport_dist in front of the target (its local +z)
        // world-space offset (debug): x, y, z from --teleport Class:x;y;z
        for (int k = 0; k < 3; k++) wf32(pt + 48 + 4 * k, f32(t + 48 + 4 * k) + g_teleport_off[k]);
        static u32 fn_teleport = elf::find("_ZN18GameObjectPlayable8teleportERK9btVector3");
        cpu::call(fn_teleport, {player, pt + 48});  // btVector3 origin (x, y, z, w)
        log_transform("player moved", pt);
        mem::heap_free(t);
        mem::heap_free(pt);
        return;
    }
    LOGW("debug: no %s found to teleport to", g_teleport_class.c_str());
}
bool g_keys[SDL_NUM_SCANCODES] = {};
bool g_mouse_btn[8] = {};

// Rebindable actions, in menu order
enum Bind {
    B_FORWARD, B_BACK, B_LEFT, B_RIGHT, B_RUN, B_AIM, B_FIRE, B_INTERACT, B_RELOAD, B_STASIS,
    B_QUICK_TURN, B_MELEE, B_STOMP, B_ALT_FIRE, B_JUMP, B_RIG, B_LOCATOR, B_COUNT
};
const Binding kBindings[B_COUNT] = {
    {"move_forward", "Move Forward", SDL_SCANCODE_W},
    {"move_back", "Move Back", SDL_SCANCODE_S},
    {"move_left", "Move Left", SDL_SCANCODE_A},
    {"move_right", "Move Right", SDL_SCANCODE_D},
    {"run", "Run", SDL_SCANCODE_LSHIFT},
    {"aim", "Aim", kMouseCode + SDL_BUTTON_RIGHT},
    {"fire", "Fire / Melee", kMouseCode + SDL_BUTTON_LEFT},
    {"interact", "Interact / Kinesis", SDL_SCANCODE_E},
    {"reload", "Reload", SDL_SCANCODE_R},
    {"stasis", "Stasis", SDL_SCANCODE_F},
    {"quick_turn", "Quick Turn", SDL_SCANCODE_Q},
    {"melee", "Melee", SDL_SCANCODE_V},
    {"stomp", "Stomp", SDL_SCANCODE_C},
    {"alt_fire", "Alt Fire", SDL_SCANCODE_X},
    {"jump", "Zero-G Jump", SDL_SCANCODE_SPACE},
    {"rig", "RIG / Weapons", SDL_SCANCODE_TAB},
    {"locator", "Navigation Line", SDL_SCANCODE_B},
};
int g_bind[B_COUNT];
std::atomic<int> g_capture{-1};  // action waiting for a key, or -1

const char* mouse_name(int button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return "Mouse Left";
    case SDL_BUTTON_RIGHT: return "Mouse Right";
    case SDL_BUTTON_MIDDLE: return "Mouse Middle";
    case SDL_BUTTON_X1: return "Mouse 4";
    case SDL_BUTTON_X2: return "Mouse 5";
    }
    return nullptr;
}
int code_from_name(const std::string& n) {
    for (int b = 1; b < 8; b++)
        if (const char* m = mouse_name(b); m && n == m) return kMouseCode + b;
    SDL_Scancode sc = SDL_GetScancodeFromName(n.c_str());
    return sc == SDL_SCANCODE_UNKNOWN ? 0 : int(sc);
}
// ---- controller bindings (defaults follow the Dead Space remake's layout)
enum PadBind { P_RUN, P_RUN_TOGGLE, P_AIM, P_FIRE, P_INTERACT, P_RELOAD, P_STASIS, P_QUICK_TURN, P_STOMP, P_ALT_FIRE, P_JUMP, P_RIG, P_LOCATOR, P_COUNT };
constexpr int kLT = kPadCode + 100, kRT = kPadCode + 101;
struct PadDef {
    Binding b;
    Bind act;
};
const PadDef kPad[P_COUNT] = {
    {{"run", "Run (hold)", kPadCode + SDL_CONTROLLER_BUTTON_LEFTSHOULDER}, B_RUN},
    {{"run_toggle", "Run (click)", kPadCode + SDL_CONTROLLER_BUTTON_LEFTSTICK}, B_RUN},
    {{"aim", "Aim", kLT}, B_AIM},
    {{"fire", "Fire / Melee", kRT}, B_FIRE},
    {{"interact", "Interact / Kinesis", kPadCode + SDL_CONTROLLER_BUTTON_A}, B_INTERACT},
    {{"reload", "Reload", kPadCode + SDL_CONTROLLER_BUTTON_X}, B_RELOAD},
    {{"stasis", "Stasis", kPadCode + SDL_CONTROLLER_BUTTON_Y}, B_STASIS},
    {{"quick_turn", "Quick Turn", kPadCode + SDL_CONTROLLER_BUTTON_B}, B_QUICK_TURN},
    {{"stomp", "Stomp (not aiming)", kPadCode + SDL_CONTROLLER_BUTTON_RIGHTSHOULDER}, B_STOMP},
    {{"alt_fire", "Alt Fire (aiming)", kPadCode + SDL_CONTROLLER_BUTTON_RIGHTSHOULDER}, B_ALT_FIRE},
    {{"jump", "Zero-G Jump (aiming)", kPadCode + SDL_CONTROLLER_BUTTON_B}, B_JUMP},
    {{"rig", "RIG / Weapons", kPadCode + SDL_CONTROLLER_BUTTON_BACK}, B_RIG},
    {{"locator", "Navigation Line", kPadCode + SDL_CONTROLLER_BUTTON_RIGHTSTICK}, B_LOCATOR},
};
int g_padbind[P_COUNT];
std::atomic<float> g_health_frac{1.0f};  // player health / maximum
bool g_run_toggle = false;      // sprint switched on with the click button, until the stick is let go
bool g_pad_down[128] = {};      // by code - kPadCode
float g_axis[8] = {};           // SDL_GameControllerAxis, -1..1
std::atomic<bool> g_pad_active{false};
SDL_GameController* g_pad = nullptr;

const char* pad_name(int code) {
    switch (code - kPadCode) {
    case SDL_CONTROLLER_BUTTON_A: return "A";
    case SDL_CONTROLLER_BUTTON_B: return "B";
    case SDL_CONTROLLER_BUTTON_X: return "X";
    case SDL_CONTROLLER_BUTTON_Y: return "Y";
    case SDL_CONTROLLER_BUTTON_BACK: return "Back";
    case SDL_CONTROLLER_BUTTON_START: return "Start";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L3";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R3";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-pad Up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-pad Down";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-pad Left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-pad Right";
    case 100: return "LT";
    case 101: return "RT";
    }
    return nullptr;
}
bool pad_code_down(int code) { return code >= kPadCode && code < kPadCode + 128 && g_pad_down[code - kPadCode]; }

void load_bindings() {
    for (int i = 0; i < B_COUNT; i++) {
        int c = code_from_name(settings::get_string(std::string("bind_") + kBindings[i].key, ""));
        g_bind[i] = c ? c : kBindings[i].def;
    }
    for (int i = 0; i < P_COUNT; i++) {
        std::string n = settings::get_string(std::string("padbind_") + kPad[i].b.key, "");
        g_padbind[i] = kPad[i].b.def;
        for (int c = kPadCode; c < kPadCode + 128; c++)
            if (const char* pn = pad_name(c); pn && n == pn) g_padbind[i] = c;
    }
}
bool code_down(int code) {
    if (code >= kMouseCode) return code - kMouseCode < 8 && g_mouse_btn[code - kMouseCode];
    return code > 0 && code < SDL_NUM_SCANCODES && g_keys[code];
}
bool bind_down(Bind b) { return code_down(g_bind[b]); }
bool code_bound(int code) { return std::find(std::begin(g_bind), std::end(g_bind), code) != std::end(g_bind); }

// main-thread input state
bool g_mouse_captured = false;
float g_look_dx = 0, g_look_dy = 0;
// Mouse look is linear: every mouse count turns the camera by the same angle (kLookDegPerCount
// at 100% sensitivity), whatever the speed, resolution or frame rate. Optional smoothing
// averages the last kSmoothFrames frames of movement, so the total turn stays the same.
constexpr float kLookDegPerCount = 0.05f;
constexpr float kLookRadPerCount = kLookDegPerCount * 3.14159265f / 180.0f;
constexpr int kSmoothFrames = 3;
float g_smooth_x[kSmoothFrames] = {}, g_smooth_y[kSmoothFrames] = {};
int g_smooth_i = 0;
bool g_moving = false;
bool g_rmb = false, g_lmb = false;
int g_pending_fire = 0;  // frames left to fire once the weapon is up (LMB while RMB is still raising it)
u32 g_last_tick_seen = 0;
int g_frames_without_player = 1000;

float f32(u32 a) {
    float f;
    u32 v = mem::r32(a);
    std::memcpy(&f, &v, 4);
    return f;
}
void wf32(u32 a, float f) {
    u32 v;
    std::memcpy(&v, &f, 4);
    mem::w32(a, v);
}

std::vector<std::function<void(unsigned)>> g_jobs;  // guarded by g_lock
std::atomic<bool> g_interact_debug{false};
void push(Event e) {
    std::lock_guard lk(g_lock);
    g_queue.push_back(std::move(e));
}

void action(int id) { push({EV_ACTION, id}); }

// ---------------------------------------------------------------- game thread
u32 g_evbuf = 0;

void send_vector2(u32 player, int sub, float x, float y, int flags) {
    std::memset(mem::host(g_evbuf), 0, 32);
    mem::w32(g_evbuf, vt_vector2);
    mem::w32(g_evbuf + 4, 1006);
    mem::w32(g_evbuf + 8, u32(sub));
    wf32(g_evbuf + 12, x);
    wf32(g_evbuf + 16, y);
    mem::w32(g_evbuf + 20, u32(flags));
    cpu::call(fn_player_event, {player, g_evbuf});
}

void send_action(u32 target_onevent, u32 target, int id) {
    std::memset(mem::host(g_evbuf), 0, 32);
    mem::w32(g_evbuf, vt_action);
    mem::w32(g_evbuf + 4, 1008);
    mem::w32(g_evbuf + 8, u32(id));
    cpu::call(target_onevent, {target, g_evbuf});
}

void hud_button(u32 hud, const std::string& id) {
    static u32 fn_ctor = elf::find("_ZN13StringIdEventC1ERKN5eastl12basic_stringIwN2im20StringEASTLAllocatorEEEi");
    u32 ws = guest::wstring(id);
    u32 ev = mem::heap_alloc(64);
    std::memset(mem::host(ev), 0, 64);
    if (fn_ctor) {
        cpu::call(fn_ctor, {ev, ws, 0});
    } else {  // build it like the constructor does
        mem::w32(ev, elf::find("_ZTV13StringIdEvent") + 8);
        mem::w32(ev + 4, 1009);
        std::memcpy(mem::host(ev + 8), mem::host(ws), 20);
    }
    cpu::call(fn_hud_event, {hud, ev});
}

// Free look: while the player stands still (not aiming), horizontal look orbits the camera around
// him instead of turning him. g_free_yaw is that orbit (radians, same sense as
// GameObject::adjustYaw); moving, aiming or any action eases the player round by it (draining it),
// so he ends up facing where the camera looks. Game thread only.
float g_free_yaw = 0;
bool g_free_moving = false;  // last move input was non-zero
u32 g_lookat_sites[4] = {};  // return addresses of CameraController's getLookAtTransform calls

bool free_look_enabled() { return settings::get_int("free_look", 1) != 0; }

// Turns the player by `angle` radians of the free-look orbit and removes it from the orbit, so the
// view stays put while he turns.
void turn_free_look(u32 player, float angle) {
    static u32 fn_adjust_yaw = elf::find("_ZN10GameObject9adjustYawEf");
    g_free_yaw -= angle;
    u32 v;
    std::memcpy(&v, &angle, 4);
    if (player && fn_adjust_yaw) cpu::call(fn_adjust_yaw, {player, v});
}

// Moving, aiming or acting starts turning the player to face the camera; tick_free_look eases him
// round over a few frames (time constant kTurnTau) rather than snapping.
bool g_free_turning = false;
constexpr float kTurnTauMs = 120.0f;
void snap_free_look(u32) {
    if (g_free_yaw != 0) g_free_turning = true;
}
void tick_free_look(u32 player, int dt_ms) {
    if (!g_free_turning) return;
    float step = g_free_yaw * (1.0f - std::exp(-float(std::clamp(dt_ms, 1, 100)) / kTurnTauMs));
    if (std::fabs(g_free_yaw - step) < 0.002f) {  // close enough: finish the turn
        step = g_free_yaw;
        g_free_turning = false;
    }
    turn_free_look(player, step);
}

// CameraController::getLookAtTransform(out, eye, target, up): the controller has just written its
// eye/target/up for this frame; orbit them around the player by the free-look yaw.
float g_cam[9] = {};  // eye, target, up of the gameplay camera as last computed
u32 g_cam_ms = 0;
void record_camera(cpu::Ctx& c) {
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) g_cam[i * 3 + k] = f32(c.reg(1 + i) + u32(k) * 4);
    g_cam_ms = SDL_GetTicks();
}
void on_look_at(cpu::Ctx& c) {
    u32 player = g_player;
    u32 lr = c.lr();
    if (std::find(std::begin(g_lookat_sites), std::end(g_lookat_sites), lr) == std::end(g_lookat_sites)) return;
    record_camera(c);
    // only while that player is alive and updating (not during level loads)
    if (g_free_yaw == 0 || !player || g_frames_without_player > 2) return;
    u32 body = mem::r32(player + 0x58);  // btRigidBody: basis rows at +4, origin at +0x34
    if (!body) return;
    float px = f32(body + 0x34), pz = f32(body + 0x3c);
    float cs = std::cos(g_free_yaw), sn = std::sin(g_free_yaw);
    for (u32 v : {c.reg(1), c.reg(2)}) {
        float x = f32(v) - px, z = f32(v + 8) - pz;
        wf32(v, px + x * cs + z * sn);
        wf32(v + 8, pz - x * sn + z * cs);
    }
    // the up vector tilts with the camera pitch; it must turn too or the view rolls
    u32 up = c.reg(3);
    float ux = f32(up), uz = f32(up + 8);
    wf32(up, ux * cs + uz * sn);
    wf32(up + 8, -ux * sn + uz * cs);
    record_camera(c);
}

// ---- grabs
// A necromorph that grabs the player starts a struggle the game drives with touch gestures
// (GameObjectPlayable grapple state at +0x37c): 1 = swipe along the first arrow, 5 = tap repeatedly,
// 7 = swipe along the second arrow. Swarms that jump on the player (slots of 12 bytes from +0x3f4) are
// tapped off one by one. With a keyboard one key does all of it: each press makes the move the current
// phase is waiting for. The game has this for gamepads (emulateGrappleGestureEvent); taps are sent as
// the pointer events its handlers expect.
int g_grab_state = 0;  // grapple state last seen; 100 = swarms attached
bool struggle(u32 player) {
    static u32 fn_emulate = elf::find("_ZN18GameObjectPlayable26emulateGrappleGestureEventEa"),
               fn_grapple_ev = elf::find("_ZN18GameObjectPlayable14onGrappleEventERKN2im5EventE"),
               fn_swarm_ev = elf::find("_ZN18GameObjectPlayable19onGrappleSwarmEventERKN2im5EventE");
    static u32 ev = 0;
    if (!ev) {
        ev = mem::data_alloc(64);
        std::memset(mem::host(ev), 0, 64);
    }
    if (!fn_emulate || !fn_grapple_ev || !fn_swarm_ev) return false;
    mem::w32(ev + 4, 1001);  // pointer press
    int gs = s32(mem::r32(player + 0x37c));
    if (gs == 1) return cpu::call(fn_emulate, {player, 1}), true;
    if (gs == 7) return cpu::call(fn_emulate, {player, 0}), true;
    if (gs == 5) return cpu::call(fn_grapple_ev, {player, ev}), true;
    if (gs > 0 && gs < 10) return true;  // between phases: the key does nothing else
    for (u32 slot = 0; slot < 8; slot++) {
        u32 sw = mem::r32(player + 0x3f4 + slot * 0xc);
        if (sw < 0x10000 || mem::r32(sw) < 0x10000) continue;
        if (!(cpu::call(mem::r32(mem::r32(sw) + 0x110), {sw}) & 0xff) || (mem::r32(sw + 0x48) & 1)) continue;
        float sx = 0, sy = 0;
        if (!objs::to_screen(g_world, objs::pos(sw), sx, sy)) continue;
        mem::w32(ev + 8, u32(s32(sx)));
        mem::w32(ev + 0xc, u32(s32(sy)));
        cpu::call(fn_swarm_ev, {player, ev});
        return true;
    }
    return false;
}
int grab_state(u32 player) {
    int gs = s32(mem::r32(player + 0x37c));
    if (gs > 0 && gs < 10) return gs;
    for (u32 slot = 0; slot < 8; slot++) {
        u32 sw = mem::r32(player + 0x3f4 + slot * 0xc);
        if (sw >= 0x10000 && mem::r32(sw) >= 0x10000 && !(mem::r32(sw + 0x48) & 1)) return 100;
    }
    return 0;
}

void on_player_update(cpu::Ctx& c) {
    u32 player = c.reg(0);
    int dt_ms = int(c.reg(1));
    if (g_trace && g_player_tick == 100) LOGI("[input] player update dt=%d", dt_ms);
    if (g_player != player) {
        LOGI("input: player object %08x", player);
        if (g_give_weapons) {
            cpu::call(elf::find("_ZN18GameObjectPlayable19debugGiveAllWeaponsEv"), {player});
            LOGI("input: debug - all weapons given");
        }
    }
    g_player = player;
    g_player_tick++;
    if (++g_ticks_this_player == 600 && !g_teleport_class.empty() && g_world) debug_teleport(player, g_world);
    g_aiming = mem::r8(player + 0x1ed) != 0;
    {
        // health as the RIG shows it, for the controller's light bar
        static u32 fn_health_max = elf::find("_ZNK18GameObjectPlayable12getHealthMaxEv");
        float hp = f32(player + 0x25c), mx = 0;
        if (fn_health_max) {
            u32 v = cpu::call(fn_health_max, {player});
            std::memcpy(&mx, &v, 4);
        }
        g_health_frac = mx > 0 ? std::clamp(hp / mx, 0.0f, 1.0f) : 1.0f;
    }
    g_state = s32(mem::r32(player + 0x210));
    {
        int gs = grab_state(player);
        if (gs != g_grab_state) LOGI("input: grab state %d -> %d", g_grab_state, gs);
        g_grab_state = gs;
    }
    u32 hud = g_hud;
    if (hud) {
        bool paused = (cpu::call(fn_is_paused, {hud}) & 0xff) || (cpu::call(fn_in_rig, {hud}) & 0xff) ||
                      (cpu::call(fn_in_pause_menu, {hud}) & 0xff);
        g_paused = paused;
    }
    if (!g_evbuf) g_evbuf = mem::data_alloc(64);
    if (g_frames_without_cine < 10 || !free_look_enabled()) {
        g_free_yaw = 0;
        g_free_turning = false;
    }

    std::deque<Event> q;
    std::vector<std::function<void(unsigned)>> jobs;
    {
        std::lock_guard lk(g_lock);
        q.swap(g_queue);
        jobs.swap(g_jobs);
    }
    for (auto& j : jobs) j(player);
    for (auto& e : q) {
        switch (e.kind) {
        case EV_MOVE:
            g_free_moving = e.x != 0 || e.y != 0;
            if (g_free_moving) snap_free_look(player);
            send_vector2(player, 1, e.x, e.y, e.flags);
            break;
        case EV_LOOK: {
            // e.x/e.y are mouse counts (already scaled by the port sensitivity); the game turns
            // look pixels into yaw at Tweaks+0x54 radians per pixel (pitch uses its own matching rate)
            static u32 tweaks = 0;
            if (!tweaks) {
                tweaks = cpu::call(elf::find("_ZN6Tweaks3getEv"), {});
                if (g_trace) LOGI("[input] look tweaks yaw=%g rad/px pitch=%g/px", f32(tweaks + 0x54), f32(tweaks + 0x58));
            }
            float yaw_rate = tweaks ? f32(tweaks + 0x54) : 0.0f;
            float px = yaw_rate > 0 ? kLookRadPerCount / yaw_rate : 1.0f;
            e.x *= px;
            e.y *= px;
            if (free_look_enabled() && !g_free_moving && !g_aiming && g_state == 0) {
                // standing still: orbit the camera instead of turning the player (pitch stays his)
                static u32 fn_settings = elf::find("_ZN8Settings11getInstanceEv"),
                           fn_sens = elf::find("_ZNK8Settings14getSensitivityEv");
                u32 sv = cpu::call(fn_sens, {cpu::call(fn_settings, {})});
                float sens;
                std::memcpy(&sens, &sv, 4);
                g_free_yaw = std::remainder(g_free_yaw - yaw_rate * e.x * sens, 6.2831853f);
                g_free_turning = false;
                e.x = 0;
                if (e.y == 0) break;
            }
            send_vector2(player, 5, e.x, e.y, 0);
            send_vector2(player, 3, e.x, e.y, 0);
            break;
        }
        case EV_ACTION:
            if ((e.a == 6 || e.a == 14) && struggle(player)) break;  // fire / melee during a grab: break free
            snap_free_look(player);
            send_action(fn_player_event, player, e.a);
            break;
        case EV_INTERACT: {
            float look = g_free_yaw;  // where the camera looks, before the player turns to it
            if (e.a == 1) {           // F9: report only
                std::string r = interact::preview(player, g_world, look);
                LOGI("[interact] %s", r.c_str());
                if (dev::active()) dev::out(r);
                break;
            }
            if (struggle(player)) break;  // during a grab the key breaks free
            snap_free_look(player);
            interact::use(player, g_world, look, g_trace || dev::active());
            break;
        }
        case EV_HUD:
            if (hud) hud_button(hud, e.id);
            break;
        case EV_WEAPON: {
            u32 slot = mem::r32(player + 0x2d4 + 4 * u32(e.a));  // owned weapons
            if (slot && (cpu::call(fn_can_change_weapon, {player}) & 0xff))
                cpu::call(fn_change_weapon, {player, u32(e.a)});
            break;
        }
        }
    }
    if (g_interact_debug && !g_paused) interact::draw_debug(player, g_world, g_free_yaw);
    if (!g_paused) debugview::draw(player, g_world);
    if (g_player_tick % 30 == 0) interact::repair_objective_panel(g_world);
    if (g_state != 0 && g_free_yaw != 0) {
        turn_free_look(player, g_free_yaw);
        g_free_turning = false;
    }
    if (g_aiming) snap_free_look(player);
    tick_free_look(player, dt_ms);
}

void on_hud_event(cpu::Ctx& c) {
    g_hud = c.reg(0);
    if (!g_trace) return;
    u32 ev = c.reg(1);
    u32 type = mem::r32(ev + 4);
    if (type == 10 || type == 2 || type == 1000) return;
    std::string id;
    if (type == 1009)  // StringIdEvent: wstring at +8
        for (u32 p = mem::r32(ev + 8), e = mem::r32(ev + 12); p && p < e && id.size() < 64; p += 2) id += char(mem::r16(p));
    LOGI("[input] Hud::onEvent ev=%s type=%u sub=%d %s", cpu::symbolize(mem::r32(ev)).c_str(), type,
         s32(mem::r32(ev + 8)), id.c_str());
}

void on_player_event(cpu::Ctx& c) {
    if (!g_trace) return;
    u32 ev = c.reg(1);
    u32 type = mem::r32(ev + 4);
    if (type == 10 || type == 1000) return;
    LOGI("[input] player event %s type=%u sub=%d %.2f %.2f %x", cpu::symbolize(mem::r32(ev)).c_str(), type,
         s32(mem::r32(ev + 8)), f32(ev + 12), f32(ev + 16), mem::r32(ev + 20));
}

void on_scheme_init(cpu::Ctx& c) { g_world = c.reg(1); }

// GameObjectPlayable destructor (level change, death reload): forget everything about that player
void on_player_destroyed(cpu::Ctx& c) {
    if (c.reg(0) != g_player) return;
    g_player = 0;
    g_free_yaw = 0;
    g_free_turning = false;
    g_frames_without_player = 1000;
}

// GameObjectPlayable::revertPitch(rate, target) eases the camera pitch back to level. Walking or
// running calls it from updateFacing, which pulls mouse aim back to the horizon; that call gets
// a zero rate. Other callers (grapples, scripted states) keep it.
u32 g_revert_from_walk = 0;  // return address of the updateFacing call
void on_revert_pitch(cpu::Ctx& c) {
    if (c.lr() == g_revert_from_walk) c.set_reg(1, 0);
}

// A power node lock's dialog ("Use a Power Node?" / "you have none"): while it is up (state at +0x264),
// the game is not taking gameplay input, so the mouse is freed and the menu keys / controller work on its
// buttons.
std::atomic<u32> g_dialog_ms{0};
void on_lock_update(cpu::Ctx& c) {
    if (mem::r32(c.reg(0) + 0x264) != 0) g_dialog_ms = SDL_GetTicks() | 1;
}
// The bench (state at +0x224: 3..9 from the moment it is used until its screen has closed) and the
// store (+0x1e4: 1..5) are full-screen menus over the game: the same applies, and the camera stays put.
void on_bench_update(cpu::Ctx& c) {
    u32 st = mem::r32(c.reg(0) + 0x224);
    if (st >= 3 && st <= 9) g_dialog_ms = SDL_GetTicks() | 1;
}
void on_store_update(cpu::Ctx& c) {
    u32 st = mem::r32(c.reg(0) + 0x1e4);
    if (st >= 1 && st <= 5) g_dialog_ms = SDL_GetTicks() | 1;
}
bool dialog_open() { return g_dialog_ms && SDL_GetTicks() - g_dialog_ms < 200; }

void on_cinematic_update(cpu::Ctx& c) {
    u32 cine = c.reg(0);
    s32 state = s32(mem::r32(cine + 0x174));
    if (state != 1 && state != 2) {
        if (g_cinematic == cine) g_cinematic = 0;
        return;
    }
    g_cinematic = cine;
    g_cinematic_tick++;
    if (g_cine_skip && fn_cine_transition) {
        g_cine_skip = false;
        LOGI("input: cinematic %08x ended by skip (was in state %d)", cine, state);
        // (state 2 is the cinematic's last stretch, in which the HUD and the objective come up: it is
        // entered first, as the game does, then the end)
        if (state == 1) cpu::call(fn_cine_transition, {cine, 2});
        cpu::call(fn_cine_transition, {cine, 3});
    }
}

// ---------------------------------------------------------------- main thread
void back_key() {
    static int module = -1;
    if (module < 0)
        module = jni::call_native("com/ea/blast/ModuleCatalog", "NativeGetModuleTypeIdPhysicalKeyboard", "()I",
                                  jni::class_object("com/ea/blast/ModuleCatalog"), {}).i();
    auto kb = jni::make("com/ea/blast/PhysicalKeyboardAndroid");
    for (const char* m : {"NativeOnKeyDown", "NativeOnKeyUp"})
        jni::call_native("com/ea/blast/KeyboardAndroid", m, "(III)V", kb,
                         {jni::JValue::I(module), jni::JValue::I(4), jni::JValue::I(0)});
}

bool cinematic_playing() { return g_frames_without_cine < 10; }

bool gameplay_active() {
    return g_frames_without_player < 10 && !g_paused && g_player != 0 && !cinematic_playing() && !dialog_open();
}

float mouse_scale() { return std::max(1, settings::get_int("mouse_sensitivity", 100)) / 100.0f; }
bool mouse_smoothing() { return settings::get_int("mouse_smoothing", 0) != 0; }

}  // namespace

void set_trace(bool on) { g_trace = on; }

int binding_count() { return B_COUNT; }
int pad_binding_count() { return P_COUNT; }
bool using_pad() { return g_pad_active; }
const Binding& binding(int i) { return i >= kPadBindBase ? kPad[i - kPadBindBase].b : kBindings[i]; }
int binding_code(int i) { return i >= kPadBindBase ? g_padbind[i - kPadBindBase] : g_bind[i]; }
void reset_pad_bindings() {
    for (int i = 0; i < P_COUNT; i++) {
        g_padbind[i] = kPad[i].b.def;
        settings::set_string(std::string("padbind_") + kPad[i].b.key, code_name(g_padbind[i]));
    }
}
std::string action_label(const std::string& key, bool pad) {
    if (pad) {
        if (key.rfind("move_", 0) == 0) return "Left Stick";
        const std::string k = key == "melee" ? "fire" : key;  // melee is the fire button while not aiming
        for (int i = 0; i < P_COUNT; i++)
            if (k == kPad[i].b.key) return code_name(g_padbind[i]);
        return "?";
    }
    for (int i = 0; i < B_COUNT; i++)
        if (key == kBindings[i].key) return code_name(g_bind[i]);
    return "?";
}
void set_binding(int i, int code) {
    if (i >= kPadBindBase) {  // controller: buttons may be shared (the action taken depends on aiming)
        if (i - kPadBindBase >= P_COUNT) return;
        g_padbind[i - kPadBindBase] = code;
        settings::set_string(std::string("padbind_") + kPad[i - kPadBindBase].b.key, code_name(code));
        return;
    }
    if (i < 0 || i >= B_COUNT) return;
    for (int k = 0; k < B_COUNT; k++)
        if (k != i && g_bind[k] == code) {
            g_bind[k] = g_bind[i];
            settings::set_string(std::string("bind_") + kBindings[k].key, code_name(g_bind[k]));
        }
    g_bind[i] = code;
    settings::set_string(std::string("bind_") + kBindings[i].key, code_name(code));
}
void reset_bindings() {
    for (int i = 0; i < B_COUNT; i++) {
        g_bind[i] = kBindings[i].def;
        settings::set_string(std::string("bind_") + kBindings[i].key, code_name(g_bind[i]));
    }
}
std::string code_name(int code) {
    if (code >= kPadCode) {
        const char* p = pad_name(code);
        return p ? p : "Button ?";
    }
    if (code >= kMouseCode) {
        const char* m = mouse_name(code - kMouseCode);
        return m ? m : "Mouse ?";
    }
    const char* n = SDL_GetScancodeName(SDL_Scancode(code));
    return n && *n ? n : "?";
}
const std::vector<int>& bindable_codes() {
    static std::vector<int> v = [] {
        std::vector<int> r;
        for (int sc = 1; sc < SDL_NUM_SCANCODES; sc++) {
            const char* n = SDL_GetScancodeName(SDL_Scancode(sc));
            if (n && *n) r.push_back(sc);
        }
        for (int b = 1; b < 8; b++)
            if (mouse_name(b)) r.push_back(kMouseCode + b);
        for (int c = kPadCode; c < kPadCode + 128; c++)
            if (pad_name(c)) r.push_back(c);
        return r;
    }();
    return v;
}
void run_on_game_thread(std::function<void(unsigned)> fn) {
    std::lock_guard lk(g_lock);
    g_jobs.push_back(std::move(fn));
}
unsigned world() { return g_world; }
int grab() { return g_grab_state; }
unsigned player() { return g_frames_without_player > 2 ? 0u : unsigned(g_player); }
bool camera(float eye[3], float target[3], float up[3]) {
    if (SDL_GetTicks() - g_cam_ms > 100) return false;
    std::memcpy(eye, g_cam, 12);
    std::memcpy(target, g_cam + 3, 12);
    std::memcpy(up, g_cam + 6, 12);
    return true;
}
float free_look_yaw() { return g_free_yaw; }
void set_interact_debug(bool on) { g_interact_debug = on; }
bool interact_debug() { return g_interact_debug; }
bool controls_active() { return g_mouse_captured; }
void press_back() { back_key(); }
void begin_capture(int i) { g_capture = i; }
bool capturing() { return g_capture >= 0; }
bool fast_forwarding() { return g_fast_forward; }
void set_test_mode(bool on) { g_test = on; }
void set_give_weapons(bool on) { g_give_weapons = on; }
void set_teleport(const std::string& cls, float x, float y, float z) {
    g_teleport_class = cls;
    g_teleport_off[0] = x;
    g_teleport_off[1] = y;
    g_teleport_off[2] = z;
}

void install() {
    load_bindings();
    fn_player_event = elf::find("_ZN18GameObjectPlayable7onEventERKN2im5EventE");
    fn_hud_event = elf::find("_ZN3Hud7onEventERKN2im5EventE");
    fn_is_paused = elf::find("_ZNK3Hud8isPausedEv");
    fn_in_rig = elf::find("_ZNK3Hud7isInRigEv");
    fn_in_pause_menu = elf::find("_ZNK3Hud13isInPauseMenuEv");
    fn_change_weapon = elf::find("_ZN18GameObjectPlayable12changeWeaponENS_11WeaponIndexE");
    fn_can_change_weapon = elf::find("_ZNK18GameObjectPlayable15canChangeWeaponEv");
    fn_cine_transition = elf::find("_ZN19GameObjectCinematic15stateTransitionENS_5StateE");
    vt_vector2 = elf::find("_ZTV12Vector2Event") + 8;
    vt_action = elf::find("_ZTV11ActionEvent") + 8;
    if (!fn_player_event || !fn_hud_event || !fn_is_paused || !fn_in_rig || !fn_in_pause_menu || vt_vector2 == 8 ||
        vt_action == 8) {
        LOGE("input: game functions not found; keyboard/mouse gameplay controls disabled");
        return;
    }
    struct Hook {
        const char* sym;
        const char* name;
        cpu::HleFn fn;
    };
    const Hook hooks[] = {
        {"_ZN18GameObjectPlayable8onUpdateEi", "GameObjectPlayable::onUpdate", on_player_update},
        {"_ZN3Hud7onEventERKN2im5EventE", "Hud::onEvent", on_hud_event},
        {"_ZN18GameObjectPlayable7onEventERKN2im5EventE", "GameObjectPlayable::onEvent", on_player_event},
        {"_ZN16InputSchemeBasic4initEP9GameWorld", "InputSchemeBasic::init", on_scheme_init},
        {"_ZN18GameObjectPlayableD0Ev", "GameObjectPlayable::~GameObjectPlayable (deleting)", on_player_destroyed},
        {"_ZN18GameObjectPlayable11revertPitchEff", "GameObjectPlayable::revertPitch", on_revert_pitch},
        {"_ZN19GameObjectCinematic8onUpdateEi", "GameObjectCinematic::onUpdate", on_cinematic_update},
        {"_ZN23GameObjectPowerNodeLock8onUpdateEi", "GameObjectPowerNodeLock::onUpdate", on_lock_update},
        {"_ZN5Bench8onUpdateEi", "Bench::onUpdate", on_bench_update},
        {"_ZN5Store8onUpdateEi", "Store::onUpdate", on_store_update},
        {"_ZN16CameraController18getLookAtTransformERN3m3g9TransformERK9btVector3S5_S5_",
         "CameraController::getLookAtTransform", on_look_at},
    };
    // CameraController's own getLookAtTransform calls (eye/target are its fields, rewritten each frame)
    const struct {
        const char* sym;
        u32 ret;
    } sites[] = {{"_ZN16CameraController17applyCameraSourceEPK12CameraSource", 0x5c},
                 {"_ZN16CameraController18applyCameraSourcesEPK12CameraSourceS2_f", 0xfc},
                 {"_ZN16CameraController6updateEi", 0x1f4},
                 {"_ZN16CameraController6updateEi", 0x2ac}};
    for (int i = 0; i < 4; i++)
        if (u32 f = elf::find(sites[i].sym)) g_lookat_sites[i] = f + sites[i].ret;
    if (u32 f = elf::find("_ZN18GameObjectPlayable12updateFacingERK9btVector3i")) g_revert_from_walk = f + 0xd4;
    for (auto& h : hooks) {
        u32 a = elf::find(h.sym);
        if (!a || !cpu::hook_entry(a, h.name, h.fn)) LOGW("input: cannot hook %s", h.name);
    }
}

// Press / release of a bound input during gameplay.
void press_action(int b) {
    switch (b) {
    case B_AIM:
        g_rmb = true;
        // drop to walking first: GameObjectPlayable::setRunning ignores changes while aiming
        if (g_moving) {
            float mx = float(bind_down(B_RIGHT)) - float(bind_down(B_LEFT));
            float my = float(bind_down(B_BACK)) - float(bind_down(B_FORWARD));
            float len = std::sqrt(mx * mx + my * my);
            if (len > 0) push({EV_MOVE, 0, mx / len, my / len, 0});
        }
        if (!g_aiming) action(6);  // raise weapon
        break;
    case B_FIRE:
        g_lmb = true;
        if (g_aiming || g_state == 0x12) {
            action(6);  // fire / launch kinesis object
        } else if (g_rmb) {
            g_pending_fire = 30;  // weapon still coming up: fire as soon as it is
        } else {
            action(14);  // not aiming: melee
        }
        break;
    case B_INTERACT: push({EV_INTERACT}); break;
    case B_RELOAD: action(10); break;
    case B_STASIS: action(11); break;
    case B_QUICK_TURN: action(8); break;
    case B_MELEE: action(14); break;
    case B_STOMP: action(13); break;
    case B_ALT_FIRE: action(7); break;  // switch fire mode
    case B_JUMP: action(12); break;     // zero-G jump
    case B_RIG: push({EV_HUD, 0, 0, 0, 0, "BTN_RIG"}); break;
    case B_LOCATOR:  // the HUD only takes BTN_MARKER while the RIG is open: open it, then press it
        push({EV_HUD, 0, 0, 0, 0, "BTN_RIG"});
        push({EV_HUD, 0, 0, 0, 0, "BTN_MARKER"});
        break;
    default: break;  // movement and run are polled each frame
    }
}
void release_action(int b) {
    if (b == B_AIM) {
        g_rmb = false;
        g_pending_fire = 0;
        action(9);  // lower weapon
    } else if (b == B_FIRE) {
        g_lmb = false;
    }
}
// Returns true if the code did something.
bool press_code(int code) {
    if (code == SDL_SCANCODE_F9) {  // interaction report (see interact.cpp)
        push({EV_INTERACT, 1});
        return true;
    }
    if (code == SDL_SCANCODE_F5) {  // painted vertex lighting: normal / hidden / alone
        render::set_vertex_color_debug((render::vertex_color_debug() + 1) % 3);
        LOGI("vertex lighting debug mode %d", render::vertex_color_debug());
        return true;
    }
    if (code == SDL_SCANCODE_F6) {  // light placement view
        debugview::set_lights(!debugview::lights());
        LOGI("light debug view %s", debugview::lights() ? "on" : "off");
        return true;
    }
    if (code == SDL_SCANCODE_F7) {  // hitbox view
        debugview::set_hitboxes(!debugview::hitboxes());
        LOGI("hitbox debug view %s", debugview::hitboxes() ? "on" : "off");
        return true;
    }
    if (code == SDL_SCANCODE_F10) {  // interaction debug view
        g_interact_debug = !g_interact_debug;
        LOGI("interaction debug view %s", g_interact_debug ? "on" : "off");
        return true;
    }
    bool any = false;
    for (int i = 0; i < B_COUNT; i++)
        if (g_bind[i] == code) {
            press_action(i);
            any = true;
        }
    if (any) return true;
    // fixed extras, used only while not bound to something else
    if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_4) {  // the four guns (slot 5 is the stasis module)
        push({EV_WEAPON, code - SDL_SCANCODE_1});
        return true;
    }
    if (code == kMouseCode + SDL_BUTTON_X1) {
        press_action(B_MELEE);
        return true;
    }
    if (code == kMouseCode + SDL_BUTTON_MIDDLE) {
        press_action(B_ALT_FIRE);
        return true;
    }
    return false;
}
void release_code(int code) {
    for (int i = 0; i < B_COUNT; i++)
        if (g_bind[i] == code) release_action(i);
}

void release_all() {
    for (int i = 0; i < B_COUNT; i++)
        if (code_down(g_bind[i])) release_action(i);
    std::memset(g_keys, 0, sizeof g_keys);
    std::memset(g_mouse_btn, 0, sizeof g_mouse_btn);
}

// ---- controller input
constexpr Uint32 kSynth = 0x50414453;  // marks key events made from controller input (menus)
void synth_key(SDL_Scancode sc, SDL_Keycode sym) {
    SDL_Event e{};
    e.type = SDL_KEYDOWN;
    e.key.state = SDL_PRESSED;
    e.key.keysym.scancode = sc;
    e.key.keysym.sym = sym;
    e.key.keysym.unused = kSynth;
    SDL_PushEvent(&e);
    e.type = SDL_KEYUP;
    e.key.state = SDL_RELEASED;
    SDL_PushEvent(&e);
}

void pad_button(int code, bool down) {
    if (code < kPadCode || code >= kPadCode + 128) return;
    g_pad_down[code - kPadCode] = down;
    const int b = code - kPadCode;
    if (!g_mouse_captured) {
        // menus, cinematics, loading screens: the D-pad moves the highlight, A confirms, B / Start go back
        if (!down) return;
        bool in_rig = g_hud && fn_in_rig && (cpu::call(fn_in_rig, {g_hud}) & 0xff);
        switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: synth_key(SDL_SCANCODE_UP, SDLK_UP); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: synth_key(SDL_SCANCODE_DOWN, SDLK_DOWN); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: synth_key(SDL_SCANCODE_LEFT, SDLK_LEFT); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: synth_key(SDL_SCANCODE_RIGHT, SDLK_RIGHT); break;
        case SDL_CONTROLLER_BUTTON_A: synth_key(SDL_SCANCODE_RETURN, SDLK_RETURN); break;
        case SDL_CONTROLLER_BUTTON_B:
        case SDL_CONTROLLER_BUTTON_START:
            if (in_rig) hud_button(g_hud, "BTN_RIG");  // (the back key would open the pause menu over it)
            else back_key();
            break;
        default:
            if (in_rig && code == g_padbind[P_RIG]) hud_button(g_hud, "BTN_RIG");
            break;
        }
        return;
    }
    if (b == SDL_CONTROLLER_BUTTON_START) {
        if (down) back_key();  // pause
        return;
    }
    const bool aiming = g_aiming || g_rmb;
    bool bound = false;
    for (int p = 0; p < P_COUNT; p++) {
        if (g_padbind[p] != code) continue;
        bound = true;
        const Bind act = kPad[p].act;
        if (!down) {
            if (act == B_AIM || act == B_FIRE) release_action(act);
            continue;
        }
        // one button can carry two actions: which one depends on whether the weapon is up
        if (p == P_RUN_TOGGLE) g_run_toggle = !g_run_toggle;
        if (act == B_RUN || ((act == B_ALT_FIRE || act == B_JUMP) && !aiming) || ((act == B_STOMP || act == B_QUICK_TURN) && aiming)) continue;
        press_action(act);
    }
    if (bound || !down) return;
    // D-pad: the four weapons (unless bound to something)
    int w = b == SDL_CONTROLLER_BUTTON_DPAD_UP ? 0 : b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT ? 1 : b == SDL_CONTROLLER_BUTTON_DPAD_DOWN ? 2
            : b == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? 3 : -1;
    if (w >= 0) push({EV_WEAPON, w});
}

// Controller events; true if `e` was one.
bool pad_event(const SDL_Event& e) {
    switch (e.type) {
    case SDL_CONTROLLERDEVICEADDED:
        if (!g_pad) {
            g_pad = SDL_GameControllerOpen(e.cdevice.which);
            LOGI("input: controller connected: %s", g_pad ? SDL_GameControllerName(g_pad) : SDL_GetError());
        }
        return true;
    case SDL_CONTROLLERDEVICEREMOVED:
        if (g_pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad)) == e.cdevice.which) {
            SDL_GameControllerClose(g_pad);
            g_pad = nullptr;
            g_pad_active = false;
            for (int c = 0; c < 128; c++)
                if (g_pad_down[c]) pad_button(kPadCode + c, false);
            std::memset(g_axis, 0, sizeof g_axis);
            LOGI("input: controller disconnected");
        }
        return true;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
        int code = kPadCode + e.cbutton.button;
        if (down) g_pad_active = true;
        if (int cap = g_capture; cap >= kPadBindBase && down) {  // Options: waiting for a button to bind
            if (e.cbutton.button != SDL_CONTROLLER_BUTTON_START && pad_name(code)) {
                set_binding(cap, code);
                LOGI("input: controller %s bound to %s", kPad[cap - kPadBindBase].b.label, code_name(code).c_str());
            }
            g_capture = -1;
            return true;
        }
        pad_button(code, down);
        return true;
    }
    case SDL_CONTROLLERAXISMOTION: {
        int a = e.caxis.axis;
        if (a < 0 || a >= 8) return true;
        float v = std::clamp(float(e.caxis.value) / 32767.0f, -1.0f, 1.0f);
        g_axis[a] = v;
        if (std::fabs(v) > 0.5f) g_pad_active = true;
        if (a == SDL_CONTROLLER_AXIS_TRIGGERLEFT || a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
            int code = a == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? kLT : kRT;
            bool was = g_pad_down[code - kPadCode], now = v > (was ? 0.25f : 0.45f);
            if (now != was) {
                if (int cap = g_capture; cap >= kPadBindBase && now) {
                    set_binding(cap, code);
                    g_capture = -1;
                    g_pad_down[code - kPadCode] = true;
                } else {
                    pad_button(code, now);
                }
            }
        }
        return true;
    }
    default: return false;
    }
}

bool event(const SDL_Event& e, SDL_Window* win) {
    if (pad_event(e)) return true;
    // keyboard or mouse in use again: prompts go back to keys
    if ((e.type == SDL_KEYDOWN && e.key.keysym.unused != kSynth) || e.type == SDL_MOUSEBUTTONDOWN ||
        (e.type == SDL_MOUSEMOTION && std::abs(e.motion.xrel) + std::abs(e.motion.yrel) > 6))
        g_pad_active = false;
    if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
        SDL_Scancode sc = e.key.keysym.scancode;
        if (sc > 0 && sc < SDL_NUM_SCANCODES) g_keys[sc] = e.type == SDL_KEYDOWN;
    }
    if ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) && e.button.button < 8)
        g_mouse_btn[e.button.button] = e.type == SDL_MOUSEBUTTONDOWN;
    // Options menu waiting for a key to bind: the next key / mouse button press is taken
    // (releases still pass, so the click that opened the prompt finishes normally)
    if (int cap = g_capture; cap >= kPadBindBase) {
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
            g_capture = -1;
            return true;
        }
    } else if (cap >= 0) {
        int code = 0;
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                g_capture = -1;
                return true;
            }
            code = int(e.key.keysym.scancode);
        } else if (e.type == SDL_MOUSEBUTTONDOWN && mouse_name(e.button.button)) {
            code = kMouseCode + e.button.button;
        }
        if (code) {
            set_binding(cap, code);
            LOGI("input: %s bound to %s", kBindings[cap].label, code_name(code).c_str());
            g_capture = -1;
            return true;
        }
        if (e.type == SDL_KEYDOWN) return true;
    }
    // skip a playing cinematic
    if (cinematic_playing() && fn_cine_transition &&
        ((e.type == SDL_KEYDOWN && !e.key.repeat &&
          (e.key.keysym.sym == SDLK_SPACE || e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER)) ||
         (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT))) {
        if (!g_fast_forward && !g_cine_skip_frames) {
            g_cine_skip = true;
            g_cine_skip_frames = 1;
        }
        return true;
    }
    // the RIG key closes the RIG menu (it is a menu: gameplay keys are not handled while it is open)
    if (e.type == SDL_KEYDOWN && !e.key.repeat && int(e.key.keysym.scancode) == g_bind[B_RIG] && g_hud &&
        fn_in_rig && (cpu::call(fn_in_rig, {g_hud}) & 0xff)) {
        hud_button(g_hud, "BTN_RIG");  // same as tapping the RIG icon again
        return true;
    }
    // Esc always maps to the Android back button (pause in game, back in menus)
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && !e.key.repeat) {
        back_key();
        return true;
    }
    if (!g_mouse_captured) return false;
    switch (e.type) {
    case SDL_MOUSEMOTION:
        g_look_dx += float(e.motion.xrel);
        g_look_dy += float(e.motion.yrel);
        return true;
    case SDL_MOUSEBUTTONDOWN:
        press_code(kMouseCode + e.button.button);
        return true;
    case SDL_MOUSEBUTTONUP:
        release_code(kMouseCode + e.button.button);
        return true;
    case SDL_MOUSEWHEEL: {
        u32 player = g_player;
        if (!player) return true;
        int cur = int(mem::r32(player + 0x2e8));
        int dir = e.wheel.y > 0 ? -1 : 1;
        // cycle the four guns; slot 4 holds the stasis module, which is not a selectable weapon
        // (the RIG never offers it; selecting it leaves the player empty-handed and frozen)
        for (int i = 1; i <= 4; i++) {
            int w = ((cur + dir * i) % 4 + 4) % 4;
            if (mem::r32(player + 0x2d4 + 4 * u32(w))) {
                push({EV_WEAPON, w});
                break;
            }
        }
        return true;
    }
    case SDL_KEYDOWN:
        if (e.key.repeat) return true;
        return press_code(int(e.key.keysym.scancode));
    case SDL_KEYUP:
        release_code(int(e.key.keysym.scancode));
        return false;
    default: return false;
    }
    (void)win;
}

// Light bar (DualSense, DualShock 4, any controller with an RGB light): the colour of the RIG's health
// bar, by the game's own thresholds: blue above half health, yellow down to a quarter, red below and when
// dead. Outside gameplay it is the healthy blue.
void update_light_bar() {
    static SDL_GameController* lit = nullptr;
    static int shown = -1;
    if (g_pad != lit) lit = g_pad, shown = -1;
    if (!g_pad || !SDL_GameControllerHasLED(g_pad)) return;
    float hf = g_frames_without_player > 2 ? 1.0f : g_health_frac.load();
    int level = hf <= 0.25f ? 2 : hf <= 0.5f ? 1 : 0;
    if (level == shown) return;
    static const Uint8 rgb[3][3] = {{0, 150, 255}, {255, 170, 0}, {255, 0, 0}};
    if (SDL_GameControllerSetLED(g_pad, rgb[level][0], rgb[level][1], rgb[level][2]) == 0) {
        if (shown < 0) LOGI("input: controller light bar follows the player's health");
        shown = level;
    }
}

bool frame(SDL_Window* win) {
    update_light_bar();
    u32 ctick = g_cinematic_tick;
    if (ctick != g_cine_tick_seen && g_cinematic) {
        g_frames_without_cine = 0;
        g_cine_tick_seen = ctick;
    } else if (g_frames_without_cine < 1000) {
        g_frames_without_cine++;
    }
    if (g_cine_skip_frames) {
        if (!cinematic_playing()) {
            g_cine_skip_frames = 0, g_cine_skip = false;
        } else if (++g_cine_skip_frames > 30 && !g_fast_forward) {  // it did not end: fast-forward through it
            LOGI("input: cinematic still playing after the skip: fast-forwarding");
            g_cine_skip = false;
            g_fast_forward = true;
            timing::set_time_scale(kSkipSpeed);
        }
    }
    if (g_fast_forward && !cinematic_playing()) {
        g_fast_forward = false;
        timing::set_time_scale(1.0);
        LOGI("input: cinematic finished");
    }
    u32 tick = g_player_tick;
    if (tick != g_last_tick_seen) {
        g_frames_without_player = 0;
        g_last_tick_seen = tick;
    } else if (g_frames_without_player < 1000) {
        g_frames_without_player++;
    }
    const bool* ks = g_keys;
    // holding Left Alt frees the cursor so it can be used as a touch pointer
    bool want = gameplay_active() && !ks[SDL_SCANCODE_LALT] && (g_test || (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS));
    if (want != g_mouse_captured) {
        if (!g_test) SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
        LOGI("input: gameplay controls %s", want ? "active (mouse captured)" : "released");
        g_mouse_captured = want;
        g_look_dx = g_look_dy = 0;
        std::memset(g_smooth_x, 0, sizeof g_smooth_x);
        std::memset(g_smooth_y, 0, sizeof g_smooth_y);
        if (!want && g_moving) {
            push({EV_MOVE});
            g_moving = false;
        }
        if (!want) g_rmb = g_lmb = false;
    }
    if (!g_mouse_captured) {
        // menus: the left stick moves the highlight like the D-pad (with repeat)
        static int held = 0;
        static u32 next = 0;
        int dir = !g_pad_active ? 0 : g_axis[1] < -0.6f ? 1 : g_axis[1] > 0.6f ? 2 : g_axis[0] < -0.6f ? 3 : g_axis[0] > 0.6f ? 4 : 0;
        u32 now = SDL_GetTicks();
        if (dir && (dir != held || now >= next)) {
            static const SDL_Scancode sc[5] = {SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT};
            static const SDL_Keycode ky[5] = {0, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT};
            synth_key(sc[dir], ky[dir]);
            next = now + (dir != held ? 400u : 160u);
        }
        held = dir;
        return false;
    }

    // movement: keys, or the left stick (how far it is pushed sets the pace up to a walk)
    float mx = float(bind_down(B_RIGHT)) - float(bind_down(B_LEFT));
    float my = float(bind_down(B_BACK)) - float(bind_down(B_FORWARD));
    float pace = 1.0f;
    bool pad_run = false;
    if (mx == 0 && my == 0) {
        float sm = std::hypot(g_axis[0], g_axis[1]);
        if (sm > 0.24f) {
            mx = g_axis[0], my = g_axis[1];
            pace = std::clamp((sm - 0.24f) / 0.5f, 0.3f, 1.0f);
            pad_run = pad_code_down(g_padbind[P_RUN]) || g_run_toggle;
        } else {
            g_run_toggle = false;  // stick let go: the sprint ends
        }
    }
    if (mx != 0 || my != 0) {
        float len = std::sqrt(mx * mx + my * my) / pace;
        bool run_key = pad_run || bind_down(B_RUN) || (!code_bound(SDL_SCANCODE_RSHIFT) && ks[SDL_SCANCODE_RSHIFT]);
        if (g_rmb) g_run_toggle = false;
        int run = run_key && my < 0 && !g_rmb ? 1 : 0;  // holding aim stops sprinting
        push({EV_MOVE, 0, mx / len, my / len, run});
        g_moving = true;
    } else if (g_moving) {
        push({EV_MOVE});
        g_moving = false;
    }

    // look
    {
        float lx = g_look_dx, ly = g_look_dy;
        g_look_dx = g_look_dy = 0;
        if (mouse_smoothing()) {
            g_smooth_x[g_smooth_i] = lx;
            g_smooth_y[g_smooth_i] = ly;
            g_smooth_i = (g_smooth_i + 1) % kSmoothFrames;
            lx = ly = 0;
            for (int i = 0; i < kSmoothFrames; i++) {
                lx += g_smooth_x[i] / kSmoothFrames;
                ly += g_smooth_y[i] / kSmoothFrames;
            }
        }
        // right stick: a turning speed (full deflection = 200 degrees a second at 100%; slower while aiming),
        // in the same units as mouse counts (0.05 degrees each)
        float px = 0, py = 0;
        {
            static u32 last = 0;
            u32 now = SDL_GetTicks();
            float dt = last && now - last < 100 ? float(now - last) / 1000.0f : 0.016f;
            last = now;
            auto curve = [](float v) {
                float a = std::fabs(v);
                if (a < 0.18f) return 0.0f;
                a = (a - 0.18f) / 0.82f;
                return (v < 0 ? -1.0f : 1.0f) * a * (0.35f + 0.65f * a);
            };
            float k = float(std::clamp(settings::get_int("pad_sensitivity", 100), 10, 300)) / 100.0f * (g_aiming ? 0.55f : 1.0f) * dt;
            px = curve(g_axis[2]) * 4000.0f * k;
            py = curve(g_axis[3]) * 2400.0f * k;
        }
        if (lx != 0 || ly != 0 || px != 0 || py != 0) {
            float s = mouse_scale();
            push({EV_LOOK, 0, lx * s + px, ly * s + py});
        }
    }

    // aim released: the game ignores "lower" while a shot is still playing, so keep asking until
    // the weapon is down (not while holding a kinesis object)
    static int lower_retry = 0;
    if (g_aiming && !g_rmb && g_pending_fire == 0 && g_state != 0x12) {
        if (++lower_retry % 6 == 0) action(9);
    } else {
        lower_retry = 0;
    }

    // left click while the weapon was still being raised: fire once it is up
    if (g_pending_fire > 0) {
        if (g_aiming) {
            action(6);
            g_pending_fire = 0;
        } else {
            g_pending_fire--;
        }
    }
    return true;
}

}  // namespace input
