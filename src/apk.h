#pragma once
#include <string>

namespace apk {
// Extracts the game's assets/ folder and its 32-bit ARM libDeadSpace.so from an Android APK
// into dest_dir (dest_dir/assets/..., dest_dir/libDeadSpace.so). Returns false on failure.
bool import(const std::string& apk_path, const std::string& dest_dir);
}
