// Debug views drawn over the game (console: /debug_lights, /debug_hitboxes; keys F6, F7).
#pragma once

namespace debugview {

void set_lights(bool on);    // light placement: every light the port adds, with radius and state
void set_hitboxes(bool on);  // bounding boxes of the game's objects (GameObject::getBounds)
bool lights();
bool hitboxes();
// Queues this frame's overlay lines. Game thread, with a live player.
void draw(unsigned player, unsigned world);

}  // namespace debugview
