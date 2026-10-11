// Debug "dev mode": a running game takes commands from <dir>/cmd.txt (written by tools/dev.py),
// so tests can drive one session instead of relaunching the game for every check.
#pragma once
#include <SDL.h>

#include <functional>
#include <string>
#include <vector>

namespace dev {

struct Host {
    std::function<void(float x, float y, int hold)> tap;         // game screen coordinates
    std::function<void(const std::string& path)> screenshot;     // PNG, quarter size
    std::function<void()> quit;
    std::function<std::vector<unsigned char>(int div, int& w, int& h)> grab;  // current frame, RGB rows, scaled down
    std::function<void(const std::string& path, int w, int h, const std::vector<unsigned char>& rgb)> write_png;
};

void init(const std::string& dir, Host host);
bool active();
int turbo();  // extra game frames to run per displayed frame (0 = normal speed)
// Once per displayed frame on the main thread, before input is processed.
void frame();
// Writes a line to <dir>/out.txt (any thread).
void out(const std::string& line);

}  // namespace dev
