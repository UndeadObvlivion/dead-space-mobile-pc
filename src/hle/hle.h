// High-level emulation of the Android system libraries the game imports.
#pragma once
#include <string>

#include "cpu.h"
#include "log.h"
#include "memory.h"

namespace hle {

using cpu::Ctx;

void reg(const char* name, cpu::HleFn fn);
void reg_data(const char* name, u32 addr);
u32 resolve(const std::string& name, bool is_func, bool is_weak);

// Module registration (called once at startup, before the game library is loaded)
void register_all();
void register_libc();
void register_stdio();
void register_printf();
void register_pthread();
void register_android();
void register_gles();

// errno for the calling guest thread
void set_errno(int e);

// Reads variadic arguments the way the ARM EABI lays them out.
struct ArgReader {
    Ctx* c = nullptr;  // slot mode: arguments in r0-r3 then the stack
    int slot = 0;
    u32 va = 0;        // va_list mode: pointer into the argument save area

    static ArgReader slots(Ctx& c, int first) { ArgReader r; r.c = &c; r.slot = first; return r; }
    static ArgReader valist(u32 ap) { ArgReader r; r.va = ap; return r; }

    u32 next32() {
        if (c) return c->arg(slot++);
        u32 v = mem::r32(va);
        va += 4;
        return v;
    }
    u64 next64() {
        if (c) {
            slot = (slot + 1) & ~1;
            u64 v = c->arg64(slot);
            slot += 2;
            return v;
        }
        va = (va + 7) & ~7u;
        u64 v = u64(mem::r32(va)) | (u64(mem::r32(va + 4)) << 32);
        va += 8;
        return v;
    }
    double nextd() {
        u64 v = next64();
        double d;
        std::memcpy(&d, &v, 8);
        return d;
    }
};

// printf family core: formats into out using guest-format fmt and guest args.
std::string format(const char* fmt, ArgReader& args);
// sscanf core; returns number of assigned fields (or -1 on input failure before any).
int scan(const char* input, const char* fmt, ArgReader& args);

// Virtual file system: guest path -> host path.
std::string host_path(const std::string& guest);
// Host directory holding the game assets (the APK's assets/ folder).
extern std::string g_assets_dir;
extern std::string g_data_dir;  // writable user data root

// Guest errno values (Linux)
enum {
    G_EPERM = 1, G_ENOENT = 2, G_EINTR = 4, G_EIO = 5, G_EBADF = 9, G_EAGAIN = 11, G_ENOMEM = 12,
    G_EACCES = 13, G_EBUSY = 16, G_EEXIST = 17, G_ENOTDIR = 20, G_EISDIR = 21, G_EINVAL = 22,
    G_ENOSPC = 28, G_ENOSYS = 38, G_ENOTEMPTY = 39, G_ETIMEDOUT = 110, G_ENETUNREACH = 101
};
int host_errno_to_guest(int e);

}  // namespace hle
