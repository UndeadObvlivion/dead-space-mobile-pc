// Port-specific additions to the game itself: an "Anti-Aliasing" row in the Options menu.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "memory.h"

namespace patches {

// Asset overrides: returns patched file contents for the given asset path, or null.
std::shared_ptr<const std::vector<u8>> asset(const std::string& path);

// Installs code hooks into the loaded game library.
void install();

// Called once per frame on the render thread.
void frame();

// Options menu paging (main thread): turns the open options window one page (dir -1 / +1).
// False if no options window is open or it is already at that end.
bool options_page(int dir);
bool options_open();  // an options window is on screen
bool loading_wait();  // the loading screen that waits for a tap is up
bool options_wheel(int dy);  // scrolls a category page by rows (mouse wheel, keyboard); false if it did not move
// A tap at game screen pixels: true (consumed) if it hit the page arrows in the options header.
bool options_click(float x, float y);

// Debug: load this level (map id, e.g. 01_gov_02) when gameplay starts.
void set_level_override(const std::string& id);

}  // namespace patches
