// bionic libc: memory, strings, stdlib, math, time, setjmp, file descriptors, misc.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "hle/hle.h"
#include "hle/timing.h"

namespace hle {

namespace {

namespace fs = std::filesystem;

u32 gptr(const void* p) { return p ? mem::guest(p) : 0; }

// ---------------------------------------------------------------- time helpers
void write_tm(u32 dst, const std::tm& t) {
    mem::w32(dst + 0, u32(t.tm_sec));
    mem::w32(dst + 4, u32(t.tm_min));
    mem::w32(dst + 8, u32(t.tm_hour));
    mem::w32(dst + 12, u32(t.tm_mday));
    mem::w32(dst + 16, u32(t.tm_mon));
    mem::w32(dst + 20, u32(t.tm_year));
    mem::w32(dst + 24, u32(t.tm_wday));
    mem::w32(dst + 28, u32(t.tm_yday));
    mem::w32(dst + 32, u32(t.tm_isdst));
    mem::w32(dst + 36, 0);
    mem::w32(dst + 40, 0);
}

std::tm read_tm(u32 src) {
    std::tm t{};
    t.tm_sec = int(mem::r32(src + 0));
    t.tm_min = int(mem::r32(src + 4));
    t.tm_hour = int(mem::r32(src + 8));
    t.tm_mday = int(mem::r32(src + 12));
    t.tm_mon = int(mem::r32(src + 16));
    t.tm_year = int(mem::r32(src + 20));
    t.tm_wday = int(mem::r32(src + 24));
    t.tm_yday = int(mem::r32(src + 28));
    t.tm_isdst = int(mem::r32(src + 32));
    return t;
}

u32 g_tm_buf = 0;
std::mutex g_tm_lock;

// ---------------------------------------------------------------- stat
struct HostStat {
    bool ok = false;
    bool dir = false;
    u64 size = 0;
    s64 mtime = 0;
};

HostStat host_stat(const std::string& p) {
    HostStat r;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(p.c_str(), &st) != 0) return r;
#else
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return r;
#endif
    r.ok = true;
    r.dir = (st.st_mode & S_IFMT) == S_IFDIR;
    r.size = u64(st.st_size);
    r.mtime = s64(st.st_mtime);
    return r;
}

void write_stat(u32 dst, const HostStat& s) {
    std::memset(mem::host(dst), 0, 104);
    mem::w32(dst + 16, s.dir ? 0040755 : 0100644);  // st_mode
    mem::w32(dst + 20, 1);                           // st_nlink
    mem::w32(dst + 48, u32(s.size));                 // st_size (64-bit)
    mem::w32(dst + 52, u32(s.size >> 32));
    mem::w32(dst + 56, 4096);                        // st_blksize
    mem::w32(dst + 64, u32((s.size + 511) / 512));   // st_blocks
    mem::w32(dst + 72, u32(s.mtime));                // atime
    mem::w32(dst + 80, u32(s.mtime));                // mtime
    mem::w32(dst + 88, u32(s.mtime));                // ctime
}

// ---------------------------------------------------------------- directories
struct Dir {
    std::vector<std::pair<std::string, bool>> entries;
    size_t pos = 0;
    u32 dirent = 0;  // guest struct dirent buffer
};
std::mutex g_dir_lock;
std::map<u32, Dir> g_dirs;  // guest DIR* -> state

void fill_dirent(u32 d, const std::string& name, bool is_dir, u32 idx) {
    std::memset(mem::host(d), 0, 280);
    mem::w32(d, idx + 1);                    // d_ino
    mem::w32(d + 8, idx + 1);                // d_off
    mem::w16(d + 16, 280);                   // d_reclen
    mem::w8(d + 18, is_dir ? 4 : 8);         // d_type (DT_DIR / DT_REG)
    std::strncpy(mem::ptr<char>(d + 19), name.c_str(), 255);
}

// ---------------------------------------------------------------- open flags
int host_open_flags(u32 g) {
    int f = 0;
    switch (g & 3) {
    case 0: f = O_RDONLY; break;
    case 1: f = O_WRONLY; break;
    default: f = O_RDWR; break;
    }
    if (g & 0x40) f |= O_CREAT;
    if (g & 0x80) f |= O_EXCL;
    if (g & 0x200) f |= O_TRUNC;
    if (g & 0x400) f |= O_APPEND;
#ifdef _WIN32
    f |= O_BINARY;
#endif
    return f;
}

// ---------------------------------------------------------------- misc state
std::mutex g_env_lock;
std::map<std::string, u32> g_env;  // name -> guest "name=value"... we store the value string

std::mutex g_rand_lock;
u64 g_rand48 = 0x1234ABCD330Eull;

u32 g_strtok_save = 0;

}  // namespace

static u32 now_us_since_boot() {
    using namespace std::chrono;
    static auto t0 = steady_clock::now();
    return u32(duration_cast<microseconds>(steady_clock::now() - t0).count());
}

void register_libc() {
    // ---------------------------------------------------------- memory
    reg("malloc", [](Ctx& c) { c.ret(mem::heap_alloc(c.arg(0))); });
    reg("calloc", [](Ctx& c) {
        u64 n = u64(c.arg(0)) * c.arg(1);
        u32 a = mem::heap_alloc(u32(n));
        std::memset(mem::host(a), 0, size_t(n));
        c.ret(a);
    });
    reg("realloc", [](Ctx& c) { c.ret(mem::heap_realloc(c.arg(0), c.arg(1))); });
    reg("free", [](Ctx& c) { mem::heap_free(c.arg(0)); });
    reg("sbrk", [](Ctx& c) { c.ret(0xFFFFFFFF); });
    reg("memcpy", [](Ctx& c) {
        std::memmove(mem::host(c.arg(0)), mem::host(c.arg(1)), c.arg(2));
        c.ret(c.arg(0));
    });
    reg("memmove", [](Ctx& c) {
        std::memmove(mem::host(c.arg(0)), mem::host(c.arg(1)), c.arg(2));
        c.ret(c.arg(0));
    });
    reg("memset", [](Ctx& c) {
        std::memset(mem::host(c.arg(0)), int(c.arg(1)), c.arg(2));
        c.ret(c.arg(0));
    });
    reg("memcmp", [](Ctx& c) { c.ret(u32(std::memcmp(mem::host(c.arg(0)), mem::host(c.arg(1)), c.arg(2)))); });

    // ---------------------------------------------------------- strings
    reg("strlen", [](Ctx& c) { c.ret(u32(std::strlen(c.argstr(0)))); });
    reg("strcmp", [](Ctx& c) { c.ret(u32(std::strcmp(c.argstr(0), c.argstr(1)))); });
    reg("strncmp", [](Ctx& c) { c.ret(u32(std::strncmp(c.argstr(0), c.argstr(1), c.arg(2)))); });
    reg("strcpy", [](Ctx& c) {
        std::strcpy(c.argp<char>(0), c.argstr(1));
        c.ret(c.arg(0));
    });
    reg("strncpy", [](Ctx& c) {
        std::strncpy(c.argp<char>(0), c.argstr(1), c.arg(2));
        c.ret(c.arg(0));
    });
    reg("strcat", [](Ctx& c) {
        std::strcat(c.argp<char>(0), c.argstr(1));
        c.ret(c.arg(0));
    });
    reg("strchr", [](Ctx& c) { c.ret(gptr(std::strchr(c.argstr(0), int(c.arg(1))))); });
    reg("strrchr", [](Ctx& c) { c.ret(gptr(std::strrchr(c.argstr(0), int(c.arg(1))))); });
    reg("strstr", [](Ctx& c) { c.ret(gptr(std::strstr(c.argstr(0), c.argstr(1)))); });
    reg("strdup", [](Ctx& c) { c.ret(mem::heap_strdup(c.argstr(0))); });
    reg("strtok", [](Ctx& c) {
        u32 s = c.arg(0) ? c.arg(0) : g_strtok_save;
        const char* delim = c.argstr(1);
        if (!s) return c.ret(0);
        while (mem::r8(s) && std::strchr(delim, mem::r8(s))) s++;
        if (!mem::r8(s)) {
            g_strtok_save = 0;
            return c.ret(0);
        }
        u32 tok = s;
        while (mem::r8(s) && !std::strchr(delim, mem::r8(s))) s++;
        if (mem::r8(s)) {
            mem::w8(s, 0);
            g_strtok_save = s + 1;
        } else {
            g_strtok_save = 0;
        }
        c.ret(tok);
    });
    reg("wcslen", [](Ctx& c) {  // the game uses 2-byte wchar_t
        u32 a = c.arg(0), n = 0;
        while (mem::r16(a + 2 * n)) n++;
        c.ret(n);
    });

    // ---------------------------------------------------------- stdlib
    reg("atoi", [](Ctx& c) { c.ret(u32(std::atoi(c.argstr(0)))); });
    reg("strtol", [](Ctx& c) {
        char* end;
        long long v = std::strtoll(c.argstr(0), &end, int(c.arg(2)));
        if (v > 0x7FFFFFFFLL) v = 0x7FFFFFFF;
        if (v < -0x80000000LL) v = -0x80000000LL;
        if (c.arg(1)) mem::w32(c.arg(1), gptr(end));
        c.ret(u32(s32(v)));
    });
    reg("strtoul", [](Ctx& c) {
        char* end;
        unsigned long long v = std::strtoull(c.argstr(0), &end, int(c.arg(2)));
        if (v > 0xFFFFFFFFull && c.argstr(0)[0] != '-') v = 0xFFFFFFFF;
        if (c.arg(1)) mem::w32(c.arg(1), gptr(end));
        c.ret(u32(v));
    });
    reg("strtod", [](Ctx& c) {
        char* end;
        double v = std::strtod(c.argstr(0), &end);
        if (c.arg(1)) mem::w32(c.arg(1), gptr(end));
        c.retd(v);
    });
    reg("qsort", [](Ctx& c) {
        u32 base = c.arg(0), n = c.arg(1), size = c.arg(2), cmp = c.arg(3);
        if (n < 2) return;
        std::vector<u32> idx(n);
        for (u32 i = 0; i < n; i++) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](u32 a, u32 b) {
            return s32(cpu::call(cmp, {base + a * size, base + b * size})) < 0;
        });
        std::vector<u8> tmp(size_t(n) * size);
        for (u32 i = 0; i < n; i++) std::memcpy(&tmp[size_t(i) * size], mem::host(base + idx[i] * size), size);
        std::memcpy(mem::host(base), tmp.data(), tmp.size());
    });
    reg("lrand48", [](Ctx& c) {
        std::lock_guard lk(g_rand_lock);
        g_rand48 = (g_rand48 * 0x5DEECE66Dull + 0xB) & ((1ull << 48) - 1);
        c.ret(u32(g_rand48 >> 17));
    });
    reg("getenv", [](Ctx& c) {
        std::lock_guard lk(g_env_lock);
        auto it = g_env.find(c.argstr(0));
        c.ret(it == g_env.end() ? 0 : it->second);
    });
    reg("setenv", [](Ctx& c) {
        std::lock_guard lk(g_env_lock);
        std::string k = c.argstr(0);
        if (g_env.count(k) && !c.arg(2)) return c.ret(0);
        g_env[k] = mem::heap_strdup(c.argstr(1));
        c.ret(0);
    });
    reg("system", [](Ctx& c) {
        LOGW("system(\"%s\") ignored", c.argstr(0) ? c.argstr(0) : "");
        c.ret(0xFFFFFFFF);
    });
    reg("fork", [](Ctx& c) { set_errno(G_ENOSYS); c.ret(0xFFFFFFFF); });
    reg("execv", [](Ctx& c) { set_errno(G_ENOSYS); c.ret(0xFFFFFFFF); });
    reg("waitpid", [](Ctx& c) { set_errno(G_ENOSYS); c.ret(0xFFFFFFFF); });
    reg("prctl", [](Ctx& c) { c.ret(0); });
    reg("sysconf", [](Ctx& c) {
        switch (c.arg(0)) {
        case 0x27: return c.ret(4096);                                        // _SC_PAGESIZE
        case 0x60: case 0x61: return c.ret(std::max(1u, std::thread::hardware_concurrency()));
        case 6: return c.ret(100);                                            // _SC_CLK_TCK
        default: return c.ret(0xFFFFFFFF);
        }
    });

    // ---------------------------------------------------------- math (soft-float ABI)
#define F1(name, fn) reg(name, [](Ctx& c) { c.retf(fn(c.argf(0))); })
#define D1(name, fn) reg(name, [](Ctx& c) { c.retd(fn(c.argd(0))); })
    F1("sinf", std::sin); F1("cosf", std::cos); F1("tanf", std::tan); F1("sqrtf", std::sqrt);
    F1("acosf", std::acos); F1("asinf", std::asin); F1("expf", std::exp); F1("logf", std::log);
    F1("log10f", std::log10); F1("floorf", std::floor); F1("ceilf", std::ceil);
    D1("sin", std::sin); D1("cos", std::cos); D1("tan", std::tan); D1("sqrt", std::sqrt);
    D1("acos", std::acos); D1("atan", std::atan); D1("exp", std::exp); D1("log", std::log);
    D1("log10", std::log10); D1("floor", std::floor); D1("ceil", std::ceil);
#undef F1
#undef D1
    reg("atan2f", [](Ctx& c) { c.retf(std::atan2(c.argf(0), c.argf(1))); });
    reg("fmodf", [](Ctx& c) { c.retf(std::fmod(c.argf(0), c.argf(1))); });
    reg("powf", [](Ctx& c) { c.retf(std::pow(c.argf(0), c.argf(1))); });
    reg("atan2", [](Ctx& c) { c.retd(std::atan2(c.argd(0), c.argd(2))); });
    reg("pow", [](Ctx& c) { c.retd(std::pow(c.argd(0), c.argd(2))); });
    reg("ldexp", [](Ctx& c) { c.retd(std::ldexp(c.argd(0), int(c.arg(2)))); });
    reg("modf", [](Ctx& c) {
        double ip;
        double fr = std::modf(c.argd(0), &ip);
        if (c.arg(2)) std::memcpy(mem::host(c.arg(2)), &ip, 8);
        c.retd(fr);
    });
    reg("__isinf", [](Ctx& c) { c.ret(std::isinf(c.argd(0)) ? 1 : 0); });

    // ---------------------------------------------------------- time
    reg("time", [](Ctx& c) {
        u32 t = u32(std::time(nullptr));
        if (c.arg(0)) mem::w32(c.arg(0), t);
        c.ret(t);
    });
    reg("clock", [](Ctx& c) { c.ret(u32(std::clock() * (1000000.0 / CLOCKS_PER_SEC))); });
    reg("gettimeofday", [](Ctx& c) {
        s64 us = timing::realtime_ns() / 1000;
        if (c.arg(0)) {
            mem::w32(c.arg(0), u32(us / 1000000));
            mem::w32(c.arg(0) + 4, u32(us % 1000000));
        }
        c.ret(0);
    });
    reg("clock_gettime", [](Ctx& c) {
        using namespace std::chrono;
        s64 ns;
        if (c.arg(0) == 0)
            ns = timing::realtime_ns();
        else
            ns = timing::game_ns() + 1000000000LL;  // monotonic, on the game clock
        mem::w32(c.arg(1), u32(ns / 1000000000));
        mem::w32(c.arg(1) + 4, u32(ns % 1000000000));
        c.ret(0);
    });
    reg("gmtime", [](Ctx& c) {
        std::lock_guard lk(g_tm_lock);
        if (!g_tm_buf) g_tm_buf = mem::data_alloc(44);
        std::time_t t = std::time_t(mem::r32(c.arg(0)));
        std::tm* r = std::gmtime(&t);
        if (!r) return c.ret(0);
        write_tm(g_tm_buf, *r);
        c.ret(g_tm_buf);
    });
    reg("localtime", [](Ctx& c) {
        std::lock_guard lk(g_tm_lock);
        if (!g_tm_buf) g_tm_buf = mem::data_alloc(44);
        std::time_t t = std::time_t(mem::r32(c.arg(0)));
        std::tm* r = std::localtime(&t);
        if (!r) return c.ret(0);
        write_tm(g_tm_buf, *r);
        c.ret(g_tm_buf);
    });
    reg("mktime", [](Ctx& c) {
        std::tm t = read_tm(c.arg(0));
        std::time_t r = std::mktime(&t);
        write_tm(c.arg(0), t);
        c.ret(u32(r));
    });

    // ---------------------------------------------------------- setjmp / longjmp
    // jmp_buf layout (ours): r4-r11, sp, lr, fpscr, s16-s31
    reg("setjmp", [](Ctx& c) {
        u32 b = c.arg(0);
        for (int i = 0; i < 8; i++) mem::w32(b + 4 * i, c.reg(4 + i));
        mem::w32(b + 32, c.sp());
        mem::w32(b + 36, c.lr());
        mem::w32(b + 40, c.fpscr());
        for (int i = 0; i < 16; i++) mem::w32(b + 44 + 4 * i, c.ext(16 + i));
        c.ret(0);
    });
    reg("longjmp", [](Ctx& c) {
        u32 b = c.arg(0), v = c.arg(1);
        for (int i = 0; i < 8; i++) c.set_reg(4 + i, mem::r32(b + 4 * i));
        c.set_reg(13, mem::r32(b + 32));
        c.set_reg(14, mem::r32(b + 36));
        c.set_fpscr(mem::r32(b + 40));
        for (int i = 0; i < 16; i++) c.set_ext(16 + i, mem::r32(b + 44 + 4 * i));
        c.ret(v ? v : 1);
    });

    // ---------------------------------------------------------- file descriptors
    reg("open", [](Ctx& c) {
        std::string p = host_path(c.argstr(0));
        int fd = ::open(p.c_str(), host_open_flags(c.arg(1)), 0644);
        LOGD("open(%s) -> %d", c.argstr(0), fd);
        if (fd < 0) set_errno(host_errno_to_guest(errno));
        c.ret(u32(fd));
    });
    reg("close", [](Ctx& c) { c.ret(u32(::close(int(c.arg(0))))); });
    reg("read", [](Ctx& c) {
        int r = ::read(int(c.arg(0)), mem::host(c.arg(1)), c.arg(2));
        if (r < 0) set_errno(host_errno_to_guest(errno));
        c.ret(u32(r));
    });
    reg("write", [](Ctx& c) {
        int fd = int(c.arg(0));
        if (fd == 1 || fd == 2) {
            std::fwrite(mem::host(c.arg(1)), 1, c.arg(2), stdout);
            return c.ret(c.arg(2));
        }
        int r = ::write(fd, mem::host(c.arg(1)), c.arg(2));
        if (r < 0) set_errno(host_errno_to_guest(errno));
        c.ret(u32(r));
    });
    reg("lseek", [](Ctx& c) { c.ret(u32(::lseek(int(c.arg(0)), long(s32(c.arg(1))), int(c.arg(2))))); });
    reg("fsync", [](Ctx& c) { c.ret(0); });
    reg("fcntl", [](Ctx& c) { c.ret(0); });
    reg("ftruncate", [](Ctx& c) {
#ifdef _WIN32
        c.ret(u32(_chsize(int(c.arg(0)), long(c.arg(1)))));
#else
        c.ret(u32(::ftruncate(int(c.arg(0)), off_t(c.arg(1)))));
#endif
    });
    reg("fstat", [](Ctx& c) {
#ifdef _WIN32
        struct _stat64 st;
        if (_fstat64(int(c.arg(0)), &st) != 0) {
#else
        struct stat st;
        if (::fstat(int(c.arg(0)), &st) != 0) {
#endif
            set_errno(G_EBADF);
            return c.ret(0xFFFFFFFF);
        }
        HostStat s;
        s.ok = true;
        s.dir = (st.st_mode & S_IFMT) == S_IFDIR;
        s.size = u64(st.st_size);
        s.mtime = s64(st.st_mtime);
        write_stat(c.arg(1), s);
        c.ret(0);
    });
    reg("stat", [](Ctx& c) {
        HostStat s = host_stat(host_path(c.argstr(0)));
        LOGD("stat(%s) -> %d", c.argstr(0), s.ok);
        if (!s.ok) {
            set_errno(G_ENOENT);
            return c.ret(0xFFFFFFFF);
        }
        write_stat(c.arg(1), s);
        c.ret(0);
    });
    reg("mkdir", [](Ctx& c) {
        std::error_code ec;
        std::string p = host_path(c.argstr(0));
        if (fs::exists(p, ec)) {
            set_errno(G_EEXIST);
            return c.ret(0xFFFFFFFF);
        }
        c.ret(fs::create_directories(p, ec) ? 0 : 0xFFFFFFFF);
    });
    reg("rmdir", [](Ctx& c) {
        std::error_code ec;
        c.ret(fs::remove(host_path(c.argstr(0)), ec) ? 0 : 0xFFFFFFFF);
    });
    reg("remove", [](Ctx& c) {
        std::error_code ec;
        c.ret(fs::remove(host_path(c.argstr(0)), ec) ? 0 : 0xFFFFFFFF);
    });
    reg("unlink", [](Ctx& c) {
        std::error_code ec;
        c.ret(fs::remove(host_path(c.argstr(0)), ec) ? 0 : 0xFFFFFFFF);
    });
    reg("rename", [](Ctx& c) {
        std::error_code ec;
        fs::rename(host_path(c.argstr(0)), host_path(c.argstr(1)), ec);
        c.ret(ec ? 0xFFFFFFFF : 0);
    });
    reg("chdir", [](Ctx& c) { c.ret(0); });
    reg("chmod", [](Ctx& c) { c.ret(0); });
    reg("utime", [](Ctx& c) { c.ret(0); });
    reg("getcwd", [](Ctx& c) {
        if (c.arg(1) < 2) return c.ret(0);
        std::strcpy(c.argp<char>(0), "/");
        c.ret(c.arg(0));
    });
    reg("opendir", [](Ctx& c) {
        std::string p = host_path(c.argstr(0));
        std::error_code ec;
        if (!fs::is_directory(p, ec)) {
            LOGD("opendir(%s) failed", c.argstr(0));
            set_errno(G_ENOENT);
            return c.ret(0);
        }
        Dir d;
        d.entries.push_back({".", true});
        d.entries.push_back({"..", true});
        for (auto& e : fs::directory_iterator(p, ec)) d.entries.push_back({e.path().filename().string(), e.is_directory()});
        d.dirent = mem::heap_alloc(288);
        u32 h = mem::heap_alloc(16);
        std::lock_guard lk(g_dir_lock);
        g_dirs[h] = std::move(d);
        c.ret(h);
    });
    reg("readdir", [](Ctx& c) {
        std::lock_guard lk(g_dir_lock);
        auto it = g_dirs.find(c.arg(0));
        if (it == g_dirs.end() || it->second.pos >= it->second.entries.size()) return c.ret(0);
        Dir& d = it->second;
        auto& [name, isdir] = d.entries[d.pos];
        fill_dirent(d.dirent, name, isdir, u32(d.pos));
        d.pos++;
        c.ret(d.dirent);
    });
    reg("readdir_r", [](Ctx& c) {
        std::lock_guard lk(g_dir_lock);
        auto it = g_dirs.find(c.arg(0));
        if (it == g_dirs.end() || it->second.pos >= it->second.entries.size()) {
            mem::w32(c.arg(2), 0);
            return c.ret(0);
        }
        Dir& d = it->second;
        auto& [name, isdir] = d.entries[d.pos];
        fill_dirent(c.arg(1), name, isdir, u32(d.pos));
        d.pos++;
        mem::w32(c.arg(2), c.arg(1));
        c.ret(0);
    });
    reg("closedir", [](Ctx& c) {
        std::lock_guard lk(g_dir_lock);
        auto it = g_dirs.find(c.arg(0));
        if (it != g_dirs.end()) {
            mem::heap_free(it->second.dirent);
            g_dirs.erase(it);
            mem::heap_free(c.arg(0));
        }
        c.ret(0);
    });
    reg("poll", [](Ctx& c) {
        s32 ms = s32(c.arg(2));
        if (ms > 0) timing::sleep_ns(s64(ms) * 1000000);
        c.ret(0);
    });
    reg("mmap", [](Ctx& c) {
        u32 len = c.arg(1), flags = c.arg(3);
        s32 fd = s32(c.arg(4));
        u32 off = c.arg(5);
        u32 a = mem::map_alloc(len);
        if (!a) {
            set_errno(G_ENOMEM);
            return c.ret(0xFFFFFFFF);
        }
        if (!(flags & 0x20) && fd >= 0) {  // file mapping: read the contents
            long cur = ::lseek(fd, 0, SEEK_CUR);
            ::lseek(fd, long(off), SEEK_SET);
            int r = ::read(fd, mem::host(a), len);
            (void)r;
            ::lseek(fd, cur, SEEK_SET);
        }
        c.ret(a);
    });
    reg("munmap", [](Ctx& c) {
        if (c.arg(0) >= mem::MAP_BASE && c.arg(0) < mem::MAP_END) mem::map_free(c.arg(0), c.arg(1));
        c.ret(0);
    });

    // ---------------------------------------------------------- sockets: no network
    for (const char* n : {"socket", "accept", "bind", "connect", "listen", "send", "sendto", "recv", "recvfrom",
                          "setsockopt", "getsockopt", "getpeername", "getsockname", "shutdown"})
        reg(n, [](Ctx& c) {
            set_errno(G_ENETUNREACH);
            c.ret(0xFFFFFFFF);
        });
    reg("getaddrinfo", [](Ctx& c) {
        if (c.arg(3)) mem::w32(c.arg(3), 0);
        c.ret(4);  // EAI_FAIL
    });
    reg("freeaddrinfo", [](Ctx& c) {});
    (void)now_us_since_boot;
}

}  // namespace hle
