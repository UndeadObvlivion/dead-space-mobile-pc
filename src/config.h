#pragma once
#include <atomic>
#include <string>

struct Config {
    int screen_w = 1280;  // size the game is told the "device screen" has
    int screen_h = 720;
    float dpi = 240.0f;
    std::string language = "en";
    std::string locale = "en_US";
    bool fullscreen = false;
};

extern Config g_config;
extern std::atomic<bool> g_quit;
