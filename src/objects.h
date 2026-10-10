// Reading game objects (GameObject and subclasses) from the host. Game thread only.
//
// Positions and boxes are in the game's physics/render space (the space GameObject::getPos,
// teleport and the portal culling use; GameObject::getWorldTransform reports a mirrored frame).
#pragma once
#include <string>
#include <vector>

#include "memory.h"

namespace objs {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
struct Box {
    Vec3 lo, hi;
};

float rf(u32 addr);
void wf(u32 addr, float v);

std::string class_name(u32 obj);  // "GameObjectDoor"
Vec3 pos(u32 obj);                 // GameObject::getPos
Box bounds(u32 obj);               // GameObject::getBounds (virtual; pickups/lifts override)
Vec3 facing(u32 obj);              // GameObject::getFacing (unit, horizontal)
// World positions of an interactive object's touch-region markers (+0x190: model nodes placed
// where the game shows the object's tap icon, e.g. a console's screen).
std::vector<Vec3> region_points(u32 obj);
bool empty(const Box& b);

// GameObjectInteractive state (only valid for interactive classes)
bool is_interactive_class(u32 obj);  // derives from GameObjectInteractive
bool interactive(u32 obj);           // isInteractive()
bool interactive_plus(u32 obj);      // isInteractivePlus() (doors, lockers, panels...)
bool telekinesisable(u32 obj);
bool use_enabled(u32 obj);           // +0x174: the object takes "use" (onOpen) at all
float reach(u32 obj);                // +0x178: the game's own interaction radius
u32 flags(u32 obj);                  // GameObject +0x48 (0x10 = game says in range, 0x8 = hidden/disabled)
bool on_interact(u32 obj);           // GameObjectInteractive::onInteract (what a tap / E does)

// Every object in the world (GameWorld's object vector)
std::vector<u32> world_objects(u32 world);

// World point -> game screen pixels (CameraController::worldToScreenSpace); false if behind the
// camera.
bool to_screen(u32 world, Vec3 p, float& sx, float& sy);

// Creates an enemy (GameObject::Type, e.g. 34 slasher) standing at `feet` facing direction `face`,
// as a spawn zone would (GameWorld::createObject + initPostSpawn). Returns it, or 0.
u32 spawn(u32 world, int type, Vec3 feet, Vec3 face);
// Enemy names -> types: slasher lurker brute exploder pregnant swarm boss (0 if unknown)
int enemy_type(const std::string& name);

// Player helpers
void teleport(u32 player, Vec3 p);
void turn_by(u32 player, float radians);  // GameObject::adjustYaw
float yaw_of(Vec3 dir);                     // heading angle in adjustYaw's convention

}  // namespace objs
