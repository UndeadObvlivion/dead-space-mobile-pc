// In-game console (` / ~ key): type a command and press Enter. See console.cpp for commands.
#pragma once
#include <SDL.h>

namespace console {

// Main thread. Returns true if the event was consumed (console open, or its toggle key).
bool event(const SDL_Event& e);
// Main thread, after the game's frame: draws the console over it.
void draw(int screen_w, int screen_h);
bool is_open();
// Hooks into the game (call once after the library is loaded).
void install();
void set_god(bool on);  // player takes no damage

}  // namespace console
