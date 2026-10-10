// Keyboard navigation for the game's touch menus: W/A/S/D (and arrow keys) move a highlight
// between the buttons on screen, Space / Enter press the highlighted one.
#pragma once
#include <SDL.h>

#include <functional>
#include <string>
#include <vector>

#include "memory.h"

namespace menu_nav {

void install();
// How to press a button: a tap at game screen pixels (x, y).
void set_tap(std::function<void(float, float)> tap);
// Main thread. Returns true if the event was used for menu navigation. Only acts while
// gameplay controls are not active (menus, pause, RIG...).
bool event(const SDL_Event& e, bool gameplay_active);
// Main thread, after the game's frame: forgets buttons that stopped updating, draws the highlight.
void frame(bool gameplay_active);
// Hides the highlight until a navigation key is pressed again (e.g. the menu is closing).
void hide();

struct Button {
    u32 obj;
    std::string name;
    int x, y, w, h;  // game screen pixels
};
std::vector<Button> buttons();  // buttons updated during the last game frame (debug)

}  // namespace menu_nav
