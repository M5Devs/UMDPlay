#include "recent.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

// The test runs with UMDPLAY_CONFIG_DIR pointed at a scratch dir (see
// CMakeLists.txt) so the user's real ~/.config/umdplay is never touched.
int main() {
    const char* cfg = std::getenv("UMDPLAY_CONFIG_DIR");
    assert(cfg != nullptr);

    // Start clean.
    std::error_code ec;
    fs::remove_all(cfg, ec);

    // Empty list initially.
    assert(recent::load().empty());
    assert(recent::config_path() == std::string(cfg) + "/recent.txt");

    // Add two paths; most-recent first.
    assert(recent::add("/discs/a.iso"));
    assert(recent::add("/discs/b.iso"));
    {
        auto v = recent::load();
        assert(v.size() == 2);
        assert(v[0] == "/discs/b.iso");
        assert(v[1] == "/discs/a.iso");
    }

    // Re-adding an existing path moves it to the front without duplicating.
    assert(recent::add("/discs/a.iso"));
    {
        auto v = recent::load();
        assert(v.size() == 2);
        assert(v[0] == "/discs/a.iso");
        assert(v[1] == "/discs/b.iso");
    }

    // Trim to max_entries.
    for (int i = 0; i < 5; ++i) recent::add("/discs/c" + std::to_string(i) + ".iso", 3);
    {
        auto v = recent::load();
        assert(v.size() == 3);
        assert(v[0] == "/discs/c4.iso");
    }

    // Blank lines and duplicates in the file are ignored on load.
    {
        std::ofstream f(recent::config_path(), std::ios::trunc);
        f << "/x.iso\n\n/x.iso\n/y.iso\n";
    }
    {
        auto v = recent::load();
        assert(v.size() == 2);
        assert(v[0] == "/x.iso");
        assert(v[1] == "/y.iso");
    }

    // Empty path is rejected.
    assert(!recent::add(""));

    // Equivalent spellings of the same disc (dot segments and symlinks) must
    // collapse to a single entry rather than filling the list with duplicates.
    {
        fs::remove_all(cfg, ec);
        fs::create_directories(std::string(cfg) + "/discs", ec);
        const std::string real = std::string(cfg) + "/discs/real.iso";
        { std::ofstream f(real); f << "x"; }
        const std::string link = std::string(cfg) + "/discs/link.iso";
        std::error_code lec;
        fs::create_symlink(real, link, lec);

        assert(recent::add(real));
        assert(recent::add(real + "/../real.iso"));   // dot-segment spelling
        if (!lec) assert(recent::add(link));          // symlink spelling
        auto v = recent::load();
        assert(v.size() == 1);

        // A different real file is still kept as a separate entry.
        const std::string other = std::string(cfg) + "/discs/other.iso";
        { std::ofstream f(other); f << "y"; }
        assert(recent::add(other));
        assert(recent::load().size() == 2);
    }

    // Relative paths are stored absolute so a later CWD change cannot break them.
    {
        fs::remove_all(cfg, ec);
        assert(recent::add("relative_disc.iso"));
        auto v = recent::load();
        assert(v.size() == 1);
        assert(fs::path(v[0]).is_absolute());
    }

    fs::remove_all(cfg, ec);
    std::cout << "All recent-list tests passed!\n";
    return 0;
}
