#include "recent.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

std::string config_dir() {
    if (const char* env = std::getenv("UMDPLAY_CONFIG_DIR"); env && *env) {
        return env;
    }
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/umdplay";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.config/umdplay";
    }
    return ".umdplay";
}

}  // namespace

namespace recent {

std::string config_path() { return config_dir() + "/recent.txt"; }

std::vector<std::string> load() {
    std::vector<std::string> out;
    std::ifstream f(config_path());
    if (!f) return out;

    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (std::find(out.begin(), out.end(), line) != out.end()) continue;
        out.push_back(line);
    }
    return out;
}

bool add(const std::string& iso_path, size_t max_entries) {
    if (iso_path.empty()) return false;

    std::error_code ec;
    fs::path abs = fs::absolute(iso_path, ec);
    const std::string key = ec ? iso_path : abs.lexically_normal().string();

    std::vector<std::string> list;
    list.push_back(key);
    for (const auto& p : load()) {
        if (p != key) list.push_back(p);
    }
    if (list.size() > max_entries) list.resize(max_entries);

    fs::create_directories(config_dir(), ec);
    std::ofstream f(config_path(), std::ios::trunc);
    if (!f) return false;
    for (const auto& p : list) f << p << '\n';
    return static_cast<bool>(f);
}

}  // namespace recent
