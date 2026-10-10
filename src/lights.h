// Dynamic light sources for the native renderer, taken from game events.
#pragma once
#include <vector>

#include "render/native.h"

namespace lights {

void install();           // hooks (before the game runs)
void set_trace(bool on);  // dev: log fire / impact / projectile events
// A light at a point in the game's physics/render space that fades out over life_ms.
void add(float x, float y, float z, float r, float g, float b, float radius, float life_ms);
// The lights alive now, in the gameplay camera's eye space (newest first). Empty when that camera is
// not in use (cinematics, menus).
void eye_space(std::vector<render::Light>& out);
// Dev: every light the port knows about right now, in world space (debug view).
struct DebugLight {
    float pos[3], color[3], radius;
    float dir[3] = {0, 0, 0}, cos_outer = 0;
    bool spot = false, active = true;  // active: lit this frame
    int tile = -1;                     // shadow map number, or -1
    const char* kind = "";
};
void debug_list(std::vector<DebugLight>& out);
// World -> eye matrix (column-major) of the gameplay camera and the point it looks at; false when that
// camera is not in use.
bool camera_matrix(float view[16], float target[3]);
// The strip light fixtures within view distance of `at` (world space: wpos, color, radius), nearest first.
void fixtures_world(const float at[3], std::vector<render::Light>& out);
// Dev: force hidden level nodes (registered flares / animated nodes) near x, z to be drawn. Game thread.
int debug_show_nodes(float x, float z, float radius);

}  // namespace lights
