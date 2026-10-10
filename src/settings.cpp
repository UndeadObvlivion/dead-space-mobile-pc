#include "settings.h"

#include <fstream>
#include <map>
#include <mutex>

#include "log.h"

namespace settings {

namespace {
std::mutex g_lock;
std::string g_path;
std::map<std::string, std::string> g_values;
}  // namespace

void load(const std::string& path) {
    std::lock_guard lk(g_lock);
    g_path = path;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.pop_back();
            while (!s.empty() && (s[0] == ' ' || s[0] == '\t')) s.erase(0, 1);
            return s;
        };
        g_values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
}

void save() {
    std::lock_guard lk(g_lock);
    if (g_path.empty()) return;
    std::ofstream f(g_path);
    f << "# Dead Space PC port settings\n";
    for (auto& [k, v] : g_values) f << k << " = " << v << "\n";
}

int get_int(const std::string& key, int def) {
    std::lock_guard lk(g_lock);
    auto it = g_values.find(key);
    if (it == g_values.end()) return def;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return def;
    }
}

void set_int(const std::string& key, int value) {
    {
        std::lock_guard lk(g_lock);
        g_values[key] = std::to_string(value);
    }
    save();
}

std::string get_string(const std::string& key, const std::string& def) {
    std::lock_guard lk(g_lock);
    auto it = g_values.find(key);
    return it == g_values.end() ? def : it->second;
}

void set_string(const std::string& key, const std::string& value) {
    {
        std::lock_guard lk(g_lock);
        g_values[key] = value;
    }
    save();
}

}  // namespace settings
