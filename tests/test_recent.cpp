#include "recent.h"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

// Assert-style checks that survive NDEBUG (= Release/CI): the standard assert()
// is compiled out there, which would make these tests silently do nothing.
namespace {
int g_failures = 0;
}  // namespace
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("[-] FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

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

    // Resume positions: per-disc, keyed by the same canonical path, erased by
    // saving 0, and never corrupted by a malformed line. (CHECK, because these
    // must run in Release too.)
    {
        fs::remove_all(cfg, ec);
        fs::create_directories(std::string(cfg) + "/discs", ec);
        const std::string disc = std::string(cfg) + "/discs/resume.iso";
        { std::ofstream f(disc); f << "x"; }

        CHECK(recent::load_position(disc) < 0.0);        // nothing saved yet
        CHECK(recent::save_position(disc, 754.5));
        double at = recent::load_position(disc);
        CHECK(std::fabs(at - 754.5) < 1e-6);
        // The dot-segment spelling resolves to the same disc.
        CHECK(std::fabs(recent::load_position(disc + "/../resume.iso") - 754.5) < 1e-6);
        // Saving a new point replaces the old one instead of appending.
        CHECK(recent::save_position(disc, 120.0));
        CHECK(std::fabs(recent::load_position(disc) - 120.0) < 1e-6);
        {
            std::ifstream f(recent::positions_path());
            std::string line; int lines = 0;
            while (std::getline(f, line)) ++lines;
            CHECK(lines == 1);
        }
        // Zero erases the entry (finished disc: no resume offer).
        CHECK(recent::save_position(disc, 0.0));
        CHECK(recent::load_position(disc) < 0.0);
        // A corrupt line is skipped, the good ones still load.
        {
            std::ofstream f(recent::positions_path(), std::ios::app);
            f << "garbage-without-tab\n";
            f << "\tnot-a-number\n";
        }
        CHECK(recent::save_position(disc, 42.0));
        CHECK(std::fabs(recent::load_position(disc) - 42.0) < 1e-6);
        CHECK(recent::load_position(std::string(cfg) + "/discs/missing.iso") < 0.0);
    }

    fs::remove_all(cfg, ec);
    if (g_failures) {
        std::printf("[-] %d check(s) failed\n", g_failures);
        return 1;
    }
    std::cout << "All recent-list tests passed!\n";
    return 0;
}
