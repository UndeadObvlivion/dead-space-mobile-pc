// Dead Space (2011 mobile) PC port: loads the game's ARM library and drives it like Android would.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <zlib.h>
#include <SDL_opengl.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

#include "apk.h"
#include "config.h"
#include "crash.h"
#include "settings.h"
#include "cpu.h"
#include "elf_loader.h"
#include "game_patches.h"
#include "console.h"
#include "devmode.h"
#include "input.h"
#include "lights.h"
#include "menu_nav.h"
#include "render/native.h"
#include "hle/gles.h"
#include "hle/hle.h"
#include "jni/jni.h"
#include "log.h"
#include "memory.h"

Config g_config;
std::atomic<bool> g_quit{false};

namespace jni {
void audio_open(int rate, int channels, int buffer_frames);
void audio_dump(const char* path);
ObjPtr activity();
}

namespace fs = std::filesystem;

namespace {

const char* kActivity = "com/ea/blast/MainActivity";
const char* kRenderer = "com/ea/blast/AndroidRenderer";
const char* kTouch = "com/ea/blast/TouchSurfaceAndroid";

jni::JValue native(const char* cls, const char* name, const char* sig, const jni::ObjPtr& thiz,
                   std::vector<jni::JValue> args = {}) {
    return jni::call_native(cls, name, sig, thiz, args);
}

jni::ObjPtr cls(const char* name) { return jni::class_object(name); }

struct ScriptTap {
    unsigned long long frame;
    float x, y;
    int hold;
    float dx, dy;  // swipe distance over the hold
};
std::vector<ScriptTap> g_taps;

// debug input script: frame:NAME:value  (NAME = SDL key name like W / Left Shift, or
// LMB/RMB/MMB with 1/0, or MOUSE with dx;dy)
struct ScriptInput {
    unsigned long long frame;
    std::string name, value;
};
std::vector<ScriptInput> g_script;

void run_script(unsigned long long frame) {
    for (auto& s : g_script) {
        if (s.frame != frame) continue;
        SDL_Event e{};
        if (s.name == "LMB" || s.name == "RMB" || s.name == "MMB") {
            e.type = s.value == "1" ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            e.button.button = s.name == "LMB" ? SDL_BUTTON_LEFT : s.name == "RMB" ? SDL_BUTTON_RIGHT : SDL_BUTTON_MIDDLE;
        } else if (s.name == "MOUSE") {
            e.type = SDL_MOUSEMOTION;
            std::sscanf(s.value.c_str(), "%d;%d", &e.motion.xrel, &e.motion.yrel);
        } else if (s.name == "WHEEL") {
            e.type = SDL_MOUSEWHEEL;
            e.wheel.y = std::atoi(s.value.c_str());
        } else {
            e.type = s.value == "1" ? SDL_KEYDOWN : SDL_KEYUP;
            e.key.keysym.scancode = SDL_GetScancodeFromName(s.name.c_str());
            e.key.keysym.sym = SDL_GetKeyFromScancode(e.key.keysym.scancode);
        }
        SDL_PushEvent(&e);
    }
}

struct TouchIds {
    int down = 0, up = 1, move = 2, cancel = 3, module = 0;
} g_touch;

void send_touch(int event, int id, float x, float y) {
    native(kTouch, "NativeOnPointerEvent", "(IIIFF)V", cls(kTouch),
           {jni::JValue::I(event), jni::JValue::I(g_touch.module), jni::JValue::I(id), jni::JValue::F(x),
            jni::JValue::F(y)});
}

// Saves the game's current frame as a BMP.
void save_screenshot(const std::string& path, int w, int h) {
    std::vector<u8> px(size_t(w) * h * 4), flipped(px.size());
    hle::present_read(w, h, px.data());
    for (int y = 0; y < h; y++) std::memcpy(&flipped[size_t(y) * w * 4], &px[size_t(h - 1 - y) * w * 4], size_t(w) * 4);
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(flipped.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
    if (s) {
        SDL_SaveBMP(s, path.c_str());
        SDL_FreeSurface(s);
        LOGI("screenshot %s", path.c_str());
    }
}

// Saves the game's current frame as a PNG scaled down by `div` (box filter).
void save_png(const std::string& path, int w, int h, int div) {
    std::vector<u8> px(size_t(w) * h * 4);
    hle::present_read(w, h, px.data());
    int ow = w / div, oh = h / div;
    std::vector<u8> raw(size_t(oh) * (size_t(ow) * 3 + 1));
    for (int y = 0; y < oh; y++) {
        u8* row = &raw[size_t(y) * (size_t(ow) * 3 + 1)];
        row[0] = 0;  // filter: none
        for (int x = 0; x < ow; x++)
            for (int c = 0; c < 3; c++) {
                unsigned sum = 0;
                for (int dy = 0; dy < div; dy++)
                    for (int dx = 0; dx < div; dx++)
                        sum += px[(size_t(h - 1 - (y * div + dy)) * w + size_t(x * div + dx)) * 4 + c];
                row[1 + x * 3 + c] = u8(sum / unsigned(div * div));
            }
    }
    uLongf zlen = compressBound(uLong(raw.size()));
    std::vector<u8> z(zlen);
    compress2(z.data(), &zlen, raw.data(), uLong(raw.size()), 6);
    z.resize(zlen);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    auto be32 = [](u8* p, u32 v) { p[0] = u8(v >> 24), p[1] = u8(v >> 16), p[2] = u8(v >> 8), p[3] = u8(v); };
    auto chunk = [&](const char* type, const u8* data, u32 len) {
        u8 hdr[8];
        be32(hdr, len);
        std::memcpy(hdr + 4, type, 4);
        std::fwrite(hdr, 1, 8, f);
        if (len) std::fwrite(data, 1, len, f);
        uLong crc = crc32(0, hdr + 4, 4);
        if (len) crc = crc32(crc, data, len);
        u8 c[4];
        be32(c, u32(crc));
        std::fwrite(c, 1, 4, f);
    };
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    std::fwrite(sig, 1, 8, f);
    u8 ihdr[13];
    be32(ihdr, u32(ow));
    be32(ihdr + 4, u32(oh));
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = ihdr[11] = ihdr[12] = 0;  // 8-bit RGB
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), u32(z.size()));
    chunk("IEND", nullptr, 0);
    std::fclose(f);
}

std::string first_existing(std::initializer_list<std::string> paths) {
    for (auto& p : paths) {
        std::error_code ec;
        if (fs::exists(p, ec)) return p;
    }
    return "";
}

}  // namespace

int main(int argc, char** argv) {
    std::string assets, lib, data, logfile, shot_dir, apk_path, dev_dir;
    std::vector<u64> shot_frames;
    int fps_arg = -1;  // --fps N overrides the Frame Rate Limit setting
    u64 exit_frame = 0, gl_trace_frame = 0;
    bool res_set = false;
    int msaa_arg = -1;
    bool crash_test = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--assets") assets = next();
        else if (a == "--lib") lib = next();
        else if (a == "--apk") apk_path = next();
        else if (a == "--dump-audio") jni::audio_dump(next().c_str());
        else if (a == "--data") data = next();
        else if (a == "--log") logfile = next();
        else if (a == "--width") g_config.screen_w = std::stoi(next()), res_set = true;
        else if (a == "--height") g_config.screen_h = std::stoi(next()), res_set = true;
        else if (a == "--fullscreen") g_config.fullscreen = true;
        else if (a == "--verbose" || a == "-v") logging::g_verbose = true;
        else if (a == "--fps") fps_arg = std::stoi(next());
        else if (a == "--msaa") msaa_arg = std::stoi(next());
        else if (a == "--trace-input") input::set_trace(true);
        else if (a == "--level") patches::set_level_override(next());
        else if (a == "--teleport") {  // debug: ClassName[:x;y;z]
            std::string v = next();
            size_t c = v.find(':');
            float o[3] = {0, 0, 1.5f};
            if (c != std::string::npos) std::sscanf(v.c_str() + c + 1, "%f;%f;%f", &o[0], &o[1], &o[2]);
            input::set_teleport(v.substr(0, c), o[0], o[1], o[2]);
        }
        else if (a == "--crash-test") crash_test = true;
        else if (a == "--dev") dev_dir = next();  // debug: take commands from DIR/cmd.txt
        else if (a == "--give-weapons") input::set_give_weapons(true);
        else if (a == "--input") {
            input::set_test_mode(true);
            std::string list = next();
            for (size_t p = 0; p < list.size();) {
                size_t q = list.find(',', p);
                if (q == std::string::npos) q = list.size();
                std::string item = list.substr(p, q - p);
                size_t c1 = item.find(':'), c2 = item.find(':', c1 + 1);
                if (c1 != std::string::npos && c2 != std::string::npos)
                    g_script.push_back({std::stoull(item.substr(0, c1)), item.substr(c1 + 1, c2 - c1 - 1), item.substr(c2 + 1)});
                p = q + 1;
            }
        }
        else if (a == "--dump-textures") hle::g_dump_dir = next();
        else if (a == "--gltrace") gl_trace_frame = std::stoull(next());  // debug: log that frame's GL calls
        else if (a == "--atc-textures") hle::g_real_atc = true;
        else if (a == "--exit-frame") exit_frame = std::stoull(next());
        else if (a == "--shots") {  // comma separated frame numbers to capture
            std::string list = next();
            for (size_t p = 0; p < list.size();) {
                size_t q = list.find(',', p);
                if (q == std::string::npos) q = list.size();
                shot_frames.push_back(std::stoull(list.substr(p, q - p)));
                p = q + 1;
            }
        } else if (a == "--shot-dir") shot_dir = next();
        else if (a == "--taps") {  // debug input script: frame:x:y[:holdframes],...
            std::string list = next();
            for (size_t p = 0; p < list.size();) {
                size_t q = list.find(',', p);
                if (q == std::string::npos) q = list.size();
                std::string item = list.substr(p, q - p);
                ScriptTap t{};
                int hold = 4;
                if (std::sscanf(item.c_str(), "%llu:%f:%f:%d:%f:%f", &t.frame, &t.x, &t.y, &hold, &t.dx, &t.dy) >= 3) {
                    t.hold = hold;
                    g_taps.push_back(t);
                }
                p = q + 1;
            }
        }
    }

    char* base_c = SDL_GetBasePath();
    std::string base = base_c ? base_c : "./";
    SDL_free(base_c);
    if (logfile.empty()) logfile = base + "deadspace.log";
    logging::open_file(logfile.c_str());
    crash::install(base);
    if (crash_test) *static_cast<volatile int*>(nullptr) = 1;  // debug: verify crash reporting

    // Game files: imported from the user's APK into gamedata/ (once), or given explicitly.
    const std::string gamedata = base + "gamedata";
    if (apk_path.empty() && assets.empty() && !fs::exists(gamedata + "/libDeadSpace.so")) {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(base, ec))
            if (e.path().extension() == ".apk") {
                apk_path = e.path().string();
                break;
            }
    }
    if (!apk_path.empty()) {
        LOGI("importing game data from %s ...", apk_path.c_str());
        if (!apk::import(apk_path, gamedata)) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Dead Space",
                                     "Could not import the game data from the APK. See deadspace.log.", nullptr);
            return 1;
        }
    }
    if (assets.empty())
        assets = first_existing({gamedata + "/assets", base + "../apk/assets", "apk/assets"});
    if (lib.empty())
        lib = first_existing({gamedata + "/libDeadSpace.so", assets + "/guest_libs/libDeadSpace.so"});
    if (data.empty()) data = base + "userdata";
    if (assets.empty() || lib.empty()) {
        LOGE("could not find the game files. Run once with --apk <Dead Space APK> (or put the APK next to the exe)");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Dead Space",
                                 "Game data not found.\n\nPut your Dead Space APK next to deadspace.exe and start it "
                                 "again, or run:  deadspace --apk <path to APK>",
                                 nullptr);
        return 1;
    }
    hle::g_assets_dir = assets;
    hle::g_data_dir = data;
    fs::create_directories(data);
    settings::load(data + "/port_settings.ini");
    if (msaa_arg >= 0) settings::set_int("msaa", msaa_arg);
    LOGI("assets: %s", assets.c_str());
    LOGI("library: %s", lib.c_str());
    LOGI("user data: %s", data.c_str());

#ifdef _WIN32
    timeBeginPeriod(1);
#endif

    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");  // real pixels, no DPI virtualisation
    // PlayStation controllers: full feature mode, so the light bar can be set over Bluetooth as well as USB
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS4_RUMBLE", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS5_RUMBLE", "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        LOGE("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // The game renders at the desktop resolution (landscape) unless --width/--height is given.
    SDL_DisplayMode desk{};
    bool have_desk = SDL_GetDesktopDisplayMode(0, &desk) == 0;
    if (!res_set && have_desk) {
        g_config.screen_w = std::max(desk.w, desk.h);
        g_config.screen_h = std::min(desk.w, desk.h);
    }
    // a windowed start gets a window that fits comfortably on the desktop
    int win_w0 = g_config.screen_w, win_h0 = g_config.screen_h;
    if (have_desk && !g_config.fullscreen) {
        float fit = std::min(desk.w * 0.8f / win_w0, desk.h * 0.8f / win_h0);
        if (fit < 1.0f) {
            win_w0 = int(win_w0 * fit);
            win_h0 = int(win_h0 * fit);
        }
    }
    Uint32 wflags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                    (g_config.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_Window* win = SDL_CreateWindow("Dead Space", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w0, win_h0,
                                       wflags);
    if (!win) {
        LOGE("SDL_CreateWindow: %s", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        LOGE("SDL_GL_CreateContext: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(settings::get_int("vsync", 1) ? 1 : 0);
    hle::gles_init_host();
    // the game always sees a fixed "device screen"; the frontend scales it to the window
    hle::present_init(g_config.screen_w, g_config.screen_h, settings::get_int("msaa", 4));
    if (render::init()) {
        render::set_enabled(settings::get_string("renderer", "legacy") == "native");
        render::set_anisotropy(settings::get_int("anisotropy", 16));
        render::set_reversed_z(settings::get_int("reversed_depth", 1) != 0);
        render::set_bloom(settings::get_int("bloom", 1));
        render::set_sharpen(settings::get_int("sharpen", 1));
        render::set_ao(settings::get_int("ambient_occlusion", 1));
        render::set_lights(settings::get_int("dynamic_lights", 2));
        render::set_specular(settings::get_int("specular", 2));
        render::set_shadows(settings::get_int("shadows", 2));
        render::set_light_shadows(settings::get_int("light_shadows", 1) != 0);
        render::set_vignette(settings::get_int("vignette", 1) != 0);
        render::set_env_light(settings::get_int("light_contrast", 2));
    }
    else LOGW("native renderer unavailable; using the legacy path");

    if (!mem::init() || !cpu::init()) return 1;
    hle::register_all();
    jni::init();
    if (!elf::load(lib, mem::ELF_BASE, elf::g_game, hle::resolve)) return 1;

    patches::install();
    input::install();
    lights::install();
    console::install();
    menu_nav::install();
    LOGI("running %zu static constructors", elf::g_game.init_array.size());
    elf::run_init_array(elf::g_game);

    if (u32 onload = elf::find("JNI_OnLoad")) {
        u32 v = cpu::call(onload, {jni::vm(), 0});
        LOGI("JNI_OnLoad -> 0x%x", v);
    }

    auto act = jni::activity();
    // Activity.onCreate (MainActivity) then DeadSpaceActivity's audio startup
    native(kActivity, "NativeOnCreate", "()V", act);
    {
        const int rate = 44100, channels = 2, frames = 2048;
        jni::audio_open(rate, channels, frames);
        auto track = jni::make("android/media/AudioTrack");
        native("com/ea/EAAudioCore/AndroidEAAudioCore", "Init", "(Landroid/media/AudioTrack;III)V",
               cls("com/ea/EAAudioCore/AndroidEAAudioCore"),
               {jni::JValue::L(jni::new_global(track)), jni::JValue::I(frames), jni::JValue::I(channels),
                jni::JValue::I(rate)});
    }
    native(kActivity, "NativeOnResume", "()V", act);
    native(kActivity, "NativeOnWindowFocusChanged", "(Z)V", act, {jni::JValue::Z(true)});

    g_touch.down = native(kTouch, "NativeGetIdRawPointerDown", "()I", cls(kTouch)).i();
    g_touch.up = native(kTouch, "NativeGetIdRawPointerUp", "()I", cls(kTouch)).i();
    g_touch.move = native(kTouch, "NativeGetIdRawPointerMove", "()I", cls(kTouch)).i();
    g_touch.cancel = native(kTouch, "NativeGetIdRawPointerCancel", "()I", cls(kTouch)).i();
    g_touch.module = native("com/ea/blast/ModuleCatalog", "NativeGetModuleTypeIdTouchScreen", "()I",
                            cls("com/ea/blast/ModuleCatalog")).i();
    LOGI("touch ids: down %d up %d move %d cancel %d module %d", g_touch.down, g_touch.up, g_touch.move,
         g_touch.cancel, g_touch.module);

    auto renderer = jni::make(kRenderer);
    native(kRenderer, "NativeOnSurfaceCreated", "()V", renderer);
    native(kRenderer, "NativeOnSurfaceChanged", "(II)V", renderer,
           {jni::JValue::I(g_config.screen_w), jni::JValue::I(g_config.screen_h)});

    LOGI("entering main loop");
    bool mouse_down = false;
    u64 frames = 0;
    menu_nav::set_tap([&](float x, float y) { g_taps.push_back({frames + 1, x, y, 4, 0, 0}); });
    if (!dev_dir.empty()) {
        dev::Host host;
        host.tap = [&](float x, float y, int hold) { g_taps.push_back({frames + 1, x, y, hold, 0, 0}); };
        host.screenshot = [](const std::string& path) { save_png(path, g_config.screen_w, g_config.screen_h, path.find("_full.png") != std::string::npos ? 1 : 4); };
        host.quit = [] { g_quit = true; };
        dev::init(dev_dir, host);
    }
    int win_w = 0, win_h = 0;  // drawable size
    // window (logical) coordinates -> game screen coordinates
    auto to_game = [&](int x, int y, float& gx, float& gy) {
        int lw, lh;
        SDL_GetWindowSize(win, &lw, &lh);
        float sx = lw ? float(win_w) / lw : 1, sy = lh ? float(win_h) / lh : 1;
        return hle::present_map(win_w, win_h, x * sx, y * sy, gx, gy);
    };
    while (!g_quit) {
        SDL_GL_GetDrawableSize(win, &win_w, &win_h);
        run_script(frames + 1);
        dev::frame();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            float gx, gy;
            if (console::event(e)) continue;
            // loading screen "press any key to continue": any key or mouse button taps where the game waits for
            // one; the left / right arrow keys swipe through its hints
            if (patches::loading_wait() && ((e.type == SDL_KEYDOWN && !e.key.repeat && (e.key.keysym.sym < SDLK_F1 || e.key.keysym.sym > SDLK_F12)) ||
                                            e.type == SDL_MOUSEBUTTONDOWN)) {
                float w = float(g_config.screen_w), h = float(g_config.screen_h);
                if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_LEFT || e.key.keysym.sym == SDLK_RIGHT)) {
                    float dir = e.key.keysym.sym == SDLK_RIGHT ? -1.0f : 1.0f;  // right = next hint = swipe to the left
                    g_taps.push_back({frames + 1, w * 0.5f - dir * w * 0.15f, h * 0.45f, 10, dir * w * 0.3f, 0});
                } else {
                    g_taps.push_back({frames + 1, w * 0.5f, h * 0.93f, 4, 0, 0});
                }
                continue;
            }
            if (e.type == SDL_MOUSEWHEEL && patches::options_open()) {  // scrolls an options page
                patches::options_wheel(e.wheel.y);
                continue;
            }  // scrolls an options page
            if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) menu_nav::hide();
            if (input::event(e, win)) continue;
            if (menu_nav::event(e, input::controls_active())) continue;
            switch (e.type) {
            case SDL_QUIT: g_quit = true; break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT && to_game(e.button.x, e.button.y, gx, gy)) {
                    if (patches::options_click(gx, gy)) break;  // options header page arrows
                    mouse_down = true;
                    send_touch(g_touch.down, 0, gx, gy);
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT && mouse_down) {
                    mouse_down = false;
                    to_game(e.button.x, e.button.y, gx, gy);
                    send_touch(g_touch.up, 0, gx, gy);
                }
                break;
            case SDL_MOUSEMOTION:
                if (mouse_down) {
                    to_game(e.motion.x, e.motion.y, gx, gy);
                    send_touch(g_touch.move, 0, gx, gy);
                }
                break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_F12) shot_frames.push_back(frames + 1);
                if (e.key.keysym.sym == SDLK_F8 && render::available()) {  // renderer toggle
                    render::set_enabled(!render::enabled());
                    settings::set_string("renderer", render::enabled() ? "native" : "legacy");
                }
                if (e.key.keysym.sym == SDLK_F11 ||
                    (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                    bool fs_now = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(win, fs_now ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
                break;
            default: break;
            }
        }
        for (auto& t : g_taps) {
            if (t.frame == frames + 1) {
                if (patches::options_click(t.x, t.y)) {  // options header page arrows
                    t.hold = 0;
                    continue;
                }
                send_touch(g_touch.down, 0, t.x, t.y);
            }
            if (t.hold == 0) continue;
            u64 f = frames + 1;
            if (f > t.frame && f < t.frame + u64(t.hold) && (t.dx != 0 || t.dy != 0)) {
                float k = float(f - t.frame) / float(t.hold);
                send_touch(g_touch.move, 0, t.x + t.dx * k, t.y + t.dy * k);
            }
            if (t.frame + u64(t.hold) == f) send_touch(g_touch.up, 0, t.x + t.dx, t.y + t.dy);
        }
        input::frame(win);
        hle::present_begin();
        if (gl_trace_frame && frames + 1 == gl_trace_frame) {
            cpu::set_gl_trace(true);
            render::trace_next_frame();
        }
        render::begin_frame();
        native(kRenderer, "NativeOnDrawFrame", "()V", renderer);
        render::end_frame();
        if (gl_trace_frame && frames + 1 == gl_trace_frame) cpu::set_gl_trace(false);
        // skipping a cinematic: run extra game frames per displayed frame, without vsync
        static bool ff_vsync_off = false;
        if (input::fast_forwarding()) {
            if (!ff_vsync_off) SDL_GL_SetSwapInterval(0), ff_vsync_off = true;
            for (int i = 0; i < 7 && input::fast_forwarding(); i++) {
                input::frame(win);
                render::begin_frame();
                native(kRenderer, "NativeOnDrawFrame", "()V", renderer);
                render::end_frame();
            }
        } else if (ff_vsync_off) {
            SDL_GL_SetSwapInterval(settings::get_int("vsync", 1) ? 1 : 0);
            ff_vsync_off = false;
        }
        if (int grab = input::grab()) {
            // grabbed: the button prompt, in place of the game's swipe / tap instructions
            std::string key = input::action_label("melee", input::using_pad());
            for (char& ch : key) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            bool mash = grab == 5 || grab == 100;
            bool waiting = grab == 1 || grab == 5 || grab == 7 || grab == 100;
            if (waiting) {
                std::string txt = std::string(mash ? "TAP  [ " : "PRESS  [ ") + key + " ]";
                float px = g_config.screen_h / 180.0f;
                float w = hle::overlay_text(-10000, -10000, px, txt.c_str(), 0), x = (g_config.screen_w - w) * 0.5f, y = g_config.screen_h * 0.72f;
                bool on = !mash || (SDL_GetTicks() / 120) % 2 == 0;  // a tapping prompt blinks
                hle::overlay_rect(x - 3 * px, y - 3 * px, w + 6 * px, 13 * px, 0x000000b0u);
                hle::overlay_text(x, y, px, txt.c_str(), on ? 0x60e8ffffu : 0x2c7890ffu);
            }
        }
        {  // frames per second, averaged over half a second (Options: Show FPS)
            static u64 fps_t0 = SDL_GetPerformanceCounter();
            static int fps_n = 0;
            static float fps = 0;
            fps_n++;
            u64 now = SDL_GetPerformanceCounter(), freq = SDL_GetPerformanceFrequency();
            if (now - fps_t0 >= freq / 2) {
                fps = float(double(fps_n) * double(freq) / double(now - fps_t0));
                fps_n = 0;
                fps_t0 = now;
            }
            if (settings::get_int("show_fps", 0) && fps > 0) {
                char txt[32];
                std::snprintf(txt, sizeof txt, "%d FPS", int(fps + 0.5f));
                float px = std::max(2.0f, float(g_config.screen_h / 360));
                float tw = float(std::strlen(txt)) * 6 * px, x = (g_config.screen_w - tw) * 0.5f, y = 3 * px;
                hle::overlay_rect(x - 2 * px, y - 2 * px, tw + 3 * px, 11 * px, 0x00000090u);
                hle::overlay_text(x, y, px, txt, 0xffe060ffu);
            }
        }
        console::draw(g_config.screen_w, g_config.screen_h);
        menu_nav::frame(input::controls_active());
        hle::overlay_draw();  // debug overlays queued during the game frame
        ++frames;
        for (u64 f : shot_frames)
            if (f == frames) {
                std::string dir = shot_dir.empty() ? base + "shots" : shot_dir;
                fs::create_directories(dir);
                save_screenshot(dir + "/frame_" + std::to_string(frames) + ".bmp", g_config.screen_w, g_config.screen_h);
            }
        patches::frame();
        hle::present_end(win_w, win_h);
        SDL_GL_SwapWindow(win);
        if (frames % 600 == 0) LOGI("frame %llu", (unsigned long long)frames);
        if (exit_frame && frames >= exit_frame) g_quit = true;

        const int fps_limit = fps_arg >= 0 ? fps_arg : settings::get_int("fps_limit", 60);  // 0 = unlimited
        if (fps_limit > 0 && !input::fast_forwarding()) {  // the game was built for phone frame rates
            static u64 next_tick = SDL_GetPerformanceCounter();
            const u64 freq = SDL_GetPerformanceFrequency();
            next_tick += freq / u64(fps_limit);
            u64 now = SDL_GetPerformanceCounter();
            if (now < next_tick) {
                u64 ms = (next_tick - now) * 1000 / freq;
                if (ms > 1) SDL_Delay(u32(ms - 1));
                while (SDL_GetPerformanceCounter() < next_tick) {}
            } else if (now - next_tick > freq / u64(fps_limit)) {
                next_tick = now;  // more than a frame behind: resync instead of running fast to catch up
            }
        }
    }

    LOGI("shutting down");
    native(kActivity, "NativeOnPause", "()V", act);
    native(kActivity, "NativeOnStop", "()V", act);
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    std::fflush(stdout);
    std::_Exit(0);  // guest threads are still running; skip static destructors
}
