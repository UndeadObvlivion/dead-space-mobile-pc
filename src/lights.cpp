// Dynamic light sources for the native renderer, taken from game events (the game itself has no
// lights: its Weapon::cacheLight / GameObjectProjectile::initLight are empty).
//
//   weapon fired          -> short flash at the muzzle
//   Weapon::resolveImpact -> short flash where the shot landed
//   projectile in flight  -> a light that follows it (line gun bolts, enemy projectiles)
//   explosive detonates   -> a larger, longer flash
//
// Sources are kept in the game's physics/render space and handed to the renderer in eye space,
// using the gameplay camera's eye/target/up (input::camera).
#include "lights.h"

#include <SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <string>

#include "cpu.h"
#include "elf_loader.h"
#include "input.h"
#include "log.h"
#include "memory.h"
#include "objects.h"

namespace lights {
namespace {

struct Src {
    float p[3];
    float c[3];
    float radius;
    u32 born_ms;
    float life_ms;  // fades out linearly over its life
    u32 id;         // object the light follows (0 = none): one light per object, moved on each update
};
std::vector<Src> g_src;
std::map<u32, std::array<float, 3>> g_weapon_color;  // weapon object -> colour of its last shot
int g_trace = 0;

float f32(u32 a) {
    u32 v = mem::r32(a);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void push(float x, float y, float z, const float* c, float radius, float life_ms, u32 id = 0) {
    if (id)
        for (Src& s : g_src)
            if (s.id == id) {
                s.p[0] = x, s.p[1] = y, s.p[2] = z;
                s.c[0] = c[0], s.c[1] = c[1], s.c[2] = c[2];
                s.radius = radius;
                s.born_ms = SDL_GetTicks();
                return;
            }
    if (g_src.size() >= 48) g_src.erase(g_src.begin());
    g_src.push_back({{x, y, z}, {c[0], c[1], c[2]}, radius, SDL_GetTicks(), life_ms, id});
}

const float kPlasma[3] = {0.45f, 0.85f, 1.0f}, kRipper[3] = {1.0f, 0.75f, 0.45f}, kLineGun[3] = {0.4f, 0.9f, 1.0f};
const float kCorer[3] = {1.0f, 0.6f, 0.3f}, kStasis[3] = {0.35f, 0.6f, 1.0f}, kEnemy[3] = {0.95f, 0.8f, 0.35f};
const float kExplosion[3] = {1.0f, 0.65f, 0.3f};

void on_fire(cpu::Ctx& c) {
    std::string name = cpu::current_hle_name(c);
    const float* col = name.find("Ripper") != std::string::npos    ? kRipper
                       : name.find("LineGun") != std::string::npos ? kLineGun
                       : name.find("Corer") != std::string::npos   ? kCorer
                       : name.find("Stasis") != std::string::npos  ? kStasis
                                                                   : kPlasma;
    g_weapon_color[c.reg(0)] = {col[0], col[1], col[2]};
    if (g_trace) LOGI("[lights] onFire weapon=%08x (%s)", c.reg(0), name.c_str());
    u32 pl = input::player();
    if (!pl) return;
    // muzzle: about chest height, an arm's length in front of the player
    objs::Vec3 p = objs::pos(pl), f = objs::facing(pl);
    push(p.x + f.x * 0.8f, p.y + 0.45f, p.z + f.z * 0.8f, col, 5.0f, 120.0f);
}

// Weapon::resolveImpact(void* object, const btVector3& position, const btVector3& normal, ...)
void on_impact(cpu::Ctx& c) {
    u32 a = c.reg(2), b = c.reg(3);
    if (!a || !b) return;
    float px = f32(a), py = f32(a + 4), pz = f32(a + 8), nx = f32(b), ny = f32(b + 4), nz = f32(b + 8);
    if (g_trace) LOGI("[lights] impact weapon=%08x at %.2f %.2f %.2f normal %.2f %.2f %.2f", c.reg(0), px, py, pz, nx, ny, nz);
    if (!std::isfinite(px + py + pz + nx + ny + nz)) return;
    auto it = g_weapon_color.find(c.reg(0));
    const float* col = it != g_weapon_color.end() ? it->second.data() : kPlasma;
    // lifted off the surface so the surface itself is lit
    push(px + nx * 0.25f, py + ny * 0.25f, pz + nz * 0.25f, col, 2.5f, 160.0f);
}

void on_projectile(cpu::Ctx& c) {
    u32 obj = c.reg(0);
    std::string cls = objs::class_name(obj);
    objs::Vec3 p = objs::pos(obj);
    if (!std::isfinite(p.x + p.y + p.z)) return;
    bool player_shot = cls.find("LineGun") != std::string::npos || cls.find("Corer") != std::string::npos;
    const float* col = cls.find("Corer") != std::string::npos ? kCorer : player_shot ? kLineGun : kEnemy;
    static int n = 0;
    if (g_trace && (n++ % 10) == 0) LOGI("[lights] projectile %08x %s at %.2f %.2f %.2f", obj, cls.c_str(), p.x, p.y, p.z);
    // follows the projectile; fades out quickly once it stops updating (hit something)
    const float dim[3] = {col[0] * 0.6f, col[1] * 0.6f, col[2] * 0.6f};  // in flight for a while: gentler than a flash
    push(p.x, p.y, p.z, dim, player_shot ? 3.5f : 3.0f, 120.0f, obj);
}

void on_explode(cpu::Ctx& c) {
    objs::Vec3 p = objs::pos(c.reg(0));
    if (g_trace) LOGI("[lights] explosion %08x at %.2f %.2f %.2f", c.reg(0), p.x, p.y, p.z);
    if (!std::isfinite(p.x + p.y + p.z)) return;
    push(p.x, p.y + 0.5f, p.z, kExplosion, 8.0f, 450.0f);
}

// ---- level scene nodes (m3g): flares the level registers with the world
struct SceneNode {
    u32 node;
    int kind;  // 0 direction flare, 1 side flare, 2 animated
};
std::vector<SceneNode> g_nodes;
u32 g_nodes_world = 0, fn_transform_to = 0, g_xf = 0;

void note_node(u32 world, u32 node, int kind) {
    if (world != g_nodes_world) {
        g_nodes.clear();
        g_nodes_world = world;
    }
    if (node) g_nodes.push_back({node, kind});
}
std::vector<u32> g_fade_nodes;  // GameWorld::registerProximityFade: effects the game hides when the player is close
u32 g_fade_world = 0;
// The game fades these effects (fog banks, light shafts) out by distance as the player walks into them,
// over a band about two metres wide: at a run that is an instant. Here the fade is spread over time.
// Each frame, after the game has set an effect's opacity (m3g::Node +0x130, 16.16; +0x12c = drawn at
// all) and before it is drawn, the value actually used moves toward the game's value at a limited
// speed, both ways.
struct FadeState {
    float shown = -1;  // opacity in use (-1: not started)
    u32 wrote_alpha = 0xffffffffu;
    u8 wrote_en = 0xff;
    float target = 1;
};
std::unordered_map<u32, FadeState> g_fade_state;
constexpr float kFadeSeconds = 1.4f;  // from fully shown to gone, or back

// Position data of the fog banks' meshes (m3g::Mesh: VertexBuffer +0x160, its positions array +0x44,
// that array's data +0x48): the renderer recognises their draws by it.
std::vector<u32> g_fog_data;
void note_fog_mesh(u32 node) {
    if (cpu::symbolize(mem::r32(node)).find("Mesh") == std::string::npos) return;
    u32 vb = mem::r32(node + 0x160);
    u32 va = vb > 0x10000 ? mem::r32(vb + 0x44) : 0;
    u32 data = va > 0x10000 ? mem::r32(va + 0x48) : 0;
    if (data > 0x10000 && std::find(g_fog_data.begin(), g_fog_data.end(), data) == g_fog_data.end()) g_fog_data.push_back(data);
}

void smooth_fades(cpu::Ctx& c) {
    if (c.reg(0) != g_fade_world || g_fade_nodes.empty()) return;
    g_fog_data.clear();
    static u32 last = 0;
    u32 now = SDL_GetTicks();
    float dt = last && now - last < 250 ? float(now - last) / 1000.0f : 0.0f;
    last = now;
    auto one = [&](u32 node) {
        FadeState& st = g_fade_state[node];
        u32 a = mem::r32(node + 0x130);
        u8 en = mem::r8(node + 0x12c);
        // what the game wants: its own value, unless what is there is still what was written last frame
        if (a != st.wrote_alpha || en != st.wrote_en) st.target = en ? std::clamp(float(s32(a)) / 65536.0f, 0.0f, 1.0f) : 0.0f;
        if (st.shown < 0) st.shown = st.target;
        float step = dt / kFadeSeconds;
        st.shown = st.shown < st.target ? std::min(st.target, st.shown + step) : std::max(st.target, st.shown - step);
        st.wrote_alpha = u32(s32(st.shown * 65536.0f));
        st.wrote_en = st.shown > 0.004f ? 1 : 0;
        mem::w32(node + 0x130, st.wrote_alpha);
        mem::w8(node + 0x12c, st.wrote_en);
    };
    for (u32 n : g_fade_nodes) {
        if (cpu::symbolize(mem::r32(n)).find("Group") == std::string::npos) {
            one(n);
            note_fog_mesh(n);
            continue;
        }
        u32 cb = mem::r32(n + 0x13c), ce = mem::r32(n + 0x140);
        for (u32 q = cb; q < ce && q < cb + 4 * 32; q += 4)
            if (u32 ch = mem::r32(q)) one(ch), note_fog_mesh(ch);
    }
}

void on_reg_fade(cpu::Ctx& c) {
    if (c.reg(0) != g_fade_world) g_fade_nodes.clear(), g_fade_state.clear(), g_fog_data.clear(), g_fade_world = c.reg(0);
    if (c.reg(1)) g_fade_nodes.push_back(c.reg(1));
}
void on_reg_dir_flare(cpu::Ctx& c) { note_node(c.reg(0), c.reg(2), 0); }
void on_reg_side_flare(cpu::Ctx& c) { note_node(c.reg(0), c.reg(1), 1); }
void on_reg_animate(cpu::Ctx& c) { note_node(c.reg(0), c.reg(2), 2); }

// m3g::Node: parent +0x128, rendering enabled +0x12c (byte), alpha factor +0x130 (int, 16.16)
bool node_shown(u32 node) {
    for (int i = 0; node && i < 64; i++, node = mem::r32(node + 0x128))
        if (!mem::r8(node + 0x12c)) return false;
    return true;
}
// Column-major world matrix of a scene node (m3g::Node::getTransformTo its root).
bool node_world(u32 node, float m[16]) {
    if (!fn_transform_to) return false;
    u32 root = node;
    for (int i = 0; i < 64 && mem::r32(root + 0x128); i++) root = mem::r32(root + 0x128);
    if (!g_xf) g_xf = mem::data_alloc(0x80);
    for (u32 i = 0; i < 16; i++) {
        float v = (i % 5 == 0) ? 1.0f : 0.0f;
        u32 b;
        std::memcpy(&b, &v, 4);
        mem::w32(g_xf + 8 + i * 4, b);
    }
    cpu::call(fn_transform_to, {node, root, g_xf});
    for (u32 i = 0; i < 16; i++) m[i] = f32(g_xf + 8 + i * 4);
    return std::isfinite(m[12] + m[13] + m[14]);
}

void probe_tree(u32 node, int depth) {
    std::string cls = cpu::symbolize(mem::r32(node));
    float m[16] = {};
    node_world(node, m);
    LOGI("[lights] %*s%08x %s at %.2f %.2f %.2f shown %d", depth * 2, "", node, cls.c_str(), m[12], m[13], m[14], mem::r8(node + 0x12c));
    if (cls.find("Group") != std::string::npos) {
        u32 b = mem::r32(node + 0x13c), e = mem::r32(node + 0x140);
        for (u32 q = b; q < e && q < b + 4 * 40; q += 4) probe_tree(mem::r32(q), depth + 1);
    } else if (cls.find("Mesh") != std::string::npos) {
        u32 vb = mem::r32(node + 0x160), va = vb ? mem::r32(vb + 0x44) : 0;
        LOGI("[lights] %*s  vb %08x scale %.6f bias %.3f %.3f %.3f va %08x", depth * 2, "", vb, vb ? f32(vb + 0x48) : 0, vb ? f32(vb + 0x4c) : 0, vb ? f32(vb + 0x50) : 0,
             vb ? f32(vb + 0x54) : 0, va);
        if (va)
            LOGI("[lights] %*s  va: %08x %08x %08x %08x %08x %08x %08x %08x %08x", depth * 2, "", mem::r32(va + 0x3c), mem::r32(va + 0x40), mem::r32(va + 0x44),
                 mem::r32(va + 0x48), mem::r32(va + 0x4c), mem::r32(va + 0x50), mem::r32(va + 0x54), mem::r32(va + 0x58), mem::r32(va + 0x5c));
    }
}

void trace_nodes(objs::Vec3 p) {
    static std::map<u32, std::array<float, 16>> prev;
    u32 w = g_nodes_world, qb = mem::r32(w + 0x228), qe = mem::r32(w + 0x22c);
    LOGI("[lights] %u quarantine nodes", (qe - qb) / 4);
    auto under_q = [&](u32 node) {
        for (int i = 0; node && i < 64; i++, node = mem::r32(node + 0x128))
            for (u32 q = qb; q < qe; q += 4)
                if (mem::r32(q) == node) return true;
        return false;
    };
    int shown = 0;
    for (u32 q = qb; q < qe && shown < 60; q += 4) {
        float m[16];
        u32 n = mem::r32(q);
        if (!node_world(n, m)) continue;
        float d = std::sqrt((m[12] - p.x) * (m[12] - p.x) + (m[14] - p.z) * (m[14] - p.z));
        shown++;
        LOGI("[lights] qnode %08x shown %d at %.2f %.2f %.2f (%.1f m)", n, node_shown(n), m[12], m[13], m[14], d);
        if (shown <= 2) probe_tree(n, 1);
    }
    int moving = 0;
    for (const SceneNode& n : g_nodes) {
        float m[16];
        if (!node_world(n.node, m)) continue;
        std::array<float, 16> a;
        std::memcpy(a.data(), m, sizeof m);
        auto it = prev.find(n.node);
        bool changed = it != prev.end() && std::memcmp(it->second.data(), m, sizeof m) != 0;
        prev[n.node] = a;
        if (!changed || moving++ > 10) continue;
        float d = std::sqrt((m[12] - p.x) * (m[12] - p.x) + (m[14] - p.z) * (m[14] - p.z));
        LOGI("[lights] moving node %08x kind %d underq %d shown %d at %.2f %.2f %.2f (%.1f m) x %.2f %.2f %.2f y %.2f %.2f %.2f z %.2f %.2f %.2f", n.node, n.kind,
             under_q(n.node), node_shown(n.node), m[12], m[13], m[14], d, m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
    }
}

// ---- ceiling strip lights
// The small strip lights along the corridors exist twice in a level: the normal (white) ones in the level
// mesh, and a red copy per section that the game fades in during a lockdown (GameWorld's "quarantine
// nodes", +0x228: one m3g::Group per section holding one Mesh with all of that section's light quads).
// The fixtures' positions are read from those meshes once per level; each becomes a small point light
// whose colour follows the lockdown level (GameWorld +0x28c, 0 = normal .. 1 = lockdown).
struct Fixture {
    float p[3];
    float n[3] = {0, 0, 0};  // the lamp's plane normal (either side), or zero if unknown
    float w = 1.0f;          // brightness (lamps merged into this one)
};
std::vector<Fixture> g_fixtures;
u32 g_fixtures_world = 0;

void collect_fixtures(u32 node, int depth) {
    if (!node || depth > 6) return;
    std::string cls = cpu::symbolize(mem::r32(node));
    if (cls.find("m3g::Group") != std::string::npos) {
        u32 b = mem::r32(node + 0x13c), e = mem::r32(node + 0x140);
        for (u32 q = b; q < e && q < b + 4 * 64; q += 4) collect_fixtures(mem::r32(q), depth + 1);
        return;
    }
    if (cls.find("m3g::Mesh") == std::string::npos) return;
    // Mesh +0x160 VertexBuffer: +0x44 positions (VertexArray), +0x48 scale, +0x4c bias[3]
    // VertexArray: +0x3c vertex count, +0x40 components, +0x44 type (2 = short), +0x48 data, +0x54 stride
    u32 vb = mem::r32(node + 0x160), va = vb ? mem::r32(vb + 0x44) : 0;
    if (!va) return;
    u32 count = mem::r32(va + 0x3c), comps = mem::r32(va + 0x40), type = mem::r32(va + 0x44), data = mem::r32(va + 0x48), stride = mem::r32(va + 0x54);
    if (count == 0 || count > 20000 || comps != 3 || type != 2 || !data || stride < 6 || stride > 64) return;
    float m[16];
    if (!node_world(node, m)) return;
    float scale = f32(vb + 0x48), bias[3] = {f32(vb + 0x4c), f32(vb + 0x50), f32(vb + 0x54)};
    // quads that belong to one fixture lie close together: group vertices within 0.9 m
    struct Cluster {
        float sum[3];
        int n;
        float v[8][3];
        int nv;
    };
    std::vector<Cluster> cl;
    for (u32 i = 0; i < count; i++) {
        float o[3];
        for (u32 k = 0; k < 3; k++) o[k] = float(int16_t(mem::r16(data + i * stride + k * 2))) * scale + bias[k];
        float w[3];
        for (int r = 0; r < 3; r++) w[r] = m[r] * o[0] + m[4 + r] * o[1] + m[8 + r] * o[2] + m[12 + r];
        Cluster* hit = nullptr;
        for (Cluster& c : cl) {
            float dx = c.sum[0] / float(c.n) - w[0], dy = c.sum[1] / float(c.n) - w[1], dz = c.sum[2] / float(c.n) - w[2];
            if (dx * dx + dy * dy + dz * dz < 0.9f * 0.9f) {
                hit = &c;
                break;
            }
        }
        if (!hit) {
            cl.push_back({{0, 0, 0}, 0, {}, 0});
            hit = &cl.back();
        }
        for (int r = 0; r < 3; r++) hit->sum[r] += w[r];
        if (hit->nv < 8) {
            for (int r = 0; r < 3; r++) hit->v[hit->nv][r] = w[r];
            hit->nv++;
        }
        hit->n++;
    }
    for (const Cluster& c : cl) {
        if (g_fixtures.size() >= 4000) break;
        Fixture f{{c.sum[0] / float(c.n), c.sum[1] / float(c.n), c.sum[2] / float(c.n)}};
        // the lamp's plane: the largest cross product among its first vertices
        float best = 0;
        for (int i = 1; i < c.nv; i++)
            for (int j = i + 1; j < c.nv; j++) {
                float a[3], b[3];
                for (int r = 0; r < 3; r++) a[r] = c.v[i][r] - c.v[0][r], b[r] = c.v[j][r] - c.v[0][r];
                float x[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
                float l = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
                if (l > best) best = l, f.n[0] = x[0], f.n[1] = x[1], f.n[2] = x[2];
            }
        if (best > 1e-6f)
            for (float& x : f.n) x /= best;
        g_fixtures.push_back(f);
    }
}

void update_fixtures(u32 world) {
    if (world == g_fixtures_world) return;
    g_fixtures_world = world;
    g_fixtures.clear();
    u32 qb = mem::r32(world + 0x228), qe = mem::r32(world + 0x22c);
    if (!qb || qe < qb || qe - qb > 4 * 2000) return;
    for (u32 q = qb; q < qe; q += 4) collect_fixtures(mem::r32(q), 0);
    // lamps a hand apart (the two rows of one strip) become one light: fewer, steadier lights
    size_t before = g_fixtures.size();
    for (size_t i = 0; i < g_fixtures.size(); i++)
        for (size_t j = i + 1; j < g_fixtures.size();) {
            Fixture &a = g_fixtures[i], &b = g_fixtures[j];
            float dx = a.p[0] - b.p[0], dy = a.p[1] - b.p[1], dz = a.p[2] - b.p[2];
            if (dx * dx + dy * dy + dz * dz < 0.8f * 0.8f) {
                for (int r = 0; r < 3; r++) a.p[r] = (a.p[r] * a.w + b.p[r]) / (a.w + 1.0f);
                a.w += 1.0f;
                g_fixtures.erase(g_fixtures.begin() + long(j));
            } else {
                j++;
            }
        }
    LOGI("lights: %zu lamps merged into %zu lights", before, g_fixtures.size());
    LOGI("lights: %zu strip light fixtures in %u lockdown sections", g_fixtures.size(), (qe - qb) / 4);
}

// ---- the RIG on the player's back
// The spine bar shows health: GameObjectPlayable::setDisplayedHealth picks one of three textures
// (above 50% / 25-50% / below 25% of maximum) and slides it down as health drops. The disc beside it is
// the stasis meter. Both become small lights that sit just behind the back and follow those values.
u32 g_slash_until = 0, g_slash_flash = 0;  // plasma saw: glow while the swing plays, flash when it connects
int n_nav_log = 0;
float g_stasis = -1;  // last GameObjectPlayable::setStasis value (-1: not seen yet = full)
u32 fn_health_max = 0, fn_stasis_max = 0, fn_is_tk = 0;

float call_f(u32 fn, u32 obj) {
    u32 v = cpu::call(fn, {obj});
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void on_set_stasis(cpu::Ctx& c) {
    u32 v = c.reg(1);
    std::memcpy(&g_stasis, &v, 4);
}

void on_start_slash(cpu::Ctx&) { g_slash_until = SDL_GetTicks() + 900; }
void on_do_slash(cpu::Ctx&) {
    g_slash_flash = SDL_GetTicks();
    g_slash_until = std::max(g_slash_until, g_slash_flash + 300);
}

void on_player_post_camera(cpu::Ctx& c) {
    u32 pl = c.reg(0);
    if (!pl || !fn_health_max) return;
    if (input::world()) update_fixtures(input::world());
    objs::Vec3 p = objs::pos(pl), f = objs::facing(pl);
    if (!std::isfinite(p.x + p.y + p.z + f.x + f.z)) return;
    float hmax = call_f(fn_health_max, pl), health = f32(pl + 0x25c);
    float hf = hmax > 0 ? std::clamp(health / hmax, 0.0f, 1.0f) : 0.0f;
    static const float kHigh[3] = {0.25f, 0.95f, 1.0f}, kMid[3] = {1.0f, 0.8f, 0.2f}, kLow[3] = {1.0f, 0.15f, 0.1f};
    const float* hc = hf <= 0.25f ? kLow : hf <= 0.5f ? kMid : kHigh;
    // spine: runs from the waist to between the shoulders; the lit part ends lower as health drops
    // The glow stays on the spine: two small lights along the lit part of the meter, close to the back,
    // each reaching a hand's width or two (one light a metre wide tinted the whole suit).
    const float back = 0.24f, spine_lo = 0.05f, spine_len = 0.45f;
    float hk = 0.95f;
    const float hcol[3] = {hc[0] * hk, hc[1] * hk, hc[2] * hk};
    if (hf > 0) {
        const float lit = spine_len * hf;
        push(p.x - f.x * back, p.y + spine_lo + lit * 0.28f, p.z - f.z * back, hcol, 0.36f, 150.0f, 1);
        if (hf > 0.35f) push(p.x - f.x * back, p.y + spine_lo + lit * 0.78f, p.z - f.z * back, hcol, 0.36f, 150.0f, 5);
    }

    float smax = fn_stasis_max ? call_f(fn_stasis_max, pl) : 0;
    float sf = smax > 0 ? (g_stasis < 0 ? 1.0f : std::clamp(g_stasis / smax, 0.0f, 1.0f)) : 0.0f;
    bool tk = fn_is_tk && (cpu::call(fn_is_tk, {pl}) & 0xff);
    // stasis meter: dims as it empties; while kinesis is held the module pulses brighter
    float sk = 0.08f + 0.27f * sf;
    if (tk) sk = 0.75f + 0.25f * std::sin(float(SDL_GetTicks()) * 0.012f);
    const float scol[3] = {0.3f * sk, 0.85f * sk, 1.0f * sk};
    const float side = 0.17f;  // to the player's right
    if (smax > 0) push(p.x - f.x * back - f.z * side, p.y + 0.42f, p.z - f.z * back + f.x * side, scol, tk ? 1.2f : 0.3f, 150.0f, 2);

    // helmet visor: a faint glow in front of the face
    static const float kVisor[3] = {0.2f, 0.32f, 0.5f};  // the visor slits are blue-white
    push(p.x + f.x * 0.36f, p.y + 0.72f, p.z + f.z * 0.36f, kVisor, 0.5f, 150.0f, 3);

    u32 now = SDL_GetTicks();
    // plasma saw: the light sits on the glowing blade itself, where the renderer sees it drawn, and takes
    // its colour; no blade on screen, no light
    {
        float gp[3], gc[3];
        if (now < g_slash_until + 300 && render::player_glow(gp, gc)) {
            float k = 0.85f + 0.15f * std::sin(float(now) * 0.05f);  // flicker
            if (now - g_slash_flash < 120) k += 0.6f;
            const float saw[3] = {gc[0] * k, gc[1] * k, gc[2] * k};
            push(gp[0], gp[1], gp[2], saw, 2.4f, 100.0f, 4);
        }
    }

    // navigation line: lights along the path it draws on the floor (LocatorMesh at +0x544, active flag
    // +0xc; the path is the list of PathNodes at [+0x128]+0xc, node position +0x28)
    u32 mesh = mem::r32(pl + 0x544), path = mem::r32(pl + 0x128);
    int nav = 0;
    if (mesh && path && mem::r8(mesh + 0xc)) {
        static const float kNav[3] = {0.16f, 0.5f, 0.6f};
        const float step = 1.6f;
        float prev[3] = {p.x, p.y - 1.0f, p.z}, carry = step * 0.5f;
        u32 anchor = path + 0xc;
        int guard = 0;
        for (u32 it = mem::r32(anchor); it && it != anchor && guard < 64 && nav < 10; it = mem::r32(it), guard++) {
            u32 node = mem::r32(it + 8);
            if (!node) break;
            float q[3] = {f32(node + 0x28), f32(node + 0x2c), f32(node + 0x30)};
            if (!std::isfinite(q[0] + q[1] + q[2])) break;
            if (g_trace && (n_nav_log++ % 600) == 0) LOGI("[lights] nav node %.2f %.2f %.2f (player %.2f %.2f %.2f)", q[0], q[1], q[2], p.x, p.y, p.z);
            float d[3] = {q[0] - prev[0], 0, q[2] - prev[2]};
            float len = std::sqrt(d[0] * d[0] + d[2] * d[2]);
            while (len > 1e-3f && carry <= len && nav < 10) {
                float t = carry / len;
                push(prev[0] + d[0] * t, prev[1] + (q[1] - prev[1]) * t + 0.35f, prev[2] + d[2] * t, kNav, 1.5f, 150.0f, u32(10 + nav));
                nav++;
                carry += step;
            }
            carry -= len;
            prev[0] = q[0], prev[1] = q[1], prev[2] = q[2];
        }
    }
    static int n = 0;
    if (g_trace && (n % 60) == 0 && g_nodes_world == input::world()) {
        LOGI("[lights] %zu scene nodes", g_nodes.size());
        trace_nodes(p);
    }
    if (g_trace && (n++ % 60) == 0) LOGI("[lights] nav lights %d, slash %d", nav, now < g_slash_until);
    if (g_trace && (n % 60) == 1) LOGI("[lights] rig: health %.0f/%.0f stasis %.0f/%.0f kinesis %d", health, hmax, g_stasis, smax, tk);
}

}  // namespace

void set_trace(bool on) { g_trace = on ? 1 : 0; }

void add(float x, float y, float z, float r, float g, float b, float radius, float life_ms) {
    const float c[3] = {r, g, b};
    push(x, y, z, c, radius, life_ms);
}

void eye_space(std::vector<render::Light>& out) {
    u32 now = SDL_GetTicks();
    g_src.erase(std::remove_if(g_src.begin(), g_src.end(), [&](const Src& s) { return float(now - s.born_ms) >= s.life_ms; }),
                g_src.end());
    float e[3] = {}, t[3] = {}, up[3] = {};
    bool cam = input::camera(e, t, up);
    static int n = 0;
    if (g_trace && !g_src.empty() && (n++ % 30) == 0)
        LOGI("[lights] %zu sources, camera %s eye %.2f %.2f %.2f target %.2f %.2f %.2f up %.2f %.2f %.2f; first at %.2f %.2f %.2f",
             g_src.size(), cam ? "ok" : "stale", e[0], e[1], e[2], t[0], t[1], t[2], up[0], up[1], up[2], g_src[0].p[0], g_src[0].p[1], g_src[0].p[2]);
    if (!cam) return;
    std::vector<Src> fixed;  // (the fixtures go to the renderer at the start of the frame: fixtures_world)
    if (g_src.empty() && fixed.empty()) return;
    // right-handed look-at: the camera looks down -Z
    float f[3] = {t[0] - e[0], t[1] - e[1], t[2] - e[2]};
    float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-6f) return;
    for (float& v : f) v /= fl;
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl < 1e-6f) return;
    for (float& v : s) v /= sl;
    float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    // newest first: the renderer keeps a limited number
    for (auto it = g_src.rbegin(); it != g_src.rend(); ++it) {
        float d[3] = {it->p[0] - e[0], it->p[1] - e[1], it->p[2] - e[2]};
        float k = 1.0f - float(now - it->born_ms) / it->life_ms;
        render::Light l;
        l.pos[0] = s[0] * d[0] + s[1] * d[1] + s[2] * d[2];
        l.pos[1] = u[0] * d[0] + u[1] * d[1] + u[2] * d[2];
        l.pos[2] = -(f[0] * d[0] + f[1] * d[1] + f[2] * d[2]);
        l.radius = it->radius;
        for (int i = 0; i < 3; i++) l.color[i] = it->c[i] * k;
        out.push_back(l);
    }
    // the strip lights last: they are the first to go when there are too many lights
    for (const Src& fx : fixed) {
        float d[3] = {fx.p[0] - e[0], fx.p[1] - e[1], fx.p[2] - e[2]};
        render::Light l;
        l.pos[0] = s[0] * d[0] + s[1] * d[1] + s[2] * d[2];
        l.pos[1] = u[0] * d[0] + u[1] * d[1] + u[2] * d[2];
        l.pos[2] = -(f[0] * d[0] + f[1] * d[1] + f[2] * d[2]);
        l.radius = fx.radius;
        for (int i = 0; i < 3; i++) l.color[i] = fx.c[i];
        out.push_back(l);
    }
}

std::string debug_fade_nodes(float x, float y, float z) {
    std::string out = std::to_string(g_fade_nodes.size()) + " proximity-fade nodes";
    struct E {
        float d;
        std::string s;
    };
    std::vector<E> v;
    for (u32 n : g_fade_nodes) {
        float m[16];
        if (!node_world(n, m)) continue;
        float d = std::sqrt((m[12] - x) * (m[12] - x) + (m[13] - y) * (m[13] - y) + (m[14] - z) * (m[14] - z));
        char b[160];
        std::snprintf(b, sizeof b, " | %08x %s at %.2f %.2f %.2f (%.1f m) enabled %d alpha %08x shown %d", n, cpu::symbolize(mem::r32(n)).c_str(), m[12], m[13], m[14], d,
                      mem::r8(n + 0x12c), mem::r32(n + 0x130), node_shown(n));
        std::string line = b;
        if (cpu::symbolize(mem::r32(n)).find("Group") != std::string::npos) {
            u32 cb = mem::r32(n + 0x13c), ce = mem::r32(n + 0x140);
            for (u32 q = cb; q < ce && q < cb + 24; q += 4) {
                u32 ch = mem::r32(q);
                char cbuf[96];
                std::snprintf(cbuf, sizeof cbuf, " [child %08x %s en %d a %08x]", ch, cpu::symbolize(mem::r32(ch)).substr(11, 14).c_str(), mem::r8(ch + 0x12c), mem::r32(ch + 0x130));
                line += cbuf;
            }
        }
        v.push_back({d, line});
    }
    std::sort(v.begin(), v.end(), [](const E& a, const E& b) { return a.d < b.d; });
    for (size_t i = 0; i < v.size() && i < 6; i++) out += v[i].s;
    return out;
}

int debug_show_nodes(float x, float z, float radius) {
    int n = 0;
    for (const SceneNode& sn : g_nodes) {
        float m[16];
        if (!node_world(sn.node, m)) continue;
        if (std::fabs(m[12] - x) > radius || std::fabs(m[14] - z) > radius) continue;
        u32 node = sn.node;
        for (int i = 0; node && i < 64; i++, node = mem::r32(node + 0x128))
            if (!mem::r8(node + 0x12c)) {
                LOGI("[lights] showing node %08x (level %d above %08x, kind %d)", node, i, sn.node, sn.kind);
                mem::w8(node + 0x12c, 1);
                n++;
            }
    }
    return n;
}

float lockdown_level() {
    u32 world = input::world();
    if (!world) return 0;
    float level = f32(world + 0x28c);
    return std::isfinite(level) ? std::clamp(level, 0.0f, 1.0f) : 0.0f;
}

void fixtures_world(const float at[3], std::vector<render::Light>& out) {
    u32 world = input::world();
    if (!world || world != g_fixtures_world) return;
    float level = lockdown_level();
    static const float kNormal[3] = {0.34f, 0.38f, 0.42f}, kLockdown[3] = {0.95f, 0.10f, 0.07f};
    const float kRange = 34.0f;  // the camera's far plane is at 30 m
    size_t first = out.size();
    for (const Fixture& f : g_fixtures) {
        float dx = f.p[0] - at[0], dy = f.p[1] - at[1], dz = f.p[2] - at[2], d2 = dx * dx + dy * dy + dz * dz;
        if (d2 >= kRange * kRange) continue;
        float k = std::clamp((kRange - std::sqrt(d2)) / 2.0f, 0.0f, 1.0f) * std::min(1.6f, 0.75f + 0.25f * f.w);
        render::Light l;
        for (int i = 0; i < 3; i++) {
            l.wpos[i] = f.p[i];
            l.wdir[i] = f.n[i];
            l.color[i] = (kNormal[i] + (kLockdown[i] - kNormal[i]) * level) * k;
        }
        l.radius = 3.2f;
        out.push_back(l);
    }
    std::sort(out.begin() + long(first), out.end(), [&](const render::Light& a, const render::Light& b) {
        auto d = [&](const render::Light& l) {
            return (l.wpos[0] - at[0]) * (l.wpos[0] - at[0]) + (l.wpos[1] - at[1]) * (l.wpos[1] - at[1]) + (l.wpos[2] - at[2]) * (l.wpos[2] - at[2]);
        };
        return d(a) < d(b);
    });
}

void debug_list(std::vector<DebugLight>& out) {
    u32 now = SDL_GetTicks();
    for (const Src& s : g_src) {
        if (float(now - s.born_ms) >= s.life_ms) continue;
        DebugLight d;
        for (int i = 0; i < 3; i++) d.pos[i] = s.p[i], d.color[i] = s.c[i];
        d.radius = s.radius;
        d.kind = s.id == 1 || s.id == 5 ? "RIG HEALTH" : s.id == 2 ? "STASIS MODULE" : s.id == 3 ? "VISOR" : s.id == 4 ? "SAW" : s.id >= 10 && s.id < 20 ? "NAV LINE" : s.id ? "PROJECTILE" : "FLASH";
        out.push_back(d);
    }
    // lit this frame by the renderer: alarm spotlights and the nearest strip lights
    const std::vector<render::Light>& lit = render::debug_frame_lights();
    for (const render::Light& l : lit) {
        DebugLight d;
        for (int i = 0; i < 3; i++) d.pos[i] = l.wpos[i], d.color[i] = l.color[i], d.dir[i] = l.wdir[i];
        d.radius = l.radius;
        d.spot = l.cos_outer > 0;
        d.cos_outer = l.cos_outer;
        d.tile = l.tile;
        d.kind = d.spot ? (l.gobo > 0 ? "FAN LIGHT" : "ALARM SPOT") : l.glow ? "PANEL GLOW" : "STRIP LIGHT";
        out.push_back(d);
    }
    // every other strip light fixture of the level (not lit: too far from the camera, or over the limit)
    u32 world = input::world();
    if (world && world == g_fixtures_world) {
        float level = std::clamp(f32(world + 0x28c), 0.0f, 1.0f);
        for (const Fixture& f : g_fixtures) {
            bool is_lit = false;
            for (const render::Light& l : lit)
                if (l.cos_outer <= 0 && !l.glow && std::fabs(l.wpos[0] - f.p[0]) < 0.01f && std::fabs(l.wpos[1] - f.p[1]) < 0.01f && std::fabs(l.wpos[2] - f.p[2]) < 0.01f) is_lit = true;
            if (is_lit) continue;
            DebugLight d;
            for (int i = 0; i < 3; i++) d.pos[i] = f.p[i];
            d.color[0] = 0.34f + 0.61f * level, d.color[1] = 0.38f - 0.28f * level, d.color[2] = 0.42f - 0.35f * level;
            d.radius = 3.2f;
            d.active = false;
            d.kind = "STRIP LIGHT";
            out.push_back(d);
        }
    }
}

bool camera_matrix(float view[16], float target[3]) {
    float e[3], t[3], up[3];
    if (!input::camera(e, t, up)) return false;
    float f[3] = {t[0] - e[0], t[1] - e[1], t[2] - e[2]};
    float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-6f) return false;
    for (float& v : f) v /= fl;
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl < 1e-6f) return false;
    for (float& v : s) v /= sl;
    float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    for (int c = 0; c < 3; c++) {
        view[c * 4] = s[c], view[c * 4 + 1] = u[c], view[c * 4 + 2] = -f[c], view[c * 4 + 3] = 0;
    }
    view[12] = -(s[0] * e[0] + s[1] * e[1] + s[2] * e[2]);
    view[13] = -(u[0] * e[0] + u[1] * e[1] + u[2] * e[2]);
    view[14] = f[0] * e[0] + f[1] * e[1] + f[2] * e[2];
    view[15] = 1;
    std::memcpy(target, t, 12);
    return true;
}

void install() {
    fn_transform_to = elf::find("_ZNK3m3g4Node14getTransformToEPKS0_PNS_9TransformE");
    fn_health_max = elf::find("_ZNK18GameObjectPlayable12getHealthMaxEv");
    fn_stasis_max = elf::find("_ZNK18GameObjectPlayable12getStasisMaxEv");
    fn_is_tk = elf::find("_ZNK18GameObjectPlayable16isTelekinesisingEv");
    const struct {
        const char* sym;
        const char* name;
        cpu::HleFn fn;
    } hooks[] = {
        {"_ZN18WeaponPlasmaCutter6onFireEv", "WeaponPlasmaCutter::onFire", on_fire},
        {"_ZN12WeaponRipper6onFireEv", "WeaponRipper::onFire", on_fire},
        {"_ZN13WeaponLineGun6onFireEv", "WeaponLineGun::onFire", on_fire},
        {"_ZN11WeaponCorer6onFireEv", "WeaponCorer::onFire", on_fire},
        {"_ZN12WeaponStasis6onFireEv", "WeaponStasis::onFire", on_fire},
        {"_ZN6Weapon13resolveImpactEPvRK9btVector3S3_S3_ii", "Weapon::resolveImpact", on_impact},
        {"_ZN20GameObjectProjectile8onUpdateEi", "GameObjectProjectile::onUpdate", on_projectile},
        {"_ZN19GameObjectExplosive13reallyExplodeEv", "GameObjectExplosive::reallyExplode", on_explode},
        {"_ZN9GameWorld26registerNodeDirectionFlareEPK8CullNodePN3m3g4NodeEa", "GameWorld::registerNodeDirectionFlare", on_reg_dir_flare},
        {"_ZN9GameWorld21registerProximityFadeEPN3m3g4NodeE", "GameWorld::registerProximityFade", on_reg_fade},
        {"_ZN9GameWorld6renderERKN5eastl13intrusive_ptrIN3m3g8RendererEEE", "GameWorld::render", smooth_fades},
        {"_ZN9GameWorld21registerNodeSideFlareEPN3m3g4NodeE", "GameWorld::registerNodeSideFlare", on_reg_side_flare},
        {"_ZN9GameWorld19registerNodeAnimateEPK8CullNodePN3m3g4NodeE", "GameWorld::registerNodeAnimate", on_reg_animate},
        {"_ZN18GameObjectPlayable10startSlashEv", "GameObjectPlayable::startSlash", on_start_slash},
        {"_ZN18GameObjectPlayable7doSlashEv", "GameObjectPlayable::doSlash", on_do_slash},
        {"_ZN18GameObjectPlayable9setStasisEf", "GameObjectPlayable::setStasis", on_set_stasis},
        {"_ZN18GameObjectPlayable18onUpdatePostCameraEi", "GameObjectPlayable::onUpdatePostCamera", on_player_post_camera},
    };
    for (auto& h : hooks) {
        u32 a = elf::find(h.sym);
        if (!a || !cpu::hook_entry(a, h.name, h.fn)) LOGW("lights: cannot hook %s", h.name);
    }
}

bool is_fog_vertices(u32 guest_addr) { return std::find(g_fog_data.begin(), g_fog_data.end(), guest_addr) != g_fog_data.end(); }

}  // namespace lights
