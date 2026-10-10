#include "crash.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cpu.h"
#include "log.h"
#include "memory.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#else
#include <execinfo.h>
#include <unistd.h>
#endif

namespace crash {

namespace {

std::string g_dir;
volatile long g_in_handler = 0;

void log_guest_state() {
    u32 pc = cpu::current_pc();
    if (pc) LOGE("  guest code running on this thread: %08x %s", pc, cpu::symbolize(pc).c_str());
    else LOGE("  no guest code running on this thread");
}

#ifdef _WIN32
std::string module_of(void* addr, uintptr_t& offset) {
    HMODULE mod = nullptr;
    offset = uintptr_t(addr);
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(addr), &mod))
        return "?";
    char name[MAX_PATH] = "?";
    GetModuleFileNameA(mod, name, sizeof name);
    offset = uintptr_t(addr) - uintptr_t(mod);
    const char* base = std::strrchr(name, '\\');
    return base ? base + 1 : name;
}

void write_dump(EXCEPTION_POINTERS* ep) {
    std::string path = g_dir + "crash.dmp";
    HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
    if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), ep ? &mei : nullptr,
                          nullptr, nullptr))
        LOGE("  minidump written to %s", path.c_str());
    CloseHandle(f);
}

void log_backtrace() {
    void* frames[48];
    USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < n; i++) {
        uintptr_t off;
        std::string m = module_of(frames[i], off);
        LOGE("    #%02u %s+0x%llx", i, m.c_str(), (unsigned long long)off);
    }
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_in_handler, 1)) return EXCEPTION_CONTINUE_SEARCH;
    auto* er = ep->ExceptionRecord;
    uintptr_t off;
    std::string mod = module_of(er->ExceptionAddress, off);
    LOGE("CRASH: exception 0x%08lx at %s+0x%llx (thread %lu)", er->ExceptionCode, mod.c_str(),
         (unsigned long long)off, GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        uintptr_t a = er->ExceptionInformation[1];
        const char* kind = er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing";
        uintptr_t gb = uintptr_t(mem::g_base);
        if (a >= gb && a < gb + (1ull << 32))
            LOGE("  %s guest address %08llx (not mapped)", kind, (unsigned long long)(a - gb));
        else
            LOGE("  %s host address %p", kind, reinterpret_cast<void*>(a));
    }
    log_guest_state();
    LOGE("  host stack:");
    log_backtrace();
    write_dump(ep);
    std::fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void on_signal(int sig) {
    if (g_in_handler) std::_Exit(3);
    g_in_handler = 1;
    LOGE("CRASH: %s", sig == SIGABRT ? "abort() called (assertion or fatal error, see stderr.log)" : "fatal signal");
    log_guest_state();
#ifdef _WIN32
    LOGE("  host stack:");
    log_backtrace();
    write_dump(nullptr);
#else
    void* frames[48];
    int n = ::backtrace(frames, 48);
    backtrace_symbols_fd(frames, n, STDOUT_FILENO);
#endif
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(3);
}

}  // namespace

void install(const std::string& dir) {
    g_dir = dir;
    if (!g_dir.empty() && g_dir.back() != '/' && g_dir.back() != '\\') g_dir += '/';
    // assertion messages from the C runtime / dynarmic go to stderr, which has no console
    std::freopen((g_dir + "stderr.log").c_str(), "w", stderr);
#ifdef _WIN32
    SetUnhandledExceptionFilter(on_exception);
#else
    std::signal(SIGSEGV, on_signal);
    std::signal(SIGBUS, on_signal);
    std::signal(SIGILL, on_signal);
    std::signal(SIGFPE, on_signal);
#endif
    std::signal(SIGABRT, on_signal);
}

}  // namespace crash
