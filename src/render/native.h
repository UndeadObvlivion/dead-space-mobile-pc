// Native renderer: the game's GL ES 1.1 draw calls, captured as state + vertex data and drawn
// with a modern OpenGL shader path. Lives beside the legacy fixed-function translation; the
// active path is chosen with set_enabled() (port_settings "renderer").
//
// What the game actually uses (measured with --gltrace on gameplay and menu frames): indexed
// triangles only, no buffer objects, vertex data straight from guest memory (short/float
// positions, short/float texcoords, ubyte colours), one texture unit with MODULATE, no GL
// lighting, no skinning extension, linear fog, alpha test, scissor, blend, depth, cull.
#pragma once
#include <SDL_opengl.h>

#include "memory.h"

namespace render {

// Call with the GL context current (after hle::gles_init_host). Returns false if shaders are
// unavailable (then the legacy path stays active).
bool init();
bool available();
void set_enabled(bool on);
bool enabled();

// ---- state mirror, fed by the GLES translation layer for every call it receives (always,
// so the renderer can be switched on at any time)
void matrix_mode(GLenum mode);
void load_identity();
void load_matrix(const float* m);
void mult_matrix(const float* m);
void push_matrix();
void pop_matrix();
void translate(float x, float y, float z);
void scale(float x, float y, float z);
void rotate(float deg, float x, float y, float z);
void frustum(float l, float r, float b, float t, float n, float f);
void ortho(float l, float r, float b, float t, float n, float f);
void enable(GLenum cap, bool on);
void color(float r, float g, float b, float a);
void active_texture(int unit);
void bind_texture(int unit, GLuint tex);
// Base format of an uploaded texture (GL_RGB, GL_RGBA, GL_ALPHA, GL_LUMINANCE, GL_LUMINANCE_ALPHA):
// ES 1.1 MODULATE behaves differently per format.
void texture_format(GLuint tex, GLenum base_format);
void set_debug_hud(bool on);
// Texture bookkeeping for the enhanced filtering (sampler objects used by the native path only):
void tex_param(GLuint tex, GLenum pname, GLenum value);  // MIN/MAG filter, WRAP_S/T as the game set them
void texture_level(GLuint tex, int level);                // a level was uploaded (0 = base image changed)
void texture_game_mipmaps(GLuint tex);                    // the game generated the mip chain itself
void texture_is_target(GLuint tex);                       // attached to a framebuffer (rendered into)
void texture_deleted(GLuint tex);

// Anisotropic filtering level for 3D draws on the native path (1 = off: the game's own filtering).
// Above 1, textures the game left without mipmaps also get a generated mip chain.
void set_anisotropy(int level);
int anisotropy();
int max_anisotropy();
// Reversed-Z (far = 0) into the float depth buffer on the native path: far more depth precision at
// distance. reversed_z() is true while native draws use it (the frame's depth clear must match).
void set_reversed_z(bool on);
bool reversed_z();
// Post-processing of the 3D scene on the native path (the HUD is drawn after it, unaffected).
// Levels: 0 off, 1 low, 2 medium, 3 high.
void set_bloom(int level);
void set_sharpen(int level);
void set_ao(int level);  // ambient occlusion 0-3
void set_ao_debug(bool on);
void set_ao_tuning(float gain, float radius);  // dev: occlusion gain, radius override (0 = default)
int ao();
int bloom();
int sharpen();  // debug: draw depth-test-off (HUD) draws flat magenta
void tex_env_mode(int unit, GLenum mode);
void tex_env_color(int unit, const float* rgba);
void blend_func(GLenum src, GLenum dst);
void depth_func(GLenum f);
void depth_mask(bool on);
void color_mask(bool r, bool g, bool b, bool a);
void alpha_func(GLenum f, float ref);
void cull_face(GLenum mode);
void front_face(GLenum mode);
void fog_mode(GLenum mode);
void fog_param(GLenum pname, float v);
void fog_color(const float* rgba);
void scissor(int x, int y, int w, int h);
void viewport(int x, int y, int w, int h);
void polygon_offset(float factor, float units);

struct Array {
    bool enabled = false;
    int size = 4;
    GLenum type = GL_FLOAT;
    int stride = 0;
    const u8* data = nullptr;  // host pointer (guest memory or buffer shadow)
};
enum { A_VERTEX, A_NORMAL, A_COLOR, A_TEX0, A_TEX1, A_COUNT };
void set_array(int which, const Array& a);

// ---- drawing (replaces the legacy glDrawElements / glDrawArrays when enabled)
void draw_elements(GLenum mode, int count, GLenum index_type, const void* indices);
void draw_arrays(GLenum mode, int first, int count);

// Per-frame hooks
void begin_frame();
void end_frame();
void trace_next_frame();  // debug: log every native draw of the next frame

}  // namespace render
