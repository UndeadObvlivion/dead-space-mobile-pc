// Port settings that are not part of the game's own save data (userdata/port_settings.ini).
#pragma once
#include <string>

namespace settings {
void load(const std::string& path);
void save();
int get_int(const std::string& key, int def);
void set_int(const std::string& key, int value);  // saves immediately
std::string get_string(const std::string& key, const std::string& def);
void set_string(const std::string& key, const std::string& value);  // saves immediately
}  // namespace settings
