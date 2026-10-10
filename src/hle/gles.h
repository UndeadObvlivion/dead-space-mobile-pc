#pragma once
#include <SDL_opengl.h>

#include <string>

namespace hle {
// Load host GL entry points; call once with the GL context current.
void gles_init_host();
// Framebuffer the guest's "FBO 0" maps to (0 = the window).
extern GLuint g_default_fbo;
extern GLenum g_depth_fmt;
extern bool g_normal_target;

// Offscreen render target the game draws into, scaled to the window when presenting.
void present_init(int w, int h, int msaa_samples);
// Anti-aliasing (multisample count, 0 = off); can change at any time.
void set_msaa(int samples);
int msaa_samples();
int max_msaa();
void present_begin();                       // bind the game's framebuffer
void present_end(int win_w, int win_h);     // letterbox-blit to the window (back buffer)
// Maps a window position to game coordinates; false if outside the picture.
bool present_map(int win_w, int win_h, float wx, float wy, float& gx, float& gy);
void present_read(int w, int h, void* rgba); // read back the game's last frame
// Debug overlay: line segments in game-screen pixels (x right, y down), RGBA colour, drawn over
// the game's frame at the next present_end (also visible in screenshots taken after it).
void overlay_line(float x0, float y0, float x1, float y1, unsigned rgba);
void overlay_rect(float x, float y, float w, float h, unsigned rgba);  // filled
// Text with a built-in 5x7 pixel font; px = size of one font pixel. Returns the width drawn.
float overlay_text(float x, float y, float px, const char* text, unsigned rgba);
void overlay_draw();  // draws and clears the queued lines into the game's frame
extern std::string g_dump_dir;  // debug: dump compressed texture payloads
extern bool g_real_atc;
}  // namespace hle
