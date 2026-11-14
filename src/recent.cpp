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

// Normalise a path to a stable identity key so the same disc opened via a
// relative path, a `..`/`.` spelling, or a symlink dedupes to one entry.
// weakly_canonical resolves symlinks and dot segments without requiring the
// file to exist (a missing disc must still round-trip through the list).
std::string canonical_key(const std::string& path) {
    std::error_code ec;
    fs::path abs = fs::absolute(path, ec);
    if (ec) return path;
    fs::path canon = fs::weakly_canonical(abs, ec);
    if (ec) canon = abs.lexically_normal();
    return canon.string();
}

}  // namespace

namespace recent {

std::string config_path() { return config_dir() + "/recent.txt"; }

std::vector<std::string> load() {
    std::vector<std::string> out;
    std::vector<std::string> seen_keys;
    std::ifstream f(config_path());
    if (!f) return out;

    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        const std::string key = canonical_key(line);
        if (std::find(seen_keys.begin(), seen_keys.end(), key) != seen_keys.end()) continue;
        seen_keys.push_back(key);
        out.push_back(line);
    }
    return out;
}

bool add(const std::string& iso_path, size_t max_entries) {
    if (iso_path.empty()) return false;

    const std::string key = canonical_key(iso_path);

    std::vector<std::string> list;
    list.push_back(key);
    for (const auto& p : load()) {
        if (canonical_key(p) != key) list.push_back(p);
    }
    if (list.size() > max_entries) list.resize(max_entries);

    std::error_code ec;
    fs::create_directories(config_dir(), ec);
    std::ofstream f(config_path(), std::ios::trunc);
    if (!f) return false;
    for (const auto& p : list) f << p << '\n';
    return static_cast<bool>(f);
}

}  // namespace recent
