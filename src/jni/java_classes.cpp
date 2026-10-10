// Host implementations of the Java classes the game's native code talks to.
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>

#include "config.h"
#include "game_patches.h"
#include "hle/hle.h"
#include "jni/jni.h"
#include "log.h"

namespace jni {

namespace fs = std::filesystem;

namespace {

ObjPtr g_activity;
ObjPtr g_asset_manager;

// ---------------------------------------------------------------- assets
// An open asset: either a file or a patched in-memory copy (see game_patches.cpp).
struct AssetStream {
    std::FILE* f = nullptr;
    std::shared_ptr<const std::vector<u8>> mem;
    size_t pos = 0;
    bool open() const { return f || mem; }
    size_t size() {
        if (mem) return mem->size();
        long cur = std::ftell(f);
        std::fseek(f, 0, SEEK_END);
        long end = std::ftell(f);
        std::fseek(f, cur, SEEK_SET);
        return size_t(end);
    }
    size_t tell() { return mem ? pos : size_t(std::ftell(f)); }
    void seek(size_t p) {
        if (mem) pos = std::min(p, mem->size());
        else std::fseek(f, long(p), SEEK_SET);
    }
    size_t read(void* dst, size_t n) {
        if (!mem) return std::fread(dst, 1, n, f);
        n = std::min(n, mem->size() - pos);
        std::memcpy(dst, mem->data() + pos, n);
        pos += n;
        return n;
    }
    void close() {
        if (f) std::fclose(f);
        f = nullptr;
        mem.reset();
    }
    ~AssetStream() { close(); }
};

std::string asset_path(const std::string& name) {
    std::string n = name;
    while (!n.empty() && n[0] == '/') n.erase(0, 1);
    return hle::g_assets_dir + "/" + n;
}

JValue str(const std::string& s) { return JValue::L(new_local(make_string(s))); }

// ---------------------------------------------------------------- audio (android.media.AudioTrack)
struct AudioOut {
    SDL_AudioDeviceID dev = 0;
    int rate = 44100, channels = 2;
    u32 max_queued = 0;
};
AudioOut g_audio;
std::FILE* g_audio_dump = nullptr;  // debug: raw PCM capture

}  // namespace

void audio_dump(const char* path) { g_audio_dump = std::fopen(path, "wb"); }

void audio_open(int rate, int channels, int buffer_frames) {
    SDL_AudioSpec want{}, have{};
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = u8(channels);
    want.samples = 1024;
    g_audio.dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_audio.dev) {
        LOGE("SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return;
    }
    g_audio.rate = rate;
    g_audio.channels = channels;
    // allow ~3 game buffers of latency before write() blocks
    g_audio.max_queued = u32(std::max(buffer_frames, 1024) * channels * 2 * 3);
    LOGI("audio: %d Hz, %d ch, game buffer %d frames", rate, channels, buffer_frames);
}

ObjPtr activity() { return g_activity; }

void register_java_classes() {
    def_superclass("com/eamobile/deadspace_full_azn/DeadSpaceActivity", "com/eamobile/port/PadActivity");
    def_superclass("com/eamobile/port/PadActivity", "com/ea/blast/MainActivity");
    def_superclass("com/ea/blast/MainActivity", "android/app/Activity");
    def_superclass("android/app/Activity", "android/content/Context");
    def_superclass("java/io/FileNotFoundException", "java/io/IOException");
    def_superclass("java/io/IOException", "java/lang/Exception");

    g_activity = make("com/eamobile/deadspace_full_azn/DeadSpaceActivity");
    g_asset_manager = make("android/content/res/AssetManager");

    // ---------------------------------------------------------- activity
    def("com/ea/blast/MainActivity", "GetInstance", [](auto&, auto&, auto&) { return JValue::L(new_local(g_activity)); });
    def("android/content/Context", "getAssets", [](auto&, auto&, auto&) { return JValue::L(new_local(g_asset_manager)); });
    def("android/app/Activity", "finish", [](auto&, auto&, auto&) {
        LOGI("activity finish() requested by the game");
        g_quit = true;
        return JValue{};
    });

    // ---------------------------------------------------------- assets
    def("android/content/res/AssetManager", "open", [](auto&, const std::vector<JValue>& a, auto&) {
        std::string name = string_value(a[0].l());
        auto st = std::make_shared<AssetStream>();
        st->mem = patches::asset(name);
        if (!st->mem) st->f = std::fopen(asset_path(name).c_str(), "rb");
        if (!st->mem && !st->f) LOGW("missing asset %s", name.c_str());
        else LOGD("AssetManager.open(%s) -> %s", name.c_str(), st->mem ? "patched" : "ok");
        if (!st->open()) {
            throw_new("java/io/FileNotFoundException", name);
            return JValue{};
        }
        auto o = make("android/content/res/AssetManager$AssetInputStream");
        o->native = st;
        return JValue::L(new_local(o));
    });
    def("android/content/res/AssetManager", "openFd", [](auto&, const std::vector<JValue>& a, auto&) {
        std::string name = string_value(a[0].l());
        std::error_code ec;
        uintmax_t size;
        if (auto m = patches::asset(name)) size = m->size();
        else size = fs::file_size(asset_path(name), ec);
        if (ec) LOGW("missing asset %s", name.c_str());
        else LOGD("AssetManager.openFd(%s) -> ok", name.c_str());
        if (ec) {
            throw_new("java/io/FileNotFoundException", name);
            return JValue{};
        }
        auto o = make("android/content/res/AssetFileDescriptor");
        o->fields["length"] = JValue::J(s64(size));
        return JValue::L(new_local(o));
    });
    def("android/content/res/AssetManager", "list", [](auto&, const std::vector<JValue>& a, auto&) {
        std::string name = string_value(a[0].l());
        std::vector<std::string> names;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(asset_path(name), ec)) names.push_back(e.path().filename().string());
        std::sort(names.begin(), names.end());
        auto arr = make_array('L', u32(names.size()));
        arr->cls = "[Ljava/lang/String;";
        for (size_t i = 0; i < names.size(); i++) arr->objs[i] = make_string(names[i]);
        LOGD("AssetManager.list(%s) -> %zu entries", name.c_str(), names.size());
        return JValue::L(new_local(arr));
    });
    def("android/content/res/AssetFileDescriptor", "getLength", [](const ObjPtr& t, auto&, auto&) {
        return t ? t->fields["length"] : JValue{};
    });
    def("android/content/res/AssetFileDescriptor", "getStartOffset", [](auto&, auto&, auto&) { return JValue::J(0); });
    def("android/content/res/AssetFileDescriptor", "close", [](auto&, auto&, auto&) { return JValue{}; });
    def_superclass("android/content/res/AssetManager$AssetInputStream", "java/io/InputStream");
    def("java/io/InputStream", "read", [](const ObjPtr& t, const std::vector<JValue>& a, const std::string& sig) {
        auto s = t ? std::static_pointer_cast<AssetStream>(t->native) : nullptr;
        if (!s || !s->open()) return JValue::I(-1);
        if (a.empty()) {  // read()I
            u8 ch;
            return JValue::I(s->read(&ch, 1) ? ch : -1);
        }
        ObjPtr arr = get(a[0].l());
        if (!arr) return JValue::I(-1);
        u32 off = a.size() >= 3 ? u32(a[1].i()) : 0;
        u32 len = a.size() >= 3 ? u32(a[2].i()) : arr->len;
        if (len == 0) return JValue::I(0);
        size_t n = s->read(mem::host(arr->data + off), len);
        return JValue::I(n == 0 ? -1 : int(n));
    });
    def("java/io/InputStream", "skip", [](const ObjPtr& t, const std::vector<JValue>& a, auto&) {
        auto s = t ? std::static_pointer_cast<AssetStream>(t->native) : nullptr;
        if (!s || !s->open()) return JValue::J(0);
        size_t before = s->tell();
        s->seek(std::min<size_t>(s->size(), before + size_t(a[0].j())));
        return JValue::J(s64(s->tell() - before));
    });
    def("java/io/InputStream", "available", [](const ObjPtr& t, auto&, auto&) {
        auto s = t ? std::static_pointer_cast<AssetStream>(t->native) : nullptr;
        if (!s || !s->open()) return JValue::I(0);
        return JValue::I(int(s->size() - s->tell()));
    });
    def("java/io/InputStream", "close", [](const ObjPtr& t, auto&, auto&) {
        if (t)
            if (auto s = std::static_pointer_cast<AssetStream>(t->native)) s->close();
        return JValue{};
    });

    // ---------------------------------------------------------- device information
    const char* sys = "com/ea/blast/SystemAndroidDelegate";
    auto I = [](int v) { return [v](auto&, auto&, auto&) { return str(std::to_string(v)); }; };
    auto S = [](std::string v) { return [v](auto&, auto&, auto&) { return str(v); }; };
    def(sys, "<init>", [](auto&, auto&, auto&) { return JValue{}; });
    def(sys, "GetAccelerometerCount", I(1));  // the engine assumes one exists
    def(sys, "IsBatteryStateAvailable", I(0));
    def(sys, "GetCameraCount", I(0));
    def(sys, "GetChipset", S("x86_64"));
    def(sys, "GetCompassCount", I(0));
    def(sys, "GetManufacturer", S("PC"));
    def(sys, "GetDeviceModel", S("PC"));
    def(sys, "GetDeviceName", S("PC"));
    def(sys, "GetPhoneNumber", S(""));
    def(sys, "GetDeviceSubscriberID", S(""));
    def(sys, "GetDeviceUniqueId", S("0000000000000000"));
    def(sys, "GetDisplayCount", I(1));
    def(sys, "GetGyroscopeCount", I(0));
    def(sys, "GetLocationAvailable", I(0));
    def(sys, "GetMicrophoneCount", I(0));
    def(sys, "GetApiLevel", I(10));
    def(sys, "GetPlatformRawName", S("Android"));
    def(sys, "GetPlatformStdName", S("Android"));
    def(sys, "GetPlatformVersion", S("2.3.4"));
    def(sys, "GetPhysicalKeyboardCount", I(1));
    def(sys, "GetProcessorArchitecture", S("armeabi"));
    def(sys, "GetLanguage", [](auto&, auto&, auto&) { return str(g_config.language); });
    def(sys, "GetLocale", [](auto&, auto&, auto&) { return str(g_config.locale); });
    def(sys, "GetTotalRAM", S("-1"));  // what the shipped Java returns
    def(sys, "GetTouchPadCount", I(0));
    def(sys, "GetTouchScreenCount", I(1));
    def(sys, "GetTrackBallCount", I(0));
    def(sys, "GetVibratorCount", I(0));
    def(sys, "GetVirtualKeyboardCount", I(0));
    def(sys, "IntentView", [](auto&, const std::vector<JValue>& a, auto&) {
        LOGI("game wants to open %s (ignored)", string_value(a[0].l()).c_str());
        return JValue::Z(false);
    });

    def("com/ea/blast/PowerManagerAndroid", "<init>", [](auto&, auto&, auto&) { return JValue{}; });
    def("com/ea/blast/PowerManagerAndroid", "ApplyKeepAwake", [](auto&, const std::vector<JValue>& a, auto&) {
        if (a[0].z()) SDL_DisableScreenSaver(); else SDL_EnableScreenSaver();
        return JValue{};
    });
    def("com/ea/blast/GetAppDataDirectoryDelegate", "<init>", [](auto&, auto&, auto&) { return JValue{}; });
    def("com/ea/blast/GetAppDataDirectoryDelegate", "GetAppDataDirectory", S("/data/data/com.ea.deadspace/files"));
    def("com/ea/blast/GetAppDataDirectoryDelegate", "GetExternalStorageDirectory", S("/sdcard"));

    const char* acc = "com/ea/blast/AccelerometerAndroidDelegate";
    def(acc, "<init>", [](auto&, auto&, auto&) { return JValue{}; });
    def(acc, "SetEnabled", [](auto&, auto&, auto&) { return JValue{}; });
    def(acc, "SetUpdateFrequency", [](auto&, auto&, auto&) { return JValue{}; });
    def("com/ea/blast/DeviceOrientationHandlerAndroidDelegate", "<init>", [](auto&, auto&, auto&) { return JValue{}; });
    def("com/ea/blast/DeviceOrientationHandlerAndroidDelegate", "SetEnabled", [](auto&, auto&, auto&) { return JValue{}; });
    def("com/ea/blast/DeviceOrientationHandlerAndroidDelegate", "OnLifeCycleFocusGained",
        [](auto&, auto&, auto&) { return JValue{}; });

    const char* disp = "com/ea/blast/DisplayAndroidDelegate";
    def(disp, "<init>", [](auto&, auto&, auto&) { return JValue{}; });
    def(disp, "GetStdOrientation", [](auto&, auto&, auto&) {
        return call_native("com/ea/blast/DisplayAndroidDelegate", "NativeGetOrientationNormal", "()I",
                           class_object("com/ea/blast/DisplayAndroidDelegate"), {});
    });
    def(disp, "SetStdOrientation", [](auto&, auto&, auto&) { return JValue{}; });
    def(disp, "GetDefaultWidth", [](auto&, auto&, auto&) { return JValue::I(g_config.screen_w); });
    def(disp, "GetDefaultHeight", [](auto&, auto&, auto&) { return JValue::I(g_config.screen_h); });
    def(disp, "GetDpiX", [](auto&, auto&, auto&) { return JValue::F(g_config.dpi); });
    def(disp, "GetDpiY", [](auto&, auto&, auto&) { return JValue::F(g_config.dpi); });
    def("com/ea/blast/TouchSurfaceAndroid", "<init>", [](auto&, auto&, auto&) { return JValue{}; });
    def("com/ea/blast/TouchSurfaceAndroid", "IsTouchScreenMultiTouch", [](auto&, auto&, auto&) { return JValue::Z(true); });

    const char* q = "com/eamobile/Query";
    def(q, "getVersion", S("1.2.0"));
    def(q, "getTotalMemory", [](auto&, auto&, auto&) { return JValue::J(s64(1) << 30); });
    def(q, "isContentReady", [](auto&, auto&, auto&) { return JValue::Z(true); });
    def(q, "saveImage", [](auto&, auto&, auto&) {
        LOGI("Query.saveImage ignored");
        return JValue{};
    });

    // ---------------------------------------------------------- android.media.AudioTrack
    const char* at = "android/media/AudioTrack";
    def(at, "play", [](auto&, auto&, auto&) {
        if (g_audio.dev) SDL_PauseAudioDevice(g_audio.dev, 0);
        return JValue{};
    });
    def(at, "pause", [](auto&, auto&, auto&) {
        if (g_audio.dev) SDL_PauseAudioDevice(g_audio.dev, 1);
        return JValue{};
    });
    def(at, "stop", [](auto&, auto&, auto&) {
        if (g_audio.dev) SDL_PauseAudioDevice(g_audio.dev, 1);
        return JValue{};
    });
    def(at, "flush", [](auto&, auto&, auto&) {
        if (g_audio.dev) SDL_ClearQueuedAudio(g_audio.dev);
        return JValue{};
    });
    def(at, "release", [](auto&, auto&, auto&) { return JValue{}; });
    def(at, "write", [](auto&, const std::vector<JValue>& a, const std::string& sig) {
        ObjPtr arr = get(a[0].l());
        if (!arr) return JValue::I(-3);
        u32 off = u32(a[1].i()), n = u32(a[2].i());
        static std::atomic<u64> total{0};
        if (total.fetch_add(n) == 0) LOGI("audio: game started streaming (%u samples per write)", n);
        int es = arr->elem == 'S' ? 2 : 1;
        if (!g_audio.dev) {
            // no audio device: pace the game's mixer in real time
            std::this_thread::sleep_for(std::chrono::microseconds(u64(n) * 1000000 / es / g_audio.channels /
                                                                  std::max(1, g_audio.rate) * (es == 2 ? 1 : 1)));
            return JValue::I(int(n));
        }
        // stats: time the game spends producing each buffer, and how often the device ran dry
        static u64 last_return = 0, busy = 0, writes = 0, underruns = 0;
        u64 t_in = SDL_GetPerformanceCounter();
        if (last_return) busy += t_in - last_return;
        if (SDL_GetQueuedAudioSize(g_audio.dev) == 0 && writes > 10) underruns++;
        while (SDL_GetQueuedAudioSize(g_audio.dev) > g_audio.max_queued && !g_quit)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        SDL_QueueAudio(g_audio.dev, mem::host(arr->data + off * es), n * es);
        if (++writes % 1000 == 0) {
            LOGI("audio: %llu underruns so far, game mixes a %u-sample buffer in %.2f ms on average",
                 (unsigned long long)underruns, n, busy * 1000.0 / SDL_GetPerformanceFrequency() / 1000);
            busy = 0;
        }
        last_return = SDL_GetPerformanceCounter();
        if (g_audio_dump) std::fwrite(mem::host(arr->data + off * es), 1, n * es, g_audio_dump);
        return JValue::I(int(n));
    });
}

}  // namespace jni
