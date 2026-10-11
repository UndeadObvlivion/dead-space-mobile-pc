#include "texpack.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <unordered_map>

#include "log.h"

namespace fs = std::filesystem;

namespace texpack {

namespace {

bool g_enabled = false, g_dump = false;
std::string g_dump_dir;
std::map<std::string, std::string> g_files;        // "hhhhhhhh", "hhhhhhhh_n", ... -> path
std::unordered_map<unsigned, u32> g_hash_of;       // GL texture -> hash of what the game uploaded
std::unordered_map<u32, Image> g_colour;           // loaded replacement colour images
std::unordered_map<u32, char> g_dumped;

const char* kReadme =
    "TEXTURE PACKS\r\n"
    "=============\r\n"
    "\r\n"
    "Put each pack in its own folder here:   mods/textures/<pack name>/\r\n"
    "Sub-folders inside a pack are fine. Packs are read in name order; if two packs hold the same\r\n"
    "file, the later one wins. A folder whose name starts with an underscore is ignored.\r\n"
    "Turn packs on or off with Options > Graphics > Texture Packs (takes effect at the next start).\r\n"
    "\r\n"
    "FILE NAMES\r\n"
    "Every texture of the game has an 8-digit id (a hash of its original pixels). For a texture\r\n"
    "with the id 1a2b3c4d a pack contains:\r\n"
    "\r\n"
    "  1a2b3c4d.png      colour texture (replaces the game's; any size, same layout)\r\n"
    "\r\n"
    "Textures a pack does not contain stay the game's own.\r\n"
    "\r\n"
    "PNG only: 8 bits per channel, not interlaced (greyscale, RGB, RGBA or palette).\r\n"
    "Keep a replacement's layout exactly as the original: the game's models map onto it.\r\n"
    "\r\n"
    "FINDING THE IDS\r\n"
    "Start the game once with   deadspace.exe --dump-textures-png   (or put texture_dump=1 in\r\n"
    "userdata/port_settings.ini) and play through the places you want to change. Every texture\r\n"
    "the game loads is written to mods/textures/_dump/<id>.png. Copy the ones you want into your\r\n"
    "pack folder, edit them, and keep the file name.\r\n";

std::string hex8(u32 h) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", h);
    return b;
}

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }

}  // namespace

u32 hash(const u8* rgba, int w, int h) {
    u32 x = 2166136261u;
    auto mix = [&](u8 b) { x = (x ^ b) * 16777619u; };
    for (int k = 0; k < 4; k++) mix(u8(u32(w) >> (8 * k))), mix(u8(u32(h) >> (8 * k)));
    const size_t n = size_t(w) * size_t(h) * 4;
    for (size_t i = 0; i < n; i++) mix(rgba[i]);
    return x ? x : 1;
}

bool load_png(const std::string& path, Image& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<u8> d;
    u8 buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    if (d.size() < 33 || std::memcmp(d.data(), sig, 8) != 0) return false;
    int w = 0, h = 0, depth = 0, type = 0, interlace = 0;
    std::vector<u8> idat, pal, trns;
    for (size_t pos = 8; pos + 12 <= d.size();) {
        u32 len = be32(&d[pos]);
        if (pos + 12 + len > d.size()) return false;
        const u8* c = &d[pos + 8];
        if (!std::memcmp(&d[pos + 4], "IHDR", 4) && len >= 13) {
            w = int(be32(c)), h = int(be32(c + 4)), depth = c[8], type = c[9], interlace = c[12];
        } else if (!std::memcmp(&d[pos + 4], "IDAT", 4)) {
            idat.insert(idat.end(), c, c + len);
        } else if (!std::memcmp(&d[pos + 4], "PLTE", 4)) {
            pal.assign(c, c + len);
        } else if (!std::memcmp(&d[pos + 4], "tRNS", 4)) {
            trns.assign(c, c + len);
        }
        pos += 12 + len;
    }
    const int ch = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : type == 6 ? 4 : 0;
    if (w < 1 || h < 1 || w > 8192 || h > 8192 || depth != 8 || !ch || interlace) {
        LOGW("texture pack: %s is not an 8-bit, non-interlaced PNG (skipped)", path.c_str());
        return false;
    }
    const size_t stride = size_t(w) * size_t(ch);
    std::vector<u8> raw((stride + 1) * size_t(h));
    uLongf rl = uLongf(raw.size());
    if (uncompress(raw.data(), &rl, idat.data(), uLong(idat.size())) != Z_OK || rl != raw.size()) return false;
    std::vector<u8> img(stride * size_t(h));
    for (int y = 0; y < h; y++) {
        const u8* in = &raw[(stride + 1) * size_t(y)];
        u8* row = &img[stride * size_t(y)];
        const u8* up = y ? row - stride : nullptr;
        const int ft = in[0];
        for (size_t x = 0; x < stride; x++) {
            int a = x >= size_t(ch) ? row[x - size_t(ch)] : 0, b = up ? up[x] : 0, c2 = up && x >= size_t(ch) ? up[x - size_t(ch)] : 0, v = in[1 + x];
            if (ft == 1) v += a;
            else if (ft == 2) v += b;
            else if (ft == 3) v += (a + b) / 2;
            else if (ft == 4) {
                int p = a + b - c2, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c2);
                v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c2;
            }
            row[x] = u8(v);
        }
    }
    out.w = w, out.h = h;
    out.px.resize(size_t(w) * size_t(h) * 4);
    for (size_t i = 0; i < size_t(w) * size_t(h); i++) {
        const u8* s = &img[i * size_t(ch)];
        u8* o = &out.px[i * 4];
        if (type == 0) o[0] = o[1] = o[2] = s[0], o[3] = 255;
        else if (type == 2) o[0] = s[0], o[1] = s[1], o[2] = s[2], o[3] = 255;
        else if (type == 4) o[0] = o[1] = o[2] = s[0], o[3] = s[1];
        else if (type == 6) std::memcpy(o, s, 4);
        else {  // palette
            const size_t k = size_t(s[0]) * 3;
            o[0] = k + 2 < pal.size() ? pal[k] : 0, o[1] = k + 2 < pal.size() ? pal[k + 1] : 0, o[2] = k + 2 < pal.size() ? pal[k + 2] : 0;
            o[3] = s[0] < trns.size() ? trns[s[0]] : 255;
        }
    }
    return true;
}

bool save_png(const std::string& path, const u8* rgba, int w, int h) {
    std::vector<u8> raw(size_t(h) * (size_t(w) * 4 + 1));
    for (int y = 0; y < h; y++) {
        u8* row = &raw[size_t(y) * (size_t(w) * 4 + 1)];
        row[0] = 0;
        std::memcpy(row + 1, rgba + size_t(y) * size_t(w) * 4, size_t(w) * 4);
    }
    uLongf zlen = compressBound(uLong(raw.size()));
    std::vector<u8> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), uLong(raw.size()), 6) != Z_OK) return false;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto put32 = [](u8* p, u32 v) { p[0] = u8(v >> 24), p[1] = u8(v >> 16), p[2] = u8(v >> 8), p[3] = u8(v); };
    auto chunk = [&](const char* type, const u8* data, u32 len) {
        u8 hdr[8], c[4];
        put32(hdr, len);
        std::memcpy(hdr + 4, type, 4);
        std::fwrite(hdr, 1, 8, f);
        if (len) std::fwrite(data, 1, len, f);
        uLong crc = crc32(0, hdr + 4, 4);
        if (len) crc = crc32(crc, data, len);
        put32(c, u32(crc));
        std::fwrite(c, 1, 4, f);
    };
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    std::fwrite(sig, 1, 8, f);
    u8 ihdr[13];
    put32(ihdr, u32(w));
    put32(ihdr + 4, u32(h));
    ihdr[8] = 8, ihdr[9] = 6, ihdr[10] = ihdr[11] = ihdr[12] = 0;  // 8-bit RGBA
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), u32(zlen));
    chunk("IEND", nullptr, 0);
    std::fclose(f);
    return true;
}

void init(const std::string& base, bool packs_enabled, bool dump) {
    g_enabled = packs_enabled;
    const std::string root = base + "mods/textures";
    std::error_code ec;
    fs::create_directories(root, ec);
    g_dump = dump;
    g_dump_dir = root + "/_dump";
    if (g_dump) fs::create_directories(g_dump_dir, ec);
    {  // a note for pack makers, written once
        const std::string readme = root + "/README.txt";
        if (!fs::exists(readme, ec))
            if (std::FILE* f = std::fopen(readme.c_str(), "wb")) {
                std::fputs(kReadme, f);
                std::fclose(f);
            }
    }
    g_files.clear();
    if (g_enabled) {
        std::vector<fs::path> packs;
        for (auto& e : fs::directory_iterator(root, ec))
            if (e.is_directory() && e.path().filename().string().rfind("_", 0) != 0) packs.push_back(e.path());
        std::sort(packs.begin(), packs.end());
        for (auto& p : packs)
            for (auto& e : fs::recursive_directory_iterator(p, ec)) {
                if (!e.is_regular_file()) continue;
                std::string ext = e.path().extension().string(), stem = e.path().stem().string();
                for (char& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                for (char& ch : stem) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                if (ext == ".png" && stem.size() >= 8) g_files[stem] = e.path().string();
            }
    }
    LOGI("texture packs: %s, %zu files in %s%s", g_enabled ? "on" : "off", g_files.size(), root.c_str(), g_dump ? " (dumping the game's textures to _dump)" : "");
}

bool packs_enabled() { return g_enabled; }
int pack_files() { return int(g_files.size()); }

const Image* on_upload(unsigned tex, const u8* rgba, int w, int h) {
    if (w < 1 || h < 1) return nullptr;
    const u32 hs = hash(rgba, w, h);
    g_hash_of[tex] = hs;
    if (g_dump && w >= 8 && h >= 8 && !g_dumped.count(hs)) {
        g_dumped[hs] = 1;
        const std::string path = g_dump_dir + "/" + hex8(hs) + ".png";
        std::error_code ec;
        if (!fs::exists(path, ec)) save_png(path, rgba, w, h);
    }
    if (!g_enabled) return nullptr;
    auto it = g_colour.find(hs);
    if (it != g_colour.end()) return it->second.w ? &it->second : nullptr;
    Image img;
    auto f = g_files.find(hex8(hs));
    if (f != g_files.end() && load_png(f->second, img)) LOGI("texture pack: %s replaces a %dx%d texture (%dx%d)", f->second.c_str(), w, h, img.w, img.h);
    Image& kept = g_colour[hs] = std::move(img);  // (an empty one remembers that there is none)
    return kept.w ? &kept : nullptr;
}

void on_delete(unsigned tex) { g_hash_of.erase(tex); }

u32 hash_of(unsigned tex) {
    auto it = g_hash_of.find(tex);
    return it == g_hash_of.end() ? 0 : it->second;
}

}  // namespace texpack
