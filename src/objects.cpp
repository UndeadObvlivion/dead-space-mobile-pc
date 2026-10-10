#include "objects.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "cpu.h"
#include "elf_loader.h"
#include "guest_util.h"

namespace objs {

namespace {
u32 scratch(int i) {  // small guest buffers for sret results
    static u32 buf[4] = {};
    if (!buf[i]) buf[i] = mem::heap_alloc(96, 16);
    return buf[i];
}
}  // namespace

float rf(u32 a) {
    float f;
    u32 v = mem::r32(a);
    std::memcpy(&f, &v, 4);
    return f;
}
void wf(u32 a, float f) {
    u32 v;
    std::memcpy(&v, &f, 4);
    mem::w32(a, v);
}
static u32 fbits(float f) {
    u32 v;
    std::memcpy(&v, &f, 4);
    return v;
}

std::string class_name(u32 obj) {
    std::string s = cpu::symbolize(mem::r32(obj));
    const char* pre = "vtable for ";
    if (s.rfind(pre, 0) == 0) s = s.substr(std::strlen(pre));
    if (auto p = s.find('+'); p != std::string::npos) s.resize(p);
    return s;
}

// The base GameObject::getPos / getBounds read the physics body (+0x58), which some objects
// (triggers, markers...) do not have.
static bool no_body(u32 obj, u32 slot, const char* base_sym) {
    static u32 base_pos = elf::find("_ZNK10GameObject6getPosEv"), base_bounds = elf::find("_ZNK10GameObject9getBoundsEv");
    u32 base = std::strcmp(base_sym, "pos") == 0 ? base_pos : base_bounds;
    if (mem::r32(mem::r32(obj) + slot) != base) return false;
    u32 body = mem::r32(obj + 0x58);
    if (!body) return true;
    // getBounds also needs the body's collision shape (+0xc0), unless flag 0x100 skips it
    return base == base_bounds && !(mem::r32(obj + 0x48) & 0x100) && !mem::r32(body + 0xc0);
}

Vec3 pos(u32 obj) {
    if (no_body(obj, 0xc4, "pos")) return {};
    u32 p = guest::vcall(obj, 0xc4);
    return {rf(p), rf(p + 4), rf(p + 8)};
}

Box bounds(u32 obj) {
    if (no_body(obj, 0xe8, "bounds")) return {};
    u32 b = scratch(0);
    std::memset(mem::host(b), 0, 96);
    cpu::call(mem::r32(mem::r32(obj) + 0xe8), {b, obj});  // AABB getBounds() const (sret)
    return {{rf(b + 0x10), rf(b + 0x14), rf(b + 0x18)}, {rf(b + 0x20), rf(b + 0x24), rf(b + 0x28)}};
}

bool empty(const Box& b) { return !(b.hi.x > b.lo.x || b.hi.y > b.lo.y || b.hi.z > b.lo.z); }

std::vector<Vec3> region_points(u32 obj) {
    // Model::getNodeTransform(out btTransform, node, relative-to node): origin at +0x30
    static u32 fn = elf::find("_ZN5Model16getNodeTransformERKN3m3g4NodeES3_");
    std::vector<Vec3> v;
    u32 b = mem::r32(obj + 0x190), e = mem::r32(obj + 0x194);
    if (!fn || e < b || e - b > 64 * 4) return v;
    for (u32 p = b; p < e; p += 4) {
        u32 node = mem::r32(p);
        u32 root = node ? mem::r32(node + 0x128) : 0;  // scene root (what AABB(Node*) measures against)
        if (!root) continue;
        u32 t = scratch(3);
        cpu::call(fn, {t, node, root});
        v.push_back({rf(t + 0x30), rf(t + 0x34), rf(t + 0x38)});
    }
    if (v.empty()) return v;
    // model space -> world: local z is the object's facing, x its right, y up
    Vec3 o = pos(obj), f = facing(obj), r{f.z, 0, -f.x};
    for (auto& q : v) q = {o.x + r.x * q.x + f.x * q.z, o.y + q.y, o.z + r.z * q.x + f.z * q.z};
    return v;
}

Vec3 facing(u32 obj) {
    static u32 fn = elf::find("_ZNK10GameObject9getFacingEv");
    u32 b = scratch(1);
    cpu::call(fn, {b, obj});
    return {rf(b), rf(b + 4), rf(b + 8)};
}

bool is_interactive_class(u32 obj) {
    static std::map<u32, bool> cache;
    u32 vt = mem::r32(obj);
    auto it = cache.find(vt);
    if (it != cache.end()) return it->second;
    // slot 0x170 is onInteract() in GameObjectInteractive and its subclasses
    std::string n = cpu::symbolize(mem::r32(vt + 0x170));
    bool r = n.find("onInteract") != std::string::npos;
    cache[vt] = r;
    return r;
}
bool interactive(u32 obj) { return guest::vcall(obj, 0x48) & 0xff; }
bool interactive_plus(u32 obj) { return guest::vcall(obj, 0x4c) & 0xff; }
bool telekinesisable(u32 obj) { return guest::vcall(obj, 0x5c) & 0xff; }
bool use_enabled(u32 obj) { return mem::r8(obj + 0x174) != 0; }
float reach(u32 obj) { return rf(obj + 0x178); }
u32 flags(u32 obj) { return mem::r32(obj + 0x48); }
bool on_interact(u32 obj) { return guest::vcall(obj, 0x170) & 0xff; }

std::vector<u32> world_objects(u32 world) {
    std::vector<u32> v;
    if (!world) return v;
    // vector<shared_ptr<GameObject>> of every object (GameWorld::getObjectIndex; 12-byte elements)
    u32 b = mem::r32(world + 0x58), e = mem::r32(world + 0x5c);
    if (e < b || e - b > 1200000) return v;
    for (u32 p = b; p < e; p += 12)
        if (u32 obj = mem::r32(p)) v.push_back(obj);
    return v;
}

bool to_screen(u32 world, Vec3 p, float& sx, float& sy) {
    static u32 fn = elf::find("_ZNK16CameraController18worldToScreenSpaceERK9btVector3");
    u32 cam = world ? mem::r32(world + 0x28) : 0;
    if (!fn || !cam) return false;
    u32 in = scratch(2), out = scratch(1);
    wf(in, p.x);
    wf(in + 4, p.y);
    wf(in + 8, p.z);
    wf(in + 12, 0);
    cpu::call(fn, {out, cam, in});
    sx = rf(out);
    sy = rf(out + 4);
    return rf(out + 8) > 0;
}

int enemy_type(const std::string& n) {
    static const std::pair<const char*, int> kTypes[] = {{"boss", 24},     {"brute", 25},    {"exploder", 27},
                                                         {"lurker", 29},   {"pregnant", 31}, {"swarm", 33},
                                                         {"slasher", 34}};
    for (auto& [name, t] : kTypes)
        if (n == name) return t;
    return 0;
}

u32 spawn(u32 world, int type, Vec3 feet, Vec3 face) {
    static u32 fn = elf::find("_ZN9GameWorld12createObjectEN10GameObject4TypeERK11btTransform");
    static u32 xf = mem::heap_alloc(64, 16), sp = mem::heap_alloc(16, 8);
    if (!fn || !world) return 0;
    float len = std::sqrt(face.x * face.x + face.z * face.z);
    Vec3 f = len > 1e-4f ? Vec3{face.x / len, 0, face.z / len} : Vec3{0, 0, 1};
    // btTransform: basis rows (x, y, z axes as columns), origin at +0x30; local z = facing
    float m[16] = {f.z, 0, f.x, 0, 0, 1, 0, 0, -f.x, 0, f.z, 0, feet.x, feet.y, feet.z, 0};
    std::memcpy(mem::host(xf), m, sizeof m);
    std::memset(mem::host(sp), 0, 16);
    cpu::call(fn, {sp, world, u32(type), xf});
    u32 obj = mem::r32(sp);
    if (obj) guest::vcall(obj, 0x70, {0, 0});  // initPostSpawn(no spawn zone, plain spawn)
    return obj;
}

void teleport(u32 player, Vec3 p) {
    static u32 fn = elf::find("_ZN18GameObjectPlayable8teleportERK9btVector3");
    u32 b = scratch(2);
    wf(b, p.x);
    wf(b + 4, p.y);
    wf(b + 8, p.z);
    wf(b + 12, 0);
    cpu::call(fn, {player, b});
}

void turn_by(u32 player, float radians) {
    static u32 fn = elf::find("_ZN10GameObject9adjustYawEf");
    cpu::call(fn, {player, fbits(radians)});
}

float yaw_of(Vec3 d) { return std::atan2(d.x, d.z); }

}  // namespace objs
