// Keyboard / mouse controls for gameplay. The game was built for touch; these drive the
// player through the same events its touch input produces (see input.cpp).
#pragma once
#include <SDL.h>

#include <functional>
#include <string>
#include <vector>

namespace input {

void install();          // hooks into the game (call after the library is loaded)
void set_trace(bool on);  // debug: log game input events
void set_test_mode(bool on);  // debug: scripted input, capture without window focus
void set_give_weapons(bool on);                                         // debug
void set_teleport(const std::string& cls, float x, float y, float z);  // debug
// Per frame on the render thread, before the game's frame. Returns true when gameplay is
// active (mouse captured) so the caller can stop forwarding the mouse as touch.
bool frame(SDL_Window* win);
// True while a skipped cinematic is being fast-forwarded (frame cap should be lifted).
bool fast_forwarding();
// Raw SDL events; returns true if consumed.
bool event(const SDL_Event& e, SDL_Window* win);

// Runs fn on the game thread during the player's next update (fn gets the player object).
void run_on_game_thread(std::function<void(unsigned player)> fn);
unsigned world();  // current GameWorld (0 before gameplay)
float free_look_yaw();  // camera orbit relative to the player (game thread)
void set_interact_debug(bool on);  // F10 view
// Treats every key and mouse button as released (e.g. while a console takes the keyboard).
void release_all();
// True while gameplay controls are active (mouse captured; not in a menu).
bool controls_active();

// ---- rebindable controls (used by the Options menu)
// An input code is an SDL scancode (1..511) or kMouseCode + SDL mouse button.
constexpr int kMouseCode = 1000;
struct Binding {
    const char* key;    // port_settings.ini key (bind_<key>) and menu id suffix
    const char* label;  // English menu label
    int def;            // default code
};
int binding_count();
const Binding& binding(int i);
int binding_code(int i);
// Assigns a code; an action already using it takes this action's old code (swap).
void set_binding(int i, int code);
void reset_bindings();
std::string code_name(int code);  // "W", "Left Shift", "Mouse Left"...
// Every code that can be bound (for building the menu's string table).
const std::vector<int>& bindable_codes();
// Waits for the next key / mouse button press and binds it to action i (Esc cancels).
void begin_capture(int i);
bool capturing();

}  // namespace input
