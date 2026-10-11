// Debug views drawn over the game: light placement and object hitboxes. (The interaction view lives in
// interact.cpp.) Toggled from the console (/debug_lights, /debug_hitboxes, /debug_interact, /debug_off)
// or with F6 / F7 / F10.
#include "debugview.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

#include "hle/gles.h"
#include "lights.h"
#include "log.h"
#include "objects.h"

namespace debugview {
namespace {

std::atomic<bool> g_lights{false}, g_hitboxes{false};

using objs::Vec3;

float dist(Vec3 a, Vec3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

void line3(u32 world, Vec3 a, Vec3 b, unsigned rgba) {
    float ax, ay, bx, by;
    if (objs::to_screen(world, a, ax, ay) && objs::to_screen(world, b, bx, by)) hle::overlay_line(ax, ay, bx, by, rgba);
}

void box(u32 world, const objs::Box& b, unsigned rgba) {
    static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    Vec3 c[8];
    for (int i = 0; i < 8; i++) c[i] = {i & 1 ? b.hi.x : b.lo.x, i & 2 ? b.hi.y : b.lo.y, i & 4 ? b.hi.z : b.lo.z};
    for (auto& e : kEdges) line3(world, c[e[0]], c[e[1]], rgba);
}

// A light: a 3-axis cross at its position, a ring on the floor plane showing its radius, and a label.
void light_marker(u32 world, Vec3 p, float radius, unsigned rgba, const char* label, bool ring) {
    const float k = 0.12f;
    line3(world, {p.x - k, p.y, p.z}, {p.x + k, p.y, p.z}, rgba);
    line3(world, {p.x, p.y - k, p.z}, {p.x, p.y + k, p.z}, rgba);
    line3(world, {p.x, p.y, p.z - k}, {p.x, p.y, p.z + k}, rgba);
    if (ring) {
        unsigned dim = (rgba & 0xffffff00u) | 0x60u;
        for (int i = 0; i < 24; i++) {
            float a0 = float(i) * 0.2617994f, a1 = float(i + 1) * 0.2617994f;
            line3(world, {p.x + std::cos(a0) * radius, p.y, p.z + std::sin(a0) * radius}, {p.x + std::cos(a1) * radius, p.y, p.z + std::sin(a1) * radius}, dim);
        }
    }
    float sx, sy;
    if (label && *label && objs::to_screen(world, p, sx, sy)) hle::overlay_text(sx + 10, sy - 8, 2, label, rgba);
}

unsigned rgba_of(const float* c, float floor_v) {
    float m = std::max(c[0], std::max(c[1], c[2]));
    float k = m > 1e-4f ? 1.0f / m : 0.0f;
    auto ch = [&](float v) { return unsigned(std::min(255.0f, std::max(floor_v, v * k) * 255.0f)); };
    return (ch(c[0]) << 24) | (ch(c[1]) << 16) | (ch(c[2]) << 8) | 0xffu;
}

void draw_lights(u32 player, u32 world) {
    Vec3 pp = objs::pos(player);
    std::vector<lights::DebugLight> all;
    lights::debug_list(all);
    int n = 0, shadowed = 0;
    for (const lights::DebugLight& l : all) {
        Vec3 p{l.pos[0], l.pos[1], l.pos[2]};
        float d = dist(p, pp);
        if (d > 30.0f) continue;
        n++;
        if (l.tile >= 0) shadowed++;
        unsigned col = rgba_of(l.color, 0.25f);
        char label[64] = "";
        // labels only near the player, or for the lights that are actually lit this frame
        if (d < 9.0f || l.active) {
            if (l.tile >= 0) std::snprintf(label, sizeof label, "%s  R %.1f  SHADOW %d", l.kind, l.radius, l.tile + 1);
            else std::snprintf(label, sizeof label, "%s  R %.1f", l.kind, l.radius);
        }
        if (!l.active) col = (col & 0xffffff00u) | 0x70u;  // known but not lit right now (too far, or over the limit)
        light_marker(world, p, l.radius, col, label, l.active && d < 14.0f);
        if (l.spot) {  // spotlight: its axis and the edge of its cone
            Vec3 e{p.x + l.dir[0] * 3.0f, p.y + l.dir[1] * 3.0f, p.z + l.dir[2] * 3.0f};
            line3(world, p, e, col);
            float t = std::tan(std::acos(std::min(1.0f, l.cos_outer))) * 3.0f;
            // two edge rays in the horizontal plane through the axis
            Vec3 side{-l.dir[2], 0, l.dir[0]};
            line3(world, p, {e.x + side.x * t, e.y, e.z + side.z * t}, col);
            line3(world, p, {e.x - side.x * t, e.y, e.z - side.z * t}, col);
        }
    }
    char head[96];
    std::snprintf(head, sizeof head, "LIGHTS: %d WITHIN 30 M, %d WITH SHADOW MAPS  (DIM = NOT LIT NOW)", n, shadowed);
    hle::overlay_text(24, 110, 2, head, 0xffe060ffu);
}

void draw_hitboxes(u32 player, u32 world) {
    Vec3 pp = objs::pos(player);
    int n = 0;
    for (u32 o : objs::world_objects(world)) {
        if (!o) continue;
        objs::Box b = objs::bounds(o);
        if (objs::empty(b)) continue;
        Vec3 c{(b.lo.x + b.hi.x) * 0.5f, (b.lo.y + b.hi.y) * 0.5f, (b.lo.z + b.hi.z) * 0.5f};
        float d = dist(c, pp);
        if (d > 18.0f && o != player) continue;
        // skip room-sized volumes (trigger zones, spawn zones): they hide everything else
        if (b.hi.x - b.lo.x > 12.0f || b.hi.z - b.lo.z > 12.0f) continue;
        std::string cls = objs::class_name(o);
        bool enemy = cls.find("Slasher") != std::string::npos || cls.find("Lurker") != std::string::npos || cls.find("Brute") != std::string::npos ||
                     cls.find("Exploder") != std::string::npos || cls.find("Pregnant") != std::string::npos || cls.find("Swarm") != std::string::npos ||
                     cls.find("Boss") != std::string::npos || cls.find("Necromorph") != std::string::npos;
        unsigned col = o == player ? 0x40ff60ffu : enemy ? 0xff4040ffu : cls.find("Projectile") != std::string::npos ? 0xff9030ffu
                                                                         : cls.find("Limb") != std::string::npos   ? 0xff80c0ffu
                                                                                                                   : 0xffe040c0u;
        box(world, b, col);
        n++;
        float sx, sy;
        if (d < 8.0f && objs::to_screen(world, {c.x, b.hi.y, c.z}, sx, sy)) {
            std::string name = cls.rfind("GameObject", 0) == 0 ? cls.substr(10) : cls;
            for (char& ch : name) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            hle::overlay_text(sx - 40, sy - 22, 2, name.c_str(), col);
        }
    }
    char head[96];
    std::snprintf(head, sizeof head, "HITBOXES: %d OBJECTS  (GREEN PLAYER, RED ENEMY, PINK LIMB, YELLOW OTHER)", n);
    hle::overlay_text(24, g_lights ? 140.0f : 110.0f, 2, head, 0xffe060ffu);
}

}  // namespace

void set_lights(bool on) { g_lights = on; }
void set_hitboxes(bool on) { g_hitboxes = on; }
bool lights() { return g_lights; }
bool hitboxes() { return g_hitboxes; }

void draw(unsigned player, unsigned world) {
    if (!player || !world) return;
    if (g_lights) draw_lights(player, world);
    if (g_hitboxes) draw_hitboxes(player, world);
}

}  // namespace debugview
