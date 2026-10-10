#include "hle/hle.h"

#include <atomic>
#include <cerrno>
#include <mutex>
#include <set>
#include <unordered_map>

#include "elf_loader.h"

namespace hle {

namespace {
std::unordered_map<std::string, u32> g_funcs;  // name -> stub address
std::unordered_map<std::string, u32> g_data;   // name -> data address
std::mutex g_missing_lock;
std::set<u32> g_missing_reported;
}  // namespace

void reg(const char* name, cpu::HleFn fn) { g_funcs[name] = cpu::register_hle(name, fn); }
void reg_data(const char* name, u32 addr) { g_data[name] = addr; }

static void unimplemented(Ctx& c) {
    u32 stub = c.reg(15) - 4;
    {
        std::lock_guard lk(g_missing_lock);
        if (g_missing_reported.insert(stub).second)
            LOGW("UNIMPLEMENTED %s called from %s", cpu::hle_name_at(stub).c_str(),
                 cpu::symbolize(c.lr()).c_str());
    }
    c.ret(0);
}

u32 resolve(const std::string& name, bool is_func, bool is_weak) {
    auto f = g_funcs.find(name);
    if (f != g_funcs.end()) return f->second;
    auto d = g_data.find(name);
    if (d != g_data.end()) return d->second;
    if (is_weak) return 0;
    if (!is_func) {
        LOGW("missing data import %s (giving it 64 zero bytes)", name.c_str());
        return mem::data_alloc(64);
    }
    u32 a = cpu::register_hle(name, unimplemented);
    g_funcs[name] = a;
    return a;
}

void set_errno(int e) { mem::w32(cpu::thread().errno_addr, u32(e)); }

int host_errno_to_guest(int e) {
    switch (e) {
    case ENOENT: return G_ENOENT;
    case EACCES: return G_EACCES;
    case EEXIST: return G_EEXIST;
    case ENOTDIR: return G_ENOTDIR;
    case EISDIR: return G_EISDIR;
    case EINVAL: return G_EINVAL;
    case ENOSPC: return G_ENOSPC;
    case EBADF: return G_EBADF;
    case ENOMEM: return G_ENOMEM;
    case EAGAIN: return G_EAGAIN;
    case EBUSY: return G_EBUSY;
    case ENOTEMPTY: return G_ENOTEMPTY;
    case EINTR: return G_EINTR;
    case EPERM: return G_EPERM;
    default: return G_EIO;
    }
}

// ---------------------------------------------------------------- data imports
static void make_data_imports() {
    // bionic stdio: FILE __sF[3], each 84 bytes; _file (short) at offset 14
    u32 sf = mem::data_alloc(84 * 3);
    for (int i = 0; i < 3; i++) mem::w16(sf + 84 * i + 14, u16(i));
    reg_data("__sF", sf);

    u32 guard = mem::data_alloc(4);
    mem::w32(guard, 0x1BADC0DE);
    reg_data("__stack_chk_guard", guard);

    // const char* _ctype_ -> table[257] (index c + 1)
    u32 tab = mem::data_alloc(260);
    for (int c = 0; c < 256; c++) {
        u8 v = 0;
        if (c < 128) {
            if (c >= 'A' && c <= 'Z') v |= 0x01;
            if (c >= 'a' && c <= 'z') v |= 0x02;
            if (c >= '0' && c <= '9') v |= 0x04;
            if (c == ' ' || (c >= 9 && c <= 13)) v |= 0x08;
            if (c > 32 && c < 127 && !(v & 0x07)) v |= 0x10;
            if (c < 32 || c == 127) v |= 0x20;
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) v |= 0x40;
            if (c == ' ') v |= 0x80;
        }
        mem::w8(tab + 1 + c, v);
    }
    u32 ctype = mem::data_alloc(4);
    mem::w32(ctype, tab);
    reg_data("_ctype_", ctype);

    // const short* _tolower_tab_ / _toupper_tab_ -> short[257] (index c + 1, [0] = EOF)
    for (int upper = 0; upper < 2; upper++) {
        u32 t = mem::data_alloc(257 * 2);
        mem::w16(t, 0xFFFF);
        for (int c = 0; c < 256; c++) {
            int r = c;
            if (upper && c >= 'a' && c <= 'z') r = c - 32;
            if (!upper && c >= 'A' && c <= 'Z') r = c + 32;
            mem::w16(t + 2 + 2 * c, u16(r));
        }
        u32 p = mem::data_alloc(4);
        mem::w32(p, t);
        reg_data(upper ? "_toupper_tab_" : "_tolower_tab_", p);
    }

    u32 ps = mem::data_alloc(4);
    mem::w32(ps, 4096);
    reg_data("__page_size", ps);

    u32 tz = mem::data_alloc(8);
    mem::w32(tz, mem::data_strdup("UTC"));
    mem::w32(tz + 4, mem::data_strdup("UTC"));
    reg_data("tzname", tz);

    reg_data("__dso_handle", mem::data_alloc(4));
}

// ---------------------------------------------------------------- runtime support
static std::recursive_mutex g_guard_lock;

static void register_runtime() {
    reg("__errno", [](Ctx& c) { c.ret(cpu::thread().errno_addr); });
    reg("__stack_chk_fail", [](Ctx& c) {
        LOGE("stack protector failure in %s", cpu::symbolize(c.lr()).c_str());
        cpu::backtrace(&c);
        std::abort();
    });
    reg("abort", [](Ctx& c) {
        LOGE("guest abort() from %s", cpu::symbolize(c.lr()).c_str());
        cpu::dump_regs(c);
        cpu::backtrace(&c);
        std::fflush(stdout);
        std::abort();
    });
    reg("exit", [](Ctx& c) {
        LOGI("guest exit(%d) from %s", int(c.arg(0)), cpu::symbolize(c.lr()).c_str());
        std::fflush(stdout);
        std::exit(int(c.arg(0)));
    });
    reg("raise", [](Ctx& c) {
        LOGE("guest raise(%d) from %s", int(c.arg(0)), cpu::symbolize(c.lr()).c_str());
        cpu::backtrace(&c);
        std::abort();
    });
    reg("__aeabi_atexit", [](Ctx& c) { c.ret(0); });  // static destructors never run
    reg("__cxa_pure_virtual", [](Ctx& c) {
        LOGE("pure virtual call from %s", cpu::symbolize(c.lr()).c_str());
        std::abort();
    });
    reg("__cxa_guard_acquire", [](Ctx& c) {
        u32 g = c.arg(0);
        g_guard_lock.lock();
        if (mem::r32(g) & 1) {
            g_guard_lock.unlock();
            c.ret(0);
        } else {
            c.ret(1);  // keep the lock until release
        }
    });
    reg("__cxa_guard_release", [](Ctx& c) {
        mem::w32(c.arg(0), 1);
        g_guard_lock.unlock();
        c.ret(0);
    });
    // bionic atomics
    reg("__atomic_cmpxchg", [](Ctx& c) {
        int oldv = int(c.arg(0));
        bool ok = __atomic_compare_exchange_n(c.argp<int>(2), &oldv, int(c.arg(1)), false, __ATOMIC_SEQ_CST,
                                              __ATOMIC_SEQ_CST);
        c.ret(ok ? 0 : 1);
    });
    reg("__atomic_swap", [](Ctx& c) { c.ret(u32(__atomic_exchange_n(c.argp<int>(1), int(c.arg(0)), __ATOMIC_SEQ_CST))); });
    reg("__atomic_inc", [](Ctx& c) { c.ret(u32(__atomic_fetch_add(c.argp<int>(0), 1, __ATOMIC_SEQ_CST))); });
    reg("__atomic_dec", [](Ctx& c) { c.ret(u32(__atomic_fetch_sub(c.argp<int>(0), 1, __ATOMIC_SEQ_CST))); });
    // C++ exception unwinding support for the game's own libgcc unwinder
    reg("__gnu_Unwind_Find_exidx", [](Ctx& c) {
        if (c.arg(1)) mem::w32(c.arg(1), elf::g_game.exidx_count);
        c.ret(elf::g_game.exidx);
    });
}

void register_all() {
    make_data_imports();
    register_runtime();
    register_libc();
    register_stdio();
    register_printf();
    register_pthread();
    register_android();
    register_gles();
}

}  // namespace hle
