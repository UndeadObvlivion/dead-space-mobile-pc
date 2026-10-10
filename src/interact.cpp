#include "interact.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "cpu.h"
#include "devmode.h"
#include "elf_loader.h"
#include "guest_util.h"
#include "hle/gles.h"
#include "log.h"
#include "objects.h"

namespace interact {

namespace {

using objs::Box;
using objs::Vec3;

// ---- the player's hitbox, in the view frame (x right, y up, z forward; origin at the player
// position, which is about 1 m above the floor)
constexpr float kBack = 0.3f;     // starts a little behind the player's centre
constexpr float kReach = 1.8f;    // ... and reaches this far forward
constexpr float kHalfWidth = 0.7f;
constexpr float kBottom = -1.05f;  // floor
constexpr float kTop = 1.3f;       // above the head

// ---- object zones: bounding box grown to at least kMinSize on each axis, plus kPad all round
constexpr float kMinSize = 0.5f;
constexpr float kPad = 0.1f;

// ---- kinesis (when nothing is in reach): cone along the line of sight
constexpr float kKinesisRange = 9.0f;
constexpr float kKinesisCos = 0.97f;  // ~14 degrees

struct ClassRule {
    const char* name;  // class name (exact)
    bool skip;         // never used with E
    float pad;         // extra padding for this class
};
// Classes whose zone needs adjusting. Shoot switches are triggered by shooting them, not by E.
const ClassRule kRules[] = {
    {"GameObjectShootSwitch", true, 0},  // (covered ones are opened with E, then cut / shot)
    {"GameObjectPickup", false, 0.15f},
};
// Objects without a physics body (control panels, stores, power node locks, O2 stations, some lift
// stops...) report no bounds. Their zone is built around their touch-region markers (where the
// game puts the tap icon, e.g. a console's screen): kNoBodyWidth square, from kNoBodyBelow under
// the marker to kNoBodyAbove over it. Without markers, around the object's position (which is at
// the floor) up to kNoBodyHeight.
constexpr float kNoBodyWidth = 1.0f, kNoBodyBelow = 1.2f, kNoBodyAbove = 0.4f, kNoBodyHeight = 2.0f;
const ClassRule* rule_for(const std::string& cls) {
    for (auto& r : kRules)
        if (cls == r.name) return &r;
    return nullptr;
}

struct View {
    Vec3 p, f, r;  // position, forward, right (horizontal unit vectors)
};

View view_of(u32 player, float look_yaw) {
    View v;
    v.p = objs::pos(player);
    Vec3 f = objs::facing(player);
    // the free look orbit rotates the camera about the player: x' = x cos + z sin, z' = -x sin + z cos
    float c = std::cos(look_yaw), s = std::sin(look_yaw);
    Vec3 g{f.x * c + f.z * s, 0, -f.x * s + f.z * c};
    float len = std::sqrt(g.x * g.x + g.z * g.z);
    if (len < 1e-4f) g = {0, 0, 1}, len = 1;
    v.f = {g.x / len, 0, g.z / len};
    v.r = {v.f.z, 0, -v.f.x};
    return v;
}

// world point -> view frame
Vec3 local(const View& v, Vec3 w) {
    Vec3 d{w.x - v.p.x, w.y - v.p.y, w.z - v.p.z};
    return {d.x * v.r.x + d.z * v.r.z, d.y, d.x * v.f.x + d.z * v.f.z};
}

Box zone_of(u32 obj, const ClassRule* rule) {
    Box b = objs::bounds(obj);
    bool empty = !(b.hi.x > b.lo.x || b.hi.y > b.lo.y || b.hi.z > b.lo.z);
    if (empty) {
        float h = kNoBodyWidth * 0.5f;
        auto pts = objs::region_points(obj);
        if (pts.empty()) {
            Vec3 p = objs::pos(obj);
            b = {{p.x - h, p.y, p.z - h}, {p.x + h, p.y + kNoBodyHeight, p.z + h}};
        } else {
            b = {{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
            for (auto& p : pts) {
                b.lo = {std::min(b.lo.x, p.x - h), std::min(b.lo.y, p.y - kNoBodyBelow), std::min(b.lo.z, p.z - h)};
                b.hi = {std::max(b.hi.x, p.x + h), std::max(b.hi.y, p.y + kNoBodyAbove), std::max(b.hi.z, p.z + h)};
            }
        }
    }
    float pad = kPad + (rule ? rule->pad : 0);
    float* lo = &b.lo.x;
    float* hi = &b.hi.x;
    for (int k = 0; k < 3; k++) {
        float c = (lo[k] + hi[k]) * 0.5f, h = std::max((hi[k] - lo[k]) * 0.5f, kMinSize * 0.5f) + pad;
        lo[k] = c - h;
        hi[k] = c + h;
    }
    return b;
}

// zone corners -> bounds in the view frame
Box local_box(const View& v, const Box& z) {
    Box out{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
    for (int i = 0; i < 8; i++) {
        Vec3 w{i & 1 ? z.hi.x : z.lo.x, i & 2 ? z.hi.y : z.lo.y, i & 4 ? z.hi.z : z.lo.z};
        Vec3 l = local(v, w);
        out.lo = {std::min(out.lo.x, l.x), std::min(out.lo.y, l.y), std::min(out.lo.z, l.z)};
        out.hi = {std::max(out.hi.x, l.x), std::max(out.hi.y, l.y), std::max(out.hi.z, l.z)};
    }
    return out;
}

struct Cand {
    u32 obj;
    std::string cls;
    Box zone, lz;
    bool usable, kinesis, overlap;
    float score;
};

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

void emit(bool report, const std::string& s) {
    if (!report) return;
    LOGI("[interact] %s", s.c_str());
    if (dev::active()) dev::out(s);
}

u32 g_evbuf = 0, g_vt_action = 0;
bool send_use(u32 obj) {
    if (!g_evbuf) {
        g_evbuf = mem::heap_alloc(32, 8);
        g_vt_action = elf::find("_ZTV11ActionEvent") + 8;
    }
    // ActionEvent(1008, 16): what tapping the object's icon sends (InputForwarderTapToAction)
    std::memset(mem::host(g_evbuf), 0, 32);
    mem::w32(g_evbuf, g_vt_action);
    mem::w32(g_evbuf + 4, 1008);
    mem::w32(g_evbuf + 8, 16);
    return cpu::call(mem::r32(mem::r32(obj) + 8), {obj, g_evbuf}) & 0xff;
}

std::vector<Cand> gather(const View& v, u32 world, bool report) {
    std::vector<Cand> out;
    for (u32 o : objs::world_objects(world)) {
        if (!objs::is_interactive_class(o)) continue;
        Vec3 p = objs::pos(o);
        float dx = p.x - v.p.x, dy = p.y - v.p.y, dz = p.z - v.p.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > (kKinesisRange + 4) * (kKinesisRange + 4)) continue;
        Cand c{o, objs::class_name(o), {}, {}, false, false, false, 0};
        const ClassRule* rule = rule_for(c.cls);
        u32 fl = objs::flags(o);
        bool hidden = fl & 8;
        c.usable = !hidden && !(rule && rule->skip) && (objs::interactive(o) || objs::interactive_plus(o));
        c.kinesis = !hidden && objs::telekinesisable(o);
        if (!c.usable && !c.kinesis && !report) continue;
        c.zone = zone_of(o, rule);
        if (!(c.zone.hi.x >= c.zone.lo.x && c.zone.hi.y >= c.zone.lo.y && c.zone.hi.z >= c.zone.lo.z) ||
            std::fabs(c.zone.lo.x) > 1e6f || std::fabs(c.zone.lo.y) > 1e6f || std::fabs(c.zone.lo.z) > 1e6f)
            continue;
        c.lz = local_box(v, c.zone);
        c.overlap = c.lz.hi.x > -kHalfWidth && c.lz.lo.x < kHalfWidth && c.lz.hi.y > kBottom && c.lz.lo.y < kTop &&
                    c.lz.hi.z > -kBack && c.lz.lo.z < kReach;
        // nearest the line of sight first: sideways gap to the forward line, then distance
        float gap = c.lz.lo.x > 0 ? c.lz.lo.x : c.lz.hi.x < 0 ? -c.lz.hi.x : 0;
        float near_z = std::max(c.lz.lo.z, 0.0f);
        c.score = gap * 2.0f + near_z * 0.5f;
        out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const Cand& a, const Cand& b) { return a.score < b.score; });
    return out;
}

const Cand* pick(const std::vector<Cand>& cands, const View& v, bool& kinesis) {
    kinesis = false;
    for (auto& c : cands)
        if (c.usable && c.overlap) return &c;
    // kinesis: zone centre inside the cone along the line of sight
    const Cand* best = nullptr;
    float best_cos = kKinesisCos;
    for (auto& c : cands) {
        if (!c.kinesis) continue;
        Vec3 m{(c.lz.lo.x + c.lz.hi.x) * 0.5f, (c.lz.lo.y + c.lz.hi.y) * 0.5f - 0.5f, (c.lz.lo.z + c.lz.hi.z) * 0.5f};
        float len = std::sqrt(m.x * m.x + m.y * m.y + m.z * m.z);
        if (len < 0.1f || len > kKinesisRange || m.z <= 0) continue;
        // horizontal angle only (the camera pitch is not part of the view); vertical within 3 m
        float hl = std::sqrt(m.x * m.x + m.z * m.z), cs = m.z / hl;
        if (cs > best_cos && std::fabs(m.y) < 3.0f) {
            best_cos = cs;
            best = &c;
        }
    }
    (void)v;
    kinesis = best != nullptr;
    return best;
}

void report_all(const std::vector<Cand>& cands, const View& v) {
    emit(true, fmt("player %.2f %.2f %.2f looking %.2f %.2f; hitbox %.1f..%.1f m ahead, %.1f m wide", v.p.x, v.p.y,
                   v.p.z, v.f.x, v.f.z, -kBack, kReach, 2 * kHalfWidth));
    int n = 0;
    for (auto& c : cands) {
        if (n++ >= 25) break;
        emit(true, fmt("  %08x %-26s usable=%d kinesis=%d in-hitbox=%d score=%.2f local x %.2f..%.2f y %.2f..%.2f z "
                       "%.2f..%.2f",
                       c.obj, c.cls.c_str(), c.usable, c.kinesis, c.overlap, c.score, c.lz.lo.x, c.lz.hi.x, c.lz.lo.y,
                       c.lz.hi.y, c.lz.lo.z, c.lz.hi.z));
    }
}

}  // namespace

bool use(u32 player, u32 world, float look_yaw, bool report) {
    if (!player || !world) return false;
    View v = view_of(player, look_yaw);
    auto cands = gather(v, world, report);
    if (report) report_all(cands, v);
    bool kin;
    const Cand* c = pick(cands, v, kin);
    if (!c) {
        emit(report, "E: nothing in front of the player");
        return false;
    }
    bool ok = send_use(c->obj);
    emit(report, fmt("E: %s %s %08x -> %s", kin ? "kinesis" : "use", c->cls.c_str(), c->obj, ok ? "accepted" : "refused"));
    return ok;
}

namespace {
// world-space box edges, projected and queued as overlay lines
void draw_box(u32 world, const Vec3 c[8], unsigned rgba) {
    static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                      {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    float sx[8], sy[8];
    bool ok[8];
    for (int i = 0; i < 8; i++) ok[i] = objs::to_screen(world, c[i], sx[i], sy[i]);
    for (auto& e : kEdges)
        if (ok[e[0]] && ok[e[1]]) hle::overlay_line(sx[e[0]], sy[e[0]], sx[e[1]], sy[e[1]], rgba);
}
void box_corners(const Box& b, Vec3 out[8]) {
    for (int i = 0; i < 8; i++) out[i] = {i & 1 ? b.hi.x : b.lo.x, i & 2 ? b.hi.y : b.lo.y, i & 4 ? b.hi.z : b.lo.z};
}
}  // namespace

void draw_debug(u32 player, u32 world, float look_yaw) {
    if (!player || !world) return;
    View v = view_of(player, look_yaw);
    auto cands = gather(v, world, false);
    bool kin;
    const Cand* best = pick(cands, v, kin);
    for (auto& c : cands) {
        if (!c.usable && !c.kinesis) continue;
        float dx = (c.zone.lo.x + c.zone.hi.x) * 0.5f - v.p.x, dz = (c.zone.lo.z + c.zone.hi.z) * 0.5f - v.p.z;
        if (dx * dx + dz * dz > 100) continue;
        Vec3 corners[8];
        box_corners(c.zone, corners);
        unsigned col = &c == best ? 0x30ff30ffu : c.usable ? 0xffffffb0u : 0xffc030b0u;
        draw_box(world, corners, col);
    }
    // hitbox (view frame -> world)
    Vec3 corners[8];
    for (int i = 0; i < 8; i++) {
        float x = i & 1 ? kHalfWidth : -kHalfWidth, y = i & 2 ? kTop : kBottom, z = i & 4 ? kReach : -kBack;
        corners[i] = {v.p.x + v.r.x * x + v.f.x * z, v.p.y + y, v.p.z + v.r.z * x + v.f.z * z};
    }
    draw_box(world, corners, 0x30e0ffd0u);
}

std::string pick_name(u32 player, u32 world, float look_yaw) {
    if (!player || !world) return "";
    View v = view_of(player, look_yaw);
    auto cands = gather(v, world, false);
    bool kin;
    const Cand* c = pick(cands, v, kin);
    return c ? fmt("%s %08x", c->cls.c_str(), c->obj) : "";
}

Approach approach_point(u32 obj, float dist) {
    Box z = zone_of(obj, rule_for(objs::class_name(obj)));
    Vec3 c{(z.lo.x + z.hi.x) * 0.5f, (z.lo.y + z.hi.y) * 0.5f, (z.lo.z + z.hi.z) * 0.5f};
    Vec3 f = objs::facing(obj);
    // distance from the zone centre to its edge along the facing
    float ext = std::fabs(f.x) * (z.hi.x - z.lo.x) * 0.5f + std::fabs(f.z) * (z.hi.z - z.lo.z) * 0.5f;
    float d = ext + dist;
    // stand on the floor under the zone: player position is ~1 m above the floor
    float floor_y = z.lo.y + kPad;
    return {{c.x + f.x * d, floor_y + 1.0f, c.z + f.z * d}, c};
}

void repair_objective_panel(u32 world) {
    static u32 fn_target = elf::find("_ZNK9GameWorld18getObjectiveTargetEv"),
               fn_state = elf::find("_ZN20GameObjectSwitchable15stateTransitionENS_5StateE");
    static u32 stuck = 0;
    static int count = 0;
    if (!world || !fn_target || !fn_state) return;
    u32 t = cpu::call(fn_target, {world});
    // +0x174 = usable. A panel restored from a checkpoint as "switched" (initPostCheckpoint ->
    // setOnState) is unusable while its switch state +0x1e4 may still read 0.
    bool used_panel = t && objs::class_name(t) == "GameObjectControlPanel" && mem::r8(t + 0x174) == 0;
    if (!used_panel) {
        stuck = 0;
        count = 0;
        return;
    }
    if (t != stuck) stuck = t, count = 0;
    if (++count < 6) return;  // ~3 s
    Vec3 p = objs::pos(t);
    LOGW("objective points at control panel %08x (%.1f %.1f %.1f), which the save marks as already used; "
         "switching it back on",
         t, p.x, p.y, p.z);
    cpu::call(fn_state, {t, 0});
    stuck = 0;
    count = 0;
}

void report_encounters(u32 player, u32 world) {
    // Encounter scripts (lockdowns, waves) end when every spawn zone that belongs to them is done
    // (Script::checkForActiveSpawns): zone state +0x174 = 0 idle, 1 waiting to spawn, 2 waiting for
    // its spawned enemy (+0x18c) to die, 3 waiting for every enemy of its type to be gone.
    static u32 fn_member =
        elf::find("_ZN10GameObject14isScriptMemberERKN5eastl12basic_stringIwN2im20StringEASTLAllocatorEEE");
    if (!fn_member) return;
    auto all = objs::world_objects(world);
    Vec3 p = objs::pos(player);
    for (u32 sc : all) {
        std::string cls = objs::class_name(sc);
        if (cls.rfind("Script", 0) != 0) continue;
        u32 info = mem::r32(sc + 0x2c), name = info >= 0x10000 ? mem::r32(info + 0xc4) : 0;
        if (name < 0x10000) continue;
        std::string n;
        for (u32 q = mem::r32(name), e = mem::r32(name + 4); q && q < e && n.size() < 40; q += 2) n += char(mem::r16(q));
        std::vector<std::string> zones;
        for (u32 o : all) {
            if (!(guest::vcall(o, 0x40) & 0xff) || !(cpu::call(fn_member, {o, name}) & 0xff)) continue;
            s32 st = s32(mem::r32(o + 0x174));
            if (st == 0) continue;
            Vec3 q = objs::pos(o);
            std::string z = fmt("    zone %08x state=%d at %.1f %.1f %.1f (%.1f m away)", o, st, q.x, q.y, q.z,
                                std::sqrt((q.x - p.x) * (q.x - p.x) + (q.z - p.z) * (q.z - p.z)));
            if (u32 e = mem::r32(o + 0x18c); e >= 0x10000 && mem::r32(e) >= 0x10000) {
                Vec3 ep = objs::pos(e);
                z += fmt("; spawned %s %08x alive=%d flags=%x at %.1f %.1f %.1f", objs::class_name(e).c_str(), e,
                         guest::vcall(e, 0x110) & 0xff, objs::flags(e), ep.x, ep.y, ep.z);
            }
            zones.push_back(z);
        }
        if (zones.empty()) continue;
        emit(true, fmt("encounter %s (%s) state=%d still has active spawn zones:", n.c_str(), cls.c_str(),
                       s32(mem::r32(sc + 0x1a4))));
        for (auto& z : zones) emit(true, z);
    }
}

std::string preview(u32 player, u32 world, float look_yaw) {
    if (!player || !world) return "no player";
    report_encounters(player, world);
    View v = view_of(player, look_yaw);
    auto cands = gather(v, world, true);
    report_all(cands, v);
    bool kin;
    const Cand* c = pick(cands, v, kin);
    return c ? fmt("E would %s %s %08x", kin ? "kinesis" : "use", c->cls.c_str(), c->obj) : "E would do nothing";
}

}  // namespace interact
