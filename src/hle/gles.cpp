// OpenGL ES 1.1 (libGLESv1_CM) on top of desktop OpenGL (compatibility profile).
#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "hle/gles.h"
#include "texpack.h"
#include "hle/hle.h"
#include "render/native.h"

namespace hle {

namespace {

using s8 = signed char;

// ---------------------------------------------------------------- GLES-only enums
constexpr GLenum GL_FIXED_ = 0x140C;
#define DEF_ENUM(name, value) \
    constexpr GLenum k_##name = value;
DEF_ENUM(TEXTURE_CROP_RECT_OES, 0x8B9D)
DEF_ENUM(POINT_SIZE_ARRAY_OES, 0x8B9C)
DEF_ENUM(MATRIX_PALETTE_OES, 0x8840)
DEF_ENUM(MATRIX_INDEX_ARRAY_OES, 0x8844)
DEF_ENUM(WEIGHT_ARRAY_OES, 0x86AD)
DEF_ENUM(ETC1_RGB8_OES, 0x8D64)
DEF_ENUM(PALETTE4_RGB8_OES, 0x8B90)
DEF_ENUM(PALETTE8_RGB5_A1_OES, 0x8B99)
DEF_ENUM(RGB565_OES, 0x8D62)
DEF_ENUM(IMPLEMENTATION_COLOR_READ_TYPE_OES, 0x8B9A)
DEF_ENUM(IMPLEMENTATION_COLOR_READ_FORMAT_OES, 0x8B9B)
#undef DEF_ENUM

// GL 1.3 entry points (not exported by every platform's opengl library)
typedef void(APIENTRY* PFN_ClientActiveTexture)(GLenum);
typedef void(APIENTRY* PFN_MultiTexCoord4f)(GLenum, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(APIENTRY* PFN_MultiTexCoord2f)(GLenum, GLfloat, GLfloat);

// ---------------------------------------------------------------- host entry points beyond GL 1.1
PFNGLACTIVETEXTUREPROC p_glActiveTexture;
PFN_ClientActiveTexture p_glClientActiveTexture;
PFN_MultiTexCoord4f p_glMultiTexCoord4f;
PFN_MultiTexCoord2f p_glMultiTexCoord2f;
PFNGLBINDBUFFERPROC p_glBindBuffer;
PFNGLBUFFERDATAPROC p_glBufferData;
PFNGLBUFFERSUBDATAPROC p_glBufferSubData;
PFNGLDELETEBUFFERSPROC p_glDeleteBuffers;
PFNGLGENBUFFERSPROC p_glGenBuffers;
PFNGLISBUFFERPROC p_glIsBuffer;
PFNGLGETBUFFERPARAMETERIVPROC p_glGetBufferParameteriv;
PFNGLCOMPRESSEDTEXIMAGE2DPROC p_glCompressedTexImage2D;
PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC p_glCompressedTexSubImage2D;
PFNGLSAMPLECOVERAGEPROC p_glSampleCoverage;
PFNGLBLENDEQUATIONPROC p_glBlendEquation;
PFNGLBLENDEQUATIONSEPARATEPROC p_glBlendEquationSeparate;
PFNGLBLENDFUNCSEPARATEPROC p_glBlendFuncSeparate;
PFNGLPOINTPARAMETERFPROC p_glPointParameterf;
PFNGLPOINTPARAMETERFVPROC p_glPointParameterfv;
PFNGLGENFRAMEBUFFERSPROC p_glGenFramebuffers;
PFNGLDELETEFRAMEBUFFERSPROC p_glDeleteFramebuffers;
PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer;
PFNGLISFRAMEBUFFERPROC p_glIsFramebuffer;
PFNGLCHECKFRAMEBUFFERSTATUSPROC p_glCheckFramebufferStatus;
PFNGLFRAMEBUFFERTEXTURE2DPROC p_glFramebufferTexture2D;
PFNGLFRAMEBUFFERRENDERBUFFERPROC p_glFramebufferRenderbuffer;
PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC p_glGetFramebufferAttachmentParameteriv;
PFNGLGENRENDERBUFFERSPROC p_glGenRenderbuffers;
PFNGLDELETERENDERBUFFERSPROC p_glDeleteRenderbuffers;
PFNGLBINDRENDERBUFFERPROC p_glBindRenderbuffer;
PFNGLISRENDERBUFFERPROC p_glIsRenderbuffer;
PFNGLRENDERBUFFERSTORAGEPROC p_glRenderbufferStorage;
PFNGLGETRENDERBUFFERPARAMETERIVPROC p_glGetRenderbufferParameteriv;
PFNGLGENERATEMIPMAPPROC p_glGenerateMipmap;

template <class T> void load(T& fp, const char* name) {
    fp = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    if (!fp) LOGW("GL entry point %s missing", name);
}

// ---------------------------------------------------------------- tracked state
struct Array {
    bool enabled = false;
    GLint size = 4;
    GLenum type = GL_FLOAT;
    GLsizei stride = 0;
    u32 ptr = 0;     // guest pointer or buffer offset
    GLuint buffer = 0;
};
enum { A_VERTEX, A_NORMAL, A_COLOR, A_TEX0, A_TEX1, A_TEX2, A_TEX3, A_POINTSIZE, A_MATIDX, A_WEIGHT, A_COUNT };
Array g_arr[A_COUNT];
int g_client_unit = 0;
int g_active_unit = 0;
GLuint g_array_buffer = 0, g_element_buffer = 0;
GLuint g_bound_tex[4] = {};

struct TexInfo {
    bool replaced = false;  // its image comes from a texture pack (the game's own mip levels are not used)
    int w = 0, h = 0;
    int crop[4] = {0, 0, 0, 0};
};
std::unordered_map<GLuint, TexInfo> g_tex;
std::unordered_map<GLuint, std::vector<u8>> g_buf_shadow;
std::unordered_map<GLuint, u32> g_buf_mapped;  // buffer -> guest mapping

std::vector<u8> g_conv[A_COUNT];
std::set<std::string> g_warned;

void warn_once(const std::string& s) {
    if (g_warned.insert(s).second) LOGW("GLES: %s", s.c_str());
}

float fx(u32 v) { return float(s32(v)) / 65536.0f; }

const void* hp(u32 a) { return a ? mem::host(a) : nullptr; }

int type_size(GLenum t) {
    switch (t) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT: return 2;
    default: return 4;
    }
}

bool native_ok(int kind, GLenum t) {
    if (t == GL_FIXED_) return false;
    if ((kind == A_VERTEX || kind >= A_TEX0) && (t == GL_BYTE || t == GL_UNSIGNED_BYTE)) return false;
    return true;
}

const u8* array_source(const Array& a) {
    if (a.buffer) {
        auto it = g_buf_shadow.find(a.buffer);
        if (it == g_buf_shadow.end() || a.ptr >= it->second.size()) return nullptr;
        return it->second.data() + a.ptr;
    }
    return a.ptr ? mem::host(a.ptr) : nullptr;
}

// Converts vertices [first, first+count) of an array to floats; returns host pointer that is
// valid for vertex index i at (ret + i * size * 4).
const float* convert_array(int kind, const Array& a, u32 first, u32 count) {
    const u8* src = array_source(a);
    if (!src) return nullptr;
    int stride = a.stride ? a.stride : a.size * type_size(a.type);
    auto& buf = g_conv[kind];
    buf.resize(size_t(first + count) * a.size * 4);
    float* out = reinterpret_cast<float*>(buf.data());
    for (u32 i = first; i < first + count; i++) {
        const u8* e = src + size_t(i) * stride;
        for (int k = 0; k < a.size; k++) {
            float v;
            switch (a.type) {
            case GL_FIXED_: { s32 x; std::memcpy(&x, e + 4 * k, 4); v = x / 65536.0f; break; }
            case GL_BYTE: v = float(s8(e[k])); break;
            case GL_UNSIGNED_BYTE: v = float(e[k]); break;
            case GL_SHORT: { int16_t x; std::memcpy(&x, e + 2 * k, 2); v = x; break; }
            default: std::memcpy(&v, e + 4 * k, 4); break;
            }
            out[size_t(i) * a.size + k] = v;
        }
    }
    return out;
}

void bind_arrays(u32 first, u32 count) {
    for (int k = 0; k < A_COUNT; k++) {
        if (k == A_POINTSIZE || k == A_MATIDX || k == A_WEIGHT) continue;
        Array& a = g_arr[k];
        if (k >= A_TEX0) {
            p_glClientActiveTexture(GL_TEXTURE0 + (k - A_TEX0));
        }
        GLenum cap = k == A_VERTEX ? GL_VERTEX_ARRAY : k == A_NORMAL ? GL_NORMAL_ARRAY
                   : k == A_COLOR ? GL_COLOR_ARRAY : GL_TEXTURE_COORD_ARRAY;
        if (!a.enabled) {
            glDisableClientState(cap);
            continue;
        }
        glEnableClientState(cap);
        const void* p;
        GLenum type = a.type;
        GLsizei stride = a.stride;
        if (native_ok(k, a.type)) {
            p_glBindBuffer(GL_ARRAY_BUFFER, a.buffer);
            p = a.buffer ? reinterpret_cast<const void*>(uintptr_t(a.ptr)) : hp(a.ptr);
        } else {
            p_glBindBuffer(GL_ARRAY_BUFFER, 0);
            p = convert_array(k, a, first, count);
            type = GL_FLOAT;
            stride = 0;
        }
        switch (k) {
        case A_VERTEX: glVertexPointer(a.size, type, stride, p); break;
        case A_NORMAL: glNormalPointer(type, stride, p); break;
        case A_COLOR: glColorPointer(a.size, type, stride, p); break;
        default: glTexCoordPointer(a.size, type, stride, p); break;
        }
    }
    p_glClientActiveTexture(GL_TEXTURE0 + g_client_unit);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_array_buffer);
}

// Mirrors the current vertex arrays into the native renderer (host pointers resolved from guest
// memory or the buffer shadows).
void sync_native_arrays() {
    const int map[] = {A_VERTEX, A_NORMAL, A_COLOR, A_TEX0, A_TEX1};
    for (int i = 0; i < 5; i++) {
        const Array& a = g_arr[map[i]];
        render::Array r;
        r.enabled = a.enabled;
        r.size = a.size;
        r.type = a.type;
        r.stride = a.stride;
        r.data = a.enabled ? array_source(a) : nullptr;
        render::set_array(i, r);
    }
}

bool any_conversion() {
    for (int k = 0; k < A_COUNT; k++)
        if (g_arr[k].enabled && k != A_POINTSIZE && k != A_MATIDX && k != A_WEIGHT && !native_ok(k, g_arr[k].type))
            return true;
    return false;
}

int array_index_for(GLenum cap) {
    switch (cap) {
    case GL_VERTEX_ARRAY: return A_VERTEX;
    case GL_NORMAL_ARRAY: return A_NORMAL;
    case GL_COLOR_ARRAY: return A_COLOR;
    case GL_TEXTURE_COORD_ARRAY: return A_TEX0 + g_client_unit;
    case k_POINT_SIZE_ARRAY_OES: return A_POINTSIZE;
    case k_MATRIX_INDEX_ARRAY_OES: return A_MATIDX;
    case k_WEIGHT_ARRAY_OES: return A_WEIGHT;
    default: return -1;
    }
}

void set_array(int k, GLint size, GLenum type, GLsizei stride, u32 ptr) {
    Array& a = g_arr[k];
    a.size = size;
    a.type = type;
    a.stride = stride;
    a.ptr = ptr;
    a.buffer = g_array_buffer;
}

// ---------------------------------------------------------------- texture decoding
// ETC2 RGB block (superset of ETC1: adds T, H and planar modes). out: 16 x RGBA, row-major.
void decode_etc2_color(const u8* b, u8* out) {
    static const int etc1_tbl[8][4] = {{2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
                                       {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106},
                                       {47, 183, -47, -183}};
    static const int dist[8] = {3, 6, 11, 16, 23, 32, 41, 64};
    u32 hi = (u32(b[0]) << 24) | (u32(b[1]) << 16) | (u32(b[2]) << 8) | b[3];
    u32 lo = (u32(b[4]) << 24) | (u32(b[5]) << 16) | (u32(b[6]) << 8) | b[7];
    auto cl = [](int v) { return v < 0 ? 0 : v > 255 ? 255 : v; };
    auto idx = [&](int x, int y) {
        int i = x * 4 + y;
        return int(((lo >> i) & 1) | (((lo >> (i + 16)) & 1) << 1));
    };
    auto put = [&](int x, int y, int r, int g, int bl) {
        u8* p = out + (y * 4 + x) * 4;
        p[0] = u8(cl(r));
        p[1] = u8(cl(g));
        p[2] = u8(cl(bl));
        p[3] = 255;
    };
    bool diff = (hi >> 1) & 1, flip = hi & 1;
    int base[2][3];
    if (diff) {
        int r = (hi >> 27) & 31, g = (hi >> 19) & 31, bl = (hi >> 11) & 31;
        auto s3 = [](int v) { return v & 4 ? v - 8 : v; };
        int r2 = r + s3((hi >> 24) & 7), g2 = g + s3((hi >> 16) & 7), b2 = bl + s3((hi >> 8) & 7);
        if (r2 < 0 || r2 > 31) {  // T mode
            int R1 = int((((hi >> 27) & 3) << 2) | ((hi >> 24) & 3)), G1 = (hi >> 20) & 15, B1 = (hi >> 16) & 15;
            int R2 = (hi >> 12) & 15, G2 = (hi >> 8) & 15, B2 = (hi >> 4) & 15;
            int d = dist[(((hi >> 2) & 3) << 1) | (hi & 1)];
            int p[4][3] = {{R1 * 17, G1 * 17, B1 * 17}, {R2 * 17 + d, G2 * 17 + d, B2 * 17 + d},
                           {R2 * 17, G2 * 17, B2 * 17}, {R2 * 17 - d, G2 * 17 - d, B2 * 17 - d}};
            for (int x = 0; x < 4; x++)
                for (int y = 0; y < 4; y++) {
                    int* c = p[idx(x, y)];
                    put(x, y, c[0], c[1], c[2]);
                }
            return;
        }
        if (g2 < 0 || g2 > 31) {  // H mode
            int R1 = (hi >> 27) & 15, G1 = int((((hi >> 24) & 7) << 1) | ((hi >> 20) & 1));
            int B1 = int((((hi >> 19) & 1) << 3) | ((hi >> 15) & 7));
            int R2 = (hi >> 11) & 15, G2 = (hi >> 7) & 15, B2 = (hi >> 3) & 15;
            int o1 = (R1 << 8) | (G1 << 4) | B1, o2 = (R2 << 8) | (G2 << 4) | B2;
            int d = dist[(((hi >> 2) & 1) << 2) | ((hi & 1) << 1) | (o1 >= o2 ? 1 : 0)];
            int p[4][3] = {{R1 * 17 + d, G1 * 17 + d, B1 * 17 + d}, {R1 * 17 - d, G1 * 17 - d, B1 * 17 - d},
                           {R2 * 17 + d, G2 * 17 + d, B2 * 17 + d}, {R2 * 17 - d, G2 * 17 - d, B2 * 17 - d}};
            for (int x = 0; x < 4; x++)
                for (int y = 0; y < 4; y++) {
                    int* c = p[idx(x, y)];
                    put(x, y, c[0], c[1], c[2]);
                }
            return;
        }
        if (b2 < 0 || b2 > 31) {  // planar
            int R0 = (hi >> 25) & 63, G0 = int((((hi >> 24) & 1) << 6) | ((hi >> 17) & 63));
            int B0 = int((((hi >> 16) & 1) << 5) | (((hi >> 11) & 3) << 3) | ((hi >> 7) & 7));
            int RH = int((((hi >> 2) & 31) << 1) | (hi & 1)), GH = (lo >> 25) & 127, BH = (lo >> 19) & 63;
            int RV = (lo >> 13) & 63, GV = (lo >> 6) & 127, BV = lo & 63;
            auto e6 = [](int v) { return (v << 2) | (v >> 4); };
            auto e7 = [](int v) { return (v << 1) | (v >> 6); };
            int O[3] = {e6(R0), e7(G0), e6(B0)}, H[3] = {e6(RH), e7(GH), e6(BH)}, V[3] = {e6(RV), e7(GV), e6(BV)};
            for (int x = 0; x < 4; x++)
                for (int y = 0; y < 4; y++) {
                    int c[3];
                    for (int k = 0; k < 3; k++) c[k] = (x * (H[k] - O[k]) + y * (V[k] - O[k]) + 4 * O[k] + 2) >> 2;
                    put(x, y, c[0], c[1], c[2]);
                }
            return;
        }
        auto e5 = [](int v) { return (v << 3) | (v >> 2); };
        base[0][0] = e5(r); base[0][1] = e5(g); base[0][2] = e5(bl);
        base[1][0] = e5(r2); base[1][1] = e5(g2); base[1][2] = e5(b2);
    } else {
        base[0][0] = ((hi >> 28) & 15) * 17; base[1][0] = ((hi >> 24) & 15) * 17;
        base[0][1] = ((hi >> 20) & 15) * 17; base[1][1] = ((hi >> 16) & 15) * 17;
        base[0][2] = ((hi >> 12) & 15) * 17; base[1][2] = ((hi >> 8) & 15) * 17;
    }
    int t[2] = {int((hi >> 5) & 7), int((hi >> 2) & 7)};
    for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
            int sub = flip ? (y >= 2) : (x >= 2);
            int m = etc1_tbl[t[sub]][idx(x, y)];
            put(x, y, base[sub][0] + m, base[sub][1] + m, base[sub][2] + m);
        }
}

// EAC alpha block (as used by ETC2 RGBA8)
void decode_eac_alpha(const u8* b, u8* out) {
    static const int tbl[16][8] = {
        {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
        {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},
        {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10}, {-2, -6, -8, -10, 1, 5, 7, 9},
        {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},  {-2, -5, -7, -10, 1, 4, 6, 9},
        {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},  {-4, -6, -8, -9, 3, 5, 7, 8},
        {-3, -5, -7, -9, 2, 4, 6, 8}};
    int base = b[0], mult = b[1] >> 4;
    const int* t = tbl[b[1] & 15];
    u64 bits = 0;
    for (int i = 0; i < 6; i++) bits = (bits << 8) | b[2 + i];
    for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
            int i = x * 4 + y;
            int v = base + t[(bits >> (45 - 3 * i)) & 7] * mult;
            out[(y * 4 + x) * 4 + 3] = u8(v < 0 ? 0 : v > 255 ? 255 : v);
        }
}

// rgba: 16-byte EAC+ETC2 blocks; otherwise 8-byte ETC2 (or ETC1) color blocks.
std::vector<u8> decode_etc2(const u8* src, int w, int h, bool rgba) {
    std::vector<u8> img(size_t(w) * h * 4);
    int bw = (w + 3) / 4, bh = (h + 3) / 4, bsize = rgba ? 16 : 8;
    u8 blk[64];
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            const u8* b = src + (size_t(by) * bw + bx) * bsize;
            decode_etc2_color(b + (rgba ? 8 : 0), blk);
            if (rgba) decode_eac_alpha(b, blk);
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    int px = bx * 4 + x, py = by * 4 + y;
                    if (px < w && py < h) std::memcpy(&img[(size_t(py) * w + px) * 4], &blk[(y * 4 + x) * 4], 4);
                }
        }
    return img;
}

// AMD/Qualcomm ATC (ATITC). Color block: c0 = method bit + RGB555, c1 = RGB565, 2-bit indices.
void decode_atc_color(const u8* b, u8* out /* 16 x RGBA */) {
    u32 c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    auto e5 = [](u32 v) { return int((v << 3) | (v >> 2)); };
    auto e6 = [](u32 v) { return int((v << 2) | (v >> 4)); };
    int col[4][3];
    int a[3] = {e5((c0 >> 10) & 31), e5((c0 >> 5) & 31), e5(c0 & 31)};
    int z[3] = {e5((c1 >> 11) & 31), e6((c1 >> 5) & 63), e5(c1 & 31)};
    for (int k = 0; k < 3; k++) {
        if (!(c0 & 0x8000)) {
            col[0][k] = a[k];
            col[1][k] = (5 * a[k] + 3 * z[k]) / 8;
            col[2][k] = (3 * a[k] + 5 * z[k]) / 8;
            col[3][k] = z[k];
        } else {
            col[0][k] = 0;
            col[1][k] = std::max(0, a[k] - z[k] / 4);
            col[2][k] = a[k];
            col[3][k] = z[k];
        }
    }
    u32 idx = b[4] | (b[5] << 8) | (b[6] << 16) | (u32(b[7]) << 24);
    for (int i = 0; i < 16; i++) {
        int s = (idx >> (2 * i)) & 3;
        out[i * 4 + 0] = u8(col[s][0]);
        out[i * 4 + 1] = u8(col[s][1]);
        out[i * 4 + 2] = u8(col[s][2]);
        out[i * 4 + 3] = 255;
    }
}

// DXT5-style interpolated alpha block
void decode_interp_alpha(const u8* b, u8* out) {
    int a0 = b[0], a1 = b[1];
    int pal[8] = {a0, a1};
    if (a0 > a1)
        for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    else {
        for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    u64 bits = 0;
    for (int i = 0; i < 6; i++) bits |= u64(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) out[i * 4 + 3] = u8(pal[(bits >> (3 * i)) & 7]);
}

constexpr GLenum k_ATC_RGB = 0x8C92, k_ATC_RGBA_EXPLICIT = 0x8C93, k_ATC_RGBA_INTERP = 0x87EE;

std::vector<u8> decode_atc(GLenum fmt, const u8* src, int w, int h) {
    std::vector<u8> img(size_t(w) * h * 4);
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    int bsize = fmt == k_ATC_RGB ? 8 : 16;
    u8 blk[64];
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            const u8* b = src + (size_t(by) * bw + bx) * bsize;
            decode_atc_color(b + (bsize - 8), blk);
            if (fmt == k_ATC_RGBA_EXPLICIT)
                for (int i = 0; i < 16; i++) blk[i * 4 + 3] = u8(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
            else if (fmt == k_ATC_RGBA_INTERP)
                decode_interp_alpha(b, blk);
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    int px = bx * 4 + x, py = by * 4 + y;
                    if (px < w && py < h) std::memcpy(&img[(size_t(py) * w + px) * 4], &blk[(y * 4 + x) * 4], 4);
                }
        }
    return img;
}

// GLES1 core paletted textures; returns RGBA8 for every mip level packed in the blob.
std::vector<std::vector<u8>> decode_palette(GLenum fmt, const u8* src, int w, int h, int levels) {
    int idx_bits = fmt <= 0x8B94 ? 4 : 8;
    int f = (fmt - k_PALETTE4_RGB8_OES) % 5;  // 0 RGB8, 1 RGBA8, 2 R5G6B5, 3 RGBA4, 4 RGB5_A1
    int entry = f == 0 ? 3 : f == 1 ? 4 : 2;
    int pal_n = 1 << idx_bits;
    const u8* pal = src;
    const u8* idx = src + pal_n * entry;
    auto color = [&](int i, u8* o) {
        const u8* e = pal + i * entry;
        u16 v = u16(e[0] | (e[1] << 8));
        switch (f) {
        case 0: o[0] = e[0]; o[1] = e[1]; o[2] = e[2]; o[3] = 255; break;
        case 1: std::memcpy(o, e, 4); break;
        case 2: o[0] = u8((v >> 11) * 255 / 31); o[1] = u8(((v >> 5) & 63) * 255 / 63); o[2] = u8((v & 31) * 255 / 31); o[3] = 255; break;
        case 3: o[0] = u8((v >> 12) * 17); o[1] = u8(((v >> 8) & 15) * 17); o[2] = u8(((v >> 4) & 15) * 17); o[3] = u8((v & 15) * 17); break;
        default: o[0] = u8((v >> 11) * 255 / 31); o[1] = u8(((v >> 6) & 31) * 255 / 31); o[2] = u8(((v >> 1) & 31) * 255 / 31); o[3] = (v & 1) ? 255 : 0; break;
        }
    };
    std::vector<std::vector<u8>> out;
    size_t bitpos = 0;
    for (int l = 0; l < levels; l++) {
        int lw = std::max(1, w >> l), lh = std::max(1, h >> l);
        std::vector<u8> img(size_t(lw) * lh * 4);
        for (int i = 0; i < lw * lh; i++) {
            int v;
            if (idx_bits == 8) v = idx[bitpos / 8];
            else v = (idx[bitpos / 8] >> ((bitpos & 4) ? 0 : 4)) & 15;
            bitpos += idx_bits;
            color(v, &img[size_t(i) * 4]);
        }
        bitpos = (bitpos + 7) & ~size_t(7);
        out.push_back(std::move(img));
    }
    return out;
}

// ---------------------------------------------------------------- strings
u32 g_str_vendor, g_str_renderer, g_str_version, g_str_ext;

}  // namespace

// ---------------------------------------------------------------- draw texture (OES_draw_texture)
static void draw_tex(float x, float y, float z, float w, float h) {
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(vp[0], vp[0] + vp[2], vp[1], vp[1] + vp[3], -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    GLboolean lighting = glIsEnabled(GL_LIGHTING);
    if (lighting) glDisable(GL_LIGHTING);
    z = std::clamp(z, 0.0f, 1.0f);
    float ze = 1.0f - 2.0f * z;
    float tc[4][4] = {};
    for (int u = 0; u < 4; u++) {
        auto it = g_tex.find(g_bound_tex[u]);
        if (it == g_tex.end() || !it->second.w) continue;
        auto& t = it->second;
        tc[u][0] = float(t.crop[0]) / t.w;
        tc[u][1] = float(t.crop[1]) / t.h;
        tc[u][2] = float(t.crop[0] + t.crop[2]) / t.w;
        tc[u][3] = float(t.crop[1] + t.crop[3]) / t.h;
    }
    glBegin(GL_TRIANGLE_STRIP);
    const float vx[4] = {x, x + w, x, x + w}, vy[4] = {y, y, y + h, y + h};
    for (int i = 0; i < 4; i++) {
        for (int u = 0; u < 2; u++)
            p_glMultiTexCoord2f(GL_TEXTURE0 + u, (i & 1) ? tc[u][2] : tc[u][0], (i & 2) ? tc[u][3] : tc[u][1]);
        glVertex3f(vx[i], vy[i], ze);
    }
    glEnd();
    if (lighting) glEnable(GL_LIGHTING);
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

void gles_init_host() {
    load(p_glActiveTexture, "glActiveTexture");
    load(p_glClientActiveTexture, "glClientActiveTexture");
    load(p_glMultiTexCoord4f, "glMultiTexCoord4f");
    load(p_glMultiTexCoord2f, "glMultiTexCoord2f");
    load(p_glBindBuffer, "glBindBuffer");
    load(p_glBufferData, "glBufferData");
    load(p_glBufferSubData, "glBufferSubData");
    load(p_glDeleteBuffers, "glDeleteBuffers");
    load(p_glGenBuffers, "glGenBuffers");
    load(p_glIsBuffer, "glIsBuffer");
    load(p_glGetBufferParameteriv, "glGetBufferParameteriv");
    load(p_glCompressedTexImage2D, "glCompressedTexImage2D");
    load(p_glCompressedTexSubImage2D, "glCompressedTexSubImage2D");
    load(p_glSampleCoverage, "glSampleCoverage");
    load(p_glBlendEquation, "glBlendEquation");
    load(p_glBlendEquationSeparate, "glBlendEquationSeparate");
    load(p_glBlendFuncSeparate, "glBlendFuncSeparate");
    load(p_glPointParameterf, "glPointParameterf");
    load(p_glPointParameterfv, "glPointParameterfv");
    load(p_glGenFramebuffers, "glGenFramebuffers");
    load(p_glDeleteFramebuffers, "glDeleteFramebuffers");
    load(p_glBindFramebuffer, "glBindFramebuffer");
    load(p_glIsFramebuffer, "glIsFramebuffer");
    load(p_glCheckFramebufferStatus, "glCheckFramebufferStatus");
    load(p_glFramebufferTexture2D, "glFramebufferTexture2D");
    load(p_glFramebufferRenderbuffer, "glFramebufferRenderbuffer");
    load(p_glGetFramebufferAttachmentParameteriv, "glGetFramebufferAttachmentParameteriv");
    load(p_glGenRenderbuffers, "glGenRenderbuffers");
    load(p_glDeleteRenderbuffers, "glDeleteRenderbuffers");
    load(p_glBindRenderbuffer, "glBindRenderbuffer");
    load(p_glIsRenderbuffer, "glIsRenderbuffer");
    load(p_glRenderbufferStorage, "glRenderbufferStorage");
    load(p_glGetRenderbufferParameteriv, "glGetRenderbufferParameteriv");
    load(p_glGenerateMipmap, "glGenerateMipmap");
    LOGI("host GL: %s | %s | %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
}

std::string g_dump_dir;  // debug: dump compressed texture payloads here
bool g_real_atc = false;  // decode ATC-labelled textures as real ATC (pristine APK)

// The game renders into the window's default framebuffer (0); the frontend may substitute
// its own render target, which guest binds of FBO 0 are redirected to.
GLuint g_default_fbo = 0;
GLenum g_depth_fmt = 0x8CAD;  // GL_DEPTH32F_STENCIL8 (fallback: GL_DEPTH24_STENCIL8)
bool g_veil_target = false;
bool g_normal_target = false;  // the game framebuffer has COLOR_ATTACHMENT1 (surface normals, native renderer)

// ---------------------------------------------------------------- presentation
// The game draws into g_default_fbo ("FBO 0" from its point of view). With MSAA that framebuffer
// is multisampled and gets resolved into g_resolve_fbo, which is what reaches the window and what
// the game's own read-backs (glReadPixels / glCopyTexImage2D) read from.
namespace {
PFNGLBLITFRAMEBUFFERPROC p_glBlitFramebuffer;
PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC p_glRenderbufferStorageMultisample;
GLuint g_present_tex = 0, g_present_depth = 0;  // single-sample target (no MSAA)
GLuint g_veil_rb = 0;
GLuint g_normal_rb = 0;                         // surface normals (same sample count as the color target)
GLuint g_msaa_color = 0, g_msaa_depth = 0;      // multisampled target
GLuint g_resolve_fbo = 0;
int g_rw = 0, g_rh = 0;
int g_samples = 0;
double g_clear_depth = 1.0;   // the game's glClearDepth value

void letterbox(int ww, int wh, int& x, int& y, int& w, int& h) {
    float s = std::min(float(ww) / g_rw, float(wh) / g_rh);
    w = int(g_rw * s + 0.5f);
    h = int(g_rh * s + 0.5f);
    x = (ww - w) / 2;
    y = (wh - h) / 2;
}

// Resolves the multisampled frame into g_resolve_fbo. Leaves the framebuffer bindings changed.
void resolve() {
    if (!g_samples) return;
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_default_fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_resolve_fbo);
    p_glBlitFramebuffer(0, 0, g_rw, g_rh, 0, 0, g_rw, g_rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

GLuint frame_source() { return g_samples ? g_resolve_fbo : g_default_fbo; }
}  // namespace

// Before the game reads pixels: if it reads its main framebuffer while multisampled, resolve and
// read from the resolved copy instead. Returns the read framebuffer to restore (or -1).
static GLint begin_guest_read() {
    if (!g_samples) return -1;
    GLint read_fb = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    if (GLuint(read_fb) != g_default_fbo) return -1;
    GLint draw_fb = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    resolve();
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(draw_fb));
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g_resolve_fbo);
    return read_fb;
}
static void end_guest_read(GLint fb) {
    if (fb >= 0) p_glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(fb));
}

int max_msaa() {
    GLint m = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &m);
    return m;
}

int msaa_samples() { return g_samples; }

void set_msaa(int samples) {
    samples = std::clamp(samples, 0, max_msaa());
    if (samples == 1) samples = 2;
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_default_fbo);
    if (samples) {
        if (!g_msaa_color) p_glGenRenderbuffers(1, &g_msaa_color);
        if (!g_msaa_depth) p_glGenRenderbuffers(1, &g_msaa_depth);
        p_glBindRenderbuffer(GL_RENDERBUFFER, g_msaa_color);
        p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, g_rw, g_rh);
        p_glBindRenderbuffer(GL_RENDERBUFFER, g_msaa_depth);
        p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, g_depth_fmt, g_rw, g_rh);
        p_glBindRenderbuffer(GL_RENDERBUFFER, 0);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_msaa_color);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_msaa_depth);
    } else {
        p_glBindRenderbuffer(GL_RENDERBUFFER, g_present_depth);
        p_glRenderbufferStorage(GL_RENDERBUFFER, g_depth_fmt, g_rw, g_rh);
        p_glBindRenderbuffer(GL_RENDERBUFFER, 0);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_present_tex, 0);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_present_depth);
    }
    // second color attachment for surface normals; only written by native draws that select it
    if (!g_normal_rb) p_glGenRenderbuffers(1, &g_normal_rb);
    p_glBindRenderbuffer(GL_RENDERBUFFER, g_normal_rb);
    if (samples) p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, 0x8059 /*RGB10_A2*/, g_rw, g_rh);
    else p_glRenderbufferStorage(GL_RENDERBUFFER, 0x8059, g_rw, g_rh);
    p_glBindRenderbuffer(GL_RENDERBUFFER, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 1, GL_RENDERBUFFER, g_normal_rb);
    g_normal_target = p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!g_normal_target) p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 1, GL_RENDERBUFFER, 0);
    // third: coverage of fog drawn over the scene, so that lighting is not applied through it
    g_veil_target = false;
    if (g_normal_target) {
        if (!g_veil_rb) p_glGenRenderbuffers(1, &g_veil_rb);
        p_glBindRenderbuffer(GL_RENDERBUFFER, g_veil_rb);
        if (samples) p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, g_rw, g_rh);
        else p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, g_rw, g_rh);
        p_glBindRenderbuffer(GL_RENDERBUFFER, 0);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 2, GL_RENDERBUFFER, g_veil_rb);
        g_veil_target = p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (!g_veil_target) p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 2, GL_RENDERBUFFER, 0);
    }
    GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE && g_depth_fmt != GL_DEPTH24_STENCIL8) {
        LOGW("float depth buffer not accepted (0x%x); using 24-bit depth", st);
        g_depth_fmt = GL_DEPTH24_STENCIL8;
        p_glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev));
        return set_msaa(samples);
    }
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("game framebuffer incomplete with %dx MSAA (0x%x), disabling MSAA", samples, st);
        if (samples) return set_msaa(0);
    }
    g_samples = samples;
    p_glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev));
    LOGI("anti-aliasing: %s; depth buffer: %s", samples ? (std::to_string(samples) + "x MSAA").c_str() : "off",
         g_depth_fmt == GL_DEPTH24_STENCIL8 ? "24-bit" : "32-bit float");
}

void present_init(int w, int h, int samples) {
    load(p_glBlitFramebuffer, "glBlitFramebuffer");
    load(p_glRenderbufferStorageMultisample, "glRenderbufferStorageMultisample");
    g_rw = w;
    g_rh = h;
    glGenTextures(1, &g_present_tex);
    glBindTexture(GL_TEXTURE_2D, g_present_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glGenRenderbuffers(1, &g_present_depth);
    p_glBindRenderbuffer(GL_RENDERBUFFER, g_present_depth);
    p_glRenderbufferStorage(GL_RENDERBUFFER, g_depth_fmt, w, h);
    p_glBindRenderbuffer(GL_RENDERBUFFER, 0);
    // resolve target shares the single-sample color texture
    p_glGenFramebuffers(1, &g_resolve_fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_resolve_fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_present_tex, 0);
    p_glGenFramebuffers(1, &g_default_fbo);
    LOGI("game renders at %dx%d (max %dx MSAA)", w, h, max_msaa());
    set_msaa(samples);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_default_fbo);
}

void present_begin() { p_glBindFramebuffer(GL_FRAMEBUFFER, g_default_fbo); }

namespace {
struct OverlayLine {
    float x0, y0, x1, y1;
    unsigned rgba;
};
std::vector<OverlayLine> g_overlay;
struct OverlayRect {
    float x, y, w, h;
    unsigned rgba;
};
std::vector<OverlayRect> g_overlay_rects;
// 5x7 font, ASCII 32..126, one byte per column (bit 0 = top row, bit 7 = descender)
const unsigned char kFont5x7[95 * 5] = {
0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x5F,0x00,0x00, 0x00,0x07,0x00,0x07,0x00, 0x14,0x7F,0x14,0x7F,0x14,
0x24,0x2A,0x7F,0x2A,0x12, 0x23,0x13,0x08,0x64,0x62, 0x36,0x49,0x56,0x20,0x50, 0x00,0x08,0x07,0x03,0x00,
0x00,0x1C,0x22,0x41,0x00, 0x00,0x41,0x22,0x1C,0x00, 0x2A,0x1C,0x7F,0x1C,0x2A, 0x08,0x08,0x3E,0x08,0x08,
0x00,0x80,0x70,0x30,0x00, 0x08,0x08,0x08,0x08,0x08, 0x00,0x00,0x60,0x60,0x00, 0x20,0x10,0x08,0x04,0x02,
0x3E,0x51,0x49,0x45,0x3E, 0x00,0x42,0x7F,0x40,0x00, 0x72,0x49,0x49,0x49,0x46, 0x21,0x41,0x49,0x4D,0x33,
0x18,0x14,0x12,0x7F,0x10, 0x27,0x45,0x45,0x45,0x39, 0x3C,0x4A,0x49,0x49,0x31, 0x41,0x21,0x11,0x09,0x07,
0x36,0x49,0x49,0x49,0x36, 0x46,0x49,0x49,0x29,0x1E, 0x00,0x00,0x14,0x00,0x00, 0x00,0x40,0x34,0x00,0x00,
0x00,0x08,0x14,0x22,0x41, 0x14,0x14,0x14,0x14,0x14, 0x00,0x41,0x22,0x14,0x08, 0x02,0x01,0x59,0x09,0x06,
0x3E,0x41,0x5D,0x59,0x4E, 0x7C,0x12,0x11,0x12,0x7C, 0x7F,0x49,0x49,0x49,0x36, 0x3E,0x41,0x41,0x41,0x22,
0x7F,0x41,0x41,0x41,0x3E, 0x7F,0x49,0x49,0x49,0x41, 0x7F,0x09,0x09,0x09,0x01, 0x3E,0x41,0x41,0x51,0x73,
0x7F,0x08,0x08,0x08,0x7F, 0x00,0x41,0x7F,0x41,0x00, 0x20,0x40,0x41,0x3F,0x01, 0x7F,0x08,0x14,0x22,0x41,
0x7F,0x40,0x40,0x40,0x40, 0x7F,0x02,0x1C,0x02,0x7F, 0x7F,0x04,0x08,0x10,0x7F, 0x3E,0x41,0x41,0x41,0x3E,
0x7F,0x09,0x09,0x09,0x06, 0x3E,0x41,0x51,0x21,0x5E, 0x7F,0x09,0x19,0x29,0x46, 0x26,0x49,0x49,0x49,0x32,
0x03,0x01,0x7F,0x01,0x03, 0x3F,0x40,0x40,0x40,0x3F, 0x1F,0x20,0x40,0x20,0x1F, 0x3F,0x40,0x38,0x40,0x3F,
0x63,0x14,0x08,0x14,0x63, 0x03,0x04,0x78,0x04,0x03, 0x61,0x59,0x49,0x4D,0x43, 0x00,0x7F,0x41,0x41,0x41,
0x02,0x04,0x08,0x10,0x20, 0x00,0x41,0x41,0x41,0x7F, 0x04,0x02,0x01,0x02,0x04, 0x40,0x40,0x40,0x40,0x40,
0x00,0x03,0x07,0x08,0x00, 0x20,0x54,0x54,0x78,0x40, 0x7F,0x28,0x44,0x44,0x38, 0x38,0x44,0x44,0x44,0x28,
0x38,0x44,0x44,0x28,0x7F, 0x38,0x54,0x54,0x54,0x18, 0x00,0x08,0x7E,0x09,0x02, 0x18,0xA4,0xA4,0x9C,0x78,
0x7F,0x08,0x04,0x04,0x78, 0x00,0x44,0x7D,0x40,0x00, 0x20,0x40,0x40,0x3D,0x00, 0x7F,0x10,0x28,0x44,0x00,
0x00,0x41,0x7F,0x40,0x00, 0x7C,0x04,0x78,0x04,0x78, 0x7C,0x08,0x04,0x04,0x78, 0x38,0x44,0x44,0x44,0x38,
0xFC,0x18,0x24,0x24,0x18, 0x18,0x24,0x24,0x18,0xFC, 0x7C,0x08,0x04,0x04,0x08, 0x48,0x54,0x54,0x54,0x24,
0x04,0x04,0x3F,0x44,0x24, 0x3C,0x40,0x40,0x20,0x7C, 0x1C,0x20,0x40,0x20,0x1C, 0x3C,0x40,0x30,0x40,0x3C,
0x44,0x28,0x10,0x28,0x44, 0x4C,0x90,0x90,0x90,0x7C, 0x44,0x64,0x54,0x4C,0x44, 0x00,0x08,0x36,0x41,0x00,
0x00,0x00,0x77,0x00,0x00, 0x00,0x41,0x36,0x08,0x00, 0x02,0x01,0x02,0x04,0x02};
}  // namespace

void overlay_rect(float x, float y, float w, float h, unsigned rgba) { g_overlay_rects.push_back({x, y, w, h, rgba}); }

float overlay_text(float x, float y, float px, const char* text, unsigned rgba) {
    float x0 = x;
    for (const char* p = text; *p; p++) {
        unsigned c = static_cast<unsigned char>(*p);
        if (c >= 32 && c < 127)
            for (int col = 0; col < 5; col++) {
                unsigned bits = kFont5x7[(c - 32) * 5 + col];
                for (int row = 0; row < 8; row++)
                    if (bits & (1u << row)) g_overlay_rects.push_back({x + col * px, y + row * px, px, px, rgba});
            }
        x += 6 * px;
    }
    return x - x0;
}

void overlay_line(float x0, float y0, float x1, float y1, unsigned rgba) { g_overlay.push_back({x0, y0, x1, y1, rgba}); }

void overlay_draw() {
    if (g_overlay.empty() && g_overlay_rects.empty()) return;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_default_fbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    glViewport(0, 0, g_rw, g_rh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, g_rw, g_rh, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glBegin(GL_QUADS);
    for (auto& r : g_overlay_rects) {
        glColor4ub(GLubyte(r.rgba >> 24), GLubyte(r.rgba >> 16), GLubyte(r.rgba >> 8), GLubyte(r.rgba));
        glVertex2f(r.x, r.y);
        glVertex2f(r.x + r.w, r.y);
        glVertex2f(r.x + r.w, r.y + r.h);
        glVertex2f(r.x, r.y + r.h);
    }
    glEnd();
    glLineWidth(float(std::max(2, g_rh / 360)));
    glBegin(GL_LINES);
    for (auto& l : g_overlay) {
        glColor4ub(GLubyte(l.rgba >> 24), GLubyte(l.rgba >> 16), GLubyte(l.rgba >> 8), GLubyte(l.rgba));
        glVertex2f(l.x0, l.y0);
        glVertex2f(l.x1, l.y1);
    }
    glEnd();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();
    g_overlay.clear();
    g_overlay_rects.clear();
}

void present_end(int ww, int wh) {
    int x, y, w, h;
    letterbox(ww, wh, x, y, w, h);
    // The game keeps GL state across frames (it does not reset its viewport every frame), so
    // everything touched here must be restored: viewport, scissor, clear colour, color mask.
    glPushAttrib(GL_VIEWPORT_BIT | GL_SCISSOR_BIT | GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    resolve();
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, frame_source());
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glBlitFramebuffer(0, 0, g_rw, g_rh, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_default_fbo);
    glPopAttrib();
}

bool present_map(int ww, int wh, float wx, float wy, float& gx, float& gy) {
    int x, y, w, h;
    letterbox(ww, wh, x, y, w, h);
    // window y grows downwards and so do the game's touch coordinates
    float top = float(wh - y - h);
    gx = (wx - x) * g_rw / w;
    gy = (wy - top) * g_rh / h;
    return gx >= 0 && gy >= 0 && gx < g_rw && gy < g_rh;
}

void present_read(int w, int h, void* rgba) {
    resolve();
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, frame_source());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

// Uploads one decoded RGBA level of the bound texture. Level 0 is shown to the texture pack first: if it
// has a replacement, that goes up instead (with mipmaps of its own) and the game's further levels of this
// texture are dropped.
void upload_rgba(GLenum target, GLint level, int w, int h, const u8* px) {
    const GLuint tex = g_bound_tex[g_active_unit];
    TexInfo& ti = g_tex[tex];
    if (level <= 0 && target == GL_TEXTURE_2D) {
        const texpack::Image* rep = texpack::on_upload(tex, px, w, h);
        if (rep) render::texture_pixels(tex, rep->px.data(), rep->w, rep->h);
        else render::texture_pixels(tex, px, w, h);
        ti.replaced = rep != nullptr;
        if (rep) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(target, 0, GL_RGBA, rep->w, rep->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rep->px.data());
            if (p_glGenerateMipmap) p_glGenerateMipmap(target);
            render::texture_game_mipmaps(tex);
            return;
        }
    } else if (ti.replaced) {
        return;
    }
    glTexImage2D(target, level < 0 ? 0 : level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

void register_gles() {
    g_str_vendor = mem::data_strdup("Dead Space PC port");
    g_str_renderer = mem::data_strdup("OpenGL ES 1.1 translation layer");
    g_str_version = mem::data_strdup("OpenGL ES-CM 1.1");
    g_str_ext = mem::data_strdup(
        "GL_OES_byte_coordinates GL_OES_fixed_point GL_OES_single_precision GL_OES_matrix_get "
        "GL_OES_read_format GL_OES_compressed_paletted_texture GL_OES_compressed_ETC1_RGB8_texture "
        "GL_OES_draw_texture GL_OES_framebuffer_object GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_stencil8 "
        "GL_OES_blend_equation_separate GL_OES_blend_func_separate GL_OES_blend_subtract GL_OES_mapbuffer "
        "GL_OES_element_index_uint GL_OES_texture_npot GL_OES_point_sprite GL_EXT_texture_filter_anisotropic "
        "GL_AMD_compressed_ATC_texture GL_ATI_texture_compression_atitc ");

    // ---------------------------------------------------------- state
    reg("glEnable", [](Ctx& c) {
        GLenum cap = c.arg(0);
        if (cap == k_MATRIX_PALETTE_OES) return warn_once("matrix palette enabled (unsupported)");
        render::enable(cap, true);
        glEnable(cap);
    });
    reg("glDisable", [](Ctx& c) {
        GLenum cap = c.arg(0);
        if (cap == k_MATRIX_PALETTE_OES) return;
        render::enable(cap, false);
        glDisable(cap);
    });
    reg("glIsEnabled", [](Ctx& c) {
        int k = array_index_for(c.arg(0));
        if (k >= 0) return c.ret(g_arr[k].enabled);
        c.ret(glIsEnabled(c.arg(0)));
    });
    reg("glEnableClientState", [](Ctx& c) {
        int k = array_index_for(c.arg(0));
        if (k >= 0) g_arr[k].enabled = true;
    });
    reg("glDisableClientState", [](Ctx& c) {
        int k = array_index_for(c.arg(0));
        if (k >= 0) g_arr[k].enabled = false;
    });
    reg("glGetError", [](Ctx& c) {
        GLenum e = glGetError();
        if (e) LOGD("host GL error 0x%x", e);
        c.ret(0);
    });
    reg("glGetString", [](Ctx& c) {
        switch (c.arg(0)) {
        case GL_VENDOR: return c.ret(g_str_vendor);
        case GL_RENDERER: return c.ret(g_str_renderer);
        case GL_VERSION: return c.ret(g_str_version);
        case GL_EXTENSIONS: return c.ret(g_str_ext);
        default: return c.ret(0);
        }
    });
    reg("glGetIntegerv", [](Ctx& c) {
        GLenum p = c.arg(0);
        GLint* out = c.argp<GLint>(1);
        switch (p) {
        case k_IMPLEMENTATION_COLOR_READ_TYPE_OES: *out = GL_UNSIGNED_BYTE; return;
        case k_IMPLEMENTATION_COLOR_READ_FORMAT_OES: *out = GL_RGBA; return;
        case GL_ARRAY_BUFFER_BINDING: *out = GLint(g_array_buffer); return;
        case GL_ELEMENT_ARRAY_BUFFER_BINDING: *out = GLint(g_element_buffer); return;
        case GL_MAX_TEXTURE_UNITS: *out = 2; return;
        case 0x8CA6: {  // GL_FRAMEBUFFER_BINDING_OES
            glGetIntegerv(p, out);
            if (GLuint(*out) == g_default_fbo) *out = 0;
            return;
        }
        default: glGetIntegerv(p, out); return;
        }
    });
    reg("glGetFloatv", [](Ctx& c) { glGetFloatv(c.arg(0), c.argp<GLfloat>(1)); });
    reg("glGetBooleanv", [](Ctx& c) { glGetBooleanv(c.arg(0), c.argp<GLboolean>(1)); });
    reg("glGetFixedv", [](Ctx& c) {
        GLfloat f[16] = {};
        glGetFloatv(c.arg(0), f);
        s32* out = c.argp<s32>(1);
        int n = c.arg(0) == GL_MODELVIEW_MATRIX || c.arg(0) == GL_PROJECTION_MATRIX || c.arg(0) == GL_TEXTURE_MATRIX ? 16 : 4;
        for (int i = 0; i < n; i++) out[i] = s32(f[i] * 65536.0f);
    });
    reg("glGetPointerv", [](Ctx& c) {
        int k = -1;
        switch (c.arg(0)) {
        case GL_VERTEX_ARRAY_POINTER: k = A_VERTEX; break;
        case GL_NORMAL_ARRAY_POINTER: k = A_NORMAL; break;
        case GL_COLOR_ARRAY_POINTER: k = A_COLOR; break;
        case GL_TEXTURE_COORD_ARRAY_POINTER: k = A_TEX0 + g_client_unit; break;
        }
        mem::w32(c.arg(1), k >= 0 ? g_arr[k].ptr : 0);
    });
    reg("glHint", [](Ctx& c) { glHint(c.arg(0), c.arg(1)); });
    reg("glFinish", [](Ctx& c) { glFinish(); });
    reg("glFlush", [](Ctx& c) { glFlush(); });
    reg("glPixelStorei", [](Ctx& c) { glPixelStorei(c.arg(0), GLint(c.arg(1))); });
    reg("glReadPixels", [](Ctx& c) {
        GLint fb = begin_guest_read();
        glReadPixels(GLint(c.arg(0)), GLint(c.arg(1)), GLsizei(c.arg(2)), GLsizei(c.arg(3)), c.arg(4), c.arg(5),
                     c.argp<void>(6));
        end_guest_read(fb);
    });

    // ---------------------------------------------------------- framebuffer ops
    reg("glClear", [](Ctx& c) {
        // the native path stores reversed depth (far = 0): the cleared value flips with it
        if ((c.arg(0) & GL_DEPTH_BUFFER_BIT) && render::reversed_z()) {
            glClearDepth(1.0 - g_clear_depth);
            glClear(c.arg(0));
            glClearDepth(g_clear_depth);
            return;
        }
        glClear(c.arg(0));
    });
    reg("glClearColor", [](Ctx& c) { glClearColor(c.argf(0), c.argf(1), c.argf(2), c.argf(3)); });
    reg("glClearColorx", [](Ctx& c) { glClearColor(fx(c.arg(0)), fx(c.arg(1)), fx(c.arg(2)), fx(c.arg(3))); });
    reg("glClearDepthf", [](Ctx& c) { g_clear_depth = c.argf(0); glClearDepth(g_clear_depth); });
    reg("glClearDepthx", [](Ctx& c) { g_clear_depth = fx(c.arg(0)); glClearDepth(g_clear_depth); });
    reg("glClearStencil", [](Ctx& c) { glClearStencil(GLint(c.arg(0))); });
    reg("glColorMask", [](Ctx& c) { render::color_mask(c.arg(0), c.arg(1), c.arg(2), c.arg(3)); glColorMask(c.arg(0), c.arg(1), c.arg(2), c.arg(3)); });
    reg("glDepthMask", [](Ctx& c) { render::depth_mask(c.arg(0)); glDepthMask(c.arg(0)); });
    reg("glStencilMask", [](Ctx& c) { glStencilMask(c.arg(0)); });
    reg("glDepthFunc", [](Ctx& c) { render::depth_func(c.arg(0)); glDepthFunc(c.arg(0)); });
    reg("glDepthRangef", [](Ctx& c) { glDepthRange(c.argf(0), c.argf(1)); });
    reg("glDepthRangex", [](Ctx& c) { glDepthRange(fx(c.arg(0)), fx(c.arg(1))); });
    reg("glStencilFunc", [](Ctx& c) { glStencilFunc(c.arg(0), GLint(c.arg(1)), c.arg(2)); });
    reg("glStencilOp", [](Ctx& c) { glStencilOp(c.arg(0), c.arg(1), c.arg(2)); });
    reg("glAlphaFunc", [](Ctx& c) { render::alpha_func(c.arg(0), c.argf(1)); glAlphaFunc(c.arg(0), c.argf(1)); });
    reg("glAlphaFuncx", [](Ctx& c) { render::alpha_func(c.arg(0), fx(c.arg(1))); glAlphaFunc(c.arg(0), fx(c.arg(1))); });
    reg("glBlendFunc", [](Ctx& c) { render::blend_func(c.arg(0), c.arg(1)); glBlendFunc(c.arg(0), c.arg(1)); });
    reg("glBlendFuncSeparateOES", [](Ctx& c) { p_glBlendFuncSeparate(c.arg(0), c.arg(1), c.arg(2), c.arg(3)); });
    reg("glBlendEquationOES", [](Ctx& c) { p_glBlendEquation(c.arg(0)); });
    reg("glBlendEquationSeparateOES", [](Ctx& c) { p_glBlendEquationSeparate(c.arg(0), c.arg(1)); });
    reg("glLogicOp", [](Ctx& c) { glLogicOp(c.arg(0)); });
    reg("glCullFace", [](Ctx& c) { render::cull_face(c.arg(0)); glCullFace(c.arg(0)); });
    reg("glFrontFace", [](Ctx& c) { render::front_face(c.arg(0)); glFrontFace(c.arg(0)); });
    reg("glShadeModel", [](Ctx& c) { glShadeModel(c.arg(0)); });
    reg("glLineWidth", [](Ctx& c) { glLineWidth(c.argf(0)); });
    reg("glLineWidthx", [](Ctx& c) { glLineWidth(fx(c.arg(0))); });
    reg("glPointSize", [](Ctx& c) { glPointSize(c.argf(0)); });
    reg("glPointSizex", [](Ctx& c) { glPointSize(fx(c.arg(0))); });
    reg("glPolygonOffset", [](Ctx& c) { render::polygon_offset(c.argf(0), c.argf(1)); glPolygonOffset(c.argf(0), c.argf(1)); });
    reg("glPolygonOffsetx", [](Ctx& c) { render::polygon_offset(fx(c.arg(0)), fx(c.arg(1))); glPolygonOffset(fx(c.arg(0)), fx(c.arg(1))); });
    reg("glSampleCoverage", [](Ctx& c) { p_glSampleCoverage(c.argf(0), GLboolean(c.arg(1))); });
    reg("glSampleCoveragex", [](Ctx& c) { p_glSampleCoverage(fx(c.arg(0)), GLboolean(c.arg(1))); });
    reg("glScissor", [](Ctx& c) {
        render::scissor(GLint(c.arg(0)), GLint(c.arg(1)), GLsizei(c.arg(2)), GLsizei(c.arg(3)));
        glScissor(GLint(c.arg(0)), GLint(c.arg(1)), GLsizei(c.arg(2)), GLsizei(c.arg(3)));
    });
    reg("glViewport", [](Ctx& c) {
        render::viewport(GLint(c.arg(0)), GLint(c.arg(1)), GLsizei(c.arg(2)), GLsizei(c.arg(3)));
        glViewport(GLint(c.arg(0)), GLint(c.arg(1)), GLsizei(c.arg(2)), GLsizei(c.arg(3)));
    });
    reg("glPointParameterf", [](Ctx& c) { p_glPointParameterf(c.arg(0), c.argf(1)); });
    reg("glPointParameterfv", [](Ctx& c) { p_glPointParameterfv(c.arg(0), c.argp<GLfloat>(1)); });
    reg("glPointParameterx", [](Ctx& c) { p_glPointParameterf(c.arg(0), fx(c.arg(1))); });
    reg("glPointParameterxv", [](Ctx& c) {
        GLfloat v[3];
        for (int i = 0; i < 3; i++) v[i] = fx(mem::r32(c.arg(1) + 4 * i));
        p_glPointParameterfv(c.arg(0), v);
    });

    // ---------------------------------------------------------- matrices
    // (every matrix op also updates the native renderer's mirror of the matrix stacks)
    reg("glMatrixMode", [](Ctx& c) {
        if (c.arg(0) == k_MATRIX_PALETTE_OES) return warn_once("matrix mode PALETTE (unsupported)");
        render::matrix_mode(c.arg(0));
        glMatrixMode(c.arg(0));
    });
    reg("glLoadIdentity", [](Ctx& c) { render::load_identity(); glLoadIdentity(); });
    reg("glPushMatrix", [](Ctx& c) { render::push_matrix(); glPushMatrix(); });
    reg("glPopMatrix", [](Ctx& c) { render::pop_matrix(); glPopMatrix(); });
    reg("glLoadMatrixf", [](Ctx& c) { render::load_matrix(c.argp<GLfloat>(0)); glLoadMatrixf(c.argp<GLfloat>(0)); });
    reg("glMultMatrixf", [](Ctx& c) { render::mult_matrix(c.argp<GLfloat>(0)); glMultMatrixf(c.argp<GLfloat>(0)); });
    reg("glLoadMatrixx", [](Ctx& c) {
        GLfloat m[16];
        for (int i = 0; i < 16; i++) m[i] = fx(mem::r32(c.arg(0) + 4 * i));
        render::load_matrix(m);
        glLoadMatrixf(m);
    });
    reg("glMultMatrixx", [](Ctx& c) {
        GLfloat m[16];
        for (int i = 0; i < 16; i++) m[i] = fx(mem::r32(c.arg(0) + 4 * i));
        render::mult_matrix(m);
        glMultMatrixf(m);
    });
    reg("glTranslatef", [](Ctx& c) { render::translate(c.argf(0), c.argf(1), c.argf(2)); glTranslatef(c.argf(0), c.argf(1), c.argf(2)); });
    reg("glTranslatex", [](Ctx& c) { render::translate(fx(c.arg(0)), fx(c.arg(1)), fx(c.arg(2))); glTranslatef(fx(c.arg(0)), fx(c.arg(1)), fx(c.arg(2))); });
    reg("glScalef", [](Ctx& c) { render::scale(c.argf(0), c.argf(1), c.argf(2)); glScalef(c.argf(0), c.argf(1), c.argf(2)); });
    reg("glScalex", [](Ctx& c) { render::scale(fx(c.arg(0)), fx(c.arg(1)), fx(c.arg(2))); glScalef(fx(c.arg(0)), fx(c.arg(1)), fx(c.arg(2))); });
    reg("glRotatef", [](Ctx& c) { render::rotate(c.argf(0), c.argf(1), c.argf(2), c.argf(3)); glRotatef(c.argf(0), c.argf(1), c.argf(2), c.argf(3)); });
    reg("glRotatex", [](Ctx& c) { render::rotate(fx(c.arg(0)), fx(c.arg(1)), fx(c.arg(2)), fx(c.arg(3))); glRotatef(fx(c.arg(0)), fx(c.arg(1)), fx(c.arg(2)), fx(c.arg(3))); });
    reg("glOrthof", [](Ctx& c) {
        render::ortho(c.argf(0), c.argf(1), c.argf(2), c.argf(3), c.argf(4), c.argf(5));
        glOrtho(c.argf(0), c.argf(1), c.argf(2), c.argf(3), c.argf(4), c.argf(5));
    });
    reg("glOrthox", [](Ctx& c) {
        render::ortho(fx(c.arg(0)), fx(c.arg(1)), fx(c.arg(2)), fx(c.arg(3)), fx(c.arg(4)), fx(c.arg(5)));
        glOrtho(fx(c.arg(0)), fx(c.arg(1)), fx(c.arg(2)), fx(c.arg(3)), fx(c.arg(4)), fx(c.arg(5)));
    });
    reg("glFrustumf", [](Ctx& c) {
        render::frustum(c.argf(0), c.argf(1), c.argf(2), c.argf(3), c.argf(4), c.argf(5));
        glFrustum(c.argf(0), c.argf(1), c.argf(2), c.argf(3), c.argf(4), c.argf(5));
    });
    reg("glFrustumx", [](Ctx& c) {
        render::frustum(fx(c.arg(0)), fx(c.arg(1)), fx(c.arg(2)), fx(c.arg(3)), fx(c.arg(4)), fx(c.arg(5)));
        glFrustum(fx(c.arg(0)), fx(c.arg(1)), fx(c.arg(2)), fx(c.arg(3)), fx(c.arg(4)), fx(c.arg(5)));
    });
    reg("glClipPlanef", [](Ctx& c) {
        GLdouble eq[4];
        for (int i = 0; i < 4; i++) eq[i] = c.argp<GLfloat>(1)[i];
        glClipPlane(c.arg(0), eq);
    });
    reg("glClipPlanex", [](Ctx& c) {
        GLdouble eq[4];
        for (int i = 0; i < 4; i++) eq[i] = fx(mem::r32(c.arg(1) + 4 * i));
        glClipPlane(c.arg(0), eq);
    });
    reg("glGetClipPlanef", [](Ctx& c) {
        GLdouble eq[4];
        glGetClipPlane(c.arg(0), eq);
        for (int i = 0; i < 4; i++) c.argp<GLfloat>(1)[i] = GLfloat(eq[i]);
    });
    reg("glGetClipPlanex", [](Ctx& c) {
        GLdouble eq[4];
        glGetClipPlane(c.arg(0), eq);
        for (int i = 0; i < 4; i++) mem::w32(c.arg(1) + 4 * i, u32(s32(eq[i] * 65536.0)));
    });
    reg("glQueryMatrixxOES", [](Ctx& c) { warn_once("glQueryMatrixxOES"); c.ret(0xFFFF); });
    reg("glCurrentPaletteMatrixOES", [](Ctx& c) { warn_once("glCurrentPaletteMatrixOES"); });
    reg("glLoadPaletteFromModelViewMatrixOES", [](Ctx& c) { warn_once("glLoadPaletteFromModelViewMatrixOES"); });

    // ---------------------------------------------------------- current vertex attributes
    reg("glColor4f", [](Ctx& c) { render::color(c.argf(0), c.argf(1), c.argf(2), c.argf(3)); glColor4f(c.argf(0), c.argf(1), c.argf(2), c.argf(3)); });
    reg("glColor4x", [](Ctx& c) { render::color(fx(c.arg(0)), fx(c.arg(1)), fx(c.arg(2)), fx(c.arg(3))); glColor4f(fx(c.arg(0)), fx(c.arg(1)), fx(c.arg(2)), fx(c.arg(3))); });
    reg("glColor4ub", [](Ctx& c) {
        render::color(c.arg(0) / 255.0f, c.arg(1) / 255.0f, c.arg(2) / 255.0f, c.arg(3) / 255.0f);
        glColor4ub(GLubyte(c.arg(0)), GLubyte(c.arg(1)), GLubyte(c.arg(2)), GLubyte(c.arg(3)));
    });
    reg("glNormal3f", [](Ctx& c) { glNormal3f(c.argf(0), c.argf(1), c.argf(2)); });
    reg("glNormal3x", [](Ctx& c) { glNormal3f(fx(c.arg(0)), fx(c.arg(1)), fx(c.arg(2))); });
    reg("glMultiTexCoord4f", [](Ctx& c) { p_glMultiTexCoord4f(c.arg(0), c.argf(1), c.argf(2), c.argf(3), c.argf(4)); });
    reg("glMultiTexCoord4x", [](Ctx& c) {
        p_glMultiTexCoord4f(c.arg(0), fx(c.arg(1)), fx(c.arg(2)), fx(c.arg(3)), fx(c.arg(4)));
    });

    // ---------------------------------------------------------- lighting / material / fog
#define XV_TO_FV(n)                                     \
    GLfloat v[16];                                      \
    for (int i = 0; i < (n); i++) v[i] = fx(mem::r32(c.arg(2) + 4 * i));
    reg("glLightf", [](Ctx& c) { glLightf(c.arg(0), c.arg(1), c.argf(2)); });
    reg("glLightfv", [](Ctx& c) { glLightfv(c.arg(0), c.arg(1), c.argp<GLfloat>(2)); });
    reg("glLightx", [](Ctx& c) { glLightf(c.arg(0), c.arg(1), fx(c.arg(2))); });
    reg("glLightxv", [](Ctx& c) { XV_TO_FV(4) glLightfv(c.arg(0), c.arg(1), v); });
    reg("glGetLightfv", [](Ctx& c) { glGetLightfv(c.arg(0), c.arg(1), c.argp<GLfloat>(2)); });
    reg("glGetLightxv", [](Ctx& c) {
        GLfloat v[4] = {};
        glGetLightfv(c.arg(0), c.arg(1), v);
        for (int i = 0; i < 4; i++) mem::w32(c.arg(2) + 4 * i, u32(s32(v[i] * 65536)));
    });
    reg("glMaterialf", [](Ctx& c) { glMaterialf(c.arg(0), c.arg(1), c.argf(2)); });
    reg("glMaterialfv", [](Ctx& c) { glMaterialfv(c.arg(0), c.arg(1), c.argp<GLfloat>(2)); });
    reg("glMaterialx", [](Ctx& c) { glMaterialf(c.arg(0), c.arg(1), fx(c.arg(2))); });
    reg("glMaterialxv", [](Ctx& c) { XV_TO_FV(4) glMaterialfv(c.arg(0), c.arg(1), v); });
    reg("glGetMaterialfv", [](Ctx& c) { glGetMaterialfv(c.arg(0), c.arg(1), c.argp<GLfloat>(2)); });
    reg("glGetMaterialxv", [](Ctx& c) {
        GLfloat v[4] = {};
        glGetMaterialfv(c.arg(0), c.arg(1), v);
        for (int i = 0; i < 4; i++) mem::w32(c.arg(2) + 4 * i, u32(s32(v[i] * 65536)));
    });
    reg("glLightModelf", [](Ctx& c) { glLightModelf(c.arg(0), c.argf(1)); });
    reg("glLightModelfv", [](Ctx& c) { glLightModelfv(c.arg(0), c.argp<GLfloat>(1)); });
    reg("glLightModelx", [](Ctx& c) {
        if (c.arg(0) == GL_LIGHT_MODEL_TWO_SIDE) glLightModeli(c.arg(0), GLint(c.arg(1)));
        else glLightModelf(c.arg(0), fx(c.arg(1)));
    });
    reg("glLightModelxv", [](Ctx& c) {
        if (c.arg(0) == GL_LIGHT_MODEL_TWO_SIDE) return glLightModeli(c.arg(0), GLint(mem::r32(c.arg(1))));
        GLfloat v[4];
        for (int i = 0; i < 4; i++) v[i] = fx(mem::r32(c.arg(1) + 4 * i));
        glLightModelfv(c.arg(0), v);
    });
    reg("glFogf", [](Ctx& c) {
        if (c.arg(0) == GL_FOG_MODE) render::fog_mode(GLenum(c.argf(1)));
        else render::fog_param(c.arg(0), c.argf(1));
        glFogf(c.arg(0), c.argf(1));
    });
    reg("glFogfv", [](Ctx& c) {
        if (c.arg(0) == GL_FOG_COLOR) render::fog_color(c.argp<GLfloat>(1));
        else if (c.arg(0) == GL_FOG_MODE) render::fog_mode(GLenum(c.argp<GLfloat>(1)[0]));
        else render::fog_param(c.arg(0), c.argp<GLfloat>(1)[0]);
        glFogfv(c.arg(0), c.argp<GLfloat>(1));
    });
    reg("glFogx", [](Ctx& c) {
        if (c.arg(0) == GL_FOG_MODE) {
            render::fog_mode(c.arg(1));
            glFogi(c.arg(0), GLint(c.arg(1)));
        } else {
            render::fog_param(c.arg(0), fx(c.arg(1)));
            glFogf(c.arg(0), fx(c.arg(1)));
        }
    });
    reg("glFogxv", [](Ctx& c) {
        if (c.arg(0) == GL_FOG_MODE) {
            render::fog_mode(mem::r32(c.arg(1)));
            return glFogi(c.arg(0), GLint(mem::r32(c.arg(1))));
        }
        GLfloat v[4];
        for (int i = 0; i < 4; i++) v[i] = fx(mem::r32(c.arg(1) + 4 * i));
        if (c.arg(0) == GL_FOG_COLOR) render::fog_color(v);
        else render::fog_param(c.arg(0), v[0]);
        glFogfv(c.arg(0), v);
    });

    // ---------------------------------------------------------- texture environment
    reg("glTexEnvf", [](Ctx& c) {
        if (c.arg(1) == GL_TEXTURE_ENV_MODE) render::tex_env_mode(g_active_unit, GLenum(c.argf(2)));
        glTexEnvf(c.arg(0), c.arg(1), c.argf(2));
    });
    reg("glTexEnvi", [](Ctx& c) {
        if (c.arg(1) == GL_TEXTURE_ENV_MODE) render::tex_env_mode(g_active_unit, c.arg(2));
        glTexEnvi(c.arg(0), c.arg(1), GLint(c.arg(2)));
    });
    reg("glTexEnvfv", [](Ctx& c) {
        if (c.arg(1) == GL_TEXTURE_ENV_COLOR) render::tex_env_color(g_active_unit, c.argp<GLfloat>(2));
        else if (c.arg(1) == GL_TEXTURE_ENV_MODE) render::tex_env_mode(g_active_unit, GLenum(c.argp<GLfloat>(2)[0]));
        glTexEnvfv(c.arg(0), c.arg(1), c.argp<GLfloat>(2));
    });
    reg("glTexEnviv", [](Ctx& c) {
        if (c.arg(1) == GL_TEXTURE_ENV_MODE) render::tex_env_mode(g_active_unit, GLenum(c.argp<GLint>(2)[0]));
        glTexEnviv(c.arg(0), c.arg(1), c.argp<GLint>(2));
    });
    reg("glTexEnvx", [](Ctx& c) {
        GLenum p = c.arg(1);
        if (p == GL_TEXTURE_ENV_COLOR || p == GL_RGB_SCALE || p == GL_ALPHA_SCALE)
            glTexEnvf(c.arg(0), p, fx(c.arg(2)));
        else {
            if (p == GL_TEXTURE_ENV_MODE) render::tex_env_mode(g_active_unit, c.arg(2));
            glTexEnvi(c.arg(0), p, GLint(c.arg(2)));
        }
    });
    reg("glTexEnvxv", [](Ctx& c) {
        GLenum p = c.arg(1);
        if (p == GL_TEXTURE_ENV_COLOR) { XV_TO_FV(4) render::tex_env_color(g_active_unit, v); glTexEnvfv(c.arg(0), p, v); }
        else if (p == GL_RGB_SCALE || p == GL_ALPHA_SCALE) glTexEnvf(c.arg(0), p, fx(mem::r32(c.arg(2))));
        else {
            if (p == GL_TEXTURE_ENV_MODE) render::tex_env_mode(g_active_unit, mem::r32(c.arg(2)));
            glTexEnvi(c.arg(0), p, GLint(mem::r32(c.arg(2))));
        }
    });
    reg("glGetTexEnvfv", [](Ctx& c) { glGetTexEnvfv(c.arg(0), c.arg(1), c.argp<GLfloat>(2)); });
    reg("glGetTexEnviv", [](Ctx& c) { glGetTexEnviv(c.arg(0), c.arg(1), c.argp<GLint>(2)); });
    reg("glGetTexEnvxv", [](Ctx& c) { glGetTexEnviv(c.arg(0), c.arg(1), c.argp<GLint>(2)); });
    reg("glTexGenfOES", [](Ctx& c) { glTexGenf(c.arg(0), c.arg(1), c.argf(2)); });
    reg("glTexGenfvOES", [](Ctx& c) { glTexGenfv(c.arg(0), c.arg(1), c.argp<GLfloat>(2)); });
    reg("glTexGeniOES", [](Ctx& c) { glTexGeni(c.arg(0), c.arg(1), GLint(c.arg(2))); });
    reg("glTexGenivOES", [](Ctx& c) { glTexGeniv(c.arg(0), c.arg(1), c.argp<GLint>(2)); });
    reg("glTexGenxOES", [](Ctx& c) { glTexGeni(c.arg(0), c.arg(1), GLint(c.arg(2))); });
    reg("glTexGenxvOES", [](Ctx& c) { glTexGeniv(c.arg(0), c.arg(1), c.argp<GLint>(2)); });
    reg("glGetTexGenfvOES", [](Ctx& c) { glGetTexGenfv(c.arg(0), c.arg(1), c.argp<GLfloat>(2)); });
    reg("glGetTexGenivOES", [](Ctx& c) { glGetTexGeniv(c.arg(0), c.arg(1), c.argp<GLint>(2)); });
    reg("glGetTexGenxvOES", [](Ctx& c) { glGetTexGeniv(c.arg(0), c.arg(1), c.argp<GLint>(2)); });

    // ---------------------------------------------------------- textures
    reg("glActiveTexture", [](Ctx& c) {
        g_active_unit = int(c.arg(0) - GL_TEXTURE0) & 3;
        render::active_texture(g_active_unit);
        p_glActiveTexture(c.arg(0));
    });
    reg("glClientActiveTexture", [](Ctx& c) {
        g_client_unit = int(c.arg(0) - GL_TEXTURE0) & 3;
        p_glClientActiveTexture(c.arg(0));
    });
    reg("glGenTextures", [](Ctx& c) { glGenTextures(GLsizei(c.arg(0)), c.argp<GLuint>(1)); });
    reg("glDeleteTextures", [](Ctx& c) {
        GLsizei n = GLsizei(c.arg(0));
        GLuint* t = c.argp<GLuint>(1);
        for (int i = 0; i < n; i++) {
            g_tex.erase(t[i]);
            texpack::on_delete(t[i]);
            render::texture_deleted(t[i]);
        }
        glDeleteTextures(n, t);
    });
    reg("glIsTexture", [](Ctx& c) { c.ret(glIsTexture(c.arg(0))); });
    reg("glBindTexture", [](Ctx& c) {
        g_bound_tex[g_active_unit] = c.arg(1);
        render::bind_texture(g_active_unit, c.arg(1));
        glBindTexture(c.arg(0), c.arg(1));
    });
    reg("glTexImage2D", [](Ctx& c) {
        GLenum target = c.arg(0);
        GLint level = GLint(c.arg(1)), ifmt = GLint(c.arg(2));
        GLsizei w = GLsizei(c.arg(3)), h = GLsizei(c.arg(4));
        GLenum fmt = c.arg(6), type = c.arg(7);
        if (level == 0) {
            auto& t = g_tex[g_bound_tex[g_active_unit]];
            t.w = w;
            t.h = h;
        }
        if (ifmt == k_RGB565_OES) ifmt = GL_RGB;
        if (level == 0) render::texture_format(g_bound_tex[g_active_unit], fmt);
        render::texture_level(g_bound_tex[g_active_unit], level);
        LOGD("glTexImage2D tex=%u level=%d ifmt=0x%x %dx%d fmt=0x%x type=0x%x", g_bound_tex[g_active_unit], level, ifmt, w, h, fmt, type);
        // plain RGBA / RGB images can be replaced by a texture pack like the compressed ones
        if (target == GL_TEXTURE_2D && type == GL_UNSIGNED_BYTE && (fmt == GL_RGBA || fmt == GL_RGB) && c.arg(8) && w >= 8 && h >= 8) {
            const u8* src = static_cast<const u8*>(hp(c.arg(8)));
            if (fmt == GL_RGBA) {
                std::vector<u8> tight;
                upload_rgba(target, level, w, h, src);
            } else {
                std::vector<u8> rgba(size_t(w) * size_t(h) * 4);
                const size_t row = (size_t(w) * 3 + 3) & ~size_t(3);  // (rows are 4-byte aligned by default)
                GLint align = 4;
                glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
                const size_t stride = align == 1 ? size_t(w) * 3 : row;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        const u8* p = src + size_t(y) * stride + size_t(x) * 3;
                        u8* o = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
                        o[0] = p[0], o[1] = p[1], o[2] = p[2], o[3] = 255;
                    }
                GLint old = 4;
                glGetIntegerv(GL_UNPACK_ALIGNMENT, &old);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                upload_rgba(target, level, w, h, rgba.data());
                glPixelStorei(GL_UNPACK_ALIGNMENT, old);
            }
            return;
        }
        glTexImage2D(target, level, ifmt, w, h, GLint(c.arg(5)), fmt, type, hp(c.arg(8)));
    });
    reg("glTexSubImage2D", [](Ctx& c) {
        render::texture_level(g_bound_tex[g_active_unit], GLint(c.arg(1)));
        glTexSubImage2D(c.arg(0), GLint(c.arg(1)), GLint(c.arg(2)), GLint(c.arg(3)), GLsizei(c.arg(4)),
                        GLsizei(c.arg(5)), c.arg(6), c.arg(7), hp(c.arg(8)));
    });
    reg("glCompressedTexImage2D", [](Ctx& c) {
        GLenum target = c.arg(0), fmt = c.arg(2);
        GLint level = GLint(c.arg(1));
        GLsizei w = GLsizei(c.arg(3)), h = GLsizei(c.arg(4)), size = GLsizei(c.arg(6));
        const u8* data = static_cast<const u8*>(hp(c.arg(7)));
        if (level <= 0) {
            auto& t = g_tex[g_bound_tex[g_active_unit]];
            t.w = w;
            t.h = h;
        }
        if (!g_dump_dir.empty() && level == 0) {
            static int n = 0;
            char name[512];
            std::snprintf(name, sizeof name, "%s/tex%04d_%x_%dx%d.bin", g_dump_dir.c_str(), n++, fmt, w, h);
            if (std::FILE* f = std::fopen(name, "wb")) {
                std::fwrite(data, 1, size_t(size), f);
                std::fclose(f);
            }
        }
        LOGD("glCompressedTexImage2D tex=%u level=%d fmt=0x%x %dx%d", g_bound_tex[g_active_unit], level, fmt, w, h);
        if (level <= 0) render::texture_format(g_bound_tex[g_active_unit], GL_RGBA);  // every compressed path decodes to RGBA
        render::texture_level(g_bound_tex[g_active_unit], level < 0 ? 0 : level);
        if (level < 0) render::texture_game_mipmaps(g_bound_tex[g_active_unit]);  // paletted: -level extra mip levels
        if (fmt == k_ATC_RGB || fmt == k_ATC_RGBA_EXPLICIT || fmt == k_ATC_RGBA_INTERP) {
            // The fan-fixed APK re-encoded its ATC textures as ETC2 (same block sizes) but the game still
            // labels them ATC. A pristine APK has real ATC data: --atc-textures selects that decoder.
            auto img = g_real_atc ? decode_atc(fmt, data, w, h) : decode_etc2(data, w, h, fmt != k_ATC_RGB);
            upload_rgba(target, level, w, h, img.data());
            return;
        }
        if (fmt == 0x9274 || fmt == 0x9278) {  // genuine ETC2 RGB8 / RGBA8_EAC
            auto img = decode_etc2(data, w, h, fmt == 0x9278);
            upload_rgba(target, level, w, h, img.data());
            return;
        }
        if (fmt == k_ETC1_RGB8_OES) {
            auto img = decode_etc2(data, w, h, false);
            upload_rgba(target, level, w, h, img.data());
            return;
        }
        if (fmt >= k_PALETTE4_RGB8_OES && fmt <= k_PALETTE8_RGB5_A1_OES) {
            int levels = level < 0 ? 1 - level : 1;
            auto imgs = decode_palette(fmt, data, w, h, levels);
            for (int l = 0; l < levels; l++) upload_rgba(target, l, std::max(1, w >> l), std::max(1, h >> l), imgs[l].data());
            return;
        }
        char b[64];
        std::snprintf(b, sizeof b, "compressed texture format 0x%x (%dx%d) passed to host", fmt, w, h);
        warn_once(b);
        p_glCompressedTexImage2D(target, level, fmt, w, h, 0, size, data);
    });
    reg("glCompressedTexSubImage2D", [](Ctx& c) {
        warn_once("glCompressedTexSubImage2D");
        p_glCompressedTexSubImage2D(c.arg(0), GLint(c.arg(1)), GLint(c.arg(2)), GLint(c.arg(3)), GLsizei(c.arg(4)),
                                    GLsizei(c.arg(5)), c.arg(6), GLsizei(c.arg(7)), hp(c.arg(8)));
    });
    reg("glCopyTexImage2D", [](Ctx& c) {
        render::texture_level(g_bound_tex[g_active_unit], GLint(c.arg(1)));
        if (c.arg(1) == 0) {
            auto& t = g_tex[g_bound_tex[g_active_unit]];
            t.w = int(c.arg(5));
            t.h = int(c.arg(6));
        }
        GLint fb = begin_guest_read();
        glCopyTexImage2D(c.arg(0), GLint(c.arg(1)), c.arg(2), GLint(c.arg(3)), GLint(c.arg(4)), GLsizei(c.arg(5)),
                         GLsizei(c.arg(6)), GLint(c.arg(7)));
        end_guest_read(fb);
    });
    reg("glCopyTexSubImage2D", [](Ctx& c) {
        render::texture_level(g_bound_tex[g_active_unit], GLint(c.arg(1)));
        GLint fb = begin_guest_read();
        glCopyTexSubImage2D(c.arg(0), GLint(c.arg(1)), GLint(c.arg(2)), GLint(c.arg(3)), GLint(c.arg(4)),
                            GLint(c.arg(5)), GLsizei(c.arg(6)), GLsizei(c.arg(7)));
        end_guest_read(fb);
    });
    reg("glTexParameteri", [](Ctx& c) {
        render::tex_param(g_bound_tex[g_active_unit], c.arg(1), c.arg(2));
        glTexParameteri(c.arg(0), c.arg(1), GLint(c.arg(2)));
    });
    reg("glTexParameterf", [](Ctx& c) {
        render::tex_param(g_bound_tex[g_active_unit], c.arg(1), GLenum(c.argf(2)));
        glTexParameterf(c.arg(0), c.arg(1), c.argf(2));
    });
    reg("glTexParameterx", [](Ctx& c) {
        render::tex_param(g_bound_tex[g_active_unit], c.arg(1), c.arg(2));
        glTexParameteri(c.arg(0), c.arg(1), GLint(c.arg(2)));
    });
    reg("glTexParameteriv", [](Ctx& c) {
        if (c.arg(1) == k_TEXTURE_CROP_RECT_OES) {
            auto& t = g_tex[g_bound_tex[g_active_unit]];
            for (int i = 0; i < 4; i++) t.crop[i] = s32(mem::r32(c.arg(2) + 4 * i));
            return;
        }
        render::tex_param(g_bound_tex[g_active_unit], c.arg(1), GLenum(c.argp<GLint>(2)[0]));
        glTexParameteriv(c.arg(0), c.arg(1), c.argp<GLint>(2));
    });
    reg("glTexParameterfv", [](Ctx& c) {
        if (c.arg(1) == k_TEXTURE_CROP_RECT_OES) {
            auto& t = g_tex[g_bound_tex[g_active_unit]];
            for (int i = 0; i < 4; i++) t.crop[i] = int(c.argp<GLfloat>(2)[i]);
            return;
        }
        render::tex_param(g_bound_tex[g_active_unit], c.arg(1), GLenum(c.argp<GLfloat>(2)[0]));
        glTexParameterfv(c.arg(0), c.arg(1), c.argp<GLfloat>(2));
    });
    reg("glTexParameterxv", [](Ctx& c) {
        if (c.arg(1) == k_TEXTURE_CROP_RECT_OES) {
            auto& t = g_tex[g_bound_tex[g_active_unit]];
            for (int i = 0; i < 4; i++) t.crop[i] = int(fx(mem::r32(c.arg(2) + 4 * i)));
            return;
        }
        render::tex_param(g_bound_tex[g_active_unit], c.arg(1), mem::r32(c.arg(2)));
        glTexParameteri(c.arg(0), c.arg(1), GLint(mem::r32(c.arg(2))));
    });
    reg("glGetTexParameteriv", [](Ctx& c) { glGetTexParameteriv(c.arg(0), c.arg(1), c.argp<GLint>(2)); });
    reg("glGetTexParameterfv", [](Ctx& c) { glGetTexParameterfv(c.arg(0), c.arg(1), c.argp<GLfloat>(2)); });
    reg("glGetTexParameterxv", [](Ctx& c) { glGetTexParameteriv(c.arg(0), c.arg(1), c.argp<GLint>(2)); });
    reg("glGenerateMipmapOES", [](Ctx& c) {
        render::texture_game_mipmaps(g_bound_tex[g_active_unit]);
        p_glGenerateMipmap(c.arg(0));
    });

    // ---------------------------------------------------------- draw texture
    reg("glDrawTexfOES", [](Ctx& c) { draw_tex(c.argf(0), c.argf(1), c.argf(2), c.argf(3), c.argf(4)); });
    reg("glDrawTexiOES", [](Ctx& c) {
        draw_tex(float(s32(c.arg(0))), float(s32(c.arg(1))), float(s32(c.arg(2))), float(s32(c.arg(3))), float(s32(c.arg(4))));
    });
    reg("glDrawTexsOES", [](Ctx& c) {
        draw_tex(float(int16_t(c.arg(0))), float(int16_t(c.arg(1))), float(int16_t(c.arg(2))), float(int16_t(c.arg(3))),
                 float(int16_t(c.arg(4))));
    });
    reg("glDrawTexxOES", [](Ctx& c) { draw_tex(fx(c.arg(0)), fx(c.arg(1)), fx(c.arg(2)), fx(c.arg(3)), fx(c.arg(4))); });
    reg("glDrawTexfvOES", [](Ctx& c) {
        const GLfloat* v = c.argp<GLfloat>(0);
        draw_tex(v[0], v[1], v[2], v[3], v[4]);
    });
    reg("glDrawTexivOES", [](Ctx& c) {
        const GLint* v = c.argp<GLint>(0);
        draw_tex(float(v[0]), float(v[1]), float(v[2]), float(v[3]), float(v[4]));
    });
    reg("glDrawTexsvOES", [](Ctx& c) {
        const int16_t* v = c.argp<int16_t>(0);
        draw_tex(v[0], v[1], v[2], v[3], v[4]);
    });
    reg("glDrawTexxvOES", [](Ctx& c) {
        u32 a = c.arg(0);
        draw_tex(fx(mem::r32(a)), fx(mem::r32(a + 4)), fx(mem::r32(a + 8)), fx(mem::r32(a + 12)), fx(mem::r32(a + 16)));
    });

    // ---------------------------------------------------------- buffers
    reg("glGenBuffers", [](Ctx& c) { p_glGenBuffers(GLsizei(c.arg(0)), c.argp<GLuint>(1)); });
    reg("glDeleteBuffers", [](Ctx& c) {
        GLsizei n = GLsizei(c.arg(0));
        GLuint* b = c.argp<GLuint>(1);
        for (int i = 0; i < n; i++) {
            g_buf_shadow.erase(b[i]);
            if (g_array_buffer == b[i]) g_array_buffer = 0;
            if (g_element_buffer == b[i]) g_element_buffer = 0;
        }
        p_glDeleteBuffers(n, b);
    });
    reg("glIsBuffer", [](Ctx& c) { c.ret(p_glIsBuffer(c.arg(0))); });
    reg("glBindBuffer", [](Ctx& c) {
        if (c.arg(0) == GL_ARRAY_BUFFER) g_array_buffer = c.arg(1);
        else if (c.arg(0) == GL_ELEMENT_ARRAY_BUFFER) g_element_buffer = c.arg(1);
        p_glBindBuffer(c.arg(0), c.arg(1));
    });
    reg("glBufferData", [](Ctx& c) {
        GLenum target = c.arg(0);
        GLuint b = target == GL_ARRAY_BUFFER ? g_array_buffer : g_element_buffer;
        auto& sh = g_buf_shadow[b];
        sh.assign(c.arg(1), 0);
        if (c.arg(2)) std::memcpy(sh.data(), mem::host(c.arg(2)), c.arg(1));
        p_glBufferData(target, GLsizeiptr(c.arg(1)), hp(c.arg(2)), c.arg(3));
    });
    reg("glBufferSubData", [](Ctx& c) {
        GLenum target = c.arg(0);
        GLuint b = target == GL_ARRAY_BUFFER ? g_array_buffer : g_element_buffer;
        auto& sh = g_buf_shadow[b];
        u32 off = c.arg(1), size = c.arg(2);
        if (off + size > sh.size()) sh.resize(off + size);
        std::memcpy(sh.data() + off, mem::host(c.arg(3)), size);
        p_glBufferSubData(target, GLintptr(off), GLsizeiptr(size), hp(c.arg(3)));
    });
    reg("glGetBufferParameteriv", [](Ctx& c) { p_glGetBufferParameteriv(c.arg(0), c.arg(1), c.argp<GLint>(2)); });
    reg("glMapBufferOES", [](Ctx& c) {
        GLenum target = c.arg(0);
        GLuint b = target == GL_ARRAY_BUFFER ? g_array_buffer : g_element_buffer;
        auto& sh = g_buf_shadow[b];
        u32 g = mem::heap_alloc(u32(std::max<size_t>(sh.size(), 16)));
        if (!sh.empty()) std::memcpy(mem::host(g), sh.data(), sh.size());
        g_buf_mapped[b] = g;
        c.ret(g);
    });
    reg("glUnmapBufferOES", [](Ctx& c) {
        GLenum target = c.arg(0);
        GLuint b = target == GL_ARRAY_BUFFER ? g_array_buffer : g_element_buffer;
        auto it = g_buf_mapped.find(b);
        if (it == g_buf_mapped.end()) return c.ret(0);
        auto& sh = g_buf_shadow[b];
        std::memcpy(sh.data(), mem::host(it->second), sh.size());
        p_glBufferSubData(target, 0, GLsizeiptr(sh.size()), sh.data());
        mem::heap_free(it->second);
        g_buf_mapped.erase(it);
        c.ret(1);
    });
    reg("glGetBufferPointervOES", [](Ctx& c) {
        GLuint b = c.arg(0) == GL_ARRAY_BUFFER ? g_array_buffer : g_element_buffer;
        auto it = g_buf_mapped.find(b);
        mem::w32(c.arg(2), it == g_buf_mapped.end() ? 0 : it->second);
    });

    // ---------------------------------------------------------- vertex arrays & drawing
    reg("glVertexPointer", [](Ctx& c) { set_array(A_VERTEX, GLint(c.arg(0)), c.arg(1), GLsizei(c.arg(2)), c.arg(3)); });
    reg("glNormalPointer", [](Ctx& c) { set_array(A_NORMAL, 3, c.arg(0), GLsizei(c.arg(1)), c.arg(2)); });
    reg("glColorPointer", [](Ctx& c) { set_array(A_COLOR, GLint(c.arg(0)), c.arg(1), GLsizei(c.arg(2)), c.arg(3)); });
    reg("glTexCoordPointer", [](Ctx& c) {
        set_array(A_TEX0 + g_client_unit, GLint(c.arg(0)), c.arg(1), GLsizei(c.arg(2)), c.arg(3));
    });
    reg("glPointSizePointerOES", [](Ctx& c) { set_array(A_POINTSIZE, 1, c.arg(0), GLsizei(c.arg(1)), c.arg(2)); });
    reg("glMatrixIndexPointerOES", [](Ctx& c) {
        warn_once("glMatrixIndexPointerOES");
        set_array(A_MATIDX, GLint(c.arg(0)), c.arg(1), GLsizei(c.arg(2)), c.arg(3));
    });
    reg("glWeightPointerOES", [](Ctx& c) {
        warn_once("glWeightPointerOES");
        set_array(A_WEIGHT, GLint(c.arg(0)), c.arg(1), GLsizei(c.arg(2)), c.arg(3));
    });
    reg("glDrawArrays", [](Ctx& c) {
        GLint first = GLint(c.arg(1));
        GLsizei count = GLsizei(c.arg(2));
        if (count <= 0) return;
        if (render::enabled()) {
            sync_native_arrays();
            return render::draw_arrays(c.arg(0), first, count);
        }
        bind_arrays(u32(first), u32(count));
        glDrawArrays(c.arg(0), first, count);
    });
    reg("glDrawElements", [](Ctx& c) {
        GLenum mode = c.arg(0), type = c.arg(2);
        GLsizei count = GLsizei(c.arg(1));
        u32 ind = c.arg(3);
        if (count <= 0) return;
        const u8* idx = nullptr;
        if (g_element_buffer) {
            auto it = g_buf_shadow.find(g_element_buffer);
            if (it != g_buf_shadow.end() && ind < it->second.size()) idx = it->second.data() + ind;
        } else {
            idx = mem::host(ind);
        }
        if (render::enabled()) {
            sync_native_arrays();
            return render::draw_elements(mode, count, type, idx);
        }
        u32 lo = 0, hi = 0;
        if (any_conversion() && idx) {
            lo = 0xFFFFFFFF;
            for (GLsizei i = 0; i < count; i++) {
                u32 v = type == GL_UNSIGNED_BYTE ? idx[i]
                      : type == GL_UNSIGNED_SHORT ? u32(reinterpret_cast<const u16*>(idx)[i])
                      : reinterpret_cast<const u32*>(idx)[i];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        bind_arrays(lo, hi - lo + 1);
        glDrawElements(mode, count, type, g_element_buffer ? reinterpret_cast<const void*>(uintptr_t(ind)) : hp(ind));
    });

    // ---------------------------------------------------------- framebuffer objects (OES)
    reg("glGenFramebuffersOES", [](Ctx& c) { p_glGenFramebuffers(GLsizei(c.arg(0)), c.argp<GLuint>(1)); });
    reg("glDeleteFramebuffersOES", [](Ctx& c) { p_glDeleteFramebuffers(GLsizei(c.arg(0)), c.argp<GLuint>(1)); });
    reg("glBindFramebufferOES", [](Ctx& c) {
        GLuint fb = c.arg(1);
        p_glBindFramebuffer(c.arg(0), fb ? fb : g_default_fbo);
    });
    reg("glIsFramebufferOES", [](Ctx& c) { c.ret(p_glIsFramebuffer(c.arg(0))); });
    reg("glCheckFramebufferStatusOES", [](Ctx& c) { c.ret(p_glCheckFramebufferStatus(c.arg(0))); });
    reg("glFramebufferTexture2DOES", [](Ctx& c) {
        if (c.arg(3)) render::texture_is_target(c.arg(3));
        p_glFramebufferTexture2D(c.arg(0), c.arg(1), c.arg(2), c.arg(3), GLint(c.arg(4)));
    });
    reg("glFramebufferRenderbufferOES", [](Ctx& c) {
        p_glFramebufferRenderbuffer(c.arg(0), c.arg(1), c.arg(2), c.arg(3));
    });
    reg("glGetFramebufferAttachmentParameterivOES", [](Ctx& c) {
        p_glGetFramebufferAttachmentParameteriv(c.arg(0), c.arg(1), c.arg(2), c.argp<GLint>(3));
    });
    reg("glGenRenderbuffersOES", [](Ctx& c) { p_glGenRenderbuffers(GLsizei(c.arg(0)), c.argp<GLuint>(1)); });
    reg("glDeleteRenderbuffersOES", [](Ctx& c) { p_glDeleteRenderbuffers(GLsizei(c.arg(0)), c.argp<GLuint>(1)); });
    reg("glBindRenderbufferOES", [](Ctx& c) { p_glBindRenderbuffer(c.arg(0), c.arg(1)); });
    reg("glIsRenderbufferOES", [](Ctx& c) { c.ret(p_glIsRenderbuffer(c.arg(0))); });
    reg("glRenderbufferStorageOES", [](Ctx& c) {
        GLenum f = c.arg(1);
        if (f == k_RGB565_OES) f = GL_RGB8;
        p_glRenderbufferStorage(c.arg(0), f, GLsizei(c.arg(2)), GLsizei(c.arg(3)));
    });
    reg("glGetRenderbufferParameterivOES", [](Ctx& c) {
        p_glGetRenderbufferParameteriv(c.arg(0), c.arg(1), c.argp<GLint>(2));
    });
    reg("glEGLImageTargetTexture2DOES", [](Ctx& c) { warn_once("glEGLImageTargetTexture2DOES"); });
    reg("glEGLImageTargetRenderbufferStorageOES", [](Ctx& c) { warn_once("glEGLImageTargetRenderbufferStorageOES"); });
#undef XV_TO_FV
}

}  // namespace hle
