// printf / scanf families with guest (ARM EABI) varargs.
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "hle/hle.h"

namespace hle {

std::FILE* stdio_host_file(u32 gf);

std::string format(const char* fmt, ArgReader& args) {
    std::string out;
    if (!fmt) return out;
    char buf[512];
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            out += *p;
            continue;
        }
        const char* start = p++;
        if (*p == '%') {
            out += '%';
            continue;
        }
        std::string spec = "%";
        while (*p && std::strchr("-+ #0'", *p)) spec += *p++;
        if (*p == '*') {
            spec += std::to_string(s32(args.next32()));
            p++;
        } else {
            while (std::isdigit(u8(*p))) spec += *p++;
        }
        if (*p == '.') {
            spec += *p++;
            if (*p == '*') {
                s32 prec = s32(args.next32());
                spec += std::to_string(prec < 0 ? 0 : prec);
                p++;
            } else {
                while (std::isdigit(u8(*p))) spec += *p++;
            }
        }
        int lng = 0;  // 0 int, 1 long, 2 long long, -1 short, -2 char, 3 size_t/ptrdiff
        bool long_double = false;
        for (;;) {
            if (*p == 'h') { lng = lng == -1 ? -2 : -1; p++; }
            else if (*p == 'l') { lng = lng == 1 ? 2 : 1; p++; }
            else if (*p == 'q' || *p == 'j') { lng = 2; p++; }
            else if (*p == 'z' || *p == 't') { lng = 3; p++; }
            else if (*p == 'L') { long_double = true; p++; }
            else break;
        }
        char conv = *p;
        if (!conv) {
            out += start;
            break;
        }
        switch (conv) {
        case 'd':
        case 'i': {
            long long v;
            if (lng == 2) v = s64(args.next64());
            else v = s32(args.next32());
            if (lng == -1) v = short(v);
            if (lng == -2) v = (signed char)(v);
            std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), v);
            out += buf;
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long long v;
            if (lng == 2) v = args.next64();
            else v = args.next32();
            if (lng == -1) v = u16(v);
            if (lng == -2) v = u8(v);
            std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), v);
            out += buf;
            break;
        }
        case 'c': {
            std::snprintf(buf, sizeof buf, (spec + "c").c_str(), int(args.next32()));
            out += buf;
            break;
        }
        case 's': {
            u32 a = args.next32();
            if (lng == 1) {  // wide string (2-byte wchar_t)
                std::string s;
                if (a)
                    for (u32 i = 0; mem::r16(a + 2 * i); i++) {
                        u16 ch = mem::r16(a + 2 * i);
                        s += ch < 128 ? char(ch) : '?';
                    }
                else
                    s = "(null)";
                std::snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
                out += buf;
            } else {
                const char* s = a ? mem::cstr(a) : "(null)";
                if (spec == "%") {
                    out += s;
                } else {
                    std::string big(std::strlen(s) + 256, '\0');
                    int n = std::snprintf(big.data(), big.size(), (spec + "s").c_str(), s);
                    out.append(big.data(), size_t(std::max(0, n)));
                }
            }
            break;
        }
        case 'p': {
            std::snprintf(buf, sizeof buf, "0x%x", args.next32());
            out += buf;
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            (void)long_double;  // long double == double on ARM
            double d = args.nextd();
            std::snprintf(buf, sizeof buf, (spec + conv).c_str(), d);
            out += buf;
            break;
        }
        case 'n': {
            u32 a = args.next32();
            if (a) mem::w32(a, u32(out.size()));
            break;
        }
        default:
            out.append(start, p + 1);
            break;
        }
    }
    return out;
}

static u32 write_out(u32 buf, u32 size, const std::string& s) {
    if (size) {
        u32 n = std::min<u32>(u32(s.size()), size - 1);
        std::memcpy(mem::host(buf), s.data(), n);
        mem::w8(buf + n, 0);
    }
    return u32(s.size());
}

int scan(const char* in, const char* fmt, ArgReader& args) {
    int assigned = 0;
    const char* s = in;
    for (const char* p = fmt; *p; p++) {
        if (std::isspace(u8(*p))) {
            while (std::isspace(u8(*s))) s++;
            continue;
        }
        if (*p != '%') {
            if (*s != *p) return assigned;
            s++;
            continue;
        }
        p++;
        if (*p == '%') {
            while (std::isspace(u8(*s))) s++;
            if (*s != '%') return assigned;
            s++;
            continue;
        }
        bool suppress = false;
        if (*p == '*') {
            suppress = true;
            p++;
        }
        int width = 0;
        while (std::isdigit(u8(*p))) width = width * 10 + (*p++ - '0');
        int lng = 0;
        for (;;) {
            if (*p == 'h') { lng = lng == -1 ? -2 : -1; p++; }
            else if (*p == 'l') { lng = lng == 1 ? 2 : 1; p++; }
            else if (*p == 'L' || *p == 'q' || *p == 'j') { lng = 2; p++; }
            else if (*p == 'z' || *p == 't') { p++; }
            else break;
        }
        char conv = *p;
        if (conv != 'c' && conv != '[' && conv != 'n') {
            while (std::isspace(u8(*s))) s++;
        }
        if (!*s && conv != 'n') return assigned ? assigned : -1;
        std::string field;
        auto take = [&](auto pred) {
            int n = 0;
            while (*s && pred(*s) && (!width || n < width)) {
                field += *s++;
                n++;
            }
        };
        switch (conv) {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
            std::string tmp;
            const char* q = s;
            int n = 0;
            if ((*q == '-' || *q == '+') && (!width || n < width)) { tmp += *q++; n++; }
            while (*q && (!width || n < width) &&
                   (std::isxdigit(u8(*q)) || ((*q == 'x' || *q == 'X') && (base == 16 || base == 0)))) {
                tmp += *q++;
                n++;
            }
            if (tmp.empty()) return assigned;
            char* end;
            long long v = std::strtoll(tmp.c_str(), &end, base);
            if (end == tmp.c_str()) return assigned;
            s += end - tmp.c_str();
            if (!suppress) {
                u32 a = args.next32();
                if (lng == 2) { u64 x = u64(v); std::memcpy(mem::host(a), &x, 8); }
                else if (lng == -1) mem::w16(a, u16(v));
                else if (lng == -2) mem::w8(a, u8(v));
                else mem::w32(a, u32(v));
                assigned++;
            }
            break;
        }
        case 'f':
        case 'e':
        case 'g':
        case 'E':
        case 'G':
        case 'a': {
            std::string tmp = width ? std::string(s, strnlen(s, size_t(width))) : std::string(s);
            char* end;
            double v = std::strtod(tmp.c_str(), &end);
            if (end == tmp.c_str()) return assigned;
            s += end - tmp.c_str();
            if (!suppress) {
                u32 a = args.next32();
                if (lng >= 1) std::memcpy(mem::host(a), &v, 8);
                else { float f = float(v); std::memcpy(mem::host(a), &f, 4); }
                assigned++;
            }
            break;
        }
        case 's': {
            take([](char ch) { return !std::isspace(u8(ch)); });
            if (!suppress) {
                u32 a = args.next32();
                std::memcpy(mem::host(a), field.c_str(), field.size() + 1);
                assigned++;
            }
            break;
        }
        case 'c': {
            int n = width ? width : 1;
            for (int i = 0; i < n && *s; i++) field += *s++;
            if (!suppress) {
                u32 a = args.next32();
                std::memcpy(mem::host(a), field.data(), field.size());
                assigned++;
            }
            break;
        }
        case '[': {
            p++;
            bool neg = false;
            if (*p == '^') { neg = true; p++; }
            std::string set;
            if (*p == ']') set += *p++;
            while (*p && *p != ']') set += *p++;
            auto in_set = [&](char ch) {
                for (size_t i = 0; i < set.size(); i++) {
                    if (i + 2 < set.size() && set[i + 1] == '-') {
                        if (ch >= set[i] && ch <= set[i + 2]) return true;
                        i += 2;
                    } else if (set[i] == ch) {
                        return true;
                    }
                }
                return false;
            };
            take([&](char ch) { return in_set(ch) != neg; });
            if (field.empty()) return assigned;
            if (!suppress) {
                u32 a = args.next32();
                std::memcpy(mem::host(a), field.c_str(), field.size() + 1);
                assigned++;
            }
            break;
        }
        case 'n':
            if (!suppress) mem::w32(args.next32(), u32(s - in));
            break;
        case 'p': {
            char* end;
            unsigned long v = std::strtoul(s, &end, 16);
            s = end;
            if (!suppress) {
                mem::w32(args.next32(), u32(v));
                assigned++;
            }
            break;
        }
        default:
            return assigned;
        }
    }
    return assigned;
}

void register_printf() {
    reg("sprintf", [](Ctx& c) {
        ArgReader a = ArgReader::slots(c, 2);
        std::string s = format(c.argstr(1), a);
        c.ret(write_out(c.arg(0), 0xFFFFFFFF, s));
    });
    reg("snprintf", [](Ctx& c) {
        ArgReader a = ArgReader::slots(c, 3);
        std::string s = format(c.argstr(2), a);
        c.ret(write_out(c.arg(0), c.arg(1), s));
    });
    reg("vsprintf", [](Ctx& c) {
        ArgReader a = ArgReader::valist(c.arg(2));
        std::string s = format(c.argstr(1), a);
        c.ret(write_out(c.arg(0), 0xFFFFFFFF, s));
    });
    reg("vsnprintf", [](Ctx& c) {
        ArgReader a = ArgReader::valist(c.arg(3));
        std::string s = format(c.argstr(2), a);
        c.ret(write_out(c.arg(0), c.arg(1), s));
    });
    reg("fprintf", [](Ctx& c) {
        ArgReader a = ArgReader::slots(c, 2);
        std::string s = format(c.argstr(1), a);
        std::FILE* f = stdio_host_file(c.arg(0));
        if (f) std::fwrite(s.data(), 1, s.size(), f == stderr ? stdout : f);
        c.ret(u32(s.size()));
    });
    reg("sscanf", [](Ctx& c) {
        ArgReader a = ArgReader::slots(c, 2);
        c.ret(u32(scan(c.argstr(0), c.argstr(1), a)));
    });
    reg("vsscanf", [](Ctx& c) {
        ArgReader a = ArgReader::valist(c.arg(2));
        c.ret(u32(scan(c.argstr(0), c.argstr(1), a)));
    });
}

}  // namespace hle
