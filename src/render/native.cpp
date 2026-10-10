#include "render/native.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "hle/gles.h"
#include "log.h"

namespace render {

namespace {

// ---------------------------------------------------------------- host GL (3.x) entry points
PFNGLCREATESHADERPROC p_glCreateShader;
PFNGLSHADERSOURCEPROC p_glShaderSource;
PFNGLCOMPILESHADERPROC p_glCompileShader;
PFNGLGETSHADERIVPROC p_glGetShaderiv;
PFNGLGETSHADERINFOLOGPROC p_glGetShaderInfoLog;
PFNGLCREATEPROGRAMPROC p_glCreateProgram;
PFNGLATTACHSHADERPROC p_glAttachShader;
PFNGLLINKPROGRAMPROC p_glLinkProgram;
PFNGLGETPROGRAMIVPROC p_glGetProgramiv;
PFNGLGETPROGRAMINFOLOGPROC p_glGetProgramInfoLog;
PFNGLUSEPROGRAMPROC p_glUseProgram;
PFNGLGETUNIFORMLOCATIONPROC p_glGetUniformLocation;
PFNGLUNIFORM1IPROC p_glUniform1i;
PFNGLUNIFORM1FPROC p_glUniform1f;
PFNGLUNIFORM2FPROC p_glUniform2f;
PFNGLUNIFORM4FVPROC p_glUniform4fv;
PFNGLUNIFORMMATRIX4FVPROC p_glUniformMatrix4fv;
PFNGLBINDATTRIBLOCATIONPROC p_glBindAttribLocation;
PFNGLGENVERTEXARRAYSPROC p_glGenVertexArrays;
PFNGLBINDVERTEXARRAYPROC p_glBindVertexArray;
PFNGLGENBUFFERSPROC p_glGenBuffers;
PFNGLBINDBUFFERPROC p_glBindBuffer;
PFNGLBUFFERDATAPROC p_glBufferData;
PFNGLENABLEVERTEXATTRIBARRAYPROC p_glEnableVertexAttribArray;
PFNGLDISABLEVERTEXATTRIBARRAYPROC p_glDisableVertexAttribArray;
PFNGLVERTEXATTRIBPOINTERPROC p_glVertexAttribPointer;
PFNGLVERTEXATTRIB4FPROC p_glVertexAttrib4f;
PFNGLACTIVETEXTUREPROC p_glActiveTexture;
PFNGLGENERATEMIPMAPPROC p_glGenerateMipmap;
// optional (GL 3.3 / 4.5): sampler objects, clip control
PFNGLGENSAMPLERSPROC p_glGenSamplers;
PFNGLBINDSAMPLERPROC p_glBindSampler;
typedef void(APIENTRY* PFN_SamplerParameteri)(GLuint, GLenum, GLint);
typedef void(APIENTRY* PFN_SamplerParameterf)(GLuint, GLenum, GLfloat);
PFN_SamplerParameteri p_glSamplerParameteri;
PFN_SamplerParameterf p_glSamplerParameterf;
PFNGLCLIPCONTROLPROC p_glClipControl;
constexpr GLenum k_TEXTURE_MAX_ANISOTROPY = 0x84FE, k_MAX_TEXTURE_MAX_ANISOTROPY = 0x84FF;
constexpr GLenum k_LOWER_LEFT = 0x8CA1, k_NEGATIVE_ONE_TO_ONE = 0x935E, k_ZERO_TO_ONE = 0x935F;

template <class T> bool load(T& fp, const char* name) {
    fp = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    if (!fp) LOGW("native renderer: GL entry point %s missing", name);
    return fp != nullptr;
}

// ---------------------------------------------------------------- matrices (column-major, GL order)
struct Mat4 {
    float m[16];
    static Mat4 identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
        return r;
    }
    Mat4 operator*(const Mat4& b) const {  // this * b
        Mat4 r{};
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += m[k * 4 + rr] * b.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }
};

struct MatrixStack {
    std::vector<Mat4> st{Mat4::identity()};
    Mat4& top() { return st.back(); }
};
MatrixStack g_mv, g_proj, g_tex[2];
int g_matrix_mode = 0;  // 0 modelview, 1 projection, 2 texture
int g_active_unit = 0;
MatrixStack& cur_stack() {
    return g_matrix_mode == 1 ? g_proj : g_matrix_mode == 2 ? g_tex[g_active_unit & 1] : g_mv;
}

// ---------------------------------------------------------------- mirrored fixed-function state
struct Unit {
    bool enabled = false;
    GLuint tex = 0;
    GLenum env = GL_MODULATE;
    float env_color[4] = {0, 0, 0, 0};
};
struct State {
    Unit unit[2];
    float color[4] = {1, 1, 1, 1};
    bool blend = false, depth_test = false, depth_write = true, cull = false, alpha_test = false, fog = false,
         scissor_on = false, poly_offset = false;
    GLenum blend_src = GL_ONE, blend_dst = GL_ZERO, depth_func = GL_LESS, cull_mode = GL_BACK, front = GL_CCW,
           alpha_fn = GL_ALWAYS, fog_mode = GL_EXP;
    float alpha_ref = 0, fog_start = 0, fog_end = 1, fog_density = 1, fog_color[4] = {0, 0, 0, 0};
    bool mask[4] = {true, true, true, true};
    int sc[4] = {0, 0, 0, 0}, vp[4] = {0, 0, 0, 0};
    float po_factor = 0, po_units = 0;
} g_st;

Array g_arr[A_COUNT];

bool g_available = false, g_enabled = false;
bool g_trace_draws = false, g_trace_next = false;

// ---------------------------------------------------------------- shader
GLuint g_prog = 0, g_vao = 0, g_vbo = 0, g_ibo = 0;
GLint u_mvp, u_texmat0, u_tex0, u_use_tex0, u_use_vcolor, u_color, u_fog, u_fog_color, u_fog_range, u_alpha, u_debug;
std::unordered_map<GLuint, int> g_tex_kind;  // texture -> 1 RGB(A), 2 ALPHA, 3 LUMINANCE, 4 LUMINANCE_ALPHA
GLint u_revz;

// Per-texture facts for the enhanced filtering. Defaults are the GL ES texture defaults.
extern bool g_trace_draws;
struct TexMeta {
    GLenum min_f = GL_NEAREST_MIPMAP_LINEAR, mag_f = GL_LINEAR, wrap_s = GL_REPEAT, wrap_t = GL_REPEAT;
    bool game_mips = false;  // the game supplied / generated levels above 0
    bool dirty = true;       // base level changed since we last generated mipmaps
    bool target = false;     // rendered into (never give it generated mipmaps)
    bool our_mips = false;   // we generated a mip chain
};
std::unordered_map<GLuint, TexMeta> g_tex_meta;
int g_aniso = 1, g_max_aniso = 1;
bool g_samplers_ok = false, g_clip_ok = false, g_revz_wanted = true;
std::unordered_map<u64, GLuint> g_samplers;  // packed parameters -> sampler object
int g_legacy_unit = 0;                       // the game's active texture unit (restored after a draw)

bool is_mip_filter(GLenum f) { return f >= GL_NEAREST_MIPMAP_NEAREST && f <= GL_LINEAR_MIPMAP_LINEAR; }
bool is_linear_min(GLenum f) { return f == GL_LINEAR || f == GL_LINEAR_MIPMAP_NEAREST || f == GL_LINEAR_MIPMAP_LINEAR ||
                                      f == GL_NEAREST_MIPMAP_LINEAR; }

GLuint sampler_for(GLenum min_f, GLenum mag_f, GLenum ws, GLenum wt, int aniso) {
    u64 key = (u64(min_f & 0xffff) << 48) | (u64(mag_f & 0xffff) << 32) | (u64(ws & 0xff) << 24) | (u64(wt & 0xff) << 16) |
              u64(aniso & 0xff) | (u64(ws >> 8 & 0xf) << 12) | (u64(wt >> 8 & 0xf) << 8);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second;
    GLuint s = 0;
    p_glGenSamplers(1, &s);
    p_glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, GLint(min_f));
    p_glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, GLint(mag_f));
    p_glSamplerParameteri(s, GL_TEXTURE_WRAP_S, GLint(ws));
    p_glSamplerParameteri(s, GL_TEXTURE_WRAP_T, GLint(wt));
    p_glSamplerParameterf(s, k_TEXTURE_MAX_ANISOTROPY, float(std::max(1, aniso)));
    g_samplers[key] = s;
    return s;
}

// The sampler for the texture on unit 0 for this draw: the game's own parameters, or for 3D draws
// with filtering enabled, trilinear + anisotropic (generating the mip chain if the game gave none).
// The texture must be bound on the active unit.
GLuint choose_sampler(GLuint tex) {
    TexMeta& m = g_tex_meta[tex];
    GLenum min_f = m.min_f;
    int aniso = 1;
    bool enhance = g_aniso > 1 && g_st.depth_test && is_linear_min(m.min_f) && !m.target;
    if (enhance) {
        if (m.game_mips) {
            // only where the game itself samples the chain (then it is known to be complete)
            if (is_mip_filter(m.min_f)) min_f = GL_LINEAR_MIPMAP_LINEAR, aniso = g_aniso;
        } else {
            if (m.dirty) {
                p_glGenerateMipmap(GL_TEXTURE_2D);
                m.dirty = false;
                m.our_mips = true;
            }
            min_f = GL_LINEAR_MIPMAP_LINEAR;
            aniso = g_aniso;
        }
    }
    if (g_trace_draws)
        LOGI("[ntex] tex=%u 3d=%d game_min=%x game_mips=%d our_mips=%d target=%d -> min=%x aniso=%d", tex, g_st.depth_test,
             m.min_f, m.game_mips, m.our_mips, m.target, min_f, aniso);
    return sampler_for(min_f, m.mag_f, m.wrap_s, m.wrap_t, aniso);
}

bool use_revz() { return g_clip_ok && g_revz_wanted; }
GLenum flip_depth_func(GLenum f) {
    switch (f) {
    case GL_LESS: return GL_GREATER;
    case GL_LEQUAL: return GL_GEQUAL;
    case GL_GREATER: return GL_LESS;
    case GL_GEQUAL: return GL_LEQUAL;
    default: return f;
    }
}
int g_debug_hud = 0;
enum { AT_POS = 0, AT_COLOR = 1, AT_TEX0 = 2 };

const char* kVS = R"(#version 130
uniform mat4 u_mvp;
uniform mat4 u_texmat0;
uniform vec4 u_color;
uniform int u_use_vcolor;
uniform int u_revz;
in vec4 a_pos;
in vec4 a_color;
in vec4 a_tex0;
out vec4 v_color;
out vec4 v_tex0;
out float v_eyez;
void main() {
    gl_Position = u_mvp * a_pos;
    v_color = u_use_vcolor != 0 ? a_color : u_color;
    v_tex0 = u_texmat0 * a_tex0;
    v_eyez = gl_Position.w;  // eye-space distance for fog (GL uses |z_eye|; w == -z_eye for perspective)
    // reversed-Z with a [0,1] clip range: near plane -> depth 1, far plane -> 0
    if (u_revz != 0) gl_Position.z = (gl_Position.w - gl_Position.z) * 0.5;
}
)";

const char* kFS = R"(#version 130
uniform sampler2D u_tex0;
uniform int u_use_tex0;   // 0 off, 1 RGB/RGBA, 2 ALPHA (RGB passes through), 3 LUMINANCE, 4 LUMINANCE_ALPHA
uniform int u_debug;      // 1: flat magenta (geometry check)
uniform int u_fog;        // 0 off, 1 linear, 2 exp, 3 exp2
uniform vec4 u_fog_color;
uniform vec2 u_fog_range; // start, end (linear) / density in x (exp)
uniform vec2 u_alpha;     // test mode (0 off, 1 GEQUAL, 2 GREATER, 3 LESS, 4 LEQUAL, 5 EQUAL, 6 NOTEQUAL), ref
in vec4 v_color;
in vec4 v_tex0;
in float v_eyez;
out vec4 frag;
void main() {
    vec4 c = v_color;
    if (u_use_tex0 != 0) {
        vec4 t = texture(u_tex0, v_tex0.st / max(v_tex0.q, 1e-6));
        if (u_use_tex0 == 2) c.a *= t.a;                       // ALPHA: Cv = Cf, Av = Af*As
        else if (u_use_tex0 == 3) c.rgb *= t.rrr;              // LUMINANCE: Cv = Cf*L, Av = Af
        else if (u_use_tex0 == 4) { c.rgb *= t.rrr; c.a *= t.a; }
        else c *= t;
    }
    if (u_debug != 0) { frag = vec4(1.0, 0.0, 1.0, 1.0); return; }
    int am = int(u_alpha.x);
    if (am != 0) {
        float r = u_alpha.y;
        bool pass = am == 1 ? c.a >= r : am == 2 ? c.a > r : am == 3 ? c.a < r : am == 4 ? c.a <= r
                  : am == 5 ? c.a == r : c.a != r;
        if (!pass) discard;
    }
    if (u_fog != 0) {
        float d = abs(v_eyez), f;
        if (u_fog == 1) f = (u_fog_range.y - d) / max(u_fog_range.y - u_fog_range.x, 1e-6);
        else if (u_fog == 2) f = exp(-u_fog_range.x * d);
        else f = exp(-u_fog_range.x * u_fog_range.x * d * d);
        f = clamp(f, 0.0, 1.0);
        c.rgb = mix(u_fog_color.rgb, c.rgb, f);
    }
    frag = c;
}
)";

GLuint compile(GLenum type, const char* src) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, nullptr);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetShaderInfoLog(s, sizeof log, nullptr, log);
        LOGE("native renderer: shader compile failed: %s", log);
        return 0;
    }
    return s;
}

// ---------------------------------------------------------------- vertex conversion
// Converts the enabled arrays for vertices [lo, hi] into one interleaved float stream:
// pos(4) color(4) tex0(4) per vertex.
std::vector<float> g_verts;
constexpr int kStride = 12;

float read_component(const u8* e, GLenum type, int k) {
    switch (type) {
    case GL_BYTE: return float(static_cast<const signed char*>(static_cast<const void*>(e))[k]);
    case GL_UNSIGNED_BYTE: return float(e[k]);
    case GL_SHORT: { int16_t v; std::memcpy(&v, e + 2 * k, 2); return v; }
    case GL_UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, e + 2 * k, 2); return v; }
    case 0x140C: { int32_t v; std::memcpy(&v, e + 4 * k, 4); return v / 65536.0f; }  // GL_FIXED
    default: { float v; std::memcpy(&v, e + 4 * k, 4); return v; }
    }
}
int type_size(GLenum t) {
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    default: return 4;
    }
}

void build_vertices(u32 lo, u32 n) {
    g_verts.assign(size_t(n) * kStride, 0.0f);
    for (u32 i = 0; i < n; i++) {
        float* v = &g_verts[size_t(i) * kStride];
        v[3] = 1;                               // pos.w
        v[4] = v[5] = v[6] = v[7] = 1;          // color
        v[8] = v[9] = 0, v[10] = 0, v[11] = 1;  // tex0 (s, t, r, q)
    }
    struct Slot { int which, off, max; };
    const Slot slots[] = {{A_VERTEX, 0, 4}, {A_COLOR, 4, 4}, {A_TEX0, 8, 4}};
    for (auto& s : slots) {
        const Array& a = g_arr[s.which];
        if (!a.enabled || !a.data) continue;
        int stride = a.stride ? a.stride : a.size * type_size(a.type);
        int cnt = std::min(a.size, s.max);
        bool norm_color = s.which == A_COLOR && a.type == GL_UNSIGNED_BYTE;
        for (u32 i = 0; i < n; i++) {
            const u8* e = a.data + size_t(lo + i) * stride;
            float* v = &g_verts[size_t(i) * kStride + s.off];
            for (int k = 0; k < cnt; k++) v[k] = read_component(e, a.type, k) * (norm_color ? 1.0f / 255.0f : 1.0f);
        }
    }
}

void apply_state() {
    // depth / blend / cull / scissor / masks: plain GL state, same enums as the game used
    auto en = [](GLenum cap, bool on) { on ? glEnable(cap) : glDisable(cap); };
    en(GL_DEPTH_TEST, g_st.depth_test);
    bool revz = use_revz();
    glDepthFunc(revz ? flip_depth_func(g_st.depth_func) : g_st.depth_func);
    glDepthMask(g_st.depth_write);
    if (g_clip_ok) p_glClipControl(k_LOWER_LEFT, revz ? k_ZERO_TO_ONE : k_NEGATIVE_ONE_TO_ONE);
    p_glUniform1i(u_revz, revz ? 1 : 0);
    en(GL_BLEND, g_st.blend);
    glBlendFunc(g_st.blend_src, g_st.blend_dst);
    en(GL_CULL_FACE, g_st.cull);
    glCullFace(g_st.cull_mode);
    glFrontFace(g_st.front);
    en(GL_SCISSOR_TEST, g_st.scissor_on);
    if (g_st.scissor_on) glScissor(g_st.sc[0], g_st.sc[1], g_st.sc[2], g_st.sc[3]);
    glColorMask(g_st.mask[0], g_st.mask[1], g_st.mask[2], g_st.mask[3]);
    en(GL_POLYGON_OFFSET_FILL, g_st.poly_offset);
    if (g_st.poly_offset) glPolygonOffset(revz ? -g_st.po_factor : g_st.po_factor, revz ? -g_st.po_units : g_st.po_units);
    glDisable(GL_ALPHA_TEST);  // done in the shader
    glDisable(GL_FOG);
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);

    Mat4 mvp = g_proj.top() * g_mv.top();
    p_glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp.m);
    p_glUniformMatrix4fv(u_texmat0, 1, GL_FALSE, g_tex[0].top().m);
    p_glUniform4fv(u_color, 1, g_st.color);
    p_glUniform1i(u_use_vcolor, g_arr[A_COLOR].enabled ? 1 : 0);
    bool tex = g_st.unit[0].enabled && g_st.unit[0].tex != 0 && g_arr[A_TEX0].enabled;
    int kind = 1;
    if (tex) {
        auto it = g_tex_kind.find(g_st.unit[0].tex);
        if (it != g_tex_kind.end()) kind = it->second;
    }
    p_glUniform1i(u_use_tex0, tex ? kind : 0);
    p_glUniform1i(u_debug, g_debug_hud && !g_st.depth_test ? 1 : 0);
    if (tex) {
        p_glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_st.unit[0].tex);
        p_glUniform1i(u_tex0, 0);
        if (g_samplers_ok) p_glBindSampler(0, choose_sampler(g_st.unit[0].tex));
    }
    int fog = !g_st.fog ? 0 : g_st.fog_mode == GL_LINEAR ? 1 : g_st.fog_mode == GL_EXP ? 2 : 3;
    p_glUniform1i(u_fog, fog);
    p_glUniform4fv(u_fog_color, 1, g_st.fog_color);
    p_glUniform2f(u_fog_range, fog == 1 ? g_st.fog_start : g_st.fog_density, g_st.fog_end);
    int am = 0;
    if (g_st.alpha_test) {
        switch (g_st.alpha_fn) {
        case GL_GEQUAL: am = 1; break;
        case GL_GREATER: am = 2; break;
        case GL_LESS: am = 3; break;
        case GL_LEQUAL: am = 4; break;
        case GL_EQUAL: am = 5; break;
        case GL_NOTEQUAL: am = 6; break;
        case GL_NEVER: am = 2; break;  // GREATER than 1 never passes: ref forced below
        default: am = 0; break;
        }
    }
    p_glUniform2f(u_alpha, float(am), g_st.alpha_fn == GL_NEVER ? 2.0f : g_st.alpha_ref);
}

void upload_and_draw(GLenum mode, const std::vector<u16>& idx) {
    p_glBindVertexArray(g_vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(g_verts.size() * sizeof(float)), g_verts.data(), GL_STREAM_DRAW);
    const GLsizei st = kStride * sizeof(float);
    p_glEnableVertexAttribArray(AT_POS);
    p_glVertexAttribPointer(AT_POS, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<const void*>(0));
    p_glEnableVertexAttribArray(AT_COLOR);
    p_glVertexAttribPointer(AT_COLOR, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<const void*>(4 * sizeof(float)));
    p_glEnableVertexAttribArray(AT_TEX0);
    p_glVertexAttribPointer(AT_TEX0, 4, GL_FLOAT, GL_FALSE, st, reinterpret_cast<const void*>(8 * sizeof(float)));
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * 2), idx.data(), GL_STREAM_DRAW);
    glDrawElements(mode, GLsizei(idx.size()), GL_UNSIGNED_SHORT, nullptr);
    if (g_samplers_ok) p_glBindSampler(0, 0);
    if (g_clip_ok) p_glClipControl(k_LOWER_LEFT, k_NEGATIVE_ONE_TO_ONE);
    p_glActiveTexture(GL_TEXTURE0 + g_legacy_unit);
    p_glBindVertexArray(0);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

// ---------------------------------------------------------------- post-processing
// Bloom and sharpening on the 3D scene. Runs once per frame on the native path, right before the
// game's first HUD draw (so the HUD stays untouched), or at the end of the frame if it drew none:
//   scene copy (MSAA resolved) -> bright pass at 1/2 -> blurred 1/2, 1/4, 1/8 -> composite back
PFNGLGENFRAMEBUFFERSPROC p_glGenFramebuffers;
PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer;
PFNGLFRAMEBUFFERTEXTURE2DPROC p_glFramebufferTexture2D;
PFNGLBLITFRAMEBUFFERPROC p_glBlitFramebuffer;
PFNGLCHECKFRAMEBUFFERSTATUSPROC p_glCheckFramebufferStatus;

int g_bloom = 0, g_sharpen = 0, g_ao = 0;  // 0 off, 1 low, 2 medium, 3 high
float g_ao_gain = 6.0f, g_ao_radius = 0.0f;  // dev tuning (radius 0 = per level)
bool g_ao_debug = false, g_have_world_proj = false;
Mat4 g_world_proj;  // projection of the frame's 3D scene (for ambient occlusion)
bool g_seen_world = false, g_post_done = false;
int g_post_state = 0;  // 0 not initialised, 1 ready, -1 unavailable

struct Target {
    GLuint fbo = 0, tex = 0;
    int w = 0, h = 0;
};
Target g_scene, g_ba[3], g_bb[3];  // scene copy; bloom levels (and their blur scratch)
Target g_aoa, g_aob;                // ambient occlusion (and its blur scratch)
GLuint g_scene_depth = 0, g_p_ao = 0, g_p_aoblur = 0;
GLuint g_p_bright = 0, g_p_blur = 0, g_p_down = 0, g_p_final = 0, g_post_vao = 0, g_post_vbo = 0;

const char* kPostVS = R"(#version 130
in vec2 a_p;
out vec2 uv;
void main() { uv = a_p * 0.5 + 0.5; gl_Position = vec4(a_p, 0.0, 1.0); }
)";
// 2x2 box downsample keeping only bright areas (soft threshold on the brightest channel)
const char* kBrightFS = R"(#version 130
uniform sampler2D u_src;
uniform vec2 u_px;  // source texel size
in vec2 uv;
out vec4 frag;
void main() {
    vec3 c = (texture(u_src, uv + u_px * vec2(-0.5, -0.5)).rgb + texture(u_src, uv + u_px * vec2(0.5, -0.5)).rgb +
              texture(u_src, uv + u_px * vec2(-0.5, 0.5)).rgb + texture(u_src, uv + u_px * vec2(0.5, 0.5)).rgb) * 0.25;
    float l = max(c.r, max(c.g, c.b));
    frag = vec4(c * smoothstep(0.72, 1.0, l), 1.0);
}
)";
const char* kDownFS = R"(#version 130
uniform sampler2D u_src;
uniform vec2 u_px;
in vec2 uv;
out vec4 frag;
void main() {
    frag = (texture(u_src, uv + u_px * vec2(-0.5, -0.5)) + texture(u_src, uv + u_px * vec2(0.5, -0.5)) +
            texture(u_src, uv + u_px * vec2(-0.5, 0.5)) + texture(u_src, uv + u_px * vec2(0.5, 0.5))) * 0.25;
}
)";
// 9-tap gaussian as 5 bilinear taps; u_dir = one texel along the blur axis
const char* kBlurFS = R"(#version 130
uniform sampler2D u_src;
uniform vec2 u_dir;
in vec2 uv;
out vec4 frag;
void main() {
    vec4 c = texture(u_src, uv) * 0.2270270270;
    c += (texture(u_src, uv + u_dir * 1.3846153846) + texture(u_src, uv - u_dir * 1.3846153846)) * 0.3162162162;
    c += (texture(u_src, uv + u_dir * 3.2307692308) + texture(u_src, uv - u_dir * 3.2307692308)) * 0.0702702703;
    frag = c;
}
)";
// Ambient occlusion from the depth buffer alone: positions are rebuilt with the inverse projection,
// the surface normal comes from neighbouring pixels, and samples on a rotated spiral darken the pixel
// when they lie in front of its surface. Output: r = occlusion, gb = view distance (for the blur).
const char* kAoFS = R"(#version 130
uniform sampler2D u_depth;
uniform mat4 u_invp, u_p;
uniform vec2 u_px;
uniform int u_revz, u_n;
uniform float u_radius, u_gain;
in vec2 uv;
out vec4 frag;
vec4 vpos(vec2 t) {  // xyz = view-space position, w = view distance
    float d = texture(u_depth, t).r;
    float z = u_revz != 0 ? 1.0 - 2.0 * d : 2.0 * d - 1.0;
    vec4 p = u_invp * vec4(t * 2.0 - 1.0, z, 1.0);
    return vec4(p.xyz / p.w, abs(1.0 / p.w));
}
void main() {
    vec4 c = vpos(uv);
    float f = clamp(c.w / 64.0, 0.0, 1.0) * 255.0;
    vec2 packed = vec2(floor(f) / 255.0, fract(f));
    if (c.w > 60.0) { frag = vec4(1.0, packed, 1.0); return; }
    vec3 p = c.xyz;
    vec3 xa = vpos(uv + vec2(u_px.x, 0.0)).xyz - p, xb = p - vpos(uv - vec2(u_px.x, 0.0)).xyz;
    vec3 ya = vpos(uv + vec2(0.0, u_px.y)).xyz - p, yb = p - vpos(uv - vec2(0.0, u_px.y)).xyz;
    vec3 dx = dot(xa, xa) < dot(xb, xb) ? xa : xb, dy = dot(ya, ya) < dot(yb, yb) ? ya : yb;
    vec3 n = normalize(cross(dx, dy));
    if (dot(n, p) > 0.0) n = -n;
    float ang = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    mat2 pj = 0.5 * mat2(u_p[0].xy, u_p[1].xy);
    float bias = 0.012 + 0.004 * c.w, sum = 0.0, r2 = u_radius * u_radius;
    for (int i = 0; i < u_n; i++) {
        float t = (float(i) + 0.5) / float(u_n), a = ang + float(i) * 2.3999632;
        vec2 o = pj * (vec2(cos(a), sin(a)) * (sqrt(t) * u_radius)) / c.w;
        vec3 v = vpos(uv + o).xyz - p;
        float vv = dot(v, v);
        sum += max(0.0, dot(v, n) - bias - 0.15 * sqrt(vv)) / (vv + 0.0001) * max(0.0, 1.0 - vv / r2);
    }
    frag = vec4(clamp(1.0 - sum * u_radius * u_gain / float(u_n), 0.0, 1.0), packed, 1.0);
}
)";
// 9-tap blur that does not cross depth edges
const char* kAoBlurFS = R"(#version 130
uniform sampler2D u_src;
uniform vec2 u_dir;
in vec2 uv;
out vec4 frag;
float dist(vec4 s) { return (s.g * 255.0 + s.b) * (64.0 / 255.0); }
void main() {
    vec4 c = texture(u_src, uv);
    float d0 = dist(c), sum = c.r * 0.2270270270, wsum = 0.2270270270;
    float gw[4] = float[4](0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162);
    for (int i = 1; i <= 4; i++) {
        for (int s = -1; s <= 1; s += 2) {
            vec4 t = texture(u_src, uv + u_dir * float(i * s));
            float w = gw[i - 1] * max(0.0, 1.0 - abs(dist(t) - d0) / (0.04 + 0.03 * d0));
            sum += t.r * w;
            wsum += w;
        }
    }
    frag = vec4(sum / wsum, c.gb, 1.0);
}
)";
const char* kFinalFS = R"(#version 130
uniform sampler2D u_scene, u_b1, u_b2, u_b3, u_ao;
uniform float u_aos;    // ambient occlusion strength, 0 = off
uniform int u_aodebug;  // show the occlusion term alone
uniform vec2 u_px;      // scene texel size
uniform float u_sharp;  // 0 = off
uniform float u_bloom;  // 0 = off
in vec2 uv;
out vec4 frag;
void main() {
    vec3 c = texture(u_scene, uv).rgb;
    if (u_sharp > 0.0) {
        vec3 n = texture(u_scene, uv + vec2(0.0, u_px.y)).rgb, s = texture(u_scene, uv - vec2(0.0, u_px.y)).rgb;
        vec3 e = texture(u_scene, uv + vec2(u_px.x, 0.0)).rgb, w = texture(u_scene, uv - vec2(u_px.x, 0.0)).rgb;
        vec3 lo = min(c, min(min(n, s), min(e, w))), hi = max(c, max(max(n, s), max(e, w)));
        // unsharp mask, limited to slightly beyond the local range (no halos)
        c = clamp(c + (4.0 * c - n - s - e - w) * u_sharp, lo - 0.04, hi + 0.04);
    }
    if (u_aos > 0.0) {
        // bright pixels (lights, glows, light shafts) are left alone
        float l = max(c.r, max(c.g, c.b));
        float a = clamp(mix(1.0, texture(u_ao, uv).r, u_aos * (1.0 - smoothstep(0.55, 0.95, l))), 0.0, 1.0);
        c *= a;
        if (u_aodebug != 0) c = vec3(clamp(mix(1.0, texture(u_ao, uv).r, u_aos), 0.0, 1.0));
    }
    if (u_bloom > 0.0 && u_aodebug == 0) {
        // average of the three blur levels, screen-blended: areas that are already bright gain little
        vec3 b = min((texture(u_b1, uv).rgb + texture(u_b2, uv).rgb + texture(u_b3, uv).rgb) * (u_bloom / 3.0), 1.0);
        c = c + b * (1.0 - clamp(c, 0.0, 1.0));
    }
    frag = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)";

GLuint link_post(const char* fs_src) {
    GLuint vs = compile(GL_VERTEX_SHADER, kPostVS), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, vs);
    p_glAttachShader(p, fs);
    p_glBindAttribLocation(p, 0, "a_p");
    p_glLinkProgram(p);
    GLint ok = 0;
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

bool make_target(Target& t, int w, int h) {
    t.w = std::max(1, w);
    t.h = std::max(1, h);
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);  // CLAMP_TO_EDGE
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    p_glGenFramebuffers(1, &t.fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    return p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

// Creates the programs and render targets (framebuffer / texture bindings are left changed).
bool post_init() {
    if (g_post_state != 0) return g_post_state > 0;
    g_post_state = -1;
    p_glGenFramebuffers = reinterpret_cast<PFNGLGENFRAMEBUFFERSPROC>(SDL_GL_GetProcAddress("glGenFramebuffers"));
    p_glBindFramebuffer = reinterpret_cast<PFNGLBINDFRAMEBUFFERPROC>(SDL_GL_GetProcAddress("glBindFramebuffer"));
    p_glFramebufferTexture2D = reinterpret_cast<PFNGLFRAMEBUFFERTEXTURE2DPROC>(SDL_GL_GetProcAddress("glFramebufferTexture2D"));
    p_glBlitFramebuffer = reinterpret_cast<PFNGLBLITFRAMEBUFFERPROC>(SDL_GL_GetProcAddress("glBlitFramebuffer"));
    p_glCheckFramebufferStatus = reinterpret_cast<PFNGLCHECKFRAMEBUFFERSTATUSPROC>(SDL_GL_GetProcAddress("glCheckFramebufferStatus"));
    if (!p_glGenFramebuffers || !p_glBindFramebuffer || !p_glFramebufferTexture2D || !p_glBlitFramebuffer ||
        !p_glCheckFramebufferStatus)
        return false;
    g_p_bright = link_post(kBrightFS);
    g_p_blur = link_post(kBlurFS);
    g_p_down = link_post(kDownFS);
    g_p_final = link_post(kFinalFS);
    g_p_ao = link_post(kAoFS);
    g_p_aoblur = link_post(kAoBlurFS);
    if (!g_p_bright || !g_p_blur || !g_p_down || !g_p_final || !g_p_ao || !g_p_aoblur) {
        LOGW("post-processing: shaders failed to build; disabled");
        return false;
    }
    int w = g_config.screen_w, h = g_config.screen_h;
    bool ok = make_target(g_scene, w, h);
    for (int i = 0; i < 3; i++) {
        ok &= make_target(g_ba[i], w >> (i + 1), h >> (i + 1));
        ok &= make_target(g_bb[i], w >> (i + 1), h >> (i + 1));
    }
    ok &= make_target(g_aoa, w, h);
    ok &= make_target(g_aob, w, h);
    // depth copy of the scene (same format as the game framebuffer, so it can be blitted)
    bool d24 = hle::g_depth_fmt == 0x88F0;  // GL_DEPTH24_STENCIL8
    glGenTextures(1, &g_scene_depth);
    glBindTexture(GL_TEXTURE_2D, g_scene_depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GLint(hle::g_depth_fmt), w, h, 0, 0x84F9 /*DEPTH_STENCIL*/,
                 d24 ? 0x84FA /*UNSIGNED_INT_24_8*/ : 0x8DAD /*FLOAT_32_UNSIGNED_INT_24_8_REV*/, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_scene.fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, 0x821A /*DEPTH_STENCIL_ATTACHMENT*/, GL_TEXTURE_2D, g_scene_depth, 0);
    ok &= p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!ok) {
        LOGW("post-processing: render targets incomplete; disabled");
        return false;
    }
    static const float tri[6] = {-1, -1, 3, -1, -1, 3};  // one triangle covering the screen
    p_glGenVertexArrays(1, &g_post_vao);
    p_glBindVertexArray(g_post_vao);
    p_glGenBuffers(1, &g_post_vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g_post_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    p_glBindVertexArray(0);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    g_post_state = 1;
    LOGI("post-processing: ready (%dx%d)", w, h);
    return true;
}

void post_pass(GLuint prog, const Target& dst, GLuint src, float ux, float uy, const char* uname) {
    p_glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    glViewport(0, 0, dst.w, dst.h);
    p_glUseProgram(prog);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    p_glUniform1i(p_glGetUniformLocation(prog, "u_src"), 0);
    p_glUniform2f(p_glGetUniformLocation(prog, uname), ux, uy);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

bool post_wanted() { return g_bloom > 0 || g_sharpen > 0 || g_ao > 0; }

bool invert(const Mat4& a, Mat4& out) {
    double m[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) m[r][c] = c < 4 ? a.m[c * 4 + r] : (c - 4 == r ? 1.0 : 0.0);
    for (int i = 0; i < 4; i++) {
        int piv = i;
        for (int r = i + 1; r < 4; r++)
            if (std::fabs(m[r][i]) > std::fabs(m[piv][i])) piv = r;
        if (std::fabs(m[piv][i]) < 1e-12) return false;
        std::swap(m[i], m[piv]);
        double d = m[i][i];
        for (int c = 0; c < 8; c++) m[i][c] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == i) continue;
            double f = m[r][i];
            for (int c = 0; c < 8; c++) m[r][c] -= f * m[i][c];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out.m[c * 4 + r] = float(m[r][c + 4]);
    return true;
}

// Applies the effects to what has been drawn into the game framebuffer so far.
void run_post() {
    g_post_done = true;
    GLint fb = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    if (GLuint(fb) != hle::g_default_fbo) return;  // the game is drawing into a target of its own
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    bool ready = post_init();
    if (ready) {
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_FOG);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDepthMask(GL_FALSE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (g_samplers_ok)
            for (int u = 0; u < 4; u++) p_glBindSampler(GLuint(u), 0);
        // scene copy (resolves MSAA)
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, hle::g_default_fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_scene.fbo);
        Mat4 inv_proj;
        bool ao = g_ao > 0 && g_have_world_proj && invert(g_world_proj, inv_proj);
        p_glBlitFramebuffer(0, 0, g_scene.w, g_scene.h, 0, 0, g_scene.w, g_scene.h,
                            GL_COLOR_BUFFER_BIT | (ao ? GL_DEPTH_BUFFER_BIT : 0), GL_NEAREST);
        p_glBindVertexArray(g_post_vao);
        static const float kAoStrength[4] = {0.0f, 0.55f, 0.8f, 1.0f}, kAoRadius[4] = {0.0f, 0.35f, 0.45f, 0.55f};
        static const int kAoSamples[4] = {0, 8, 12, 16};
        if (ao) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                const float* m = g_world_proj.m;
                LOGI("ambient occlusion: scene projection [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
                     m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            }
            p_glBindFramebuffer(GL_FRAMEBUFFER, g_aoa.fbo);
            glViewport(0, 0, g_aoa.w, g_aoa.h);
            p_glUseProgram(g_p_ao);
            p_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_scene_depth);
            p_glUniform1i(p_glGetUniformLocation(g_p_ao, "u_depth"), 0);
            p_glUniformMatrix4fv(p_glGetUniformLocation(g_p_ao, "u_invp"), 1, GL_FALSE, inv_proj.m);
            p_glUniformMatrix4fv(p_glGetUniformLocation(g_p_ao, "u_p"), 1, GL_FALSE, g_world_proj.m);
            p_glUniform2f(p_glGetUniformLocation(g_p_ao, "u_px"), 1.0f / g_aoa.w, 1.0f / g_aoa.h);
            p_glUniform1i(p_glGetUniformLocation(g_p_ao, "u_revz"), use_revz() ? 1 : 0);
            p_glUniform1i(p_glGetUniformLocation(g_p_ao, "u_n"), kAoSamples[g_ao]);
            p_glUniform1f(p_glGetUniformLocation(g_p_ao, "u_radius"), g_ao_radius > 0 ? g_ao_radius : kAoRadius[g_ao]);
            p_glUniform1f(p_glGetUniformLocation(g_p_ao, "u_gain"), g_ao_gain);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            post_pass(g_p_aoblur, g_aob, g_aoa.tex, 1.0f / g_aoa.w, 0.0f, "u_dir");
            post_pass(g_p_aoblur, g_aoa, g_aob.tex, 0.0f, 1.0f / g_aoa.h, "u_dir");
        }
        if (g_bloom > 0) {
            post_pass(g_p_bright, g_ba[0], g_scene.tex, 1.0f / g_scene.w, 1.0f / g_scene.h, "u_px");
            for (int i = 0; i < 3; i++) {
                if (i > 0) post_pass(g_p_down, g_ba[i], g_ba[i - 1].tex, 1.0f / g_ba[i - 1].w, 1.0f / g_ba[i - 1].h, "u_px");
                post_pass(g_p_blur, g_bb[i], g_ba[i].tex, 1.0f / g_ba[i].w, 0.0f, "u_dir");
                post_pass(g_p_blur, g_ba[i], g_bb[i].tex, 0.0f, 1.0f / g_ba[i].h, "u_dir");
            }
        }
        // composite back into the game framebuffer
        static const float kSharp[4] = {0.0f, 0.12f, 0.25f, 0.45f}, kBloom[4] = {0.0f, 0.20f, 0.40f, 0.65f};
        p_glBindFramebuffer(GL_FRAMEBUFFER, hle::g_default_fbo);
        glViewport(0, 0, g_scene.w, g_scene.h);
        p_glUseProgram(g_p_final);
        const char* names[5] = {"u_scene", "u_b1", "u_b2", "u_b3", "u_ao"};
        const GLuint texs[5] = {g_scene.tex, g_ba[0].tex, g_ba[1].tex, g_ba[2].tex, g_aoa.tex};
        for (int u = 0; u < 5; u++) {
            p_glActiveTexture(GL_TEXTURE0 + u);
            glBindTexture(GL_TEXTURE_2D, texs[u]);
            p_glUniform1i(p_glGetUniformLocation(g_p_final, names[u]), u);
        }
        p_glUniform2f(p_glGetUniformLocation(g_p_final, "u_px"), 1.0f / g_scene.w, 1.0f / g_scene.h);
        p_glUniform1f(p_glGetUniformLocation(g_p_final, "u_sharp"), kSharp[g_sharpen]);
        p_glUniform1f(p_glGetUniformLocation(g_p_final, "u_bloom"), kBloom[g_bloom]);
        p_glUniform1f(p_glGetUniformLocation(g_p_final, "u_aos"), ao ? kAoStrength[g_ao] : 0.0f);
        p_glUniform1i(p_glGetUniformLocation(g_p_final, "u_aodebug"), g_ao_debug ? 1 : 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        p_glBindVertexArray(0);
        p_glUseProgram(0);
    }
    // back to what the game and the GLES layer expect (draw state is re-applied by the next draw)
    p_glBindFramebuffer(GL_FRAMEBUFFER, hle::g_default_fbo);
    for (int u = 4; u >= 0; u--) {
        p_glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, u < 2 ? g_st.unit[u].tex : 0);
    }
    p_glActiveTexture(GL_TEXTURE0 + g_legacy_unit);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glDepthMask(g_st.depth_write);
}

// Called at the start of every native draw: the first pixel-space (HUD) draw after world geometry
// marks the end of the 3D scene.
void post_before_draw() {
    if (g_st.depth_test) {
        g_seen_world = true;
        if (!g_post_done && g_proj.top().m[11] != 0.0f) {
            g_world_proj = g_proj.top();
            g_have_world_proj = true;
        }
        return;
    }
    if (!g_seen_world || g_post_done || !post_wanted()) return;
    const Mat4& p = g_proj.top();
    Mat4 mvp = p * g_mv.top();
    bool pixel_space = p.m[11] == 0.0f && p.m[15] == 1.0f && std::fabs(mvp.m[0]) < 0.05f;
    if (pixel_space) run_post();
}

std::vector<u16> g_idx;

}  // namespace

// ---------------------------------------------------------------- public API
bool init() {
    bool ok = true;
    ok &= load(p_glCreateShader, "glCreateShader");
    ok &= load(p_glShaderSource, "glShaderSource");
    ok &= load(p_glCompileShader, "glCompileShader");
    ok &= load(p_glGetShaderiv, "glGetShaderiv");
    ok &= load(p_glGetShaderInfoLog, "glGetShaderInfoLog");
    ok &= load(p_glCreateProgram, "glCreateProgram");
    ok &= load(p_glAttachShader, "glAttachShader");
    ok &= load(p_glLinkProgram, "glLinkProgram");
    ok &= load(p_glGetProgramiv, "glGetProgramiv");
    ok &= load(p_glGetProgramInfoLog, "glGetProgramInfoLog");
    ok &= load(p_glUseProgram, "glUseProgram");
    ok &= load(p_glGetUniformLocation, "glGetUniformLocation");
    ok &= load(p_glUniform1i, "glUniform1i");
    ok &= load(p_glUniform1f, "glUniform1f");
    ok &= load(p_glUniform2f, "glUniform2f");
    ok &= load(p_glUniform4fv, "glUniform4fv");
    ok &= load(p_glUniformMatrix4fv, "glUniformMatrix4fv");
    ok &= load(p_glBindAttribLocation, "glBindAttribLocation");
    ok &= load(p_glGenVertexArrays, "glGenVertexArrays");
    ok &= load(p_glBindVertexArray, "glBindVertexArray");
    ok &= load(p_glGenBuffers, "glGenBuffers");
    ok &= load(p_glBindBuffer, "glBindBuffer");
    ok &= load(p_glBufferData, "glBufferData");
    ok &= load(p_glEnableVertexAttribArray, "glEnableVertexAttribArray");
    ok &= load(p_glDisableVertexAttribArray, "glDisableVertexAttribArray");
    ok &= load(p_glVertexAttribPointer, "glVertexAttribPointer");
    ok &= load(p_glVertexAttrib4f, "glVertexAttrib4f");
    ok &= load(p_glActiveTexture, "glActiveTexture");
    if (!ok) return false;
    ok &= load(p_glGenerateMipmap, "glGenerateMipmap");
    if (!ok) return false;
    // optional features (missing ones only switch the matching enhancement off)
    p_glGenSamplers = reinterpret_cast<PFNGLGENSAMPLERSPROC>(SDL_GL_GetProcAddress("glGenSamplers"));
    p_glBindSampler = reinterpret_cast<PFNGLBINDSAMPLERPROC>(SDL_GL_GetProcAddress("glBindSampler"));
    p_glSamplerParameteri = reinterpret_cast<PFN_SamplerParameteri>(SDL_GL_GetProcAddress("glSamplerParameteri"));
    p_glSamplerParameterf = reinterpret_cast<PFN_SamplerParameterf>(SDL_GL_GetProcAddress("glSamplerParameterf"));
    g_samplers_ok = p_glGenSamplers && p_glBindSampler && p_glSamplerParameteri && p_glSamplerParameterf;
    p_glClipControl = reinterpret_cast<PFNGLCLIPCONTROLPROC>(SDL_GL_GetProcAddress("glClipControl"));
    g_clip_ok = p_glClipControl != nullptr;
    if (g_samplers_ok) {
        GLfloat m = 1;
        glGetFloatv(k_MAX_TEXTURE_MAX_ANISOTROPY, &m);
        if (glGetError() != GL_NO_ERROR) m = 1;
        g_max_aniso = std::clamp(int(m), 1, 16);
    }
    GLuint vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, kFS);
    if (!vs || !fs) return false;
    g_prog = p_glCreateProgram();
    p_glAttachShader(g_prog, vs);
    p_glAttachShader(g_prog, fs);
    p_glBindAttribLocation(g_prog, AT_POS, "a_pos");
    p_glBindAttribLocation(g_prog, AT_COLOR, "a_color");
    p_glBindAttribLocation(g_prog, AT_TEX0, "a_tex0");
    p_glLinkProgram(g_prog);
    GLint linked = 0;
    p_glGetProgramiv(g_prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048];
        p_glGetProgramInfoLog(g_prog, sizeof log, nullptr, log);
        LOGE("native renderer: link failed: %s", log);
        return false;
    }
    auto U = [&](const char* n) { return p_glGetUniformLocation(g_prog, n); };
    u_mvp = U("u_mvp");
    u_texmat0 = U("u_texmat0");
    u_tex0 = U("u_tex0");
    u_use_tex0 = U("u_use_tex0");
    u_use_vcolor = U("u_use_vcolor");
    u_color = U("u_color");
    u_fog = U("u_fog");
    u_fog_color = U("u_fog_color");
    u_fog_range = U("u_fog_range");
    u_alpha = U("u_alpha");
    u_debug = U("u_debug");
    u_revz = U("u_revz");
    p_glGenVertexArrays(1, &g_vao);
    p_glGenBuffers(1, &g_vbo);
    p_glGenBuffers(1, &g_ibo);
    g_available = true;
    LOGI("native renderer: ready (shader path; anisotropic filtering up to %dx%s; reversed-Z %s)", g_max_aniso,
         g_samplers_ok ? "" : " - no sampler objects", g_clip_ok ? "available" : "unavailable");
    return true;
}

bool available() { return g_available; }
void set_enabled(bool on) {
    bool was = g_enabled;
    g_enabled = on && g_available;
    if (was != g_enabled) LOGI("renderer: %s", g_enabled ? "native" : "legacy");
}
bool enabled() { return g_enabled; }

// ---- matrices
void matrix_mode(GLenum mode) { g_matrix_mode = mode == GL_PROJECTION ? 1 : mode == GL_TEXTURE ? 2 : 0; }
void load_identity() { cur_stack().top() = Mat4::identity(); }
void load_matrix(const float* m) {
    std::memcpy(cur_stack().top().m, m, 64);
    // debug aid: the first perspective projection tells the camera's near / far planes
    static bool logged = false;
    if (!logged && g_matrix_mode == 1 && m[11] == -1.0f && m[10] != 1.0f && m[10] != -1.0f) {
        logged = true;
        LOGI("camera planes: near %.3f, far %.1f", m[14] / (m[10] - 1.0f), m[14] / (m[10] + 1.0f));
    }
}
void mult_matrix(const float* m) {
    Mat4 b;
    std::memcpy(b.m, m, 64);
    cur_stack().top() = cur_stack().top() * b;
}
void push_matrix() {
    auto& s = cur_stack();
    if (s.st.size() < 64) s.st.push_back(s.top());
}
void pop_matrix() {
    auto& s = cur_stack();
    if (s.st.size() > 1) s.st.pop_back();
}
void translate(float x, float y, float z) {
    Mat4 t = Mat4::identity();
    t.m[12] = x, t.m[13] = y, t.m[14] = z;
    mult_matrix(t.m);
}
void scale(float x, float y, float z) {
    Mat4 t = Mat4::identity();
    t.m[0] = x, t.m[5] = y, t.m[10] = z;
    mult_matrix(t.m);
}
void rotate(float deg, float x, float y, float z) {
    float len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-8f) return;
    x /= len, y /= len, z /= len;
    float a = deg * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a), t = 1 - c;
    Mat4 r = Mat4::identity();
    r.m[0] = t * x * x + c, r.m[1] = t * x * y + s * z, r.m[2] = t * x * z - s * y;
    r.m[4] = t * x * y - s * z, r.m[5] = t * y * y + c, r.m[6] = t * y * z + s * x;
    r.m[8] = t * x * z + s * y, r.m[9] = t * y * z - s * x, r.m[10] = t * z * z + c;
    mult_matrix(r.m);
}
void frustum(float l, float r, float b, float t, float n, float f) {
    Mat4 m{};
    m.m[0] = 2 * n / (r - l);
    m.m[5] = 2 * n / (t - b);
    m.m[8] = (r + l) / (r - l);
    m.m[9] = (t + b) / (t - b);
    m.m[10] = -(f + n) / (f - n);
    m.m[11] = -1;
    m.m[14] = -2 * f * n / (f - n);
    mult_matrix(m.m);
}
void ortho(float l, float r, float b, float t, float n, float f) {
    Mat4 m = Mat4::identity();
    m.m[0] = 2 / (r - l);
    m.m[5] = 2 / (t - b);
    m.m[10] = -2 / (f - n);
    m.m[12] = -(r + l) / (r - l);
    m.m[13] = -(t + b) / (t - b);
    m.m[14] = -(f + n) / (f - n);
    mult_matrix(m.m);
}

// ---- state
void enable(GLenum cap, bool on) {
    switch (cap) {
    case GL_TEXTURE_2D: g_st.unit[g_active_unit & 1].enabled = on; break;
    case GL_BLEND: g_st.blend = on; break;
    case GL_DEPTH_TEST: g_st.depth_test = on; break;
    case GL_CULL_FACE: g_st.cull = on; break;
    case GL_ALPHA_TEST: g_st.alpha_test = on; break;
    case GL_FOG: g_st.fog = on; break;
    case GL_SCISSOR_TEST: g_st.scissor_on = on; break;
    case GL_POLYGON_OFFSET_FILL: g_st.poly_offset = on; break;
    default: break;
    }
}
void color(float r, float g, float b, float a) { g_st.color[0] = r, g_st.color[1] = g, g_st.color[2] = b, g_st.color[3] = a; }
void active_texture(int unit) { g_active_unit = unit, g_legacy_unit = unit; }
void bind_texture(int unit, GLuint tex) { g_st.unit[unit & 1].tex = tex; }
void texture_format(GLuint tex, GLenum base_format) {
    int k = base_format == GL_ALPHA ? 2 : base_format == GL_LUMINANCE ? 3 : base_format == GL_LUMINANCE_ALPHA ? 4 : 1;
    g_tex_kind[tex] = k;
}
void set_debug_hud(bool on) { g_debug_hud = on ? 1 : 0; }
void tex_param(GLuint tex, GLenum pname, GLenum value) {
    TexMeta& m = g_tex_meta[tex];
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: m.min_f = value; break;
    case GL_TEXTURE_MAG_FILTER: m.mag_f = value; break;
    case GL_TEXTURE_WRAP_S: m.wrap_s = value; break;
    case GL_TEXTURE_WRAP_T: m.wrap_t = value; break;
    default: break;
    }
}
void texture_level(GLuint tex, int level) {
    TexMeta& m = g_tex_meta[tex];
    if (level > 0) m.game_mips = true;
    else m.dirty = true;
}
void texture_game_mipmaps(GLuint tex) { g_tex_meta[tex].game_mips = true; }
void texture_is_target(GLuint tex) { g_tex_meta[tex].target = true; }
void texture_deleted(GLuint tex) {
    g_tex_meta.erase(tex);
    g_tex_kind.erase(tex);
}
void set_anisotropy(int level) {
    int v = 1;
    while (v * 2 <= std::clamp(level, 1, g_max_aniso)) v *= 2;
    if (!g_samplers_ok) v = 1;
    if (v != g_aniso) LOGI("texture filtering: %s", v > 1 ? (std::to_string(v) + "x anisotropic").c_str() : "game default");
    g_aniso = v;
}
int anisotropy() { return g_aniso; }
int max_anisotropy() { return g_samplers_ok ? g_max_aniso : 1; }
void set_reversed_z(bool on) { g_revz_wanted = on; }
void set_bloom(int level) { g_bloom = std::clamp(level, 0, 3); }
void set_sharpen(int level) { g_sharpen = std::clamp(level, 0, 3); }
void set_ao(int level) { g_ao = std::clamp(level, 0, 3); }
void set_ao_debug(bool on) { g_ao_debug = on; }
void set_ao_tuning(float gain, float radius) { g_ao_gain = gain, g_ao_radius = radius; }
int ao() { return g_ao; }
int bloom() { return g_bloom; }
int sharpen() { return g_sharpen; }
bool reversed_z() { return g_enabled && use_revz(); }
void tex_env_mode(int unit, GLenum mode) { g_st.unit[unit & 1].env = mode; }
void tex_env_color(int unit, const float* rgba) { std::memcpy(g_st.unit[unit & 1].env_color, rgba, 16); }
void blend_func(GLenum s, GLenum d) { g_st.blend_src = s, g_st.blend_dst = d; }
void depth_func(GLenum f) { g_st.depth_func = f; }
void depth_mask(bool on) { g_st.depth_write = on; }
void color_mask(bool r, bool g, bool b, bool a) { g_st.mask[0] = r, g_st.mask[1] = g, g_st.mask[2] = b, g_st.mask[3] = a; }
void alpha_func(GLenum f, float ref) { g_st.alpha_fn = f, g_st.alpha_ref = ref; }
void cull_face(GLenum m) { g_st.cull_mode = m; }
void front_face(GLenum m) { g_st.front = m; }
void fog_mode(GLenum m) { g_st.fog_mode = m; }
void fog_param(GLenum p, float v) {
    if (p == GL_FOG_START) g_st.fog_start = v;
    else if (p == GL_FOG_END) g_st.fog_end = v;
    else if (p == GL_FOG_DENSITY) g_st.fog_density = v;
}
void fog_color(const float* rgba) { std::memcpy(g_st.fog_color, rgba, 16); }
void scissor(int x, int y, int w, int h) { g_st.sc[0] = x, g_st.sc[1] = y, g_st.sc[2] = w, g_st.sc[3] = h; }
void viewport(int x, int y, int w, int h) { g_st.vp[0] = x, g_st.vp[1] = y, g_st.vp[2] = w, g_st.vp[3] = h; }
void polygon_offset(float f, float u) { g_st.po_factor = f, g_st.po_units = u; }
void set_array(int which, const Array& a) {
    if (which >= 0 && which < A_COUNT) g_arr[which] = a;
}

// ---- drawing
void draw_elements(GLenum mode, int count, GLenum type, const void* indices) {
    if (count <= 0 || !indices || !g_arr[A_VERTEX].enabled || !g_arr[A_VERTEX].data) return;
    post_before_draw();
    const u8* ip = static_cast<const u8*>(indices);
    u32 lo = 0xFFFFFFFF, hi = 0;
    g_idx.resize(size_t(count));
    for (int i = 0; i < count; i++) {
        u32 v = type == GL_UNSIGNED_BYTE ? ip[i] : type == GL_UNSIGNED_SHORT ? reinterpret_cast<const u16*>(ip)[i]
                                                                               : reinterpret_cast<const u32*>(ip)[i];
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        g_idx[size_t(i)] = u16(v);
    }
    if (hi - lo + 1 > 65535) return;  // (never seen; would need 32-bit indices)
    for (auto& i : g_idx) i = u16(i - lo);
    build_vertices(lo, hi - lo + 1);
    if (g_trace_draws) {
        const float* v = g_verts.data();
        {
            const Array& ca = g_arr[A_COLOR];
            const u8* cb = ca.data ? ca.data + size_t(lo) * (ca.stride ? ca.stride : ca.size * type_size(ca.type)) : nullptr;
            LOGI("[ndraw]   lo=%u hi=%u colour array: enabled=%d size=%d type=%x stride=%d bytes@lo=%02x %02x %02x %02x %02x %02x %02x %02x",
                 lo, hi, ca.enabled, ca.size, ca.type, ca.stride, cb ? cb[0] : 0, cb ? cb[1] : 0, cb ? cb[2] : 0, cb ? cb[3] : 0,
                 cb ? cb[4] : 0, cb ? cb[5] : 0, cb ? cb[6] : 0, cb ? cb[7] : 0);
        }
        LOGI("[ndraw] n=%d tex2d=%d tex=%u texarr=%d colarr=%d blend=%d(%x,%x) depth=%d alpha=%d fog=%d col=%.2f,%.2f,%.2f,%.2f "
             "v0=(%.1f,%.1f,%.1f,%.1f) c0=(%.2f,%.2f,%.2f,%.2f) t0=(%.3f,%.3f) texmat0=%.2f mvp0=%.3f",
             count, g_st.unit[0].enabled, g_st.unit[0].tex, g_arr[A_TEX0].enabled, g_arr[A_COLOR].enabled, g_st.blend,
             g_st.blend_src, g_st.blend_dst, g_st.depth_test, g_st.alpha_test, g_st.fog, g_st.color[0], g_st.color[1],
             g_st.color[2], g_st.color[3], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], g_tex[0].top().m[0],
             (g_proj.top() * g_mv.top()).m[0]);
    }
    p_glUseProgram(g_prog);
    apply_state();
    upload_and_draw(mode, g_idx);
    p_glUseProgram(0);
}

void draw_arrays(GLenum mode, int first, int count) {
    if (count <= 0 || !g_arr[A_VERTEX].enabled || !g_arr[A_VERTEX].data) return;
    post_before_draw();
    g_idx.resize(size_t(count));
    for (int i = 0; i < count; i++) g_idx[size_t(i)] = u16(i);
    build_vertices(u32(first), u32(count));
    p_glUseProgram(g_prog);
    apply_state();
    upload_and_draw(mode, g_idx);
    p_glUseProgram(0);
}

void begin_frame() {
    g_trace_draws = g_trace_next;
    g_trace_next = false;
    g_seen_world = g_post_done = g_have_world_proj = false;
}
void end_frame() {
    g_trace_draws = false;
    if (!g_enabled) return;
    if (g_seen_world && !g_post_done && post_wanted()) run_post();  // a frame without HUD draws
    static bool reported = false;
    if (GLenum e = glGetError(); e != GL_NO_ERROR && !reported) {
        reported = true;
        LOGW("native renderer: GL error 0x%x during a frame", e);
    }
}
void trace_next_frame() { g_trace_next = true; }

}  // namespace render
