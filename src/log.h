#pragma once
#include <cstdio>

namespace logging {
void write(char level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void open_file(const char* path);
extern bool g_verbose;
}  // namespace logging

#define LOGI(...) ::logging::write('I', __VA_ARGS__)
#define LOGW(...) ::logging::write('W', __VA_ARGS__)
#define LOGE(...) ::logging::write('E', __VA_ARGS__)
#define LOGD(...) \
    do { \
        if (::logging::g_verbose) ::logging::write('D', __VA_ARGS__); \
    } while (0)
