#include "log.h"

#include <chrono>
#include <cstdarg>
#include <mutex>

namespace logging {

bool g_verbose = false;
static std::FILE* g_file = nullptr;
static std::mutex g_lock;

void open_file(const char* path) { g_file = std::fopen(path, "w"); }

void write(char level, const char* fmt, ...) {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    double t = duration<double>(steady_clock::now() - t0).count();
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard lk(g_lock);
    std::fprintf(stdout, "%9.3f %c %s\n", t, level, buf);
    if (level == 'E') std::fflush(stdout);
    if (g_file) {
        std::fprintf(g_file, "%9.3f %c %s\n", t, level, buf);
        std::fflush(g_file);
    }
}

}  // namespace logging
