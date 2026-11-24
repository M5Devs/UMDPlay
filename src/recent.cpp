#include "recent.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

std::string config_dir() {
    if (const char* env = std::getenv("UMDPLAY_CONFIG_DIR"); env && *env) {
        return env;
    }
#if defined(_WIN32)
    // HOME is typically unset on native Windows; APPDATA is the per-user root.
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata) {
        return (fs::path(appdata) / "umdplay").string();
    }
    if (const char* home = std::getenv("USERPROFILE"); home && *home) {
        return (fs::path(home) / "umdplay").string();
    }
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return (fs::path(xdg) / "umdplay").string();
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return (fs::path(home) / ".config" / "umdplay").string();
    }
#endif
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

namespace {

struct Entry {
    std::string path;
    double seconds = 0.0;
};

std::vector<Entry> load_positions() {
    std::vector<Entry> out;
    std::ifstream f(positions_path());
    if (!f) return out;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const auto tab = line.rfind('\t');
        if (tab == std::string::npos || tab == 0) continue;
        const std::string path = line.substr(0, tab);
        try {
            const double seconds = std::stod(line.substr(tab + 1));
            if (seconds > 0.0 && std::isfinite(seconds)) out.push_back({path, seconds});
        } catch (...) {
            // A corrupt line loses one resume point, never the whole file.
        }
    }
    return out;
}

bool write_positions(const std::vector<Entry>& entries) {
    std::error_code ec;
    fs::create_directories(config_dir(), ec);
    std::ofstream f(positions_path(), std::ios::trunc);
    if (!f) return false;
    for (const auto& e : entries) f << e.path << '\t' << e.seconds << '\n';
    return static_cast<bool>(f);
}

}  // namespace

std::string positions_path() { return config_dir() + "/positions.txt"; }

double load_position(const std::string& iso_path) {
    if (iso_path.empty()) return -1.0;
    const std::string key = canonical_key(iso_path);
    for (const auto& e : load_positions()) {
        if (canonical_key(e.path) == key) return e.seconds;
    }
    return -1.0;
}

bool save_position(const std::string& iso_path, double seconds, size_t max_entries) {
    if (iso_path.empty()) return false;
    const std::string key = canonical_key(iso_path);

    std::vector<Entry> list;
    if (seconds > 0.0 && std::isfinite(seconds)) list.push_back({key, seconds});
    for (const auto& e : load_positions()) {
        if (canonical_key(e.path) != key) list.push_back(e);
    }
    if (list.size() > max_entries) list.resize(max_entries);
    return write_positions(list);
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
