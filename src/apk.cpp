#include "apk.h"

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "log.h"

namespace apk {

namespace fs = std::filesystem;

namespace {

struct Entry {
    std::string name;
    uint16_t method;
    uint32_t csize, usize, local_off;
};

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

bool read_at(std::ifstream& f, uint64_t off, void* buf, size_t n) {
    f.seekg(std::streamoff(off));
    f.read(static_cast<char*>(buf), std::streamsize(n));
    return size_t(f.gcount()) == n;
}

bool list_entries(std::ifstream& f, std::vector<Entry>& out) {
    f.seekg(0, std::ios::end);
    uint64_t size = uint64_t(f.tellg());
    uint64_t tail = std::min<uint64_t>(size, 0x10000 + 22);
    std::vector<uint8_t> t(tail);
    if (!read_at(f, size - tail, t.data(), tail)) return false;
    int64_t eocd = -1;
    for (int64_t i = int64_t(tail) - 22; i >= 0; i--)
        if (rd32(&t[size_t(i)]) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd < 0) return false;
    uint32_t count = rd16(&t[size_t(eocd) + 10]);
    uint32_t cd_size = rd32(&t[size_t(eocd) + 12]), cd_off = rd32(&t[size_t(eocd) + 16]);
    std::vector<uint8_t> cd(cd_size);
    if (!read_at(f, cd_off, cd.data(), cd_size)) return false;
    size_t p = 0;
    for (uint32_t i = 0; i < count && p + 46 <= cd.size(); i++) {
        if (rd32(&cd[p]) != 0x02014b50) return false;
        Entry e;
        e.method = rd16(&cd[p + 10]);
        e.csize = rd32(&cd[p + 20]);
        e.usize = rd32(&cd[p + 24]);
        uint16_t nlen = rd16(&cd[p + 28]), xlen = rd16(&cd[p + 30]), clen = rd16(&cd[p + 32]);
        e.local_off = rd32(&cd[p + 42]);
        e.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), nlen);
        out.push_back(std::move(e));
        p += 46 + nlen + xlen + clen;
    }
    return true;
}

bool extract(std::ifstream& f, const Entry& e, std::vector<uint8_t>& data) {
    uint8_t lh[30];
    if (!read_at(f, e.local_off, lh, 30) || rd32(lh) != 0x04034b50) return false;
    uint64_t off = uint64_t(e.local_off) + 30 + rd16(lh + 26) + rd16(lh + 28);
    std::vector<uint8_t> comp(e.csize);
    if (!read_at(f, off, comp.data(), comp.size())) return false;
    if (e.method == 0) {
        data = std::move(comp);
        return true;
    }
    if (e.method != 8) return false;
    data.resize(e.usize);
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
    z.next_in = comp.data();
    z.avail_in = uInt(comp.size());
    z.next_out = data.data();
    z.avail_out = uInt(data.size());
    int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return r == Z_STREAM_END;
}

bool is_arm32_elf(const std::vector<uint8_t>& d) {
    return d.size() > 20 && std::memcmp(d.data(), "\x7f" "ELF", 4) == 0 && d[4] == 1 && rd16(&d[18]) == 40;
}

}  // namespace

bool import(const std::string& apk_path, const std::string& dest_dir) {
    std::ifstream f(fs::u8path(apk_path), std::ios::binary);
    if (!f) {
        LOGE("cannot open APK %s", apk_path.c_str());
        return false;
    }
    std::vector<Entry> entries;
    if (!list_entries(f, entries)) {
        LOGE("%s is not a valid APK/zip file", apk_path.c_str());
        return false;
    }
    // the game library: fan-fixed APKs keep the original ARM32 library in assets/guest_libs
    const char* lib_candidates[] = {"assets/guest_libs/libDeadSpace.so", "lib/armeabi/libDeadSpace.so",
                                    "lib/armeabi-v7a/libDeadSpace.so"};
    std::vector<uint8_t> lib;
    bool original = false;  // the library came from the APK's own lib/ folder: an unmodified APK
    for (const char* cand : lib_candidates)
        for (auto& e : entries)
            if (e.name == cand && lib.empty()) {
                std::vector<uint8_t> d;
                if (extract(f, e, d) && is_arm32_elf(d)) {
                    lib = std::move(d);
                    original = std::strncmp(cand, "lib/", 4) == 0;
                    LOGI("game library: %s", cand);
                }
            }
    if (lib.empty()) {
        LOGE("no 32-bit ARM libDeadSpace.so found in %s - is this the Dead Space APK?", apk_path.c_str());
        return false;
    }
    fs::path dest = fs::u8path(dest_dir);
    fs::create_directories(dest);
    size_t n = 0, bytes = 0;
    for (auto& e : entries) {
        if (e.name.rfind("assets/", 0) != 0 || e.name.back() == '/' || e.name.find("..") != std::string::npos)
            continue;
        if (e.name.rfind("assets/guest_libs/", 0) == 0) continue;
        std::vector<uint8_t> d;
        if (!extract(f, e, d)) {
            LOGE("failed to extract %s", e.name.c_str());
            return false;
        }
        fs::path out = dest / fs::u8path(e.name);
        fs::create_directories(out.parent_path());
        std::ofstream o(out, std::ios::binary);
        o.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
        n++;
        bytes += d.size();
    }
    std::ofstream o(dest / "libDeadSpace.so", std::ios::binary);
    o.write(reinterpret_cast<const char*>(lib.data()), std::streamsize(lib.size()));
    // An unmodified APK has ATC textures; the "fan-fixed" one (which keeps the ARM library under
    // assets/guest_libs) re-encoded them as ETC2 under the same labels. The game reads this note at start.
    std::error_code ec;
    if (original) std::ofstream(dest / "atc_textures") << "The textures in this folder are real ATC (imported from an unmodified APK)." << std::endl;
    else fs::remove(dest / "atc_textures", ec);
    LOGI("imported %zu asset files (%.1f MB) into %s", n, bytes / 1048576.0, dest_dir.c_str());
    return true;
}

}  // namespace apk
