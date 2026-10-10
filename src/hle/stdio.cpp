// bionic stdio (FILE*) on top of host stdio, plus the guest->host path mapping.
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

#include "hle/hle.h"

namespace hle {

std::string g_assets_dir;
std::string g_data_dir;

// Every absolute guest path lives inside the sandbox root g_data_dir.
std::string host_path(const std::string& guest) {
    std::string p = guest;
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    // collapse "./" and duplicate slashes; refuse to climb out of the sandbox
    std::string out;
    size_t i = 0;
    std::vector<std::string> parts;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string part = p.substr(i, j - i);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        i = j + 1;
    }
    out = g_data_dir;
    for (auto& s : parts) out += "/" + s;
    return out;
}

namespace {

constexpr u32 FILE_SIZE = 84;
u32 g_sf = 0;  // __sF

std::mutex g_files_lock;
std::map<u32, std::FILE*> g_files;  // guest FILE* -> host FILE*

std::FILE* host_file(u32 gf) {
    if (gf == g_sf) return stdin;
    if (gf == g_sf + FILE_SIZE) return stdout;
    if (gf == g_sf + 2 * FILE_SIZE) return stderr;
    std::lock_guard lk(g_files_lock);
    auto it = g_files.find(gf);
    return it == g_files.end() ? nullptr : it->second;
}

}  // namespace

// used by printf.cpp for fprintf/vfprintf
std::FILE* stdio_host_file(u32 gf) { return host_file(gf); }

void register_stdio() {
    g_sf = resolve("__sF", false, false);

    reg("fopen", [](Ctx& c) {
        std::string gp = c.argstr(0);
        std::string mode = c.argstr(1);
        std::string hp = host_path(gp);
        if (mode.find('b') == std::string::npos) mode += 'b';
        if (mode[0] == 'w' || mode[0] == 'a') {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(hp).parent_path(), ec);
        }
        std::FILE* f = std::fopen(hp.c_str(), mode.c_str());
        LOGD("fopen(%s, %s) -> %s", gp.c_str(), c.argstr(1), f ? "ok" : "FAIL");
        if (!f) {
            set_errno(G_ENOENT);
            return c.ret(0);
        }
        u32 gf = mem::heap_alloc(FILE_SIZE);
        std::memset(mem::host(gf), 0, FILE_SIZE);
        mem::w16(gf + 14, 3);
        std::lock_guard lk(g_files_lock);
        g_files[gf] = f;
        c.ret(gf);
    });
    reg("fclose", [](Ctx& c) {
        u32 gf = c.arg(0);
        std::FILE* f = nullptr;
        {
            std::lock_guard lk(g_files_lock);
            auto it = g_files.find(gf);
            if (it != g_files.end()) {
                f = it->second;
                g_files.erase(it);
            }
        }
        if (!f) return c.ret(0xFFFFFFFF);
        std::fclose(f);
        mem::heap_free(gf);
        c.ret(0);
    });
    reg("fread", [](Ctx& c) {
        std::FILE* f = host_file(c.arg(3));
        c.ret(f ? u32(std::fread(mem::host(c.arg(0)), c.arg(1), c.arg(2), f)) : 0);
    });
    reg("fwrite", [](Ctx& c) {
        std::FILE* f = host_file(c.arg(3));
        c.ret(f ? u32(std::fwrite(mem::host(c.arg(0)), c.arg(1), c.arg(2), f)) : 0);
    });
    reg("fseek", [](Ctx& c) {
        std::FILE* f = host_file(c.arg(0));
        c.ret(f ? u32(std::fseek(f, long(s32(c.arg(1))), int(c.arg(2)))) : 0xFFFFFFFF);
    });
    reg("ftell", [](Ctx& c) {
        std::FILE* f = host_file(c.arg(0));
        c.ret(f ? u32(std::ftell(f)) : 0xFFFFFFFF);
    });
    reg("fflush", [](Ctx& c) {
        std::FILE* f = c.arg(0) ? host_file(c.arg(0)) : nullptr;
        c.ret(u32(std::fflush(f)));
    });
    reg("fgetc", [](Ctx& c) {
        std::FILE* f = host_file(c.arg(0));
        c.ret(f ? u32(std::fgetc(f)) : 0xFFFFFFFF);
    });
    reg("ungetc", [](Ctx& c) {
        std::FILE* f = host_file(c.arg(1));
        c.ret(f ? u32(std::ungetc(int(c.arg(0)), f)) : 0xFFFFFFFF);
    });
    reg("fgets", [](Ctx& c) {
        std::FILE* f = host_file(c.arg(2));
        if (!f) return c.ret(0);
        char* r = std::fgets(c.argp<char>(0), int(c.arg(1)), f);
        c.ret(r ? c.arg(0) : 0);
    });
    reg("fputs", [](Ctx& c) {
        std::FILE* f = host_file(c.arg(1));
        c.ret(f ? u32(std::fputs(c.argstr(0), f)) : 0xFFFFFFFF);
    });
    reg("fwide", [](Ctx& c) { c.ret(c.arg(1)); });
}

}  // namespace hle
