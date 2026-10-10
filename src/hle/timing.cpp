#include "hle/timing.h"

#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

namespace timing {

namespace {
struct Base {
    int64_t real_ns;
    steady::time_point steady_at;
    Base() {
        steady_at = steady::now();
#ifdef _WIN32
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        int64_t t = (int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;  // 100 ns since 1601
        real_ns = (t - 116444736000000000LL) * 100;
#else
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        real_ns = int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#endif
    }
};
const Base& base() {
    static Base b;
    return b;
}
}  // namespace

// Game time: elapsed steady time since start, optionally running faster (cinematic skip).
namespace {
std::mutex g_scale_lock;
double g_scale = 1.0;
int64_t g_anchor_real = 0, g_anchor_game = 0;  // ns since base().steady_at

int64_t real_elapsed() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(steady::now() - base().steady_at).count();
}
int64_t game_elapsed_locked() { return g_anchor_game + int64_t(double(real_elapsed() - g_anchor_real) * g_scale); }
}  // namespace

int64_t game_ns() {
    std::lock_guard lk(g_scale_lock);
    return game_elapsed_locked();
}

void set_time_scale(double scale) {
    std::lock_guard lk(g_scale_lock);
    g_anchor_game = game_elapsed_locked();
    g_anchor_real = real_elapsed();
    g_scale = scale;
}

double time_scale() {
    std::lock_guard lk(g_scale_lock);
    return g_scale;
}

int64_t realtime_ns() { return base().real_ns + game_ns(); }

steady::time_point from_realtime(int64_t deadline_ns) {
    return steady::now() + std::chrono::nanoseconds(int64_t(double(deadline_ns - realtime_ns()) / time_scale()));
}

void sleep_ns(int64_t ns) {
    ns = int64_t(double(ns) / time_scale());
    if (ns <= 0) {
        std::this_thread::yield();
        return;
    }
#ifdef _WIN32
    // high-resolution waitable timer (Windows 10 1803+), falling back to Sleep
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                       TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(ns / 100);  // relative, 100 ns units
        if (due.QuadPart == 0) due.QuadPart = -1;
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep(DWORD((ns + 999999) / 1000000));
#else
    timespec ts{time_t(ns / 1000000000), long(ns % 1000000000)};
    while (nanosleep(&ts, &ts) != 0) {
    }
#endif
}

}  // namespace timing
