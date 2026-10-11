#include "render/native.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "hle/gles.h"
#include "log.h"
#include "lights.h"
#include "texpack.h"

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
    bool lighting = false;
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
enum { AT_POS = 0, AT_COLOR = 1, AT_TEX0 = 2, AT_NRM = 3 };

const char* kVS = R"(#version 130
uniform mat4 u_mvp;
uniform mat4 u_texmat0;
uniform vec4 u_color;
uniform int u_use_vcolor;
uniform int u_vcdebug;     // dev, 3D scene only: 1 = ignore vertex colours, 2 = vertex colours without textures
uniform int u_revz;
uniform int u_lit;        // GL lighting: 0 off, 1 material colours, 2 colour material (vertex/current colour)
uniform vec4 u_lm_ambient, u_mat_ambient, u_mat_emission;
uniform float u_mat_alpha;
uniform mat4 u_mv;
uniform mat3 u_nmat;  // normals: object -> eye space
in vec4 a_pos;
in vec4 a_color;
in vec4 a_tex0;
in vec3 a_nrm;
out vec4 v_color;
out vec4 v_tex0;
out float v_eyez;
out vec3 v_eye;
out vec3 v_nrm;
void main() {
    gl_Position = u_mvp * a_pos;
    v_eye = (u_mv * a_pos).xyz;
    v_nrm = u_nmat * a_nrm;
    v_color = u_use_vcolor != 0 ? a_color : u_color;
    if (u_vcdebug == 1 && u_use_vcolor != 0) v_color.rgb = vec3(1.0);  // dev: without the painted vertex lighting
    // The game enables GL lighting without any light: the result is emission + ambient * scene ambient
    // (how it tints the world, e.g. the red alarm lighting).
    if (u_lit == 2) v_color = vec4(clamp(u_mat_emission.rgb + v_color.rgb * u_lm_ambient.rgb, 0.0, 1.0), v_color.a);
    else if (u_lit == 1) v_color = vec4(clamp(u_mat_emission.rgb + u_mat_ambient.rgb * u_lm_ambient.rgb, 0.0, 1.0), u_mat_alpha);
    v_tex0 = u_texmat0 * a_tex0;
    v_eyez = gl_Position.w;  // eye-space distance for fog (GL uses |z_eye|; w == -z_eye for perspective)
    // reversed-Z with a [0,1] clip range: near plane -> depth 1, far plane -> 0
    if (u_revz != 0) gl_Position.z = (gl_Position.w - gl_Position.z) * 0.5;
}
)";

const char* kFS = R"(#version 130
uniform sampler2D u_tex0;
uniform int u_texwhite;
uniform int u_use_tex0;   // 0 off, 1 RGB/RGBA, 2 ALPHA (RGB passes through), 3 LUMINANCE, 4 LUMINANCE_ALPHA
uniform int u_debug;      // 1: flat magenta (geometry check)
uniform int u_fog;        // 0 off, 1 linear, 2 exp, 3 exp2
uniform vec4 u_fog_color;
uniform vec2 u_fog_range; // start, end (linear) / density in x (exp)
uniform vec2 u_alpha;     // test mode (0 off, 1 GEQUAL, 2 GREATER, 3 LESS, 4 LEQUAL, 5 EQUAL, 6 NOTEQUAL), ref
uniform int u_gbuf;        // 1: also write the eye-space surface normal (second color target)
uniform int u_revz;
uniform int u_veilmode;    // coverage written for a see-through draw: 0 its alpha, 1 its alpha where it floats in front of the scene
uniform sampler2D u_scened;  // depth of the solid scene (copied before the first see-through draw)
uniform vec4 u_veilz;      // projection terms A, B (clip z = A*z + B), 1 / viewport size
in vec4 v_color;
in vec4 v_tex0;
in float v_eyez;
in vec3 v_eye;
in vec3 v_nrm;
out vec4 frag;
out vec4 o_nrm;
out vec4 o_veil;  // fog coverage (third color target, selected for fog draws only; blended like the color)
void main() {
    vec4 c = v_color;
    if (u_use_tex0 != 0) {
        vec4 t = texture(u_tex0, v_tex0.st / max(v_tex0.q, 1e-6));
        if (u_texwhite != 0) t.rgb = vec3(1.0);  // dev: vertex colours alone
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
    float cover = c.a;
    if (u_veilmode == 1) {
        // A decal or a hologram lies on (or just off) a surface and is lit with it; a light shaft, a flare
        // or steam hangs in the air in front of it and is not. Distance from this fragment to the solid
        // scene behind it, along the view:
        float d = texture(u_scened, gl_FragCoord.xy * u_veilz.zw).r;
        float nz = u_revz != 0 ? 1.0 - 2.0 * d : 2.0 * d - 1.0;
        float behind = u_veilz.y / (nz + u_veilz.x);
        cover *= smoothstep(0.08, 0.4, behind - v_eyez);
    }
    o_veil = vec4(1.0, 1.0, 1.0, cover);
    o_nrm = vec4(0.0);
    if (u_gbuf != 0) {
        // The game supplies no normals. The exact face normal comes from screen-space derivatives of the
        // eye position; the smooth one was averaged per vertex from the triangles of this draw. The
        // smooth normal is used where it agrees with the face (curved surfaces) and fades to the face
        // normal where it does not (hard edges whose vertices are shared).
        vec3 fl = normalize(cross(dFdx(v_eye), dFdy(v_eye)));
        if (dot(fl, v_eye) > 0.0) fl = -fl;
        vec3 n = fl;
        float sl = dot(v_nrm, v_nrm);
        if (sl > 1e-12) {
            vec3 sm = v_nrm * inversesqrt(sl);
            if (dot(sm, fl) < 0.0) sm = -sm;
            n = normalize(mix(fl, sm, smoothstep(0.70, 0.88, dot(sm, fl))));
        }
        o_nrm = vec4(n * 0.5 + 0.5, u_gbuf == 2 ? 0.667 : 1.0);  // alpha: 1 = level and props, 2/3 = things that move
        o_veil = vec4(0.0, 0.0, 0.0, 1.0);  // (a solid surface: no see-through cover here)
    }
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
int g_vc_debug = 0;        // dev: 1 = level drawn without its vertex colours, 2 = vertex colours without textures
bool g_gbuf_draw = false;  // this draw also writes surface normals
bool g_veil_draw = false;  // this draw is fog: it also writes its coverage
int g_veil_draws = 0;      // fog draws this frame
int g_veil_draws_prev = 0, g_tile_draws_prev = 0;  // last frame's (dev report)
bool g_veil_on = true;     // dev: fog is kept out of the lighting
int g_veil_mode = 0;        // this draw's coverage: 0 plain alpha (fog banks), 1 where it floats in front of the scene
bool g_veil_depth = false;  // the solid scene's depth has been copied for this frame
float g_veilz[4] = {0, 0, 0, 0};  // projection terms and 1 / viewport size for the shader's depth comparison
float g_tint_now[3] = {1, 1, 1}, g_tint_color[3] = {1, 1, 1};  // the world tint: being drawn / of the last frame (white = none)
struct TintVote { float c[3]; float weight; };  // one light-model colour seen this frame and how much geometry used it
TintVote g_tint_votes[6];
int g_tint_nvotes = 0;
bool g_tint_seen = false, g_tint_active = false;  // the game is tinting the world (this frame so far / last frame)
// ---- shadows (see "shadow map" further down)
int g_shadows = 0;             // 0 off, 1 low, 2 medium, 3 high
bool g_shadow_debug = false;   // dev: tint what receives shadow / show casters
bool g_shadow_draw = false;    // this draw is also rendered into the shadow map
bool g_shadow_cleared = false; // the shadow map holds this frame's casters
int g_shadow_frame = 0;        // 0 not set up yet this frame, 1 ready, -1 unavailable
Mat4 g_view_inv, g_light_from_eye;
Mat4 g_view;  // world -> eye (shadow_frame)
std::vector<u16> g_idx;
float g_up_eye[3] = {0, 1, 0};
int g_caster_draws = 0;
void shadow_pass(GLenum mode, GLsizei count);
void beacon_lights();
void glow_frame_end();
// ---- shadow maps of the lights fixed at the start of a frame (alarm spotlights, strip lights)
constexpr int kTiles = 24, kTileSize = 1024;  // atlas of 4 x 6 tiles
int g_ntiles = 0;
Mat4 g_tile_from_eye[kTiles];
bool g_tile_all[kTiles];      // the tile takes every mesh (spotlights); otherwise only things that move
bool g_mesh_moves = false;    // the mesh being drawn is a moving object (caster())
bool g_probe_on = false;
float g_probe[4] = {0, 0, 0, 2};
bool g_env_shadows = true;    // level geometry casts shadows from the fixtures
bool g_mesh_prop = false;     // ...or a still object of prop size (a panel, a crate, cables): it blocks fixtures too
float g_mesh_lo[3], g_mesh_hi[3];  // its world bounds
float g_tile_pos[kTiles][3];  // world position of each tile's light
float g_tile_radius[kTiles];

// ---- the lighting system's persistent state (see "lights and their shadow maps" further down)
// A shadow map belongs to one light for as long as that light stays near the player. Its view is fixed
// by the light alone: where the lamp is and which way it faces.
struct Tile {
    unsigned long long id = 0;
    bool used = false, present = false;  // owned by a light; that light exists this frame
    bool rotating = false;               // beacon: its view turns, so nothing can be kept
    bool spot = false;
    bool dirty = true;                   // the kept depth of the still scene must be drawn again
    float pos[3] = {0, 0, 0}, dir[3] = {0, -1, 0}, radius = 0, half = 1;
    Mat4 w2c;                            // world -> the map's clip space
    u32 seen_ms = 0;
    bool fixture = false;  // a lamp of the level: it faces the open space around it (open_direction)
    u32 open_ms = 0;
    bool baked = false;    // (kept maps) the still scene has been drawn into it at least once
    u32 born_ms = 0;       // (kept maps) when it was first drawn: its shadows fade in from then
};
Tile g_tiles[kTiles];
// Kept ("baked") maps: every fixed light of the level owns one for as long as it is remembered, holding
// the depth of the still scene as that light sees it. It is drawn when the light is first met and again
// only when the still scene around it changes, so a lamp's shadows are the same whatever the camera does
// and however many lamps are in view. The live tiles above then hold nothing but what moves.
constexpr int kBakes = 128, kBakeSize = 512, kBakeCols = 16, kBakeRows = 8;  // atlas of 16 x 8 maps
constexpr int kBakeBudget = 6;                                               // maps drawn per frame at most
Tile g_bakes[kBakes];
GLuint g_bmat_tex = 0;  // per kept map: eye space -> its clip space (4 texels), then (strength, -, -, -)
int g_bakes_present = 0, g_bakes_pending = 0, g_bakes_drawn = 0;  // last frame (dev report)
// dev: where the renderer's own CPU time goes in a frame (ms): lamp search, lamp facing, map drawing, uploads
double g_ms_scan = 0, g_ms_open = 0, g_ms_bake = 0, g_ms_upload = 0, g_ms_cache = 0;
int g_n_scan = 0, g_n_open = 0;
bool g_perf_log = false;
struct StopWatch {
    double& acc;
    Uint64 t0 = SDL_GetPerformanceCounter();
    explicit StopWatch(double& a) : acc(a) {}
    ~StopWatch() { acc += double(SDL_GetPerformanceCounter() - t0) * 1000.0 / double(SDL_GetPerformanceFrequency()); }
};
// The still scene, in world space: every mesh that has not moved for a while is copied here once, the
// first time it is drawn. The shadow maps draw it from here, so a light's shadows do not depend on what
// the camera is looking at (only on what has been seen so far), and they do not change from frame to frame.
struct SMesh {
    u32 first, count;  // range in g_sidx
    float lo[3], hi[3];
    u64 base;          // key of its vertex data (all its instances)
    bool dead;         // it started to move: no longer part of the still scene
};
struct SBox {
    float lo[3], hi[3];
};
std::vector<float> g_sverts;  // x, y, z
std::vector<u32> g_sidx;
std::vector<SMesh> g_smesh;
std::unordered_map<u64, size_t> g_skey;  // instance (vertex data + placement) -> g_smesh
std::vector<SBox> g_schanges;            // where the still scene changed since the maps were last drawn
bool g_supload = false, g_mesh_cached = false;
GLuint g_svao = 0, g_svbo = 0, g_sibo = 0, g_p_depth = 0, g_satlas = 0, g_satlas_fbo = 0;
std::unordered_map<u64, std::array<float, 3>> g_facing;  // fixture -> the way it faces (decided once)
GLuint g_atlas = 0, g_atlas_fbo = 0;
bool g_atlas_ok = false, g_atlas_cleared = false;
int g_tile_draws = 0, g_tile_debug = 0;
float g_glow_gain = 1.0f;               // brightness of panel glows (dev: glowgain N)
int g_env_light = 2;  // environment light level: 0 = the game's painted lighting at full, 3 = darkest
constexpr float kEnv[4] = {1.0f, 0.85f, 0.7f, 0.55f};
float g_lshade = 0.9f;                  // darkness of the shadows fixtures cast (dev: lightshade N)
float g_fanshade = 0.8f;                // darkening of things under a fan blade's shadow (dev: fanshade N)
float g_gobo_rect[4] = {0, 0, 1, 1};    // the tile of that texture the shadow uses
GLuint g_gobo_tex = 0;                  // the game's fan shadow texture (fan_floor)
float g_gobo_lo = 0, g_gobo_hi = 1;     // its darkest / brightest values
float g_cam_target[3] = {0, 0, 0};
void setup_frame_lights();
void assign_tiles();
unsigned long long light_id(const float* p, unsigned kind, float grid);
void tile_pass(GLenum mode, GLsizei count);
bool g_normals_cleared = false, g_normals_debug = false;
PFNGLDRAWBUFFERSPROC p_glDrawBuffers;
PFNGLCLEARBUFFERFVPROC p_glClearBufferfv;
PFNGLBINDFRAGDATALOCATIONPROC p_glBindFragDataLocation;
PFNGLUNIFORMMATRIX3FVPROC p_glUniformMatrix3fv;
PFNGLUNIFORM3FVPROC p_glUniform3fv_raw;
void p_glUniform3fv_(GLint loc, const float* v) {
    if (!p_glUniform3fv_raw) p_glUniform3fv_raw = reinterpret_cast<PFNGLUNIFORM3FVPROC>(SDL_GL_GetProcAddress("glUniform3fv"));
    if (p_glUniform3fv_raw) p_glUniform3fv_raw(loc, 1, v);
}
GLint u_mv = -1, u_nmat = -1, u_gbuf = -1;
constexpr int kStride = 15;  // pos4, color4, tex4, normal3

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

// Per-vertex normals for an indexed triangle list: area-weighted face normals summed over every
// triangle that touches a vertex. Vertices at the same position (split for texture seams) share one
// sum, so seams do not show.
std::vector<u32> g_weld_slot, g_weld_rep;
void build_normals(const std::vector<u16>& idx, u32 n) {
    u32 cap = 16;
    while (cap < n * 2) cap <<= 1;
    g_weld_slot.assign(cap, 0xFFFFFFFFu);
    g_weld_rep.resize(n);
    for (u32 i = 0; i < n; i++) {
        const float* p = &g_verts[size_t(i) * kStride];
        u32 b[3];
        std::memcpy(b, p, 12);
        u32 h = (b[0] * 0x9E3779B1u) ^ (b[1] * 0x85EBCA77u) ^ (b[2] * 0xC2B2AE3Du);
        h ^= h >> 15;
        for (u32 s = h & (cap - 1);; s = (s + 1) & (cap - 1)) {
            u32 j = g_weld_slot[s];
            if (j == 0xFFFFFFFFu) {
                g_weld_slot[s] = i;
                g_weld_rep[i] = i;
                break;
            }
            if (std::memcmp(&g_verts[size_t(j) * kStride], p, 12) == 0) {
                g_weld_rep[i] = j;
                break;
            }
        }
    }
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        u32 i0 = idx[t], i1 = idx[t + 1], i2 = idx[t + 2];
        if (i0 >= n || i1 >= n || i2 >= n) continue;
        const float *a = &g_verts[size_t(i0) * kStride], *b = &g_verts[size_t(i1) * kStride], *c = &g_verts[size_t(i2) * kStride];
        float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float fn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (u32 v : {i0, i1, i2}) {
            float* d = &g_verts[size_t(g_weld_rep[v]) * kStride + 12];
            d[0] += fn[0], d[1] += fn[1], d[2] += fn[2];
        }
    }
    for (u32 i = 0; i < n; i++) {
        u32 r = g_weld_rep[i];
        if (r != i) std::memcpy(&g_verts[size_t(i) * kStride + 12], &g_verts[size_t(r) * kStride + 12], 12);
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

// The world tint (alarm lighting, a hallucination's blackout) is the light-model colour the level is
// drawn with. Single objects can have a colour of their own (an orange door lock, a tinted character),
// so the colour that the most geometry uses in a frame is the world's; still meshes count four times.
void tint_vote(float weight) {
    float lma[4] = {1, 1, 1, 1};  // (solid geometry drawn without GL lighting is untinted: a vote for white)
    if (g_st.lighting) glGetFloatv(GL_LIGHT_MODEL_AMBIENT, lma);
    if (std::min(lma[0], std::min(lma[1], lma[2])) >= 0.9f) lma[0] = lma[1] = lma[2] = 1.0f;
    int best = -1;
    for (int i = 0; i < g_tint_nvotes && best < 0; i++) {
        const float* c = g_tint_votes[i].c;
        if (std::fabs(c[0] - lma[0]) + std::fabs(c[1] - lma[1]) + std::fabs(c[2] - lma[2]) < 0.03f) best = i;
    }
    if (best < 0) {
        if (g_tint_nvotes >= 6) return;
        best = g_tint_nvotes++;
        g_tint_votes[best] = {{lma[0], lma[1], lma[2]}, 0.0f};
    }
    g_tint_votes[best].weight += weight;
    // (parts of a tinted level are still drawn unlit, so white needs four times the weight to win)
    auto score = [](const TintVote& v) { return v.c[0] + v.c[1] + v.c[2] > 2.99f ? v.weight * 0.25f : v.weight; };
    int top = 0;
    for (int i = 1; i < g_tint_nvotes; i++)
        if (score(g_tint_votes[i]) > score(g_tint_votes[top])) top = i;
    g_tint_seen = g_tint_votes[top].c[0] + g_tint_votes[top].c[1] + g_tint_votes[top].c[2] < 2.99f;
    for (int k = 0; k < 3; k++) g_tint_now[k] = g_tint_votes[top].c[k];
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
    p_glUniform1i(u_gbuf, g_gbuf_draw ? (g_mesh_moves ? 2 : 1) : 0);
    if (g_gbuf_draw) {
        const float* m = g_mv.top().m;
        // cofactor matrix of the upper 3x3 (= inverse transpose up to scale; the shader normalises)
        const float nm[9] = {m[5] * m[10] - m[6] * m[9], m[6] * m[8] - m[4] * m[10], m[4] * m[9] - m[5] * m[8],
                             m[2] * m[9] - m[1] * m[10], m[0] * m[10] - m[2] * m[8], m[1] * m[8] - m[0] * m[9],
                             m[1] * m[6] - m[2] * m[5], m[2] * m[4] - m[0] * m[6], m[0] * m[5] - m[1] * m[4]};
        p_glUniformMatrix4fv(u_mv, 1, GL_FALSE, m);
        p_glUniformMatrix3fv(u_nmat, 1, GL_FALSE, nm);
    }
    p_glUniformMatrix4fv(u_texmat0, 1, GL_FALSE, g_tex[0].top().m);
    p_glUniform4fv(u_color, 1, g_st.color);
    p_glUniform1i(u_use_vcolor, g_arr[A_COLOR].enabled ? 1 : 0);
    {
        static GLint u_vc = -2, u_tw = -2;
        if (u_vc == -2) u_vc = p_glGetUniformLocation(g_prog, "u_vcdebug"), u_tw = p_glGetUniformLocation(g_prog, "u_texwhite");
        // only the 3D scene's opaque, vertex-coloured geometry (the level): not the HUD, not effects
        bool level = g_vc_debug != 0 && g_st.depth_test && g_st.depth_write && !g_st.blend && g_proj.top().m[11] != 0.0f && g_arr[A_COLOR].enabled;
        if (g_trace_draws && g_arr[A_COLOR].enabled && !g_st.blend)
            LOGI("[ndraw]   vcdebug: level=%d mode=%d depth_test=%d depth_write=%d persp=%.2f loc=%d", level, g_vc_debug, g_st.depth_test, g_st.depth_write, g_proj.top().m[11], u_vc);
        p_glUniform1i(u_vc, level ? g_vc_debug : 0);
        p_glUniform1i(u_tw, level && g_vc_debug == 2 ? 1 : 0);
    }
    {
        static GLint u_lit = -2, u_lma, u_mamb, u_memi, u_malpha;
        if (u_lit == -2) {
            u_lit = p_glGetUniformLocation(g_prog, "u_lit");
            u_lma = p_glGetUniformLocation(g_prog, "u_lm_ambient");
            u_mamb = p_glGetUniformLocation(g_prog, "u_mat_ambient");
            u_memi = p_glGetUniformLocation(g_prog, "u_mat_emission");
            u_malpha = p_glGetUniformLocation(g_prog, "u_mat_alpha");
        }
        int lit = 0;
        if (g_st.lighting) {
            // light model and material live in the (still maintained) fixed-function state
            float lma[4], amb[4], emi[4], dif[4];
            glGetFloatv(GL_LIGHT_MODEL_AMBIENT, lma);
            glGetMaterialfv(GL_FRONT, GL_AMBIENT, amb);
            glGetMaterialfv(GL_FRONT, GL_EMISSION, emi);
            glGetMaterialfv(GL_FRONT, GL_DIFFUSE, dif);
            lit = glIsEnabled(GL_COLOR_MATERIAL) ? 2 : 1;
            p_glUniform4fv(u_lma, 1, lma);
            p_glUniform4fv(u_mamb, 1, amb);
            p_glUniform4fv(u_memi, 1, emi);
            p_glUniform1f(u_malpha, dif[3]);
            static bool warned = false;
            if (!warned)
                for (int i = 0; i < 8; i++)
                    if (glIsEnabled(GLenum(GL_LIGHT0 + i))) {
                        warned = true;
                        LOGW("native renderer: the game enabled GL light %d (not supported; ambient only)", i);
                        break;
                    }
            if (g_trace_draws)
                LOGI("[ndraw]   lit=%d scene ambient %.2f %.2f %.2f material ambient %.2f %.2f %.2f emission %.2f %.2f %.2f alpha %.2f", lit,
                     lma[0], lma[1], lma[2], amb[0], amb[1], amb[2], emi[0], emi[1], emi[2], dif[3]);
        }
        p_glUniform1i(u_lit, lit);
    }
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
    p_glEnableVertexAttribArray(AT_NRM);
    p_glVertexAttribPointer(AT_NRM, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<const void*>(12 * sizeof(float)));
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * 2), idx.data(), GL_STREAM_DRAW);
    static const GLenum kBoth[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0 + 1};
    static const GLenum kVeil[3] = {GL_COLOR_ATTACHMENT0, GL_NONE, GL_COLOR_ATTACHMENT0 + 2};
    static GLint u_veilmode = -2, u_scened = -1, u_veilz = -1;
    if (u_veilmode == -2) {
        u_veilmode = p_glGetUniformLocation(g_prog, "u_veilmode");
        u_scened = p_glGetUniformLocation(g_prog, "u_scened");
        u_veilz = p_glGetUniformLocation(g_prog, "u_veilz");
        p_glUniform1i(u_scened, 3);
    }
    const int veil_mode = g_veil_draw && g_veil_depth ? g_veil_mode : 0;
    p_glUniform1i(u_veilmode, veil_mode);
    if (veil_mode == 1) p_glUniform4fv(u_veilz, 1, g_veilz);
    if (g_gbuf_draw) p_glDrawBuffers(2, kBoth);
    else if (g_veil_draw) p_glDrawBuffers(3, kVeil);
    glDrawElements(mode, GLsizei(idx.size()), GL_UNSIGNED_SHORT, nullptr);
    if (g_gbuf_draw || g_veil_draw) p_glDrawBuffers(1, kBoth);
    if (g_shadow_draw) shadow_pass(mode, GLsizei(idx.size()));
    if (g_gbuf_draw && g_ntiles > 0 && mode == GL_TRIANGLES) tile_pass(mode, GLsizei(idx.size()));
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
int g_lights = 0;  // dynamic lights: 0 off, 1 low, 2 medium, 3 high
bool g_vignette = true;       // darkened picture edges
bool g_light_shadows = true;  // the dynamic lights cast shadows
int g_specular = 0;  // specular highlights from those lights, 0-3
std::vector<Light> g_spots_prev;  // last frame's g_spots (debug view)
std::vector<Light> g_frame_lights, g_spots;  // g_spots: spotlights found while drawing this frame
bool g_gbuf_ok = false;  // the second color target can be used
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
Target g_veil;                      // fog coverage of the scene (resolved copy)
Target g_av[4];                     // the scene reduced step by step to one pixel: its average colour
Target g_nrm;                       // surface normals of the scene (resolved copy)
constexpr int kShadowSize = 2048;
constexpr float kShadowHalf = 8.0f, kShadowTop = 3.5f, kShadowBottom = 3.5f;  // metres around the camera target
float g_shadow_lean[2] = {0.12f, 0.08f};  // the floor shadow light's lean off vertical (x, z per metre of height)
float g_shadow_opacity = 0.35f;           // and its strength; both follow the nearby lights (shadow_frame)
GLuint g_shmap = 0, g_shfbo = 0;
bool g_shadow_ok = false;
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
uniform sampler2D u_depth, u_nrm;
uniform int u_have_nrm;
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
    vec4 gn = texture(u_nrm, uv);  // normals recorded by the scene's opaque draws, where there are any
    if (u_have_nrm != 0 && gn.a > 0.5) n = normalize(gn.xyz * 2.0 - 1.0);
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
uniform sampler2D u_scene, u_b1, u_b2, u_b3, u_ao, u_nrm, u_depth;
uniform mat4 u_invp;
uniform int u_revz;
uniform int u_nl;           // dynamic lights this frame
uniform vec4 u_lpos[128];    // eye-space position, radius
uniform vec4 u_lcol[128];
uniform vec4 u_ldir[128];
uniform vec4 u_lx[128];     // x = 1: ceiling fan light (blade shadows), y = heading of one blade's shadow
uniform vec3 u_wx, u_wy, u_wz;  // the world axes in eye space
uniform vec4 u_lg[128];     // fan lights: floor offset -> shadow texture offset (2x2, rows)
uniform sampler2D u_gobo;   // the game's fan shadow texture
uniform sampler2D u_veil;   // fog coverage (0 = none)
uniform int u_veilon;
uniform float u_env;        // environment light: how bright the painted lighting is left (1 = as the game made it)
uniform vec4 u_tint;        // the game's world tint (white = none): the level's lamps are tinted with the world
uniform float u_lshade;     // how dark a fixture's blocked light makes the scene
uniform float u_fanshade;   // how much a fan blade's shadow darkens what stands under it
uniform vec2 u_gobo_range;  // its darkest and brightest values
uniform vec4 u_gobo_rect;   // the tile of the texture it occupies (u0, v0, u1, v1)    // spotlights: direction (eye space), cosine of the half angle (0 = point light)
uniform float u_lgain;
uniform sampler2D u_atlas;  // shadow maps of the fixed lights, 4 x 2 tiles
uniform mat4 u_tm[24];      // eye space -> a tile's clip space
uniform sampler2D u_batlas;  // the kept maps: still scene as each fixed light sees it
uniform sampler2D u_bmat;    // per kept map: eye space -> clip space, strength
uniform sampler2D u_avg;     // one pixel: the average colour of the scene as the game lit it
uniform int u_tdebug;       // dev: show what light (tile u_tdebug - 1) can see, white = lit
uniform sampler2D u_shmap;  // depth of moving objects seen from the shadow light
uniform mat4 u_shmat;       // eye space -> shadow map clip space
uniform vec4 u_upe;         // world up in eye space
uniform float u_shs;        // shadow strength, 0 = off
uniform float u_shrange;    // metres covered by the shadow map's depth range
uniform float u_shsoft;     // blur radius in shadow map uv
uniform int u_shdebug;
uniform float u_spec;       // specular highlight strength, 0 = off
uniform mat4 u_proj;
uniform int u_lsh;          // dynamic lights cast shadows (screen-space trace through the depth buffer)
uniform int u_nrmdebug;  // show the recorded surface normals
uniform float u_vignette;  // darkening toward the edges of the picture, 0 = off
uniform float u_aos;    // ambient occlusion strength, 0 = off
uniform int u_aodebug;  // show the occlusion term alone
uniform vec2 u_px;      // scene texel size
uniform float u_sharp;  // 0 = off
uniform float u_bloom;  // 0 = off
in vec2 uv;
out vec4 frag;
// How much of light `t`'s light reaches p (1 = all): its shadow map, 4 taps.
float tile_light(int t, vec3 p, vec3 n, vec3 ldn) {
    vec4 q = u_tm[t] * vec4(p + n * 0.05 + ldn * 0.06, 1.0);
    if (q.w <= 0.0) return 1.0;
    vec3 s = q.xyz / q.w;
    if (abs(s.x) > 0.98 || abs(s.y) > 0.98 || s.z > 1.0) return 1.0;
    vec2 org = vec2(float(t - (t / 4) * 4), float(t / 4));
    vec2 tuv = (org + s.xy * 0.5 + 0.5) * vec2(0.25, 1.0 / 6.0);
    float z = s.z * 0.5 + 0.5 - 0.0004, lit = 0.0;
    vec2 px = vec2(0.7 / 4096.0, 0.7 / 6144.0);
    for (int k = 0; k < 8; k++) {
        float a = float(k) * 0.7853982 + 0.3, r = (k - (k / 2) * 2) == 0 ? 2.2 : 1.1;
        lit += step(z, texture(u_atlas, tuv + px * vec2(cos(a), sin(a)) * r).r);
    }
    float edge = smoothstep(0.98, 0.75, max(abs(s.x), abs(s.y)));
    return mix(1.0, lit * 0.125, edge);
}
// The same for a fixed light: its kept map `b` of the still scene and, if it has one, its live tile `t`
// of the things that move (both drawn with one view).
float bake_light(int b, int t, vec3 p, vec3 n, vec3 ldn) {
    mat4 m = mat4(texelFetch(u_bmat, ivec2(0, b), 0), texelFetch(u_bmat, ivec2(1, b), 0), texelFetch(u_bmat, ivec2(2, b), 0), texelFetch(u_bmat, ivec2(3, b), 0));
    float strength = texelFetch(u_bmat, ivec2(4, b), 0).r;
    if (strength <= 0.0) return 1.0;
    vec4 q = m * vec4(p + n * 0.06 + ldn * 0.07, 1.0);
    if (q.w <= 0.0) return 1.0;
    vec3 s = q.xyz / q.w;
    if (abs(s.x) > 0.98 || abs(s.y) > 0.98 || s.z > 1.0) return 1.0;
    float z = s.z * 0.5 + 0.5 - 0.0006, lit = 0.0;
    vec2 org = vec2(float(b - (b / 16) * 16), float(b / 16));
    vec2 tuv = (org + s.xy * 0.5 + 0.5) * vec2(1.0 / 16.0, 1.0 / 8.0);
    vec2 px = vec2(0.7 / 8192.0, 0.7 / 4096.0);
    for (int k = 0; k < 6; k++) {
        float a = float(k) * 1.0471976 + 0.3, r = (k - (k / 2) * 2) == 0 ? 1.6 : 0.8;
        lit += step(z, texture(u_batlas, tuv + px * vec2(cos(a), sin(a)) * r).r);
    }
    lit /= 6.0;
    if (t >= 0) {
        vec2 torg = vec2(float(t - (t / 4) * 4), float(t / 4));
        vec2 uv2 = (torg + s.xy * 0.5 + 0.5) * vec2(0.25, 1.0 / 6.0);
        vec2 px2 = vec2(0.7 / 4096.0, 0.7 / 6144.0);
        float dyn = 0.0;
        for (int k = 0; k < 4; k++) {
            float a = float(k) * 1.5707963 + 0.3;
            dyn += step(z, texture(u_atlas, uv2 + px2 * vec2(cos(a), sin(a)) * 1.8).r);
        }
        lit = min(lit, dyn * 0.25);
    }
    float edge = smoothstep(0.98, 0.75, max(abs(s.x), abs(s.y)));
    return mix(1.0, lit, edge * strength);
}
void main() {
    vec3 c = texture(u_scene, uv).rgb;
    if (u_sharp > 0.0) {
        vec3 n = texture(u_scene, uv + vec2(0.0, u_px.y)).rgb, s = texture(u_scene, uv - vec2(0.0, u_px.y)).rgb;
        vec3 e = texture(u_scene, uv + vec2(u_px.x, 0.0)).rgb, w = texture(u_scene, uv - vec2(u_px.x, 0.0)).rgb;
        vec3 lo = min(c, min(min(n, s), min(e, w))), hi = max(c, max(max(n, s), max(e, w)));
        // unsharp mask, limited to slightly beyond the local range (no halos)
        c = clamp(c + (4.0 * c - n - s - e - w) * u_sharp, lo - 0.04, hi + 0.04);
    }
    vec3 c0 = c;  // the scene as the game lit it
    // How bright the world tint leaves the scene (1 = no tint or a pure colour; less under the dark tints
    // of the hallucinations). "Bright pixel" tests below are made on the scene as it would be untinted.
    float tl = clamp(max(u_tint.r, max(u_tint.g, u_tint.b)), 0.02, 1.0);
    // How bright the game itself lit this view (0 = a dark corridor, 1 = a white, fully lit room). The added
    // lamps are the lamps the game painted its lighting from: in a room it already lit brightly there is
    // little for them to add and little to turn down first, and doing either anyway lifts every dark tone
    // and greys every white one (a washed-out picture). Shadows and occlusion are not affected.
    float bright = smoothstep(0.18, 0.34, dot(texelFetch(u_avg, ivec2(0, 0), 0).rgb, vec3(0.299, 0.587, 0.114)) / tl);
    if (u_shs > 0.0) {
        // Shadows of moving objects (characters, props, doors) from an assumed light above: the level's
        // own shadows are baked into its textures, so only surfaces facing up receive these.
        vec4 gn = texture(u_nrm, uv);
        if (gn.a > 0.5) {
            float d = texture(u_depth, uv).r;
            vec4 pp = u_invp * vec4(uv * 2.0 - 1.0, u_revz != 0 ? 1.0 - 2.0 * d : 2.0 * d - 1.0, 1.0);
            vec3 p = pp.xyz / pp.w, n = normalize(gn.xyz * 2.0 - 1.0);
            float facing = smoothstep(0.15, 0.6, dot(n, u_upe.xyz));
            vec3 q = (u_shmat * vec4(p + n * 0.03, 1.0)).xyz * 0.5 + 0.5;
            vec2 e = min(q.xy, 1.0 - q.xy);
            float edge = smoothstep(0.0, 0.08, min(e.x, e.y));
            if (facing > 0.0 && edge > 0.0 && q.z < 1.0) {
                float sum = 0.0;
                for (int i = 0; i < 16; i++) {
                    float a = float(i) * 2.3999632, r = sqrt((float(i) + 0.5) / 16.0);
                    float gap = (q.z - texture(u_shmap, q.xy + vec2(cos(a), sin(a)) * (r * u_shsoft)).r) * u_shrange;
                    // the further above the caster is, the lighter its shadow
                    if (gap > 0.05) sum += 1.0 - smoothstep(0.8, 2.4, gap);
                }
                float s = sum / 16.0 * facing * edge;
                c *= 1.0 - u_shs * s;
                if (u_shdebug != 0) c = mix(c, vec3(1.0, 0.0, 0.0), s * 0.7);
            }
        }
    }
    if (u_nl > 0) {
        // Point lights from game events (shots, impacts, projectiles, explosions), added on top of the
        // baked look. The scene colour stands in for the surface colour; a small floor lets surfaces
        // that are baked dark still pick the light up.
        vec4 gn = texture(u_nrm, uv);
        float d = texture(u_depth, uv).r;
        vec4 pp = u_invp * vec4(uv * 2.0 - 1.0, u_revz != 0 ? 1.0 - 2.0 * d : 2.0 * d - 1.0, 1.0);
        vec3 p = pp.xyz / pp.w;
        if (gn.a > 0.5 && abs(1.0 / pp.w) < 60.0) {
            vec3 n = normalize(gn.xyz * 2.0 - 1.0), sum = vec3(0.0), spec = vec3(0.0), spot = vec3(0.0);
            float fanshade = 0.0;
            float shw = 0.0, shb = 0.0;  // weight of the fixed point lights reaching this pixel; the blocked part of it
            float reach = 0.0;           // how much added lamp light there is around this pixel at all (unshadowed)
            vec3 fandbg = vec3(0.0, 0.0, 1.0);  // how deep in a fan blade's shadow this pixel is (things above the floor)
            vec3 v = normalize(-p);
            for (int i = 0; i < u_nl; i++) {
                vec3 l = u_lpos[i].xyz - p;
                float d2 = dot(l, l), r = u_lpos[i].w;
                if (d2 >= r * r) continue;  // out of reach (most lights, for most pixels)
                float att = clamp(1.0 - d2 / (r * r), 0.0, 1.0);
                if (u_ldir[i].w > 0.0) {  // spotlight cone with a soft edge
                    float cd = dot(-l * inversesqrt(d2 + 1e-6), u_ldir[i].xyz);
                    att *= sqrt(smoothstep(u_ldir[i].w, mix(u_ldir[i].w, 1.0, 0.45), cd));
                    if (u_lx[i].x > 0.5) {
                        // light from inside a ceiling fan: three turning blades and the hub shade it, on
                        // whatever is below (floor, props, characters)
                        vec3 rel = -l;
                        vec2 h = vec2(dot(rel, u_wx), dot(rel, u_wz));
                        float ang = atan(h.y, h.x) - u_lx[i].y;
                        float tanh_ = sqrt(max(1.0 - u_ldir[i].w * u_ldir[i].w, 0.0)) / u_ldir[i].w;
                        float rr = length(h) / max(-dot(rel, u_wy) * tanh_, 1e-3);
                        // thin blades, wide lit sectors between them
                        if (u_lx[i].x > 1.5) {
                            // the game's own floor shadow, carried up the light's cone: what is under the fan
                            // gets exactly the pattern the floor shows
                            vec2 fl = h * (u_lx[i].y / max(-dot(rel, u_wy), 1e-3));
                            vec2 guv = u_lx[i].zw + vec2(dot(u_lg[i].xy, fl), dot(u_lg[i].zw, fl));
                            guv = clamp(guv, u_gobo_rect.xy, u_gobo_rect.zw);
                            float g = dot(textureLod(u_gobo, guv, 0.0).rgb, vec3(0.3333));
                            g = clamp((g - u_gobo_range.x) / max(u_gobo_range.y - u_gobo_range.x, 0.05), 0.0, 1.0);
                            att *= sqrt(mix(0.06, 1.0, g));
                            // The game darkens the floor itself under the blades; whatever stands on it
                            // (characters, props) is darkened here the same way, so the blades read as
                            // shadows on them and not only as gaps in the added light.
                            float above = u_lx[i].y + dot(rel, u_wy);  // height above the fan's floor
                            float cone = smoothstep(u_ldir[i].w, mix(u_ldir[i].w, 1.0, 0.3), cd);
                            fanshade = max(fanshade, (1.0 - g) * cone * smoothstep(0.08, 0.3, above));
                            fandbg = vec3(g, cone, clamp(above / 3.0, 0.0, 1.0));
                        } else {
                            float blade = smoothstep(0.62, 0.9, cos(3.0 * ang)) * smoothstep(0.08, 0.2, rr);
                            float hub = 1.0 - smoothstep(0.10, 0.2, rr);
                            att *= 1.0 - 0.85 * max(blade, hub);
                        }
                    }
                }
                if (u_ldir[i].w <= 0.0) reach += att * att;
                float ndl = clamp((dot(n, l) * inversesqrt(d2 + 1e-6) + 0.25) / 1.25, 0.0, 1.0);
                if (att <= 0.0 || ndl <= 0.0) continue;
                float code = u_lcol[i].w;
                int bk = code > 0.5 ? int(mod(code, 256.0) + 0.5) - 1 : -1, tile = code > 0.5 ? int(code / 256.0) - 1 : -1;
                if (bk >= 0 || tile >= 0) {
                    vec3 ldn = l * inversesqrt(d2 + 1e-6);
                    float tl = bk >= 0 ? bake_light(bk, tile, p, n, ldn) : tile_light(tile, p, n, ldn);
                    if (u_ldir[i].w <= 0.0) {
                        float w = att * att * ndl;
                        shw += w;
                        shb += w * (1.0 - tl);
                    }
                    att *= mix(0.12, 1.0, tl);
                } else if (code > -0.5 && u_lsh != 0) {
                    // march from the surface toward the light; something in the depth buffer in front of
                    // the ray (and not much thicker than a limb) blocks it
                    float dist = sqrt(d2), len = min(dist * 0.9, 1.2);
                    vec3 ldn = l / max(dist, 1e-4), s0 = p + n * 0.04;
                    float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
                    float lit = 1.0;
                    for (int k = 0; k < 8; k++) {
                        vec3 sp = s0 + ldn * (len * (float(k) + jit) / 8.0);
                        vec4 cp = u_proj * vec4(sp, 1.0);
                        vec2 suv = cp.xy / cp.w * 0.5 + 0.5;
                        if (suv.x < 0.0 || suv.y < 0.0 || suv.x > 1.0 || suv.y > 1.0) break;
                        float sd = texture(u_depth, suv).r;
                        vec4 sq = u_invp * vec4(suv * 2.0 - 1.0, u_revz != 0 ? 1.0 - 2.0 * sd : 2.0 * sd - 1.0, 1.0);
                        float gap = sq.z / sq.w - sp.z;  // > 0: the scene is in front of the sample
                        if (gap > 0.04 && gap < 0.45) { lit = 0.0; break; }
                    }
                    att *= mix(0.25, 1.0, lit);
                }
                if (u_ldir[i].w > 0.0) spot += u_lcol[i].rgb * (att * att * ndl);
                else sum += u_lcol[i].rgb * (att * att * ndl);
                if (u_spec > 0.0) {  // Blinn-Phong
                    vec3 ld = l * inversesqrt(d2 + 1e-6);
                    float nh = max(dot(n, normalize(ld + v)), 0.0);
                    spec += u_lcol[i].rgb * (att * att * pow(nh, 40.0) * step(0.0, dot(n, ld)));
                }
            }
            // No material data: brighter, less saturated surfaces (metal, armour plates) shine more than
            // dark or strongly coloured ones (cloth, flesh).
            float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
            float gloss = (0.12 + mx) * (1.0 - 0.7 * (mx - mn) / max(mx, 0.05));
            // Spotlights (alarm beacons) keep their own colour: the scene under an alarm is tinted red, and a
            // blue light multiplied by a red surface would all but vanish. They use the surface's brightness.
            if (u_tdebug > 0) { frag = vec4(vec3(tile_light(u_tdebug - 1, p, n, vec3(0.0))) * (0.3 + 0.7 * max(c.r, max(c.g, c.b))), 1.0); return; }
            float luma = dot(c, vec3(0.35, 0.45, 0.2));
            if (u_fanshade > 1.5) { frag = vec4(fandbg, 1.0); return; }  // dev: show the blade shadow term
            // The level's light fixtures are already painted into its textures, so something that blocks one
            // has to take that light away again: the pixel darkens by the blocked share of the fixtures
            // that reach it. This is what gives characters and props a shadow on the far side of each
            // nearby light, darker the more that light dominates there.
            // (characters are not lit by the painted light in the first place: they darken less)
            // (a lamp's own face, and anything else the game drew bright, is not darkened: the light sits on
            // the lamp's surface, which would otherwise count as shadowed by the wall it is set in)
            float lampface = smoothstep(0.6, 0.95, max(c0.r, max(c0.g, c0.b)) / tl);
            // Environment light. The game's lighting is painted on and cannot react to anything, so around the
            // added lamps it is turned down to a base level (level, props, characters) and the lamps bring
            // their surroundings back up: what blocks one then leaves a clearly darker shadow. Where no
            // added lamp reaches, nothing would bring it back, so the painted lighting is left as it is.
            // (only as far as the added light can bring a pixel back: a bright surface has almost no headroom
            // left for it, so it is hardly turned down; otherwise white rooms go grey)
            float dimroom = pow(clamp((1.0 - max(c0.r, max(c0.g, c0.b)) / tl) * 1.6, 0.0, 1.0), 1.5);
            if (u_env < 1.0) c *= 1.0 - (1.0 - u_env) * mix(1.0 - lampface, 0.15 * dimroom, bright) * smoothstep(0.0, 0.35, reach);
            // (lamps that barely reach a pixel cannot throw a strong shadow on it: at least a quarter of a
            // full lamp's weight is assumed, so a blocked, faint, far lamp darkens little)
            c *= 1.0 - u_lshade * (gn.a < 0.85 ? 0.4 : 1.0) * clamp(shb / max(shw, 0.25), 0.0, 1.0) * (1.0 - lampface);
            c *= 1.0 - u_fanshade * fanshade;
            // (lights work on the surface as the game lit it, so a lit spot comes back to full brightness)
            // Added light can only fill the headroom a pixel has left: a surface the game already lit to
            // near white (the white medical decks) gains almost nothing, a dark one takes the light fully.
            // Without this, bright levels burn out to white.
            // Under a world tint the added point lights take the tint too, so they deepen the colour instead
            // of washing it out; the headroom is one number for all channels, for the same reason.
            // (spotlights keep their colour but dim with a dark tint: a blacked-out room stays black)
            // (All of this in step with how bright the room already is: a dark corridor is lit as before.)
            // Many lamps reach the same spot (a room can have a hundred): their sum is compressed, so that
            // a place lit by ten lamps is not ten times as bright as one lit by a single lamp. And the light
            // works on the surface's own colour: a black surface stays black (only a trace is added to it),
            // otherwise every dark area of a well-lit room is lifted to grey.
            vec3 lit = sum * u_tint.rgb;
            lit /= 1.0 + 0.6 * bright * max(lit.r, max(lit.g, lit.b));
            vec3 add = (lit * (c0 * mix(1.2 + 2.2 * (1.0 - u_env), 0.4, bright) + mix(0.03, 0.004, bright)) + spot * tl * (luma * 1.6 + mix(0.07, 0.02, bright) * tl)) * u_lgain + spec * u_tint.rgb * gloss * u_spec;
            vec3 room = vec3(clamp(1.0 - max(c0.r, max(c0.g, c0.b)) / tl, 0.0, 1.0));
            c += min(add, vec3(0.45 * tl)) * room * room;  // (and no single pixel gains more than this)
            c = min(c, max(c0, vec3(0.92 * tl)));  // and never past what the game itself shows as bright
        }
    }
    if (u_aos > 0.0) {
        // bright pixels (lights, glows, light shafts) are left alone
        float l = max(c.r, max(c.g, c.b));
        float a = clamp(mix(1.0, texture(u_ao, uv).r, u_aos * (1.0 - smoothstep(0.55, 0.95, l / clamp(max(u_tint.r, max(u_tint.g, u_tint.b)), 0.02, 1.0)))), 0.0, 1.0);
        c *= a;
        if (u_aodebug != 0) c = vec3(clamp(mix(1.0, texture(u_ao, uv).r, u_aos), 0.0, 1.0));
    }
    // Fog drawn over the scene is not a surface: what the lights, shadows and occlusion did to the pixel
    // belongs to the wall behind it, so it applies only to the share of the pixel that the fog leaves
    // open. (Otherwise the fog takes on the lit and shadowed shapes of whatever is behind it.)
    if (u_veilon != 0 && u_aodebug == 0) c = mix(c, c0, clamp(texture(u_veil, uv).r, 0.0, 1.0));
    if (u_bloom > 0.0 && u_aodebug == 0) {
        // average of the three blur levels, screen-blended: areas that are already bright gain little
        vec3 b = min((texture(u_b1, uv).rgb + texture(u_b2, uv).rgb + texture(u_b3, uv).rgb) * (u_bloom / 3.0), 1.0);
        c = c + b * (1.0 - clamp(c, 0.0, 1.0));
    }
    if (u_vignette > 0.0) {
        // soft darkening toward the edges and corners (the HUD is drawn afterwards and stays clear)
        vec2 q = (uv - 0.5) * vec2(1.0, 0.85);
        // (lighter in a bright room: there it would turn the white walls at the sides grey)
        c *= 1.0 - u_vignette * mix(1.0, 0.3, bright) * smoothstep(0.28, 0.75, length(q) * 1.25);
    }
    if (u_nrmdebug != 0) c = texture(u_nrm, uv).rgb * texture(u_nrm, uv).a;
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

bool make_target(Target& t, int w, int h, GLint fmt = GL_RGBA8) {
    t.w = std::max(1, w);
    t.h = std::max(1, h);
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
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
    ok &= make_target(g_nrm, w, h, 0x8059 /*RGB10_A2*/);
    ok &= make_target(g_veil, w, h);
    ok &= make_target(g_av[0], w / 8, h / 8) && make_target(g_av[1], w / 64, h / 64) && make_target(g_av[2], 4, 4) && make_target(g_av[3], 1, 1);
    // shadow map: depth only
    glGenTextures(1, &g_shmap);
    glBindTexture(GL_TEXTURE_2D, g_shmap);
    glTexImage2D(GL_TEXTURE_2D, 0, 0x81A6 /*DEPTH_COMPONENT24*/, kShadowSize, kShadowSize, 0, 0x1902 /*DEPTH_COMPONENT*/, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    p_glGenFramebuffers(1, &g_shfbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_shfbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, 0x8D00 /*DEPTH_ATTACHMENT*/, GL_TEXTURE_2D, g_shmap, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    g_shadow_ok = p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glGenTextures(1, &g_atlas);
    glBindTexture(GL_TEXTURE_2D, g_atlas);
    glTexImage2D(GL_TEXTURE_2D, 0, 0x81A6 /*DEPTH_COMPONENT24*/, kTileSize * 4, kTileSize * 6, 0, 0x1902 /*DEPTH_COMPONENT*/, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
    p_glGenFramebuffers(1, &g_atlas_fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_atlas_fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, 0x8D00 /*DEPTH_ATTACHMENT*/, GL_TEXTURE_2D, g_atlas, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    g_atlas_ok = p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    // a second atlas keeps each map's depth of the still scene; a depth-only program draws that scene
    glGenTextures(1, &g_satlas);
    glBindTexture(GL_TEXTURE_2D, g_satlas);
    glTexImage2D(GL_TEXTURE_2D, 0, 0x81A6 /*DEPTH_COMPONENT24*/, kBakeSize * kBakeCols, kBakeSize * kBakeRows, 0, 0x1902 /*DEPTH_COMPONENT*/, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenTextures(1, &g_bmat_tex);
    glBindTexture(GL_TEXTURE_2D, g_bmat_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, 0x8814 /*RGBA32F*/, 5, kBakes, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glGenFramebuffers(1, &g_satlas_fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_satlas_fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, 0x8D00 /*DEPTH_ATTACHMENT*/, GL_TEXTURE_2D, g_satlas, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    g_atlas_ok = g_atlas_ok && p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    {
        GLuint v = compile(GL_VERTEX_SHADER, "#version 130\nin vec3 a_p;\nuniform mat4 u_m;\nvoid main() { gl_Position = u_m * vec4(a_p, 1.0); }\n");
        GLuint f = compile(GL_FRAGMENT_SHADER, "#version 130\nvoid main() {}\n");
        if (v && f) {
            g_p_depth = p_glCreateProgram();
            p_glAttachShader(g_p_depth, v);
            p_glAttachShader(g_p_depth, f);
            p_glBindAttribLocation(g_p_depth, 0, "a_p");
            p_glLinkProgram(g_p_depth);
            GLint linked = 0;
            p_glGetProgramiv(g_p_depth, GL_LINK_STATUS, &linked);
            if (!linked) g_p_depth = 0;
        }
        p_glGenVertexArrays(1, &g_svao);
        p_glGenBuffers(1, &g_svbo);
        p_glGenBuffers(1, &g_sibo);
    }
    if (!g_atlas_ok) LOGW("shadows: light shadow atlas incomplete; lights cast no mapped shadows");
    if (!g_shadow_ok) LOGW("shadows: shadow map framebuffer incomplete; disabled");
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

bool post_wanted() { return g_bloom > 0 || g_sharpen > 0 || g_ao > 0 || g_lights > 0 || g_shadows > 0 || g_vignette || g_normals_debug; }

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
// ---------------------------------------------------------------- dev: shader reload
std::string g_reload_path;
void (*g_reload_done)(const std::string&) = nullptr;
void do_reload_shaders() {
    std::string path;
    path.swap(g_reload_path);
    std::string src, result;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[65536];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) src.append(buf, n);
        std::fclose(f);
    }
    if (src.empty()) {
        if (g_reload_done) g_reload_done("shaders: cannot read " + path);
        return;
    }
    const struct {
        const char* name;
        GLuint* prog;
    } kProgs[] = {{"kBrightFS", &g_p_bright}, {"kDownFS", &g_p_down}, {"kBlurFS", &g_p_blur},
                  {"kAoFS", &g_p_ao},         {"kAoBlurFS", &g_p_aoblur}, {"kFinalFS", &g_p_final}};
    auto literal = [&](const std::string& name) {
        size_t a = src.find("const char* " + name + " = R\"(");
        if (a == std::string::npos) return std::string();
        a = src.find("R\"(", a) + 3;
        size_t b = src.find(")\";", a);
        return b == std::string::npos ? std::string() : src.substr(a, b - a);
    };
    const std::string vs_src = literal("kPostVS");
    int ok_count = 0;
    for (auto& p : kProgs) {
        const std::string fs_src = literal(p.name);
        if (fs_src.empty() || vs_src.empty()) {
            result += std::string(" ") + p.name + ": not found;";
            continue;
        }
        const char* vp = vs_src.c_str();
        const char* fp = fs_src.c_str();
        GLuint vs = p_glCreateShader(GL_VERTEX_SHADER), fs = p_glCreateShader(GL_FRAGMENT_SHADER);
        p_glShaderSource(vs, 1, &vp, nullptr);
        p_glCompileShader(vs);
        p_glShaderSource(fs, 1, &fp, nullptr);
        p_glCompileShader(fs);
        GLint ok = 0;
        p_glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1500];
            p_glGetShaderInfoLog(fs, sizeof log, nullptr, log);
            result += std::string("\n  ") + p.name + " DID NOT COMPILE (old one kept): " + log;
            continue;
        }
        GLuint prog = p_glCreateProgram();
        p_glAttachShader(prog, vs);
        p_glAttachShader(prog, fs);
        p_glBindAttribLocation(prog, 0, "a_p");
        p_glLinkProgram(prog);
        p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            result += std::string("\n  ") + p.name + " DID NOT LINK (old one kept)";
            continue;
        }
        *p.prog = prog;  // (the old program is left to the driver: a handful per session)
        ok_count++;
    }
    if (g_reload_done) g_reload_done("shaders: " + std::to_string(ok_count) + " of 6 post shaders reloaded" + result);
}

void run_post() {
    if (!g_reload_path.empty()) do_reload_shaders();
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
        bool have_proj = g_have_world_proj && invert(g_world_proj, inv_proj);
        bool ao = g_ao > 0 && have_proj;
        bool have_nrm = g_normals_cleared && hle::g_normal_target;
        g_frame_lights.clear();
        if (g_lights > 0 && have_proj && have_nrm) {
            g_frame_lights = g_spots;  // the lights fixed at the start of the frame (with their shadow tiles) first
            lights::eye_space(g_frame_lights);
        }
        if (g_frame_lights.size() > 128) g_frame_lights.resize(128);
        bool shadows = g_shadows > 0 && g_shadow_cleared && have_proj && have_nrm;
        p_glBlitFramebuffer(0, 0, g_scene.w, g_scene.h, 0, 0, g_scene.w, g_scene.h,
                            GL_COLOR_BUFFER_BIT | (ao || shadows || !g_frame_lights.empty() ? GL_DEPTH_BUFFER_BIT : 0), GL_NEAREST);
        if (have_nrm) {  // resolved copy of the normal target
            glReadBuffer(GL_COLOR_ATTACHMENT0 + 1);
            p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_nrm.fbo);
            p_glBlitFramebuffer(0, 0, g_nrm.w, g_nrm.h, 0, 0, g_nrm.w, g_nrm.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            if (hle::g_veil_target && g_veil_draws > 0) {  // and of the fog coverage
                glReadBuffer(GL_COLOR_ATTACHMENT0 + 2);
                p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_veil.fbo);
                p_glBlitFramebuffer(0, 0, g_veil.w, g_veil.h, 0, 0, g_veil.w, g_veil.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
            glReadBuffer(GL_COLOR_ATTACHMENT0);
        }
        {  // the scene's average colour (how bright the game itself lit this view)
            GLint rfb = 0, dfb = 0;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
            const Target* from = &g_scene;
            for (Target& to : g_av) {
                p_glBindFramebuffer(GL_READ_FRAMEBUFFER, from->fbo);
                p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to.fbo);
                p_glBlitFramebuffer(0, 0, from->w, from->h, 0, 0, to.w, to.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                from = &to;
            }
            p_glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(rfb));
            p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(dfb));
        }
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
            p_glActiveTexture(GL_TEXTURE0 + 1);
            glBindTexture(GL_TEXTURE_2D, g_nrm.tex);
            p_glActiveTexture(GL_TEXTURE0);
            p_glUniform1i(p_glGetUniformLocation(g_p_ao, "u_nrm"), 1);
            p_glUniform1i(p_glGetUniformLocation(g_p_ao, "u_have_nrm"), have_nrm ? 1 : 0);
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
        const char* names[14] = {"u_scene", "u_b1", "u_b2", "u_b3", "u_ao", "u_nrm", "u_depth", "u_shmap", "u_atlas", "u_gobo", "u_veil", "u_batlas", "u_bmat", "u_avg"};
        const GLuint texs[14] = {g_scene.tex, g_ba[0].tex, g_ba[1].tex, g_ba[2].tex, g_aoa.tex, g_nrm.tex, g_scene_depth, g_shmap, g_atlas, g_gobo_tex, g_veil.tex, g_satlas, g_bmat_tex, g_av[3].tex};
        p_glUniform1i(p_glGetUniformLocation(g_p_final, "u_veilon"), have_nrm && hle::g_veil_target && g_veil_draws > 0 && g_veil_on ? 1 : 0);
        for (int u = 0; u < 14; u++) {
            p_glActiveTexture(GL_TEXTURE0 + u);
            glBindTexture(GL_TEXTURE_2D, texs[u]);
            p_glUniform1i(p_glGetUniformLocation(g_p_final, names[u]), u);
        }
        p_glUniform2f(p_glGetUniformLocation(g_p_final, "u_px"), 1.0f / g_scene.w, 1.0f / g_scene.h);
        p_glUniform1f(p_glGetUniformLocation(g_p_final, "u_sharp"), kSharp[g_sharpen]);
        p_glUniform1f(p_glGetUniformLocation(g_p_final, "u_bloom"), kBloom[g_bloom]);
        p_glUniform1f(p_glGetUniformLocation(g_p_final, "u_aos"), ao ? kAoStrength[g_ao] : 0.0f);
        p_glUniform1i(p_glGetUniformLocation(g_p_final, "u_aodebug"), g_ao_debug ? 1 : 0);
        {
            static const float kLightGain[4] = {0.0f, 0.6f, 1.0f, 1.5f};
            int nl = int(g_frame_lights.size());
            static float lp[512], lc[512], ld[512], lx[512], lg[512];
            for (int i = 0; i < nl; i++) {
                const Light& l = g_frame_lights[size_t(i)];
                lp[i * 4] = l.pos[0], lp[i * 4 + 1] = l.pos[1], lp[i * 4 + 2] = l.pos[2], lp[i * 4 + 3] = l.radius;
                lx[i * 4] = l.gobo, lx[i * 4 + 1] = l.gobo > 1.5f ? l.gh : l.phase, lx[i * 4 + 2] = l.guv[0], lx[i * 4 + 3] = l.guv[1];
                for (int k = 0; k < 4; k++) lg[i * 4 + k] = l.gm[k];
                ld[i * 4] = l.dir[0], ld[i * 4 + 1] = l.dir[1], ld[i * 4 + 2] = l.dir[2], ld[i * 4 + 3] = l.cos_outer;
                lc[i * 4] = l.color[0], lc[i * 4 + 1] = l.color[1], lc[i * 4 + 2] = l.color[2], lc[i * 4 + 3] = l.bake >= 0 || l.tile >= 0 ? float(l.bake + 1) + 256.0f * float(l.tile + 1)  // kept map, live tile
                                : l.id && g_light_shadows ? -1.0f : 0.0f;  // a fixed light without a map casts no shadow; 0: a passing light
            }
            p_glUniform1i(p_glGetUniformLocation(g_p_final, "u_nl"), nl);
            {  // this frame's world tint (the world has been drawn by now)
                const float t4[4] = {g_tint_seen ? g_tint_now[0] : 1.0f, g_tint_seen ? g_tint_now[1] : 1.0f, g_tint_seen ? g_tint_now[2] : 1.0f, 1.0f};
                p_glUniform4fv(p_glGetUniformLocation(g_p_final, "u_tint"), 1, t4);
            }
            p_glUniform1f(p_glGetUniformLocation(g_p_final, "u_env"), g_lights > 0 && have_nrm ? kEnv[g_env_light] : 1.0f);
            static const float kShadow[4] = {0.0f, 0.4f, 0.6f, 0.8f};  // at full light; scaled by g_shadow_opacity
            p_glUniform1f(p_glGetUniformLocation(g_p_final, "u_shs"), shadows ? std::min(0.85f, kShadow[g_shadows] * g_shadow_opacity) : 0.0f);
            if (shadows) {
                p_glUniformMatrix4fv(p_glGetUniformLocation(g_p_final, "u_shmat"), 1, GL_FALSE, g_light_from_eye.m);
                { const float up4[4] = {g_up_eye[0], g_up_eye[1], g_up_eye[2], 0.0f}; p_glUniform4fv(p_glGetUniformLocation(g_p_final, "u_upe"), 1, up4); }
                p_glUniform1f(p_glGetUniformLocation(g_p_final, "u_shrange"), kShadowTop + kShadowBottom);
                p_glUniform1f(p_glGetUniformLocation(g_p_final, "u_shsoft"), 0.045f / (2.0f * kShadowHalf));
                p_glUniform1i(p_glGetUniformLocation(g_p_final, "u_shdebug"), g_shadow_debug ? 1 : 0);
            }
            if (nl || shadows) {
                p_glUniformMatrix4fv(p_glGetUniformLocation(g_p_final, "u_invp"), 1, GL_FALSE, inv_proj.m);
                p_glUniform1i(p_glGetUniformLocation(g_p_final, "u_revz"), use_revz() ? 1 : 0);
            }
            if (nl) {
                p_glUniform4fv(p_glGetUniformLocation(g_p_final, "u_lpos"), nl, lp);
                p_glUniform4fv(p_glGetUniformLocation(g_p_final, "u_lcol"), nl, lc);
                p_glUniform4fv(p_glGetUniformLocation(g_p_final, "u_ldir"), nl, ld);
                p_glUniform4fv(p_glGetUniformLocation(g_p_final, "u_lx"), nl, lx);
                p_glUniform4fv(p_glGetUniformLocation(g_p_final, "u_lg"), nl, lg);
                p_glUniform2f(p_glGetUniformLocation(g_p_final, "u_gobo_range"), g_gobo_lo, g_gobo_hi);
                p_glUniform1f(p_glGetUniformLocation(g_p_final, "u_fanshade"), g_fanshade);
                p_glUniform1f(p_glGetUniformLocation(g_p_final, "u_lshade"), g_ntiles > 0 ? g_lshade : 0.0f);
                p_glUniform4fv(p_glGetUniformLocation(g_p_final, "u_gobo_rect"), 1, g_gobo_rect);
                {
                    const float* vm = g_view.m;
                    const float ax[9] = {vm[0], vm[1], vm[2], vm[4], vm[5], vm[6], vm[8], vm[9], vm[10]};
                    static const char* an[3] = {"u_wx", "u_wy", "u_wz"};
                    for (int k = 0; k < 3; k++) {
                        const float a4[4] = {ax[k * 3], ax[k * 3 + 1], ax[k * 3 + 2], 0};
                        GLint loc = p_glGetUniformLocation(g_p_final, an[k]);
                        if (loc >= 0) p_glUniform3fv_(loc, a4);
                    }
                }
                p_glUniform1i(p_glGetUniformLocation(g_p_final, "u_tdebug"), g_tile_debug <= g_ntiles ? g_tile_debug : 0);
                if (g_ntiles > 0) p_glUniformMatrix4fv(p_glGetUniformLocation(g_p_final, "u_tm"), g_ntiles, GL_FALSE, g_tile_from_eye[0].m);
                p_glUniformMatrix4fv(p_glGetUniformLocation(g_p_final, "u_invp"), 1, GL_FALSE, inv_proj.m);
                p_glUniform1i(p_glGetUniformLocation(g_p_final, "u_revz"), use_revz() ? 1 : 0);
                p_glUniform1f(p_glGetUniformLocation(g_p_final, "u_lgain"), kLightGain[g_lights]);
                p_glUniformMatrix4fv(p_glGetUniformLocation(g_p_final, "u_proj"), 1, GL_FALSE, g_world_proj.m);
                p_glUniform1i(p_glGetUniformLocation(g_p_final, "u_lsh"), g_light_shadows ? 1 : 0);
                static const float kSpec[4] = {0.0f, 0.35f, 0.7f, 1.2f};
                p_glUniform1f(p_glGetUniformLocation(g_p_final, "u_spec"), kSpec[g_specular]);
            }
        }
        p_glUniform1f(p_glGetUniformLocation(g_p_final, "u_vignette"), g_vignette ? 0.55f : 0.0f);
        p_glUniform1i(p_glGetUniformLocation(g_p_final, "u_nrmdebug"), g_normals_debug && have_nrm ? 1 : 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        p_glBindVertexArray(0);
        p_glUseProgram(0);
    }
    // back to what the game and the GLES layer expect (draw state is re-applied by the next draw)
    p_glBindFramebuffer(GL_FRAMEBUFFER, hle::g_default_fbo);
    for (int u = 9; u >= 0; u--) {
        p_glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, u < 2 ? g_st.unit[u].tex : 0);
    }
    p_glActiveTexture(GL_TEXTURE0 + g_legacy_unit);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glDepthMask(g_st.depth_write);
}

// ---------------------------------------------------------------- shadow map
// The game has no lights, and the level's shadows are baked into its textures. What is missing are
// shadows of things that move. Those are found by watching the draws: a mesh whose vertex data changes
// from frame to frame (CPU-skinned characters) or whose world transform changes (props, doors) is a
// caster from then on. Casters are drawn a second time into a depth map, seen from a light above the
// camera target; the final pass darkens upward-facing surfaces that lie below something in that map.

struct Seen {
    u64 hash = 0;
    std::vector<std::array<float, 12>> prev, cur;  // world transforms of last frame's / this frame's draws
    bool dynamic = false, touched = false;
    int moves = 0;  // frames in which its transform was seen to change
    int still = 0;  // draws in a row at exactly the same place
    u64 key = 0;
};
struct CasterPt {
    float p[3];
};
std::vector<CasterPt> g_casters_now, g_casters;  // centres of the moving meshes: being drawn / of the last frame
std::unordered_map<u64, Seen> g_seen;
std::vector<Seen*> g_touched, g_pending;
int g_cmp = 0, g_chg = 0;

void lamps_drop(u64 base);
// A mesh started to move: it leaves the still scene (its shadow is drawn every frame from now on).
void static_kill(u64 base) {
    lamps_drop(base);
    for (SMesh& m : g_smesh)
        if (m.base == base && !m.dead) {
            m.dead = true;
            SBox b;
            std::memcpy(b.lo, m.lo, 12);
            std::memcpy(b.hi, m.hi, 12);
            g_schanges.push_back(b);
        }
}

void shadow_frame_end() {
    // A frame in which most transforms "changed" means the camera estimate was off, not that the world moved.
    if (g_cmp > 0 && g_chg * 10 < g_cmp * 3)
        for (Seen* s : g_pending)
            if (++s->moves >= 4 && !s->dynamic) {  // (a still prop can seem to shift for a frame while the camera moves)
                s->dynamic = true;
                static_kill(s->key);
            }
    g_pending.clear();
    for (Seen* s : g_touched) {
        s->prev.swap(s->cur);
        s->cur.clear();
        s->touched = false;
    }
    g_touched.clear();
    g_cmp = g_chg = 0;
    if (g_seen.size() > 60000) g_seen.clear();
}

// Sets up this frame's light (once); false when there is no gameplay camera.
bool shadow_frame() {
    if (g_shadow_frame != 0) return g_shadow_frame > 0;
    g_shadow_frame = -1;
    float v[16], t[3];
    if (!g_shadow_ok || g_post_state != 1 || !lights::camera_matrix(v, t)) return false;
    std::memcpy(g_cam_target, t, 12);
    Mat4 view;
    std::memcpy(view.m, v, sizeof v);
    if (!invert(view, g_view_inv)) return false;
    g_view = view;
    g_up_eye[0] = v[4], g_up_eye[1] = v[5], g_up_eye[2] = v[6];  // view * (0,1,0,0)
    // The floor shadows follow the lights around the player: each nearby light pushes the shadow away from
    // itself (the lower and further to the side the light, the longer the shadow) and adds to its
    // strength; with no light near, a faint shadow falls almost straight down.
    setup_frame_lights();
    {
        const float pl[3] = {t[0], t[1] - 0.4f, t[2]};  // about the player's chest (the camera looks at the shoulders)
        float lean[2] = {0, 0}, wsum = 0;
        for (const Light& li : g_spots) {
            float d[3] = {li.wpos[0] - pl[0], li.wpos[1] - pl[1], li.wpos[2] - pl[2]};
            float dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], reach = li.radius * 1.8f;
            if (dist2 >= reach * reach || d[1] < 0.3f) continue;  // out of reach, or not above the player
            float w = 1.0f - dist2 / (reach * reach);
            w *= w * (li.color[0] * 0.35f + li.color[1] * 0.45f + li.color[2] * 0.2f);
            if (li.cos_outer > 0) {  // spotlight: only while its cone points at the player
                float dl = std::sqrt(dist2), cd = -(d[0] * li.wdir[0] + d[1] * li.wdir[1] + d[2] * li.wdir[2]) / std::max(dl, 1e-4f);
                w *= std::clamp((cd - li.cos_outer) / (1.0f - li.cos_outer) * 2.0f, 0.0f, 1.0f);
            }
            if (w <= 0) continue;
            lean[0] += w * -d[0] / d[1], lean[1] += w * -d[2] / d[1];
            wsum += w;
        }
        float want[2] = {0.12f, 0.08f}, opacity = 0.35f;
        // With shadow maps on the fixtures themselves, the directional shadows come from those; this map
        // stays as a faint contact shadow straight below.
        if (wsum > 1e-4f && !(g_light_shadows && g_atlas_ok)) {
            want[0] = lean[0] / wsum, want[1] = lean[1] / wsum;
            float len = std::hypot(want[0], want[1]);
            if (len > 1.6f) want[0] *= 1.6f / len, want[1] *= 1.6f / len;
            opacity = std::clamp(0.35f + wsum * 0.6f, 0.35f, 1.25f);
        }
        // eased, so the shadow slides instead of jumping as lights come and go
        static u32 last_ms = 0;
        u32 now = SDL_GetTicks();
        float k = last_ms == 0 || now - last_ms > 500 ? 1.0f : 1.0f - std::exp(-float(now - last_ms) / 120.0f);
        last_ms = now;
        for (int i = 0; i < 2; i++) g_shadow_lean[i] += (want[i] - g_shadow_lean[i]) * k;
        g_shadow_opacity += (opacity - g_shadow_opacity) * k;
        static int logn = 0;
        if (g_shadow_debug && (logn++ % 120) == 0)
            LOGI("floor shadow: lean %.2f %.2f opacity %.2f (light weight %.3f)", g_shadow_lean[0], g_shadow_lean[1], g_shadow_opacity, wsum);
    }
    const float* kShadowLean = g_shadow_lean;
    // world -> shadow clip space: x, y across the floor (sheared along the light), z from the top down
    Mat4 l{};
    l.m[0] = 1.0f / kShadowHalf, l.m[4] = kShadowLean[0] / kShadowHalf, l.m[12] = -(t[0] + kShadowLean[0] * t[1]) / kShadowHalf;
    l.m[9] = 1.0f / kShadowHalf, l.m[5] = kShadowLean[1] / kShadowHalf, l.m[13] = -(t[2] + kShadowLean[1] * t[1]) / kShadowHalf;
    float range = kShadowTop + kShadowBottom;
    l.m[6] = -2.0f / range, l.m[14] = 2.0f * (t[1] + kShadowTop) / range - 1.0f;
    l.m[15] = 1.0f;
    g_light_from_eye = l * g_view_inv;
    g_shadow_frame = 1;
    return true;
}

// Is the mesh being drawn (vertex range lo..lo+n of the bound position array) something that moves?
bool caster(u32 lo, u32 n) {
    const Array& a = g_arr[A_VERTEX];
    u64 key = (u64(reinterpret_cast<uintptr_t>(a.data)) * 0x9E3779B97F4A7C15ull) ^ (u64(lo) << 32) ^ n;
    Seen& s = g_seen[key];
    u64 h = 1469598103934665603ull;
    u32 stepv = std::max(1u, n / 48);
    for (u32 i = 0; i < n; i += stepv) {
        u32 b[3];
        std::memcpy(b, &g_verts[size_t(i) * kStride], 12);
        for (u32 w : b) h = (h ^ w) * 1099511628211ull;
    }
    Mat4 world = g_view_inv * g_mv.top();
    std::array<float, 12> w;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 3; r++) w[size_t(c * 3 + r)] = world.m[c * 4 + r];
    bool seen_before = s.hash != 0;
    s.key = key;
    if (seen_before && s.hash != h && !s.dynamic) {  // vertex data rewritten: skinned
        s.dynamic = true;
        static_kill(key);
    }
    s.hash = h ? h : 1;
    if (!s.dynamic && !s.prev.empty()) {
        bool same = false;
        float nearest = 1e9f;
        for (const auto& p : s.prev) {
            float d = 0;
            for (size_t i = 0; i < 12; i++) d = std::max(d, std::fabs(p[i] - w[i]));
            nearest = std::min(nearest, d);
            if (d < 0.02f) same = true;
        }
        s.still = nearest < 0.003f ? s.still + 1 : 0;
        g_cmp++;
        if (!same) {
            g_chg++;
            g_pending.push_back(&s);
        }
    }
    s.cur.push_back(w);
    if (!s.touched) {
        s.touched = true;
        g_touched.push_back(&s);
    }
    g_mesh_prop = false;
    {
        // where this mesh is: its bounds (sampled)
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (u32 i = 0; i < n; i += stepv) {
            const float* p = &g_verts[size_t(i) * kStride];
            for (int r = 0; r < 3; r++) {
                float v = world.m[r] * p[0] + world.m[4 + r] * p[1] + world.m[8 + r] * p[2] + world.m[12 + r];
                lo[r] = std::min(lo[r], v), hi[r] = std::max(hi[r], v);
            }
        }
        for (int r = 0; r < 3; r++) g_mesh_lo[r] = lo[r], g_mesh_hi[r] = hi[r];
        if (g_probe_on) {
            float cx = (lo[0] + hi[0]) * 0.5f, cy = (lo[1] + hi[1]) * 0.5f, cz = (lo[2] + hi[2]) * 0.5f;
            if (std::fabs(cx - g_probe[0]) < g_probe[3] && std::fabs(cz - g_probe[2]) < g_probe[3] && std::fabs(cy - g_probe[1]) < 4.0f)
                LOGI("probe mesh: %u verts, bounds %.2f %.2f %.2f .. %.2f %.2f %.2f, moving %d", n, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], s.dynamic);
        }
        // A still mesh the size of an object (not a stretch of wall, floor or ceiling) is a prop: it casts
        // shadows from the fixtures like the things that move.
        float ex = hi[0] - lo[0], ey = hi[1] - lo[1], ez = hi[2] - lo[2], big = std::max(ex, std::max(ey, ez));
        g_mesh_prop = !s.dynamic && n >= 8 && n <= 1500 && big > 0.4f && big < 4.5f && ex * ey * ez < 12.0f;
        if ((s.dynamic || g_mesh_prop) && g_casters_now.size() < 256)
            g_casters_now.push_back({{(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f}});
    }
    // the still scene: a mesh that has stayed exactly in place is copied into it, once per placement
    g_mesh_cached = false;
    if (!s.dynamic && s.still >= 8 && g_light_shadows && g_lights > 0) {
        u64 ik = key;
        for (size_t i = 0; i < 12; i++) ik = (ik ^ u64(std::llround(double(w[i]) * 100.0))) * 1099511628211ull;
        auto it = g_skey.find(ik);
        if (it != g_skey.end()) {
            g_mesh_cached = !g_smesh[it->second].dead;
        } else if (g_sverts.size() < 9000000 && g_sidx.size() < 12000000 && g_idx.size() >= 3) {
            SMesh m{};
            m.first = u32(g_sidx.size());
            m.count = u32(g_idx.size());
            m.base = key;
            u32 basev = u32(g_sverts.size() / 3);
            for (int r = 0; r < 3; r++) m.lo[r] = 1e9f, m.hi[r] = -1e9f;
            for (u32 i = 0; i < n; i++) {
                const float* p = &g_verts[size_t(i) * kStride];
                for (int r = 0; r < 3; r++) {
                    float v = world.m[r] * p[0] + world.m[4 + r] * p[1] + world.m[8 + r] * p[2] + world.m[12 + r];
                    g_sverts.push_back(v);
                    m.lo[r] = std::min(m.lo[r], v), m.hi[r] = std::max(m.hi[r], v);
                }
            }
            for (u16 ix : g_idx) g_sidx.push_back(basev + std::min(u32(ix), n - 1));
            g_skey[ik] = g_smesh.size();
            g_smesh.push_back(m);
            SBox b;
            std::memcpy(b.lo, m.lo, 12);
            std::memcpy(b.hi, m.hi, 12);
            g_schanges.push_back(b);
            g_supload = true;
            // (this frame it is still drawn into the maps directly; from the next one it is in the kept depth)
        }
    }
    return s.dynamic;
}

// ---- spinning alarm beacons
// The levels' rotating alarm lamps (a pole from the ceiling with two blue cones of light) have no light
// in the game. The cones are one additive mesh, rewritten every frame as it turns: two opposite cones
// from a common centre. A mesh of that shape that keeps turning in place gets a spotlight along each
// cone. The turning light shafts under ceiling fans are upright fins through a vertical axis; those are
// recognised and never lit.
struct Spin {
    float c[3];   // world-space centre when last drawn
    float angle;  // heading of the cone axis when it last moved
    int frames = 0, idle = 0;
};
// one mesh can be placed many times: instances are told apart by where they are
std::unordered_map<u64, std::vector<Spin>> g_spin;
struct Beacon {
    float lamp[3];          // world
    float heading, tilt;    // of the cone axis when last drawn (tilt = its y component)
    float cos_outer;
    float color[3];
    float measured, omega;  // heading at the last speed measurement; turning speed, rad/s
    u32 measured_ms, seen_ms;
    bool with_tint = false;  // seen while the alarm tint was on
};
std::vector<Beacon> g_beacons;
constexpr float kPi = 3.14159265f;

// Ceiling fans: under each is a turning additive mesh of upright fins (the light shafts between the
// blades). The game fakes the light with that and a floor texture; a fan found this way gets a real
// spotlight pointing down from it, shaded by three blades that turn with the shafts.
struct Fan {
    float top[3];           // world: on the fan's axis, at the top of the shafts
    float height, spread;   // of the shafts; their radius at the bottom
    float heading;          // of one shaft when last drawn
    float measured, omega;  // heading at the last speed measurement; turning speed, rad/s
    u32 measured_ms, seen_ms;
    int frames;
    float blade_off = 1.0471976f;  // heading of a blade's shadow minus the shafts' heading (default: half way between shafts)
    bool blade_known = false;      // ...read from the game's own floor shadow (fan_floor)
    float inv[4] = {0, 0, 0, 0}, uvc[2] = {0.5f, 0.5f}, floor_y = 0;  // floor quad: world x,z offset from its centre -> texture offset
    float qc[2] = {0, 0};  // the quad's centre (world x, z)
    u32 quad_ms = 0;
    u32 verts;  // several meshes hang under a fan (a still cone as well): each is followed separately, only one turns
};
std::vector<Fan> g_fans;
constexpr float kThird = 2.0943951f;  // the shafts repeat every 120 degrees

void fan_seen(const std::vector<float>& v, u32 n, const float c[3]) {
    float top = -1e9f, bottom = 1e9f, maxr = 0, heading = 0;
    for (u32 i = 0; i < n; i++) {
        top = std::max(top, v[i * 3 + 1]);
        bottom = std::min(bottom, v[i * 3 + 1]);
        float dx = v[i * 3] - c[0], dz = v[i * 3 + 2] - c[2], r = std::hypot(dx, dz);
        if (r > maxr) maxr = r, heading = std::atan2(dz, dx);
    }
    if (top - bottom < 1.5f || top - bottom > 6.0f || maxr < 0.5f || maxr > 4.0f) return;
    // (effects on the player, like the plasma saw's spinning blade, are not lamps of the level)
    {
        float dx = c[0] - g_cam_target[0], dy = c[1] - g_cam_target[1], dz = c[2] - g_cam_target[2];
        if (dx * dx + dy * dy + dz * dz < 2.0f * 2.0f) return;
    }

    u32 now = SDL_GetTicks();
    Fan* f = nullptr;
    for (Fan& x : g_fans)
        if (std::fabs(x.top[0] - c[0]) < 0.3f && std::fabs(x.top[1] - top) < 0.4f && std::fabs(x.top[2] - c[2]) < 0.3f && x.verts == n) f = &x;
    if (!f) {
        if (g_fans.size() >= 64) g_fans.erase(g_fans.begin());
        Fan nf{};
        nf.top[0] = c[0], nf.top[1] = top, nf.top[2] = c[2];
        nf.height = top - bottom, nf.spread = maxr, nf.heading = nf.measured = heading;
        nf.measured_ms = nf.seen_ms = now;
        nf.verts = n;
        g_fans.push_back(nf);
        return;
    }
    u32 dt = now - f->measured_ms;
    float moved = std::remainder(heading - f->measured, kThird);
    if (dt >= 100) {
        if (dt < 1000 && std::fabs(moved) > 0.01f) {
            f->omega = f->omega == 0 ? moved * 1000.0f / float(dt) : f->omega * 0.8f + 0.2f * moved * 1000.0f / float(dt);
            f->frames++;
        }
        f->measured = heading, f->measured_ms = now;
    }
    f->top[0] = c[0], f->top[1] = top, f->top[2] = c[2];
    f->height = top - bottom, f->spread = maxr;
    f->heading = heading;
    f->seen_ms = now;
    static int logn = 0;
    if (g_shadow_debug && f->frames > 0 && (logn++ % 240) == 0)
        LOGI("fan: (%u verts, turning %d) at %.2f %.2f %.2f, shafts %.2f m long, %.2f m wide, heading %.0f deg, %.0f deg/s", n, f->frames, c[0], top, c[2], f->height, maxr, heading * 57.2958f,
             f->omega * 57.2958f);
}

// The game's own fan shadow: a quad on the floor under the fan, multiplied over it, with a texture of
// three bright sectors separated by thin dark blades, turning. The fan light's blades are lined up with
// it: the texture is read once to find where its blades are, and the quad's corners give that direction
// in the world each frame.
// The quad shows one tile of a larger texture sheet: the tile's darkest and brightest values are measured
// once (they scale the pattern to 0..1), and the tile's bounds keep samples inside it.
struct GoboInfo {
    bool ok = false;
    float lo = 0, hi = 1, rect[4] = {0, 0, 1, 1};
};
std::unordered_map<GLuint, GoboInfo> g_gobo_info;

const GoboInfo& gobo_info(GLuint tex, float u0, float v0, float u1, float v1) {
    auto it = g_gobo_info.find(tex);
    if (it != g_gobo_info.end()) return it->second;
    GoboInfo gi;
    GLint w = 0, h = 0;
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (w >= 8 && h >= 8 && w <= 4096 && h <= 4096 && u1 > u0 && v1 > v0 && u0 > -0.01f && v0 > -0.01f && u1 < 1.01f && v1 < 1.01f) {
        std::vector<u8> px(size_t(w) * size_t(h) * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        if (glGetError() == GL_NO_ERROR) {
            int x0 = std::clamp(int(u0 * float(w)), 0, w - 1), x1 = std::clamp(int(u1 * float(w)), x0 + 1, w);
            int y0 = std::clamp(int(v0 * float(h)), 0, h - 1), y1 = std::clamp(int(v1 * float(h)), y0 + 1, h);
            float lo = 1e9f, hi = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    const u8* q = &px[(size_t(y) * size_t(w) + size_t(x)) * 4];
                    float l = (float(q[0]) + float(q[1]) + float(q[2])) / 765.0f;
                    lo = std::min(lo, l), hi = std::max(hi, l);
                }
            gi.lo = lo, gi.hi = hi;
            gi.rect[0] = (float(x0) + 0.5f) / float(w), gi.rect[1] = (float(y0) + 0.5f) / float(h);
            gi.rect[2] = (float(x1) - 0.5f) / float(w), gi.rect[3] = (float(y1) - 0.5f) / float(h);
            gi.ok = hi > lo + 0.15f;  // (a flat tile is not a blade shadow)
            LOGI("fan shadow texture %u (%dx%d): tile %d,%d - %d,%d, brightness %.2f to %.2f%s", tex, w, h, x0, y0, x1, y1, lo, hi, gi.ok ? "" : " (flat: not used)");
        }
    }
    return g_gobo_info[tex] = gi;
}

void fan_floor(u32 n) {
    if (n != 4 || !g_arr[A_TEX0].enabled || !g_st.unit[0].tex) return;
    Mat4 world = g_view_inv * g_mv.top();
    const float* tm = g_tex[0].top().m;
    float w[4][3], uv[4][2], c[3] = {0, 0, 0};
    for (u32 i = 0; i < 4; i++) {
        const float* v = &g_verts[size_t(i) * kStride];
        for (int r = 0; r < 3; r++) {
            w[i][r] = world.m[r] * v[0] + world.m[4 + r] * v[1] + world.m[8 + r] * v[2] + world.m[12 + r];
            c[r] += w[i][r] * 0.25f;
        }
        uv[i][0] = tm[0] * v[8] + tm[4] * v[9] + tm[12];
        uv[i][1] = tm[1] * v[8] + tm[5] * v[9] + tm[13];
    }
    // flat on a floor?
    if (std::fabs(w[0][1] - w[1][1]) > 0.05f || std::fabs(w[0][1] - w[2][1]) > 0.05f) return;
    // under a turning fan?
    u32 now = SDL_GetTicks();
    Fan* fan = nullptr;
    for (Fan& f : g_fans)
        if (f.frames >= 3 && std::fabs(f.top[0] - c[0]) < 0.6f && std::fabs(f.top[2] - c[2]) < 0.6f && f.top[1] > c[1]) fan = &f;
    if (!fan) return;
    float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
    for (int i = 0; i < 4; i++) u0 = std::min(u0, uv[i][0]), u1 = std::max(u1, uv[i][0]), v0 = std::min(v0, uv[i][1]), v1 = std::max(v1, uv[i][1]);
    const GoboInfo& gi = gobo_info(g_st.unit[0].tex, u0, v0, u1, v1);
    if (!gi.ok) return;
    g_gobo_tex = g_st.unit[0].tex;
    g_gobo_lo = gi.lo, g_gobo_hi = gi.hi;
    std::memcpy(g_gobo_rect, gi.rect, sizeof g_gobo_rect);
    // world xz = A * (uv - uv0) + w0, from three corners
    float du1 = uv[1][0] - uv[0][0], dv1 = uv[1][1] - uv[0][1], du2 = uv[2][0] - uv[0][0], dv2 = uv[2][1] - uv[0][1];
    float det = du1 * dv2 - du2 * dv1;
    if (std::fabs(det) < 1e-8f) return;
    float x1 = w[1][0] - w[0][0], z1 = w[1][2] - w[0][2], x2 = w[2][0] - w[0][0], z2 = w[2][2] - w[0][2];
    float axu = (x1 * dv2 - x2 * dv1) / det, axv = (x2 * du1 - x1 * du2) / det;
    float azu = (z1 * dv2 - z2 * dv1) / det, azv = (z2 * du1 - z1 * du2) / det;
    // and the whole mapping, for sampling the texture itself
    float det2 = axu * azv - axv * azu;
    if (std::fabs(det2) > 1e-8f) {
        fan->inv[0] = azv / det2, fan->inv[1] = -axv / det2, fan->inv[2] = -azu / det2, fan->inv[3] = axu / det2;
        float ox = c[0] - w[0][0], oz = c[2] - w[0][2];
        fan->uvc[0] = uv[0][0] + fan->inv[0] * ox + fan->inv[1] * oz;
        fan->uvc[1] = uv[0][1] + fan->inv[2] * ox + fan->inv[3] * oz;
        fan->floor_y = c[1];
        fan->qc[0] = c[0], fan->qc[1] = c[2];
        fan->quad_ms = now;
    }
}

// ---- glowing panels
// Door holograms ("OPEN", "LOCKED"), console and monitor screens and similar displays are flat meshes
// drawn additively over the scene. Each one becomes a small light just in front of it, in the colour it
// is showing: the average of the part of its texture it uses, times the colour and opacity the game
// draws it with. So the light follows the panel's state by itself: a different texture or tint changes
// its colour, a fading panel dims it, and a panel that is not shown gives no light.
struct TexImage {
    int w = 0, h = 0;
    std::vector<u8> px;  // RGBA, at most 256 x 256
};
std::unordered_map<GLuint, TexImage> g_tex_images;
std::unordered_map<GLuint, std::array<u8, 256>> g_lamp_masks;  // per texture: where it is lamp-bright (16 x 16)

// The game's own upload of a texture (decoded RGBA): the small copy is taken here, so that nothing has to
// be read back from the graphics card later (a read-back stalls the frame it happens in).
void texture_pixels_impl(GLuint tex, const u8* rgba, int w, int h) {
    if (!tex || w < 1 || h < 1 || w > 4096 || h > 4096) return;
    TexImage ti;
    const int sx = std::max(1, w / 256), sy = std::max(1, h / 256);
    ti.w = w / sx, ti.h = h / sy;
    ti.px.resize(size_t(ti.w) * size_t(ti.h) * 4);
    for (int y = 0; y < ti.h; y++)
        for (int x = 0; x < ti.w; x++) std::memcpy(&ti.px[(size_t(y) * size_t(ti.w) + size_t(x)) * 4], &rgba[(size_t(y * sy) * size_t(w) + size_t(x * sx)) * 4], 4);
    g_tex_images[tex] = std::move(ti);
    g_lamp_masks.erase(tex);
}

const TexImage& tex_image(GLuint tex) {
    auto it = g_tex_images.find(tex);
    if (it != g_tex_images.end()) return it->second;
    TexImage ti;
    GLint w = 0, h = 0;
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (w >= 1 && h >= 1 && w <= 4096 && h <= 4096) {
        std::vector<u8> full(size_t(w) * size_t(h) * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, full.data());
        if (glGetError() == GL_NO_ERROR) {
            int sx = std::max(1, w / 256), sy = std::max(1, h / 256);
            ti.w = w / sx, ti.h = h / sy;
            ti.px.resize(size_t(ti.w) * size_t(ti.h) * 4);
            for (int y = 0; y < ti.h; y++)
                for (int x = 0; x < ti.w; x++) std::memcpy(&ti.px[(size_t(y) * size_t(ti.w) + size_t(x)) * 4], &full[(size_t(y * sy) * size_t(w) + size_t(x * sx)) * 4], 4);
        }
    }
    return g_tex_images[tex] = std::move(ti);
}

struct Glow {
    float pos[3], wpos[3], color[3], size, wn[3];
};
std::vector<Glow> g_glows;  // this frame's
float g_pglow_pos[3] = {0, 0, 0}, g_pglow_col[3] = {0, 0, 0}, g_pglow_w = 0;  // glows at the player, this frame
float g_pglow_out[6];
u32 g_pglow_ms = 0;

#define GLOW_REJ(why) do { static int k_ = 0; if (g_shadow_debug && (k_++ % 300) < 2) LOGI("glow rejected (%s): %u verts tex %u col %.2f %.2f %.2f", why, n, g_st.unit[0].tex, g_st.color[0], g_st.color[1], g_st.color[2]); } while (0)
void glow(u32 n) {
    if (n < 4 || n > 240 || g_glows.size() >= 24 || g_idx.size() < 3) return;
    const float* m = g_mv.top().m;
    // eye-space centre and reach
    float c[3] = {0, 0, 0};
    static std::vector<float> e;
    e.resize(size_t(n) * 3);
    for (u32 i = 0; i < n; i++) {
        const float* v = &g_verts[size_t(i) * kStride];
        for (int r = 0; r < 3; r++) {
            e[i * 3 + r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r];
            c[r] += e[i * 3 + r] / float(n);
        }
    }
    float reach = 0;
    for (u32 i = 0; i < n; i++) {
        float d2 = 0;
        for (int r = 0; r < 3; r++) d2 += (e[i * 3 + r] - c[r]) * (e[i * 3 + r] - c[r]);
        reach = std::max(reach, d2);
    }
    reach = std::sqrt(reach);
    // at the player (the saw's blade and trail): any shape counts, it only has to glow
    bool at_player = false;
    {
        float dd = 0;
        for (int r = 0; r < 3; r++) {
            float wp = g_view_inv.m[r] * c[0] + g_view_inv.m[4 + r] * c[1] + g_view_inv.m[8 + r] * c[2] + g_view_inv.m[12 + r];
            float d = wp - g_cam_target[r] + (r == 1 ? 0.4f : 0.0f);
            dd += d * d;
        }
        at_player = dd < 1.6f * 1.6f && reach > 0.05f && reach < 2.2f;
    }
    if (!at_player && (reach < 0.18f || reach > 2.2f)) { GLOW_REJ("reach"); return; }  // sparks and dust; room-sized shafts and beams
    // flat? (light shafts, cones and beams are not)
    float nrm[3] = {0, 0, 0};
    for (size_t t = 0; t + 2 < g_idx.size() && nrm[0] == 0 && nrm[1] == 0 && nrm[2] == 0; t += 3) {
        u32 a = g_idx[t], b = g_idx[t + 1], d = g_idx[t + 2];
        if (a >= n || b >= n || d >= n) return;
        float u[3], w[3];
        for (int r = 0; r < 3; r++) u[r] = e[b * 3 + r] - e[a * 3 + r], w[r] = e[d * 3 + r] - e[a * 3 + r];
        float x[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        float l = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        if (l > 1e-6f) nrm[0] = x[0] / l, nrm[1] = x[1] / l, nrm[2] = x[2] / l;
    }
    if (!at_player && nrm[0] == 0 && nrm[1] == 0 && nrm[2] == 0) { GLOW_REJ("degenerate"); return; }
    for (u32 i = 0; i < n; i++) {
        float d = 0;
        for (int r = 0; r < 3; r++) d += (e[i * 3 + r] - c[r]) * nrm[r];
        if (!at_player && std::fabs(d) > 0.3f + 0.05f * reach) { GLOW_REJ("not flat"); return; }  // (a door shows its hologram on both faces: two layers)
    }
    // what it shows: its texture over the part it uses, times the colour it is drawn with
    float col[4] = {g_st.color[0], g_st.color[1], g_st.color[2], g_st.color[3]};
    if (g_arr[A_COLOR].enabled) {
        col[0] = col[1] = col[2] = col[3] = 0;
        for (u32 i = 0; i < n; i++)
            for (int k = 0; k < 4; k++) col[k] += g_verts[size_t(i) * kStride + 4 + size_t(k)] / float(n);
    }
    float tc[3] = {1, 1, 1}, cover = 1;
    bool tex = g_st.unit[0].enabled && g_st.unit[0].tex != 0 && g_arr[A_TEX0].enabled;
    if (tex) {
        const TexImage& ti = tex_image(g_st.unit[0].tex);
        if (ti.w > 0) {
            const float* tm = g_tex[0].top().m;
            float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
            for (u32 i = 0; i < n; i++) {
                const float* v = &g_verts[size_t(i) * kStride];
                float uu = tm[0] * v[8] + tm[4] * v[9] + tm[12], vv = tm[1] * v[8] + tm[5] * v[9] + tm[13];
                u0 = std::min(u0, uu), u1 = std::max(u1, uu), v0 = std::min(v0, vv), v1 = std::max(v1, vv);
            }
            int kind = 1;
            auto it = g_tex_kind.find(g_st.unit[0].tex);
            if (it != g_tex_kind.end()) kind = it->second;
            double sum[3] = {0, 0, 0};
            for (int sy = 0; sy < 12; sy++)
                for (int sx = 0; sx < 12; sx++) {
                    float uu = u0 + (u1 - u0) * (float(sx) + 0.5f) / 12.0f, vv = v0 + (v1 - v0) * (float(sy) + 0.5f) / 12.0f;
                    uu -= std::floor(uu), vv -= std::floor(vv);
                    const u8* q = &ti.px[(size_t(std::min(ti.h - 1, int(vv * float(ti.h)))) * size_t(ti.w) + size_t(std::min(ti.w - 1, int(uu * float(ti.w))))) * 4];
                    float a = float(q[3]) / 255.0f;
                    if (kind == 2) sum[0] += a, sum[1] += a, sum[2] += a;  // alpha-only: the drawn colour shows through it
                    else
                        for (int k = 0; k < 3; k++) sum[k] += double(q[k]) / 255.0 * double(g_st.blend_src == GL_SRC_ALPHA ? a : 1.0f);
                }
            for (int k = 0; k < 3; k++) tc[k] = float(sum[k] / 144.0);
        }
    }
    float srca = g_st.blend_src == GL_SRC_ALPHA ? col[3] : 1.0f;
    float em[3] = {tc[0] * col[0] * srca * cover, tc[1] * col[1] * srca * cover, tc[2] * col[2] * srca * cover};
    float lum = std::max(em[0], std::max(em[1], em[2]));
    // alpha-blended overlays also include dark things (decals, grime, blob shadows): only bright ones glow
    if (lum < (g_st.blend_dst == GL_ONE ? 0.015f : 0.22f)) { GLOW_REJ("dark"); return; }
    static int dbg = 0;
    if (g_shadow_debug && (dbg++ % 600) < 6)
        LOGI("glow?: %u verts reach %.2f blend %x,%x tex %u emission %.2f %.2f %.2f", n, reach, g_st.blend_src, g_st.blend_dst, g_st.unit[0].tex, em[0], em[1], em[2]);
    // just in front of the panel, on the camera's side
    if (nrm[0] * c[0] + nrm[1] * c[1] + nrm[2] * c[2] > 0) nrm[0] = -nrm[0], nrm[1] = -nrm[1], nrm[2] = -nrm[2];
    {
        // a glow at the player (the plasma saw's blade, a weapon's charge) is not a panel of the level: it
        // is handed to the player's lights, which put a light exactly there, in its colour
        float wp[3], dd = 0;
        for (int r = 0; r < 3; r++) {
            wp[r] = g_view_inv.m[r] * c[0] + g_view_inv.m[4 + r] * c[1] + g_view_inv.m[8 + r] * c[2] + g_view_inv.m[12 + r];
            dd += (wp[r] - g_cam_target[r] + (r == 1 ? 0.4f : 0.0f)) * (wp[r] - g_cam_target[r] + (r == 1 ? 0.4f : 0.0f));
        }
        if (at_player || dd < 1.6f * 1.6f) {
            float w = lum * reach * reach;
            for (int r = 0; r < 3; r++) g_pglow_pos[r] += wp[r] * w, g_pglow_col[r] += em[r] * reach * reach;
            g_pglow_w += w;
            return;
        }
    }
    Glow g{};
    for (int r = 0; r < 3; r++) g.pos[r] = c[r] + nrm[r] * 0.3f;
    for (int r = 0; r < 3; r++) g.wpos[r] = g_view_inv.m[r] * g.pos[0] + g_view_inv.m[4 + r] * g.pos[1] + g_view_inv.m[8 + r] * g.pos[2] + g_view_inv.m[12 + r];
    for (int r = 0; r < 3; r++) g.wn[r] = g_view_inv.m[r] * nrm[0] + g_view_inv.m[4 + r] * nrm[1] + g_view_inv.m[8 + r] * nrm[2];
    // the panel's parts (rings, text, scan lines) are separate meshes in the same place: one light for all
    float area = reach * reach;
    for (Glow& o : g_glows) {
        float d2 = 0;
        for (int r = 0; r < 3; r++) d2 += (o.pos[r] - g.pos[r]) * (o.pos[r] - g.pos[r]);
        if (d2 < 0.8f * 0.8f) {
            for (int k = 0; k < 3; k++) o.color[k] += em[k] * area;
            o.size = std::max(o.size, reach);
            return;
        }
    }
    for (int k = 0; k < 3; k++) g.color[k] = em[k] * area;
    g.size = reach;
    g_glows.push_back(g);
}

// Identity of a light that stays in the world: its kind and its position on a grid.
unsigned long long light_id(const float* p, unsigned kind, float grid) {
    u64 h = 1469598103934665603ull ^ kind;
    for (int r = 0; r < 3; r++) h = (h ^ u64(std::llround(double(p[r]) / double(grid)))) * 1099511628211ull;
    return h ? h : 1;
}

// Panel glows are found while a frame is drawn (they are drawn after the solid scene), so they light the
// next frame: by then their position is known at its start and they can have a shadow map like the
// other lights.
std::vector<Light> g_glow_lights;  // from the last frame, world space

void glow_frame_end() {
    if (g_pglow_w > 1e-5f) {
        float mx = std::max(g_pglow_col[0], std::max(g_pglow_col[1], g_pglow_col[2]));
        for (int r = 0; r < 3; r++) g_pglow_out[r] = g_pglow_pos[r] / g_pglow_w, g_pglow_out[3 + r] = mx > 1e-5f ? g_pglow_col[r] / mx : 1.0f;
        g_pglow_ms = SDL_GetTicks();
    }
    g_pglow_w = 0;
    for (int r = 0; r < 3; r++) g_pglow_pos[r] = g_pglow_col[r] = 0;
    g_glow_lights.clear();
    for (const Glow& g : g_glows) {
        float mx = std::max(g.color[0], std::max(g.color[1], g.color[2]));
        if (mx < 1e-4f) continue;
        // colour: the panel's hue at full strength; brightness: how much of the panel is lit, how brightly
        float power = std::clamp(mx / (g.size * g.size) * 4.0f, 0.0f, 1.0f) * std::clamp(0.5f + g.size, 0.6f, 1.0f) * 0.7f;
        Light l{};
        for (int r = 0; r < 3; r++) l.wpos[r] = g.wpos[r], l.wdir[r] = g.wn[r], l.color[r] = g.color[r] / mx * power * g_glow_gain;
        l.id = light_id(l.wpos, 3, 0.5f);
        l.radius = std::clamp(1.6f + g.size * 2.0f, 2.0f, 5.0f);
        l.glow = 1;
        g_glow_lights.push_back(l);
    }
    static int logn = 0;
    if (g_shadow_debug && !g_glows.empty() && (logn++ % 120) == 0) {
        const Glow& g = g_glows[0];
        LOGI("panel glows: %zu; first at %.2f %.2f %.2f, size %.2f, colour %.2f %.2f %.2f", g_glows.size(), g.wpos[0], g.wpos[1], g.wpos[2], g.size, g.color[0],
             g.color[1], g.color[2]);
    }
    g_glows.clear();
}

void beacon(u32 lo, u32 n) {
    if (n < 12 || n > 600) return;
    Mat4 world = g_view_inv * g_mv.top();
    static std::vector<float> v;
    v.resize(size_t(n) * 3);
    float c[3] = {0, 0, 0};
    for (u32 i = 0; i < n; i++) {
        const float* p = &g_verts[size_t(i) * kStride];
        for (int r = 0; r < 3; r++) {
            v[i * 3 + r] = world.m[r] * p[0] + world.m[4 + r] * p[1] + world.m[8 + r] * p[2] + world.m[12 + r];
            c[r] += v[i * 3 + r] / float(n);
        }
    }
    // fan light shaft: many vertices on a vertical axis through the centre
    int near_axis = 0;
    float best = 0, bd[3] = {0, 0, 0};
    for (u32 i = 0; i < n; i++) {
        float d[3] = {v[i * 3] - c[0], v[i * 3 + 1] - c[1], v[i * 3 + 2] - c[2]};
        if (std::hypot(d[0], d[2]) < 0.35f && std::fabs(d[1]) > 0.5f) near_axis++;
        float l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (l2 > best) best = l2, bd[0] = d[0], bd[1] = d[1], bd[2] = d[2];
    }
    float reach = std::sqrt(best);
    if (near_axis * 4 > int(n)) {  // a fan's light shafts, not an alarm lamp
        fan_seen(v, n, c);
        return;
    }
    if (reach < 1.0f || reach > 5.0f) return;
    // cone axis: from the middle of the vertices on one side to the middle of those on the other
    float pm[3] = {0, 0, 0}, nm[3] = {0, 0, 0};
    int pc = 0, nc = 0;
    for (u32 i = 0; i < n; i++) {
        float d[3] = {v[i * 3] - c[0], v[i * 3 + 1] - c[1], v[i * 3 + 2] - c[2]};
        float t = d[0] * bd[0] + d[1] * bd[1] + d[2] * bd[2];
        float* m = t > 0 ? pm : nm;
        (t > 0 ? pc : nc)++;
        for (int r = 0; r < 3; r++) m[r] += d[r];
    }
    if (pc < int(n) / 4 || nc < int(n) / 4) return;  // two cones: about half the vertices each way
    float ax[3] = {pm[0] / float(pc) - nm[0] / float(nc), pm[1] / float(pc) - nm[1] / float(nc), pm[2] / float(pc) - nm[2] / float(nc)};
    float al = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    if (al < 0.5f) return;
    for (float& x : ax) x /= al;
    if (std::fabs(ax[1]) > 0.6f) return;  // the cones sweep round, they do not point up and down
    // both cones reach about equally far, and the widest spread of the far vertices gives the cone angle
    float fwd = 0, back = 0, cos_edge = 1;
    for (u32 i = 0; i < n; i++) {
        float d[3] = {v[i * 3] - c[0], v[i * 3 + 1] - c[1], v[i * 3 + 2] - c[2]};
        float t = d[0] * ax[0] + d[1] * ax[1] + d[2] * ax[2], l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        fwd = std::max(fwd, t);
        back = std::min(back, t);
        if (l > 0.7f * reach) cos_edge = std::min(cos_edge, std::fabs(t) / l);
    }
    if (-back < 0.6f * fwd || fwd < 0.6f * -back) return;
    const Array& arr = g_arr[A_VERTEX];
    u64 key = (u64(reinterpret_cast<uintptr_t>(arr.data)) * 0x9E3779B97F4A7C15ull) ^ (u64(lo) << 32) ^ n;
    std::vector<Spin>& inst = g_spin[key];
    float heading = std::atan2(ax[2], ax[0]);
    Spin* found = nullptr;
    for (Spin& sp : inst)
        if (std::fabs(c[0] - sp.c[0]) < 0.25f && std::fabs(c[1] - sp.c[1]) < 0.25f && std::fabs(c[2] - sp.c[2]) < 0.25f) {
            found = &sp;
            break;
        }
    if (!found) {
        if (inst.size() >= 64) inst.clear();
        inst.push_back({{c[0], c[1], c[2]}, heading, 0, 0});
        return;
    }
    Spin& s = *found;
    // how far has the axis turned since it last moved? (the two ends are interchangeable: period pi)
    float turn = std::fabs(std::remainder(heading - s.angle, kPi));
    if (turn < 0.02f) {  // (at high frame rates one frame's turn is tiny: wait until it adds up)
        if (++s.idle > 120) s.frames = 0;
    } else {
        s.frames = turn < 0.6f ? s.frames + 1 : 0;
        s.idle = 0;
        s.angle = heading;
    }
    static int dbg = 0;
    if (g_shadow_debug && (dbg++ % 400) < 3)
        LOGI("beacon?: mesh %u verts at %.2f %.2f %.2f reach %.2f axis %.2f %.2f %.2f turn %.3f frames %d", n, c[0], c[1], c[2], reach, ax[0], ax[1], ax[2], turn,
             s.frames);
    // (effects on the player, like the plasma saw's spinning blade, are not lamps of the level)
    {
        float dx = c[0] - g_cam_target[0], dy = c[1] - g_cam_target[1], dz = c[2] - g_cam_target[2];
        if (dx * dx + dy * dy + dz * dz < 2.0f * 2.0f) return;
    }
    if (s.frames < 30) return;  // half a second of steady turning in one place
    // remember it: the game stops drawing the cones when the lamp is off screen and hides them when the
    // player is close, but the light should keep sweeping
    u32 now = SDL_GetTicks();
    Beacon* bk = nullptr;
    for (Beacon& b : g_beacons)
        if (std::fabs(b.lamp[0] - c[0]) < 0.4f && std::fabs(b.lamp[1] - c[1]) < 0.6f && std::fabs(b.lamp[2] - c[2]) < 0.4f) bk = &b;
    if (!bk) {
        if (g_beacons.size() >= 64) g_beacons.erase(g_beacons.begin());
        Beacon nb{};
        nb.measured = heading, nb.measured_ms = now;
        g_beacons.push_back(nb);
        bk = &g_beacons.back();
    }
    u32 dt = now - bk->measured_ms;
    float moved = std::remainder(heading - bk->measured, kPi);
    if (dt >= 100 && dt < 1000 && std::fabs(moved) < 1.2f) bk->omega = bk->omega == 0 ? moved * 1000.0f / float(dt) : bk->omega * 0.8f + 0.2f * moved * 1000.0f / float(dt);
    if (dt >= 100) bk->measured = heading, bk->measured_ms = now;
    for (int r = 0; r < 3; r++) bk->lamp[r] = c[r];
    bk->heading = heading;
    bk->tilt = ax[1];
    bk->cos_outer = std::clamp(cos_edge - 0.08f, 0.6f, 0.95f);  // a little wider than the drawn cone
    // colour: the mesh's own (blue), at full strength
    float col[3] = {g_st.color[0], g_st.color[1], g_st.color[2]};
    if (g_arr[A_COLOR].enabled) {
        col[0] = col[1] = col[2] = 0;
        for (u32 i = 0; i < n; i++)
            for (int k = 0; k < 3; k++) col[k] += g_verts[size_t(i) * kStride + 4 + size_t(k)];
    }
    float mx = std::max(col[0], std::max(col[1], col[2]));
    for (int k = 0; k < 3; k++) bk->color[k] = mx > 1e-4f ? col[k] / mx : (k == 2 ? 1.0f : 0.4f);
    bk->seen_ms = now;
    if (g_tint_active) bk->with_tint = true;
    static int logn = 0;
    if (g_shadow_debug && (logn++ % 120) == 0)
        LOGI("beacon: lamp at %.2f %.2f %.2f (%u verts), heading %.0f deg, tilt %.2f, %.0f deg/s, half angle %.0f deg, colour %.2f %.2f %.2f", c[0], c[1], c[2], n,
             heading * 57.2958f, ax[1], bk->omega * 57.2958f, std::acos(bk->cos_outer) * 57.2958f, bk->color[0], bk->color[1], bk->color[2]);
}

// Spotlights of the beacons seen in the last few seconds (those not drawn right now keep turning at the
// speed they had).
void beacon_lights() {
    u32 now = SDL_GetTicks();
    const float eye[3] = {g_view_inv.m[12], g_view_inv.m[13], g_view_inv.m[14]};
    // The game hides the cones when the player is close and does not draw them off screen. A beacon is
    // kept while the alarm tint it came with is still on (or, for one without a tint, while the player
    // is near it); otherwise it goes 2.5 s after it was last drawn.
    for (Beacon& b : g_beacons) {
        float dx = b.lamp[0] - eye[0], dy = b.lamp[1] - eye[1], dz = b.lamp[2] - eye[2];
        bool hold = b.with_tint ? g_tint_active : dx * dx + dy * dy + dz * dz < 36.0f;
        if (hold && now - b.seen_ms > 1500) {
            b.heading += b.omega * float(now - b.seen_ms - 1500) / 1000.0f;  // keep turning, stay "seen 1.5 s ago"
            b.seen_ms = now - 1500;
        }
    }
    g_beacons.erase(std::remove_if(g_beacons.begin(), g_beacons.end(), [&](const Beacon& b) { return now - b.seen_ms > 2500; }), g_beacons.end());
    static int logn = 0;
    if (g_shadow_debug && (logn++ % 120) == 0) {
        std::string t;
        for (const Beacon& b : g_beacons) {
            char buf[80];
            std::snprintf(buf, sizeof buf, "(%.1f %.1f %.1f, %u ms ago) ", b.lamp[0], b.lamp[1], b.lamp[2], now - b.seen_ms);
            t += buf;
        }
        LOGI("beacons kept: %zu %s eye %.1f %.1f %.1f tint %d", g_beacons.size(), t.c_str(), eye[0], eye[1], eye[2], g_tint_active);
    }
    // ceiling fans: kept while the player is near (the camera may look away from the shafts), turning on
    // at the speed last measured
    for (Fan& f : g_fans) {
        float dx = f.top[0] - eye[0], dy = f.top[1] - eye[1], dz = f.top[2] - eye[2];
        if (dx * dx + dy * dy + dz * dz < 100.0f && f.frames >= 10 && now - f.seen_ms > 1500) {
            f.heading += f.omega * float(now - f.seen_ms - 1500) / 1000.0f;
            f.seen_ms = now - 1500;
        }
    }
    g_fans.erase(std::remove_if(g_fans.begin(), g_fans.end(), [&](const Fan& f) { return now - f.seen_ms > 2500; }), g_fans.end());
    for (const Fan& f : g_fans) {
        if (f.frames < 10 || g_spots.size() >= 12) continue;  // a second of steady turning
        float fade = std::clamp(float(2500 - int(now - f.seen_ms)) / 500.0f, 0.0f, 1.0f);
        Light l{};
        const float d[3] = {0, -1, 0};
        for (int r = 0; r < 3; r++) {
            l.pos[r] = g_view.m[r] * f.top[0] + g_view.m[4 + r] * f.top[1] + g_view.m[8 + r] * f.top[2] + g_view.m[12 + r];
            l.dir[r] = g_view.m[r] * d[0] + g_view.m[4 + r] * d[1] + g_view.m[8 + r] * d[2];
            l.wpos[r] = f.top[r], l.wdir[r] = d[r];
        }
        l.color[0] = 1.25f * fade, l.color[1] = 1.15f * fade, l.color[2] = 1.0f * fade;  // warm white, like the shafts
        l.radius = std::max(6.0f, f.height * 2.2f);
        // as wide as the shafts spread, and a little more
        l.cos_outer = std::clamp(std::cos(std::atan2(f.spread, f.height) + 0.12f), 0.6f, 0.97f);
        l.gobo = 1;
        l.id = light_id(f.top, 4, 1.0f);
        if (f.quad_ms && g_gobo_tex) {
            // the floor shadow as last drawn, turned on by the time since
            float dlt = f.omega * float(now - f.quad_ms) / 1000.0f, cs = std::cos(dlt), sn = std::sin(dlt);
            l.gm[0] = f.inv[0] * cs - f.inv[1] * sn, l.gm[1] = f.inv[0] * sn + f.inv[1] * cs;
            l.gm[2] = f.inv[2] * cs - f.inv[3] * sn, l.gm[3] = f.inv[2] * sn + f.inv[3] * cs;
            // texture position under the light's own axis (the quad's centre is a few cm from it)
            float ox = f.top[0] - f.qc[0], oz = f.top[2] - f.qc[1];
            l.guv[0] = f.uvc[0] + f.inv[0] * ox + f.inv[1] * oz, l.guv[1] = f.uvc[1] + f.inv[2] * ox + f.inv[3] * oz;
            l.gh = std::max(0.5f, f.top[1] - f.floor_y);
            l.gobo = 2;
        }
        static int flog = 0;
        if (g_shadow_debug && (flog++ % 240) == 0)
            LOGI("fan light: gobo %.0f tex %u range %.2f-%.2f height %.2f uv centre %.3f %.3f map %.3f %.3f %.3f %.3f quad %u ms ago", l.gobo, g_gobo_tex, g_gobo_lo,
                 g_gobo_hi, l.gh, l.guv[0], l.guv[1], l.gm[0], l.gm[1], l.gm[2], l.gm[3], f.quad_ms ? now - f.quad_ms : 0);
        l.phase = f.heading + f.omega * float(now - f.seen_ms) / 1000.0f + f.blade_off;  // heading of a blade's shadow
        g_spots.push_back(l);
    }
    for (const Beacon& bk : g_beacons) {
        float dx = bk.lamp[0] - eye[0], dy = bk.lamp[1] - eye[1], dz = bk.lamp[2] - eye[2];
        if (dx * dx + dy * dy + dz * dz > 30.0f * 30.0f) continue;
        float a = bk.heading + bk.omega * float(now - bk.seen_ms) / 1000.0f;
        float fade = std::clamp(float(2500 - int(now - bk.seen_ms)) / 500.0f, 0.0f, 1.0f);
        float h = std::sqrt(std::max(0.0f, 1.0f - bk.tilt * bk.tilt));
        for (int side = 0; side < 2 && g_spots.size() < 12; side++) {
            float sg = side ? -1.0f : 1.0f;
            float d[3] = {std::cos(a) * h * sg, bk.tilt * sg, std::sin(a) * h * sg};
            Light l{};
            for (int r = 0; r < 3; r++) {
                l.pos[r] = g_view.m[r] * bk.lamp[0] + g_view.m[4 + r] * bk.lamp[1] + g_view.m[8 + r] * bk.lamp[2] + g_view.m[12 + r];
                l.dir[r] = g_view.m[r] * d[0] + g_view.m[4 + r] * d[1] + g_view.m[8 + r] * d[2];
                l.color[r] = bk.color[r] * 2.0f * fade;
            }
            for (int r = 0; r < 3; r++) l.wpos[r] = bk.lamp[r], l.wdir[r] = d[r];
            l.radius = 11.0f;
            l.cos_outer = bk.cos_outer;
            l.id = light_id(bk.lamp, 5, 1.0f) + u64(side);
            l.rotating = true;
            g_spots.push_back(l);
        }
    }
}

// ---- light fixtures found in the level's own geometry
// Lamps (wall, floor and ceiling strips, the lights over and beside doors) are small, bright, nearly white
// patches of the level's texture. Some are modelled as their own triangles, most are painted inside a
// larger wall panel, so every still, opaque level mesh is examined once by sampling its texture over its
// surface (a point every few centimetres): bright samples are gathered into fixtures by position, and a
// fixture of a believable size gets a point light like the strip lights known from the lockdown meshes.
struct Lamp {
    float p[3], c[3];
    float n;         // weight of the sums above = bright area, in square metres
    float nrm[3];
    u64 base;        // the mesh it was found in
};
std::vector<Lamp> g_lamps;                // of this level, world space
std::unordered_map<u64, char> g_lamp_done;  // meshes examined (by vertex data and place)
const float kLampMinArea = 0.006f;  // smaller: a highlight or a texture speck
const float kLampMaxArea = 0.9f;    // larger: a white panel, a lit wall, a window

void lamps_drop(u64 base) {
    g_lamps.erase(std::remove_if(g_lamps.begin(), g_lamps.end(), [&](const Lamp& l) { return l.base == base; }), g_lamps.end());
}

void lamp_scan(u32 lo, u32 n) {
    if (n < 3 || !g_st.unit[0].enabled || !g_st.unit[0].tex || !g_arr[A_TEX0].enabled || g_lamps.size() > 3000) return;
    Mat4 world = g_view_inv * g_mv.top();
    const Array& a = g_arr[A_VERTEX];
    u64 key = (u64(reinterpret_cast<uintptr_t>(a.data)) * 0x9E3779B97F4A7C15ull) ^ (u64(lo) << 32) ^ n;
    key ^= u64(int(world.m[12] * 4.0f)) * 0x85EBCA77ull ^ u64(int(world.m[13] * 4.0f)) * 0xC2B2AE3Dull ^ u64(int(world.m[14] * 4.0f)) * 0x27D4EB2Full;
    const u64 mesh_key = key;
    // part of the level's structure (a room, a wall section), not an object lying in it
    if (std::max(g_mesh_hi[0] - g_mesh_lo[0], std::max(g_mesh_hi[1] - g_mesh_lo[1], g_mesh_hi[2] - g_mesh_lo[2])) < 2.5f) return;
    if (g_lamp_done.count(key)) return;
    // (a few milliseconds of this per frame at most: a mesh that has to wait is examined on a later frame,
    // when it is drawn again; entering a room would otherwise stall on all its meshes at once)
    if (g_ms_scan > 2.5) return;
    StopWatch sw(g_ms_scan);
    g_lamp_done[key] = 1;
    const TexImage& ti = tex_image(g_st.unit[0].tex);
    if (ti.w <= 0) return;
    // Where the texture has lamp-bright texels at all, on a 16 x 16 grid: most triangles map to parts of
    // it that have none and are skipped without sampling.
    auto& masks = g_lamp_masks;
    auto mi = masks.find(g_st.unit[0].tex);
    if (mi == masks.end()) {
        std::array<u8, 256> mk{};
        bool any = false;
        for (int y = 0; y < ti.h; y++)
            for (int x = 0; x < ti.w; x++) {
                const u8* q = &ti.px[(size_t(y) * size_t(ti.w) + size_t(x)) * 4];
                if (std::max(q[0], std::max(q[1], q[2])) > 204 && std::min(q[0], std::min(q[1], q[2])) > 114) mk[size_t((y * 16 / ti.h) * 16 + x * 16 / ti.w)] = 1, any = true;
            }
        mk[0] = u8(mk[0] | (any ? 2 : 0));  // (bit 1 of the first cell: the texture has bright texels somewhere)
        mi = masks.emplace(g_st.unit[0].tex, mk).first;
        if (masks.size() > 4000) masks.clear(), mi = masks.emplace(g_st.unit[0].tex, mk).first;
    }
    const std::array<u8, 256>& mask = mi->second;
    if (!(mask[0] & 2)) return;  // nothing lamp-bright anywhere in this texture
    const float* tm = g_tex[0].top().m;
    const bool vc = g_arr[A_COLOR].enabled;
    std::vector<Lamp> found;  // this mesh's bright patches
    double mesh_area = 0, bright_area = 0;
    for (size_t t = 0; t + 2 < g_idx.size(); t += 3) {
        u32 id[3] = {g_idx[t], g_idx[t + 1], g_idx[t + 2]};
        if (id[0] >= n || id[1] >= n || id[2] >= n) continue;
        float w[3][3], uv[3][2], vcol[3][3];
        for (int k = 0; k < 3; k++) {
            const float* v = &g_verts[size_t(id[k]) * kStride];
            for (int r = 0; r < 3; r++) w[k][r] = world.m[r] * v[0] + world.m[4 + r] * v[1] + world.m[8 + r] * v[2] + world.m[12 + r];
            uv[k][0] = tm[0] * v[8] + tm[4] * v[9] + tm[12], uv[k][1] = tm[1] * v[8] + tm[5] * v[9] + tm[13];
            for (int r = 0; r < 3; r++) vcol[k][r] = vc ? v[4 + r] : g_st.color[r];
        }
        float e1[3], e2[3];
        for (int r = 0; r < 3; r++) e1[r] = w[1][r] - w[0][r], e2[r] = w[2][r] - w[0][r];
        float cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
        const float nl = std::sqrt(cx * cx + cy * cy + cz * cz), area = 0.5f * nl;
        if (area < 0.0004f || area > 40.0f) continue;
        mesh_area += area;
        {  // does the triangle's part of the texture hold anything bright?
            float u0 = std::min(uv[0][0], std::min(uv[1][0], uv[2][0])), u1 = std::max(uv[0][0], std::max(uv[1][0], uv[2][0]));
            float v0 = std::min(uv[0][1], std::min(uv[1][1], uv[2][1])), v1 = std::max(uv[0][1], std::max(uv[1][1], uv[2][1]));
            bool hit = u1 - u0 >= 1.0f || v1 - v0 >= 1.0f;
            const int cx0 = int(std::floor(u0 * 16.0f)), cx1 = int(std::floor(u1 * 16.0f)), cy0 = int(std::floor(v0 * 16.0f)), cy1 = int(std::floor(v1 * 16.0f));
            for (int cy = cy0; cy <= cy1 && !hit; cy++)
                for (int cx = cx0; cx <= cx1 && !hit; cx++) hit = (mask[size_t(((cy % 16) + 16) % 16 * 16 + ((cx % 16) + 16) % 16)] & 1) != 0;
            if (!hit) continue;
        }
        // a grid of points over the triangle, about 8 cm apart (coarser on very large triangles)
        const float l1 = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]), l2 = std::sqrt(e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2]);
        const int div = std::clamp(int(std::ceil(std::max(l1, l2) / 0.08f)), 1, 40);
        const float cell = area / float(div * div);  // area of one grid cell (two samples per cell, less along the edge)
        for (int i = 0; i < div; i++)
            for (int j = 0; j < div - i; j++)
                for (int half = 0; half < 2; half++) {
                    if (half && j >= div - i - 1) continue;  // the upper triangle of the last cell is outside
                    const float b1 = (float(i) + (half ? 2.0f : 1.0f) / 3.0f) / float(div), b2 = (float(j) + (half ? 2.0f : 1.0f) / 3.0f) / float(div);
                    const float b0 = 1.0f - b1 - b2;
                    float u = b0 * uv[0][0] + b1 * uv[1][0] + b2 * uv[2][0], v = b0 * uv[0][1] + b1 * uv[1][1] + b2 * uv[2][1];
                    u -= std::floor(u), v -= std::floor(v);
                    const u8* q = &ti.px[(size_t(std::min(ti.h - 1, int(v * float(ti.h)))) * size_t(ti.w) + size_t(std::min(ti.w - 1, int(u * float(ti.w))))) * 4];
                    const float c[3] = {float(q[0]) / 255.0f, float(q[1]) / 255.0f, float(q[2]) / 255.0f};
                    const float mx = std::max(c[0], std::max(c[1], c[2])), mn = std::min(c[0], std::min(c[1], c[2]));
                    if (mx <= 0.8f || mn <= 0.45f) continue;  // bright; white or warm, not a saturated sign colour
                    // (and lit by the level's own painted lighting: a lamp that is out is dark there)
                    float vl = 0;
                    for (int r = 0; r < 3; r++) vl = std::max(vl, b0 * vcol[0][r] + b1 * vcol[1][r] + b2 * vcol[2][r]);
                    if (vl < 0.5f) continue;
                    const float wa = cell * 0.5f;
                    float pos[3];
                    for (int r = 0; r < 3; r++) pos[r] = b0 * w[0][r] + b1 * w[1][r] + b2 * w[2][r];
                    bright_area += wa;
                    Lamp* hit = nullptr;
                    for (Lamp& l : found) {
                        float dx = l.p[0] / l.n - pos[0], dy = l.p[1] / l.n - pos[1], dz = l.p[2] / l.n - pos[2];
                        if (dx * dx + dy * dy + dz * dz < 0.7f * 0.7f) {
                            hit = &l;
                            break;
                        }
                    }
                    if (!hit) {
                        found.push_back({{0, 0, 0}, {0, 0, 0}, 0, {cx / nl, cy / nl, cz / nl}, mesh_key});
                        hit = &found.back();
                    }
                    for (int r = 0; r < 3; r++) hit->p[r] += pos[r] * wa, hit->c[r] += c[r] * wa;
                    hit->n += wa;
                }
    }
    // A mesh where much of the surface is bright is a white wall or floor, not a row of lamps.
    if (bright_area > 0.2 * mesh_area) {
        if (g_shadow_debug) LOGI("lamps: mesh of %u vertices is a bright surface (%.1f of %.1f m2): no lamps taken from it", n, bright_area, mesh_area);
        return;
    }
    int taken = 0;
    for (const Lamp& f : found) {
        if (f.n < kLampMinArea || f.n > kLampMaxArea) continue;
        // (one fixture can span two meshes: joined with what is already known there)
        Lamp* hit = nullptr;
        for (Lamp& l : g_lamps) {
            float dx = l.p[0] / l.n - f.p[0] / f.n, dy = l.p[1] / l.n - f.p[1] / f.n, dz = l.p[2] / l.n - f.p[2] / f.n;
            if (dx * dx + dy * dy + dz * dz < 0.7f * 0.7f) {
                hit = &l;
                break;
            }
        }
        if (hit) {
            for (int r = 0; r < 3; r++) hit->p[r] += f.p[r], hit->c[r] += f.c[r];
            hit->n += f.n;
        } else {
            g_lamps.push_back(f);
        }
        taken++;
    }
    if (g_shadow_debug && !found.empty())
        LOGI("lamps: mesh of %u vertices (texture %u, %.1f m2): %zu bright patches, %d taken as fixtures; %zu fixtures so far", n, g_st.unit[0].tex, mesh_area,
             found.size(), taken, g_lamps.size());
}

// ---- blended meshes that should be solid
// A few solid objects (the cable bundles hanging in the corridors) are drawn alpha-blended with a texture
// whose alpha is partly transparent, which makes them see-through. A blended mesh that is a real 3D shape
// (not a flat pane, decal or card), drawn at full opacity without vertex colours, is drawn solid instead:
// it then also takes light and casts shadows like any other object.
std::unordered_map<u64, char> g_solid;
bool should_be_solid(u32 lo, u32 n) {
    if (n < 40 || n > 4000 || g_arr[A_COLOR].enabled || g_st.color[3] < 0.999f) return false;
    const Array& a = g_arr[A_VERTEX];
    u64 key = (u64(reinterpret_cast<uintptr_t>(a.data)) * 0x9E3779B97F4A7C15ull) ^ (u64(lo) << 32) ^ n;
    auto it = g_solid.find(key);
    if (it != g_solid.end()) return it->second != 0;
    // A pane, decal or card lies in one plane; a cable or pipe does not. Plane through the first vertex,
    // the vertex farthest from it and the vertex farthest from the line between them; then how far the
    // rest stray from that plane, compared with how far they stray from the line.
    auto P = [&](u32 i) { return &g_verts[size_t(i) * kStride]; };
    const float* v0 = P(0);
    u32 i1 = 0, i2 = 0;
    float best = 0;
    for (u32 i = 1; i < n; i++) {
        const float* v = P(i);
        float d = (v[0] - v0[0]) * (v[0] - v0[0]) + (v[1] - v0[1]) * (v[1] - v0[1]) + (v[2] - v0[2]) * (v[2] - v0[2]);
        if (d > best) best = d, i1 = i;
    }
    const float* v1 = P(i1);
    float ax[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]}, al = std::sqrt(best);
    bool solid = false;
    if (al > 1e-6f) {
        for (float& x : ax) x /= al;
        auto off_line = [&](const float* v, float o[3]) {
            float d[3] = {v[0] - v0[0], v[1] - v0[1], v[2] - v0[2]}, t = d[0] * ax[0] + d[1] * ax[1] + d[2] * ax[2];
            for (int r = 0; r < 3; r++) o[r] = d[r] - ax[r] * t;
            return std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        };
        float width = 0, o[3], wdir[3] = {0, 0, 0};
        for (u32 i = 0; i < n; i++) {
            float d = off_line(P(i), o);
            if (d > width) width = d, i2 = i, wdir[0] = o[0], wdir[1] = o[1], wdir[2] = o[2];
        }
        (void)i2;
        if (width > 1e-6f) {
            for (float& x : wdir) x /= width;
            float nrm[3] = {ax[1] * wdir[2] - ax[2] * wdir[1], ax[2] * wdir[0] - ax[0] * wdir[2], ax[0] * wdir[1] - ax[1] * wdir[0]}, thick = 0;
            for (u32 i = 0; i < n; i++) {
                const float* v = P(i);
                thick = std::max(thick, std::fabs((v[0] - v0[0]) * nrm[0] + (v[1] - v0[1]) * nrm[1] + (v[2] - v0[2]) * nrm[2]));
            }
            solid = thick > 0.12f * width;
        }
    }
    g_solid[key] = solid ? 1 : 0;
    if (g_shadow_debug) LOGI("blended mesh of %u vertices (texture %u): %s", n, g_st.unit[0].tex, solid ? "drawn solid" : "left blended (flat)");
    return solid;
}

size_t g_solid_count() {
    size_t n = 0;
    for (auto& kv : g_solid) n += kv.second != 0;
    return n;
}

// The lights whose position is known before the frame is drawn: alarm spotlights and the nearest strip
// lights. The first kTiles of them get a shadow map (a tile of the atlas): every opaque mesh of the
// scene is drawn into it as the game draws the frame, so walls, props and characters block the light.
void setup_frame_lights() {
    g_ntiles = 0;
    if (g_lights <= 0) return;
    beacon_lights();
    for (Light l : g_glow_lights) {
        for (int r = 0; r < 3; r++) l.pos[r] = g_view.m[r] * l.wpos[0] + g_view.m[4 + r] * l.wpos[1] + g_view.m[8 + r] * l.wpos[2] + g_view.m[12 + r];
        g_spots.push_back(l);
    }
    // every strip light the camera can see: within view distance and touching the view frustum
    std::vector<Light> fx;
    const float eye[3] = {g_view_inv.m[12], g_view_inv.m[13], g_view_inv.m[14]};
    lights::fixtures_world(eye, fx);
    {
        // fixtures found in the level's geometry, where the lockdown meshes do not already give one
        size_t known = fx.size();
        for (const Lamp& lm : g_lamps) {
            float p[3] = {lm.p[0] / float(lm.n), lm.p[1] / float(lm.n), lm.p[2] / float(lm.n)};
            float d2 = (p[0] - eye[0]) * (p[0] - eye[0]) + (p[1] - eye[1]) * (p[1] - eye[1]) + (p[2] - eye[2]) * (p[2] - eye[2]);
            if (d2 > 34.0f * 34.0f) continue;
            bool dup = false;
            for (size_t i = 0; i < known && !dup; i++)
                dup = std::fabs(fx[i].wpos[0] - p[0]) < 1.2f && std::fabs(fx[i].wpos[1] - p[1]) < 1.2f && std::fabs(fx[i].wpos[2] - p[2]) < 1.2f;
            if (dup) continue;
            Light l{};
            // (a door-side lamp of a few centimetres gives less light, and reaches less far, than a long strip)
            const float size = std::clamp(std::sqrt(lm.n / 0.08f), 0.55f, 1.25f);
            float k = std::clamp((34.0f - std::sqrt(d2)) / 2.0f, 0.0f, 1.0f) * 0.42f * size;
            // (red only when the game itself is in lockdown: its own lockdown level, not what is on screen)
            const float lock = lights::lockdown_level(), red[3] = {0.95f, 0.10f, 0.07f};
            for (int r = 0; r < 3; r++) {
                l.wpos[r] = p[r], l.wdir[r] = lm.nrm[r];
                // (in lockdown the lamps are red, and stronger than they are white: the alarm lighting is meant to be harsh)
                l.color[r] = lm.c[r] / float(lm.n) * k * (1.0f - lock) + red[r] * (k / 0.42f) * lock;
            }
            l.radius = 2.4f + 1.0f * size;
            fx.push_back(l);
        }
        std::stable_sort(fx.begin(), fx.end(), [&](const Light& a, const Light& b) {
            auto d = [&](const Light& l) { return (l.wpos[0] - eye[0]) * (l.wpos[0] - eye[0]) + (l.wpos[1] - eye[1]) * (l.wpos[1] - eye[1]) + (l.wpos[2] - eye[2]) * (l.wpos[2] - eye[2]); };
            return d(a) < d(b);
        });
    }
    const float px = g_have_world_proj ? std::fabs(g_world_proj.m[0]) : 1.2f, py = g_have_world_proj ? std::fabs(g_world_proj.m[5]) : 2.1f;
    int strips = 0;
    for (Light& l : fx) {
        if (strips >= 96) break;  // (nearest first; the rest of the 128 stay free for flashes and glows)
        for (int r = 0; r < 3; r++) l.pos[r] = g_view.m[r] * l.wpos[0] + g_view.m[4 + r] * l.wpos[1] + g_view.m[8 + r] * l.wpos[2] + g_view.m[12 + r];
        float depth = -l.pos[2], rr = l.radius;
        if (depth < -rr || std::fabs(l.pos[0]) * px > depth + rr * (px + 1.0f) || std::fabs(l.pos[1]) * py > depth + rr * (py + 1.0f)) continue;
        strips++;
        // Which way the lamp faces: along its plane's normal, on the side the player is on when it is first
        // met up close (lamps face the room). Decided once, then it never changes.
        l.id = light_id(l.wpos, 1, 0.1f);
        {
            auto it = g_facing.find(l.id);
            if (it != g_facing.end()) {
                for (int r = 0; r < 3; r++) l.wdir[r] = it->second[size_t(r)];
            } else {
                float to[3] = {g_cam_target[0] - l.wpos[0], g_cam_target[1] - 0.4f - l.wpos[1], g_cam_target[2] - l.wpos[2]};
                float tl = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]), nl = std::sqrt(l.wdir[0] * l.wdir[0] + l.wdir[1] * l.wdir[1] + l.wdir[2] * l.wdir[2]);
                float d = nl > 0.5f && tl > 1e-3f ? (l.wdir[0] * to[0] + l.wdir[1] * to[1] + l.wdir[2] * to[2]) / (nl * tl) : 0.0f;
                if (nl <= 0.5f) {
                    for (int r = 0; r < 3; r++) l.wdir[r] = tl > 1e-3f ? to[r] / tl : (r == 1 ? -1.0f : 0.0f);
                } else {
                    for (int r = 0; r < 3; r++) l.wdir[r] = l.wdir[r] / nl * (d < 0 ? -1.0f : 1.0f);
                    if (tl < 12.0f && std::fabs(d) > 0.2f) g_facing[l.id] = {l.wdir[0], l.wdir[1], l.wdir[2]};
                }
            }
        }
        g_spots.push_back(l);
    }
    if (!g_light_shadows || !g_atlas_ok || !g_p_depth) return;
    assign_tiles();
}

// ---------------------------------------------------------------- lights and their shadow maps
// Every light that stays in the world (fixtures, lamps, panel glows, fans, beacons) has an identity, a
// fixed position and a fixed facing. The lights near the player each own a shadow map for as long as
// they stay near: a light is not given up for another unless that one is clearly closer. A map looks
// out of the lamp along its facing and covers the half space in front of it. The still scene is drawn
// into it once and kept; only things that move are drawn again each frame.
void tile_view(Tile& t) {
    const float* f = t.dir;
    float upv[3] = {0, 1, 0};
    if (std::fabs(f[1]) > 0.9f) upv[0] = 1, upv[1] = 0;
    float s[3] = {f[1] * upv[2] - f[2] * upv[1], f[2] * upv[0] - f[0] * upv[2], f[0] * upv[1] - f[1] * upv[0]};
    float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (float& x : s) x /= std::max(sl, 1e-6f);
    float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    // a lamp sits in the wall: its map starts a hand in front of it, beyond its own housing
    float e[3], push = t.spot ? 0.0f : 0.08f;
    for (int r = 0; r < 3; r++) e[r] = t.pos[r] + f[r] * push;
    Mat4 view{}, proj{};
    for (int c = 0; c < 3; c++) view.m[c * 4] = s[c], view.m[c * 4 + 1] = u[c], view.m[c * 4 + 2] = -f[c];
    view.m[12] = -(s[0] * e[0] + s[1] * e[1] + s[2] * e[2]);
    view.m[13] = -(u[0] * e[0] + u[1] * e[1] + u[2] * e[2]);
    view.m[14] = f[0] * e[0] + f[1] * e[1] + f[2] * e[2];
    view.m[15] = 1;
    float zn = t.spot ? 0.15f : 0.3f, zf = t.radius + 0.5f, ft = 1.0f / std::tan(t.half);
    proj.m[0] = ft, proj.m[5] = ft, proj.m[10] = (zf + zn) / (zn - zf), proj.m[11] = -1, proj.m[14] = 2 * zf * zn / (zn - zf);
    t.w2c = proj * view;
}

bool box_in_reach(const float* lo, const float* hi, const float* p, float reach) {
    float d2 = 0;
    for (int r = 0; r < 3; r++) {
        float d = p[r] < lo[r] ? lo[r] - p[r] : p[r] > hi[r] ? p[r] - hi[r] : 0.0f;
        d2 += d * d;
    }
    return d2 <= reach * reach;
}

// Which way a lamp shines: toward the open space around it. Rays are cast from the lamp through the
// still scene in all directions; each counts by how far it gets (up to 3 m). A lamp in a wall ends up
// facing into the room, a strip at the foot of a wall facing up and into the room. False when the
// surroundings are open (or unknown) all round.
bool open_direction(const float* pos, float out[3]) {
    StopWatch sw(g_ms_open);
    g_n_open++;
    const float kReach = 3.0f;
    const int kRays = 48;
    float sum[3] = {0, 0, 0}, total = 0;
    for (int i = 0; i < kRays; i++) {
        float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(kRays), rr = std::sqrt(std::max(0.0f, 1.0f - y * y)), a = float(i) * 2.3999632f;
        const float d[3] = {std::cos(a) * rr, y, std::sin(a) * rr};
        float tmin = kReach;
        for (const SMesh& m : g_smesh) {
            if (m.dead || !box_in_reach(m.lo, m.hi, pos, kReach)) continue;
            for (u32 k = m.first; k + 2 < m.first + m.count; k += 3) {
                const float *v0 = &g_sverts[size_t(g_sidx[k]) * 3], *v1 = &g_sverts[size_t(g_sidx[k + 1]) * 3], *v2 = &g_sverts[size_t(g_sidx[k + 2]) * 3];
                float e1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]}, e2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
                float pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
                float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
                if (std::fabs(det) < 1e-9f) continue;
                float inv = 1.0f / det, tv[3] = {pos[0] - v0[0], pos[1] - v0[1], pos[2] - v0[2]};
                float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
                if (u < 0 || u > 1) continue;
                float qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
                float v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
                if (v < 0 || u + v > 1) continue;
                float t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
                if (t > 0.03f && t < tmin) tmin = t;  // (0.03: the surface the lamp itself lies in)
            }
        }
        for (int r = 0; r < 3; r++) sum[r] += d[r] * tmin;
        total += tmin;
    }
    float len = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
    if (total < 1e-3f || len < 0.12f * total) return false;
    for (int r = 0; r < 3; r++) out[r] = sum[r] / len;
    return true;
}

int g_static_draws = 0, g_static_redraws = 0;
void draw_still_scene(const Tile& t) {
    static GLint loc = -2;
    if (loc == -2) loc = p_glGetUniformLocation(g_p_depth, "u_m");
    p_glUniformMatrix4fv(loc, 1, GL_FALSE, t.w2c.m);
    for (const SMesh& m : g_smesh) {
        if (m.dead || !box_in_reach(m.lo, m.hi, t.pos, t.radius + 1.0f)) continue;
        glDrawElements(GL_TRIANGLES, GLsizei(m.count), GL_UNSIGNED_INT, reinterpret_cast<const void*>(uintptr_t(m.first) * 4));
        g_static_draws++;
    }
}

void assign_tiles() {
    const u32 now = SDL_GetTicks();
    auto dist = [](const float* a, const float* b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); };

    // 1. Where the still scene changed, every kept map that looks at the change is stale (in view or not).
    for (Tile& k : g_bakes) {
        k.present = false;
        if (!k.used || k.dirty) continue;
        for (const SBox& ch : g_schanges)
            if (box_in_reach(ch.lo, ch.hi, k.pos, k.radius + 1.0f)) {
                k.dirty = true;
                break;
            }
    }
    g_schanges.clear();

    // 2. Every fixed light of this frame finds its kept map, or takes a free one (or the one unseen longest).
    for (Light& l : g_spots) {
        l.bake = l.tile = -1;
        if (!l.id || l.rotating) continue;
        int slot = -1;
        for (int i = 0; i < kBakes && slot < 0; i++)
            if (g_bakes[i].used && g_bakes[i].id == l.id) slot = i;
        if (slot < 0) {
            for (int i = 0; i < kBakes && slot < 0; i++)
                if (!g_bakes[i].used) slot = i;
            if (slot < 0) {
                u32 oldest = now;
                for (int i = 0; i < kBakes; i++)
                    if (!g_bakes[i].present && g_bakes[i].seen_ms < oldest) oldest = g_bakes[i].seen_ms, slot = i;
            }
            if (slot < 0) continue;  // (more fixed lights in view than maps: this one casts no shadow)
            g_bakes[slot] = Tile{};
        }
        Tile& k = g_bakes[slot];
        if (k.present) continue;  // (two lights on one spot)
        const bool spot = l.cos_outer > 0;
        const float half = spot ? std::acos(std::clamp(l.cos_outer, 0.0f, 1.0f)) + 0.12f : 1.3f;
        // Its place and reach are the light's. Its facing is the light's until the map has been drawn once;
        // from then on only open_direction (below) turns it, so it cannot flip with where the player stands.
        const float dd = l.wdir[0] * k.dir[0] + l.wdir[1] * k.dir[1] + l.wdir[2] * k.dir[2];
        const bool moved = !k.used || dist(k.pos, l.wpos) > 0.2f || std::fabs(k.radius - l.radius) > 0.1f || std::fabs(k.half - half) > 0.03f ||
                           (dd < 0.98f && (!k.baked || spot));
        if (moved) {
            for (int r = 0; r < 3; r++) k.pos[r] = l.wpos[r];
            if (!k.baked || spot)
                for (int r = 0; r < 3; r++) k.dir[r] = l.wdir[r];
            k.radius = l.radius, k.half = half, k.spot = spot;
            tile_view(k);
            k.dirty = true;
        }
        k.id = l.id, k.used = k.present = true, k.seen_ms = now;
        k.fixture = !spot && !l.glow;
        l.bake = slot;
    }

    // 3. Live tiles, for what moves: the turning beacons (which can keep nothing) and the fixed lights
    //    nearest the player. A light keeps its tile until it is 20 m away and gets one within 14 m.
    Light* owner[kTiles] = {};
    for (Tile& t : g_tiles) t.present = false;
    auto claim = [&](int i, Light& l) {
        Tile& t = g_tiles[i];
        if (l.bake >= 0) {
            const Tile& k = g_bakes[l.bake];
            for (int r = 0; r < 3; r++) t.pos[r] = k.pos[r], t.dir[r] = k.dir[r];
            t.radius = k.radius, t.half = k.half, t.spot = k.spot, t.w2c = k.w2c;
        } else {
            const bool spot = l.cos_outer > 0;
            for (int r = 0; r < 3; r++) t.pos[r] = l.wpos[r], t.dir[r] = l.wdir[r];
            t.radius = l.radius, t.half = spot ? std::acos(std::clamp(l.cos_outer, 0.0f, 1.0f)) + 0.12f : 1.3f, t.spot = spot;
            tile_view(t);
        }
        t.id = l.id, t.used = t.present = true, t.rotating = l.rotating, t.seen_ms = now;
        owner[i] = &l;
        l.tile = i;
        for (int r = 0; r < 3; r++) g_tile_pos[i][r] = t.pos[r];
        g_tile_radius[i] = t.radius;
    };
    struct Cand {
        float pd;
        Light* l;
    };
    std::vector<Cand> cands;
    for (Light& l : g_spots) {
        if (!l.id || (l.bake < 0 && !l.rotating)) continue;
        float pd = dist(l.wpos, g_cam_target);
        if (pd > 20.0f) continue;
        int have = -1;
        for (int i = 0; i < kTiles && have < 0; i++)
            if (g_tiles[i].used && g_tiles[i].id == l.id && !g_tiles[i].present) have = i;
        if (have >= 0) claim(have, l);
        else if (pd < 14.0f || l.rotating) cands.push_back({l.rotating ? 0.0f : pd, &l});
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.pd < b.pd; });
    for (Cand& c : cands) {
        int slot = -1;
        for (int i = 0; i < kTiles && slot < 0; i++)
            if (!g_tiles[i].used || (!g_tiles[i].present && now - g_tiles[i].seen_ms > 300)) slot = i;
        if (slot < 0) {  // all taken: only a light that is clearly farther away gives its tile up
            float worst = c.pd + 4.0f;
            for (int i = 0; i < kTiles; i++) {
                if (!g_tiles[i].present || g_tiles[i].rotating) continue;
                float d = dist(g_tiles[i].pos, g_cam_target);
                if (d > worst) worst = d, slot = i;
            }
            if (slot >= 0 && owner[slot]) owner[slot]->tile = -1;
        }
        if (slot < 0) continue;
        g_tiles[slot].used = false;
        claim(slot, *c.l);
    }

    bool any = false;
    g_bakes_present = 0;
    for (const Tile& t : g_tiles) any |= t.present;
    for (const Tile& k : g_bakes) g_bakes_present += k.present;
    if (!any && !g_bakes_present) return;
    g_ntiles = kTiles;

    // 4. Draw. Kept maps first: the stale ones in view, nearest the player first, a few per frame.
    GLint fb = 0, vp[4], act = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
    p_glBindVertexArray(g_svao);
    if (g_supload) {
        StopWatch sw(g_ms_upload);
        g_supload = false;
        p_glBindBuffer(GL_ARRAY_BUFFER, g_svbo);
        p_glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(g_sverts.size() * sizeof(float)), g_sverts.data(), GL_DYNAMIC_DRAW);
        p_glEnableVertexAttribArray(0);
        p_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_sibo);
        p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(g_sidx.size() * 4), g_sidx.data(), GL_DYNAMIC_DRAW);
        p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    if (g_clip_ok) p_glClipControl(k_LOWER_LEFT, k_NEGATIVE_ONE_TO_ONE);
    p_glUseProgram(g_p_depth);
    static const float one = 1.0f;
    glEnable(GL_SCISSOR_TEST);

    std::vector<std::pair<float, int>> stale;
    for (int i = 0; i < kBakes; i++)
        if (g_bakes[i].present && (g_bakes[i].dirty || !g_bakes[i].baked)) stale.push_back({dist(g_bakes[i].pos, g_cam_target), i});
    std::sort(stale.begin(), stale.end());
    g_bakes_pending = int(stale.size());
    g_bakes_drawn = 0;
    if (!g_sidx.empty() && !stale.empty()) {
        StopWatch sw(g_ms_bake);
        p_glBindFramebuffer(GL_FRAMEBUFFER, g_satlas_fbo);
        for (size_t n = 0; n < stale.size() && g_bakes_drawn < kBakeBudget; n++) {
            const int i = stale[n].second;
            Tile& k = g_bakes[i];
            if (k.fixture && now - k.open_ms > 250) {
                // the still scene around the lamp is known (better) now: settle which way the lamp faces
                k.open_ms = now;
                float d[3];
                if (open_direction(k.pos, d) && d[0] * k.dir[0] + d[1] * k.dir[1] + d[2] * k.dir[2] < 0.97f) {
                    for (int r = 0; r < 3; r++) k.dir[r] = d[r];
                    tile_view(k);
                }
            }
            g_facing[k.id] = {k.dir[0], k.dir[1], k.dir[2]};
            const int vx = (i % kBakeCols) * kBakeSize, vy = (i / kBakeCols) * kBakeSize;
            glViewport(vx, vy, kBakeSize, kBakeSize);
            glScissor(vx, vy, kBakeSize, kBakeSize);
            p_glClearBufferfv(GL_DEPTH, 0, &one);
            draw_still_scene(k);
            k.dirty = false;
            if (!k.baked) k.baked = true, k.born_ms = now;
            g_static_redraws++;
            g_bakes_drawn++;
        }
    }

    // Live tiles: cleared; a beacon's also takes the still scene (it turns), the others only what moves,
    // which the game's own draws add as the frame goes on (tile_pass).
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_atlas_fbo);
    for (int i = 0; i < kTiles; i++) {
        Tile& t = g_tiles[i];
        if (!t.present) continue;
        if (owner[i] && owner[i]->bake >= 0) t.w2c = g_bakes[owner[i]->bake].w2c;  // (it may just have been turned)
        const int vx = (i % 4) * kTileSize, vy = (i / 4) * kTileSize;
        glViewport(vx, vy, kTileSize, kTileSize);
        glScissor(vx, vy, kTileSize, kTileSize);
        p_glClearBufferfv(GL_DEPTH, 0, &one);
        if (t.rotating && !g_sidx.empty()) draw_still_scene(t);
        g_tile_from_eye[i] = t.w2c * g_view_inv;
    }
    glDisable(GL_SCISSOR_TEST);

    // each kept map's view from this frame's eye space, and how strong its shadows are (fading in)
    static std::vector<float> rows(size_t(kBakes) * 20);
    std::fill(rows.begin(), rows.end(), 0.0f);
    for (int i = 0; i < kBakes; i++) {
        const Tile& k = g_bakes[i];
        if (!k.present || !k.baked) continue;
        const Mat4 m = k.w2c * g_view_inv;
        std::memcpy(&rows[size_t(i) * 20], m.m, 16 * sizeof(float));
        rows[size_t(i) * 20 + 16] = std::clamp(float(now - k.born_ms) / 400.0f, 0.0f, 1.0f);
    }
    p_glActiveTexture(GL_TEXTURE0 + 12);
    glBindTexture(GL_TEXTURE_2D, g_bmat_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 5, kBakes, GL_RGBA, GL_FLOAT, rows.data());
    p_glActiveTexture(GLenum(act));

    p_glBindVertexArray(0);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    p_glUseProgram(0);
    p_glBindFramebuffer(GL_FRAMEBUFFER, GLuint(fb));
    glViewport(vp[0], vp[1], vp[2], vp[3]);
}

// Draws the mesh that upload_and_draw has just drawn into the shadow maps of the lights it can block:
// only what moves, or what is not part of the kept still scene yet.
void tile_pass(GLenum mode, GLsizei count) {
    if (!g_mesh_moves && g_mesh_cached) return;
    GLint fb = 0, vp[4];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_VIEWPORT, vp);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_atlas_fbo);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    if (g_clip_ok) p_glClipControl(k_LOWER_LEFT, k_NEGATIVE_ONE_TO_ONE);
    p_glUniform1i(u_revz, 0);
    p_glUniform1i(u_gbuf, 0);
    p_glUniform1i(u_fog, 0);
    for (int t = 0; t < kTiles; t++) {
        const Tile& tl = g_tiles[t];
        if (!tl.present || !box_in_reach(g_mesh_lo, g_mesh_hi, tl.pos, tl.radius + 1.0f)) continue;
        glViewport((t % 4) * kTileSize, (t / 4) * kTileSize, kTileSize, kTileSize);
        Mat4 m = g_tile_from_eye[t] * g_mv.top();
        p_glUniformMatrix4fv(u_mvp, 1, GL_FALSE, m.m);
        glDrawElements(mode, count, GL_UNSIGNED_SHORT, nullptr);
        g_tile_draws++;
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, GLuint(fb));
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    apply_state();
}

// Second draw of the mesh that upload_and_draw has just drawn, into the shadow map.
void shadow_pass(GLenum mode, GLsizei count) {
    GLint fb = 0, vp[4];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_VIEWPORT, vp);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_shfbo);
    glViewport(0, 0, kShadowSize, kShadowSize);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    if (g_clip_ok) p_glClipControl(k_LOWER_LEFT, k_NEGATIVE_ONE_TO_ONE);
    if (!g_shadow_cleared) {
        g_shadow_cleared = true;
        static const float one = 1.0f;
        p_glClearBufferfv(GL_DEPTH, 0, &one);
    }
    Mat4 m = g_light_from_eye * g_mv.top();
    p_glUniformMatrix4fv(u_mvp, 1, GL_FALSE, m.m);
    p_glUniform1i(u_revz, 0);
    p_glUniform1i(u_gbuf, 0);
    p_glUniform1i(u_fog, 0);
    glDrawElements(mode, count, GL_UNSIGNED_SHORT, nullptr);
    g_caster_draws++;
    p_glBindFramebuffer(GL_FRAMEBUFFER, GLuint(fb));
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    apply_state();
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
    p_glBindAttribLocation(g_prog, AT_NRM, "a_nrm");
    p_glDrawBuffers = reinterpret_cast<PFNGLDRAWBUFFERSPROC>(SDL_GL_GetProcAddress("glDrawBuffers"));
    p_glClearBufferfv = reinterpret_cast<PFNGLCLEARBUFFERFVPROC>(SDL_GL_GetProcAddress("glClearBufferfv"));
    p_glBindFragDataLocation = reinterpret_cast<PFNGLBINDFRAGDATALOCATIONPROC>(SDL_GL_GetProcAddress("glBindFragDataLocation"));
    p_glUniformMatrix3fv = reinterpret_cast<PFNGLUNIFORMMATRIX3FVPROC>(SDL_GL_GetProcAddress("glUniformMatrix3fv"));
    g_gbuf_ok = p_glDrawBuffers && p_glClearBufferfv && p_glBindFragDataLocation && p_glUniformMatrix3fv;
    if (g_gbuf_ok) {
        p_glBindFragDataLocation(g_prog, 0, "frag");
        p_glBindFragDataLocation(g_prog, 1, "o_nrm");
        p_glBindFragDataLocation(g_prog, 2, "o_veil");
    }
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
    u_mv = U("u_mv");
    u_nmat = U("u_nmat");
    u_gbuf = U("u_gbuf");
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
    case GL_LIGHTING: g_st.lighting = on; break;
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
void texture_pixels(unsigned tex, const unsigned char* rgba, int w, int h) { texture_pixels_impl(tex, rgba, w, h); }
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
void set_normals_debug(bool on) { g_normals_debug = on; }
void set_lights(int level) { g_lights = std::clamp(level, 0, 3); }
int lights_level() { return g_lights; }
void set_specular(int level) { g_specular = std::clamp(level, 0, 3); }
void set_shadows(int level) { g_shadows = std::clamp(level, 0, 3); }
int shadows() { return g_shadows; }
void set_shadow_debug(bool on) { g_shadow_debug = on; }
void set_light_shadows(bool on) { g_light_shadows = on; }
void reload_shaders(const std::string& source_path, void (*done)(const std::string&)) {
    g_reload_done = done;
    g_reload_path = source_path;
}

std::string report() {
    int spots = 0, glows = 0, tiled = 0, fans = 0;
    for (auto& l : g_frame_lights) {
        spots += l.cos_outer > 0 ? 1 : 0;
        glows += l.glow ? 1 : 0;
        tiled += l.tile >= 0 ? 1 : 0;
        fans += l.gobo > 0 ? 1 : 0;
    }
    char b[520];
    std::snprintf(b, sizeof b,
                  "render: %s; lights %zu (spot %d, panel glow %d, fan %d, with live tile %d); kept maps %d in view (%d stale, %d drawn); shadow tiles %d; "
                  "caster draws %d; fog draws %d; world tint %s %.2f %.2f %.2f; lockdown %.2f",
                  g_enabled ? "native" : "legacy", g_frame_lights.size(), spots, glows, fans, tiled, g_bakes_present, g_bakes_pending, g_bakes_drawn, g_ntiles, g_caster_draws,
                  g_veil_draws_prev, g_tint_active ? "on" : "off", g_tint_color[0], g_tint_color[1], g_tint_color[2],
                  lights::lockdown_level());
    return b;
}


void set_perf_log(bool on) { g_perf_log = on; }

int set_fog_veil(int on) {
    if (on >= 0) g_veil_on = on != 0;
    return g_veil_draws;
}
void set_vignette(bool on) { g_vignette = on; }
bool vignette() { return g_vignette; }
const std::vector<Light>& debug_frame_lights() { return g_spots_prev; }
void set_tile_debug(int tile) { g_tile_debug = tile; }
void set_vertex_color_debug(int mode) { g_vc_debug = std::clamp(mode, 0, 2); }
int vertex_color_debug() { return g_vc_debug; }
void set_glow_gain(float v) { g_glow_gain = std::clamp(v, 0.0f, 4.0f); }
void set_env_shadows(bool on) { g_env_shadows = on; }
bool player_glow(float pos[3], float color[3]) {
    if (!g_pglow_ms || SDL_GetTicks() - g_pglow_ms > 80) return false;
    std::memcpy(pos, g_pglow_out, 12);
    std::memcpy(color, g_pglow_out + 3, 12);
    return true;
}
void set_env_light(int level) { g_env_light = std::clamp(level, 0, 3); }
int env_light() { return g_env_light; }
void probe(float x, float y, float z, float r) {
    g_probe[0] = x, g_probe[1] = y, g_probe[2] = z, g_probe[3] = r;
    g_probe_on = r > 0;
    for (int t = 0; t < kTiles; t++)
        if (g_tiles[t].used) LOGI("probe map %d: light at %.2f %.2f %.2f facing %.2f %.2f %.2f rotating %d", t + 1, g_tiles[t].pos[0], g_tiles[t].pos[1], g_tiles[t].pos[2], g_tiles[t].dir[0], g_tiles[t].dir[1], g_tiles[t].dir[2], g_tiles[t].rotating);
    for (const Light& l : g_spots_prev) {
        float d = std::hypot(l.wpos[0] - x, l.wpos[2] - z);
        if (d < r + 3.0f) LOGI("probe light: at %.2f %.2f %.2f radius %.1f tile %d spot %d glow %d colour %.2f %.2f %.2f (lockdown %.2f, tint seen %d)", l.wpos[0], l.wpos[1], l.wpos[2], l.radius, l.tile, l.cos_outer > 0, l.glow, l.color[0], l.color[1], l.color[2], lights::lockdown_level(), g_tint_active);
    }
}
void set_lshade(float v) { g_lshade = std::clamp(v, 0.0f, 1.0f); }
void set_fanshade(float v) { g_fanshade = std::clamp(v, 0.0f, 2.0f); }  // (2 = debug view)
bool light_shadows() { return g_light_shadows; }
void scene_changed() {
    g_sverts.clear();
    g_sidx.clear();
    g_smesh.clear();
    g_skey.clear();
    g_schanges.clear();
    g_facing.clear();
    for (Tile& t : g_tiles) t = Tile{};
    for (Tile& k : g_bakes) k = Tile{};
    g_lamps.clear();
    g_lamp_done.clear();
    g_solid.clear();
    g_glow_lights.clear();
    g_tex_images.clear();
    g_lamp_masks.clear();
    g_gobo_tex = 0;
    g_spin.clear();
    g_fans.clear();
    g_gobo_info.clear();
    g_beacons.clear();
    g_touched.clear();
    g_pending.clear();
    g_seen.clear();
}
int specular() { return g_specular; }
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
    // see-through meshes that are really solid objects (hanging cables) are drawn solid
    bool made_solid = false;
    if (g_shadow_debug && g_st.blend && hi - lo + 1 == 159) {
        static int k = 0;
        if (k++ % 300 == 0) LOGI("cable draw: lights %d mode %x src %x dst %x depth %d post %d persp %.2f colarr %d a %.3f", g_lights, mode, g_st.blend_src, g_st.blend_dst, g_st.depth_test, g_post_done, g_proj.top().m[11], g_arr[A_COLOR].enabled, g_st.color[3]);
    }
    const bool saved_blend = g_st.blend, saved_dw = g_st.depth_write, saved_at = g_st.alpha_test;
    if (g_lights > 0 && mode == GL_TRIANGLES && g_st.blend && g_st.blend_src == GL_SRC_ALPHA && g_st.blend_dst == GL_ONE_MINUS_SRC_ALPHA && g_st.depth_test &&
        !g_post_done && g_proj.top().m[11] != 0.0f && should_be_solid(lo, hi - lo + 1)) {
        made_solid = true;
        g_st.blend = false, g_st.depth_write = true, g_st.alpha_test = false;
    }
    // opaque geometry of the 3D scene also records its surface normals (for ambient occlusion / lighting)
    g_gbuf_draw = false;
    g_shadow_draw = false;
    if (g_gbuf_ok && hle::g_normal_target && (g_ao > 0 || g_lights > 0 || g_shadows > 0 || g_normals_debug) && !g_post_done && g_st.depth_test &&
        g_st.depth_write && !g_st.blend && g_proj.top().m[11] != 0.0f) {
        GLint fb = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
        g_gbuf_draw = GLuint(fb) == hle::g_default_fbo;
    }
    if (g_gbuf_draw) {
        if (!g_normals_cleared) {
            g_normals_cleared = true;
            static const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0 + 1};
            static const float zero[4] = {0, 0, 0, 0};
            glDisable(GL_SCISSOR_TEST);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            p_glDrawBuffers(2, both);
            p_glClearBufferfv(GL_COLOR, 1, zero);
            if (hle::g_veil_target) {
                static const GLenum all[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0 + 1, GL_COLOR_ATTACHMENT0 + 2};
                p_glDrawBuffers(3, all);
                p_glClearBufferfv(GL_COLOR, 2, zero);
            }
            p_glDrawBuffers(1, both);
        }
        if (mode == GL_TRIANGLES) build_normals(g_idx, hi - lo + 1);
        if (g_lights > 0) shadow_frame();  // (fixes this frame's lights and their shadow tiles on the first scene draw)
        g_mesh_moves = false;
        if ((g_shadows > 0 || (g_lights > 0 && g_light_shadows)) && mode == GL_TRIANGLES && shadow_frame()) {
            g_mesh_moves = caster(lo, hi - lo + 1);
            g_shadow_draw = g_shadows > 0 && g_mesh_moves;
            // lamps are looked for only in the still scene: a weapon, projectile or pickup that has just
            // appeared is not part of the level, however bright it is
            if (!g_mesh_moves && g_mesh_cached && g_lights > 0 && !made_solid) lamp_scan(lo, hi - lo + 1);
        }
    }
    // See-through effects drawn over the scene (after its solid geometry) record how much of each pixel they
    // cover, so that the lighting pass leaves that share alone: the level's fog banks always, anything else
    // blended (light shafts, flares, steam, sparks) where it floats in front of the surface behind it.
    g_veil_draw = false;
    if (!g_gbuf_draw && g_veil_on && hle::g_veil_target && g_normals_cleared && g_st.blend && !g_post_done && g_proj.top().m[11] != 0.0f &&
        (g_lights > 0 || g_ao > 0 || g_shadows > 0) && g_arr[A_VERTEX].data) {
        const bool fog = lights::is_fog_vertices(u32(uintptr_t(g_arr[A_VERTEX].data) - uintptr_t(mem::g_base)));
        // (not additive draws: light shafts and flares add light on top of the lit scene and hide nothing;
        // their polygons, mostly dark, would otherwise show up as unlit patches with hard edges)
        if (fog || (g_st.depth_test && !g_st.depth_write && g_st.blend_dst != GL_ONE && g_scene.fbo && g_scene_depth)) {
            GLint fb = 0;
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
            g_veil_draw = GLuint(fb) == hle::g_default_fbo;
            g_veil_mode = fog ? 0 : 1;
            g_veilz[0] = g_proj.top().m[10], g_veilz[1] = g_proj.top().m[14], g_veilz[2] = 1.0f / float(g_scene.w), g_veilz[3] = 1.0f / float(g_scene.h);
            if (g_veil_draw) g_veil_draws++;
            if (g_veil_draw && !fog && !g_veil_depth) {
                // the solid scene's depth as it is now, for the shader to measure against
                g_veil_depth = true;
                GLint rfb = 0, act = 0;
                glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
                glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
                const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
                glDisable(GL_SCISSOR_TEST);
                p_glBindFramebuffer(GL_READ_FRAMEBUFFER, hle::g_default_fbo);
                p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_scene.fbo);
                p_glBlitFramebuffer(0, 0, g_scene.w, g_scene.h, 0, 0, g_scene.w, g_scene.h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
                p_glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(rfb));
                p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(fb));
                if (sc) glEnable(GL_SCISSOR_TEST);
                p_glActiveTexture(GL_TEXTURE0 + 3);
                glBindTexture(GL_TEXTURE_2D, g_scene_depth);
                p_glActiveTexture(GLenum(act));
            }
        }
    }
    if (g_trace_draws) {
        const float* v = g_verts.data();
        {
            const Array& ca = g_arr[A_COLOR];
            const u8* cb = ca.data ? ca.data + size_t(lo) * (ca.stride ? ca.stride : ca.size * type_size(ca.type)) : nullptr;
            LOGI("[ndraw]   lo=%u hi=%u colour array: enabled=%d size=%d type=%x stride=%d bytes@lo=%02x %02x %02x %02x %02x %02x %02x %02x",
                 lo, hi, ca.enabled, ca.size, ca.type, ca.stride, cb ? cb[0] : 0, cb ? cb[1] : 0, cb ? cb[2] : 0, cb ? cb[3] : 0,
                 cb ? cb[4] : 0, cb ? cb[5] : 0, cb ? cb[6] : 0, cb ? cb[7] : 0);
        }
        LOGI("[ndraw] dw=%d verts=%u afn=%x aref=%.2f", g_st.depth_write, hi - lo + 1, g_st.alpha_fn, g_st.alpha_ref);
        LOGI("[ndraw] n=%d tex2d=%d tex=%u texarr=%d colarr=%d blend=%d(%x,%x) depth=%d alpha=%d fog=%d col=%.2f,%.2f,%.2f,%.2f "
             "v0=(%.1f,%.1f,%.1f,%.1f) c0=(%.2f,%.2f,%.2f,%.2f) t0=(%.3f,%.3f) texmat0=%.2f mvp0=%.3f",
             count, g_st.unit[0].enabled, g_st.unit[0].tex, g_arr[A_TEX0].enabled, g_arr[A_COLOR].enabled, g_st.blend,
             g_st.blend_src, g_st.blend_dst, g_st.depth_test, g_st.alpha_test, g_st.fog, g_st.color[0], g_st.color[1],
             g_st.color[2], g_st.color[3], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], g_tex[0].top().m[0],
             (g_proj.top() * g_mv.top()).m[0]);
    }
    // glow meshes of the 3D scene: look for spinning beacons
    if (g_lights > 0 && mode == GL_TRIANGLES && g_st.blend && g_st.blend_dst == GL_ONE && g_st.depth_test && !g_st.depth_write && !g_post_done &&
        g_proj.top().m[11] != 0.0f && shadow_frame())
        beacon(lo, hi - lo + 1);
    // glowing panels: additive or alpha-blended overlays of the 3D scene (holograms, screens)
    if (g_lights > 0 && mode == GL_TRIANGLES && g_st.blend && g_st.blend_src == GL_SRC_ALPHA && (g_st.blend_dst == GL_ONE || g_st.blend_dst == GL_ONE_MINUS_SRC_ALPHA) &&
        g_st.depth_test && !g_post_done && g_proj.top().m[11] != 0.0f && shadow_frame())
        glow(hi - lo + 1);
    // the game's fan shadow on the floor (multiplied over it): gives the blades' position
    if (g_lights > 0 && mode == GL_TRIANGLES && g_st.blend && g_st.blend_src == GL_DST_COLOR && g_st.blend_dst == GL_ZERO && g_st.depth_test && !g_post_done &&
        hi - lo + 1 == 4 && g_proj.top().m[11] != 0.0f && shadow_frame())
        fan_floor(4);
    if ((g_st.lighting || (!g_st.blend && g_st.depth_write)) && !g_post_done && g_proj.top().m[11] != 0.0f) tint_vote(float(hi - lo + 1) * (g_mesh_moves ? 1.0f : 4.0f));
    p_glUseProgram(g_prog);
    apply_state();
    upload_and_draw(mode, g_idx);
    p_glUseProgram(0);
    if (made_solid) g_st.blend = saved_blend, g_st.depth_write = saved_dw, g_st.alpha_test = saved_at;
}

void draw_arrays(GLenum mode, int first, int count) {
    if (count <= 0 || !g_arr[A_VERTEX].enabled || !g_arr[A_VERTEX].data) return;
    post_before_draw();
    g_gbuf_draw = g_shadow_draw = g_mesh_moves = false;
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
    g_seen_world = g_post_done = g_have_world_proj = g_normals_cleared = false;
    shadow_frame_end();
    g_casters.swap(g_casters_now);
    g_casters_now.clear();
    static int logn = 0;
    if (g_shadow_debug && (logn++ % 120) == 0) LOGI("shadows: %d caster draws last frame, %zu meshes tracked", g_caster_draws, g_seen.size());
    g_shadow_cleared = false;
    g_tint_active = g_tint_seen;
    for (int k = 0; k < 3; k++) g_tint_color[k] = g_tint_seen ? g_tint_now[k] : 1.0f, g_tint_now[k] = 1.0f;
    g_tint_seen = false;
    g_tint_nvotes = 0;
    {
        static Uint64 last = 0;
        const Uint64 nowc = SDL_GetPerformanceCounter();
        const double frame = last ? double(nowc - last) * 1000.0 / double(SDL_GetPerformanceFrequency()) : 0.0;
        last = nowc;
        if (g_perf_log && (frame > 25.0 || g_ms_scan + g_ms_open + g_ms_bake + g_ms_upload > 4.0))
            LOGI("PERF frame %.1f ms: lamp scan %.1f, lamp facing %.1f (%d), map draws %.1f (%d), still-scene upload %.1f (%zu verts)", frame, g_ms_scan, g_ms_open,
                 g_n_open, g_ms_bake, g_bakes_drawn, g_ms_upload, g_sverts.size() / 3);
        g_ms_scan = g_ms_open = g_ms_bake = g_ms_upload = 0;
        g_n_open = 0;
    }
    g_veil_draws_prev = g_veil_draws;
    g_veil_draws = 0;
    g_veil_depth = false;
    g_spots_prev = g_spots;
    glow_frame_end();
    g_spots.clear();
    static int tlog = 0;
    if (g_shadow_debug && (tlog++ % 120) == 0) {
        int used = 0;
        for (const Tile& t : g_tiles) used += t.present;
        LOGI("light shadows: %d maps in use, %d moving-mesh draws, %d still-scene draws (%d maps redrawn) last frame; still scene %zu meshes, %zu vertices; %zu lamps found, %zu meshes made solid",
             used, g_tile_draws, g_static_draws, g_static_redraws, g_smesh.size(), g_sverts.size() / 3, g_lamps.size(), g_solid_count());
    }
    g_static_draws = g_static_redraws = 0;
    g_ntiles = 0;
    g_tile_draws = 0;
    g_atlas_cleared = false;
    if (g_spin.size() > 20000) g_spin.clear();
    g_shadow_frame = 0;
    g_caster_draws = 0;
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
