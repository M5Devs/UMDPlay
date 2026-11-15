// Unit tests for the DiscReader byte-range abstraction and its ISO / CHD
// backends.
//
// The CHD backend is exercised against a committed fixture
// (tests/fixtures/pattern.chd) whose logical bytes are a known linear pattern
// (byte i == (i*31 + 7) & 0xff), so every read can be checked byte-for-byte —
// including unaligned offsets, reads spanning hunk boundaries and short reads
// at EOF. When a real UMD ISO is reachable (UMD_ISO env var, else
// ./test_umd.iso) and chdman is installed, the suite also compresses the ISO
// head into a temporary CHD and asserts that IsoReader sees an identical
// stream listing through both backends.
//
// CHECK always evaluates (assert() is compiled out under NDEBUG/Release).

#include "disc_reader.h"
#include "iso_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__,         \
                        __LINE__);                                             \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

uint8_t pattern_byte(uint64_t i) {
    return static_cast<uint8_t>((i * 31u + 7u) & 0xffu);
}

// Read `size` bytes at `off` and compare against the fixture pattern.
void check_pattern_region(const umdplay::DiscReader& r, uint64_t off, size_t size,
                          const char* what) {
    std::vector<uint8_t> got(size, 0xAA);
    const size_t n = r.read_at(off, got.data(), size);
    if (n != size) {
        std::printf("CHECK failed: short read (%s): got %zu want %zu\n",
                    what, n, size);
        ++g_failures;
        return;
    }
    for (size_t i = 0; i < size; ++i) {
        if (got[i] != pattern_byte(off + i)) {
            std::printf("CHECK failed: pattern mismatch (%s) at %llu: "
                        "got 0x%02x want 0x%02x\n", what,
                        static_cast<unsigned long long>(off + i), got[i],
                        pattern_byte(off + i));
            ++g_failures;
            return;
        }
    }
}

void test_path_helpers() {
    CHECK(umdplay::is_chd_path("disc.chd"));
    CHECK(umdplay::is_chd_path("DISC.CHD"));
    CHECK(umdplay::is_chd_path("/a/b/My Disc.Chd"));
    CHECK(!umdplay::is_chd_path("disc.iso"));
    CHECK(!umdplay::is_chd_path("disc"));

    CHECK(umdplay::is_disc_image_path("disc.iso"));
    CHECK(umdplay::is_disc_image_path("disc.ISO"));
    CHECK(umdplay::is_disc_image_path("disc.chd"));
    CHECK(!umdplay::is_disc_image_path("disc.cso"));
    CHECK(!umdplay::is_disc_image_path("disc"));

    // A dot in a parent directory must not be mistaken for the extension.
    CHECK(!umdplay::is_disc_image_path("/home/me.v2/disc"));
}

void test_iso_backend() {
    // Write a temp image with the same pattern and read it back through the
    // FileBackend path (a .iso suffix selects it).
    const std::string path = (fs::temp_directory_path() / "umdplay_test.iso").string();
    constexpr size_t kSize = 100000;
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        for (size_t i = 0; i < kSize; ++i)
            f.put(static_cast<char>(pattern_byte(i)));
    }

    umdplay::DiscReader r;
    CHECK(r.open(path));
    CHECK(r.is_open());
    CHECK(std::string(r.backend_name()) == "iso");
    CHECK(r.size() == kSize);

    check_pattern_region(r, 0, 4096, "iso head");
    check_pattern_region(r, 7777, 2048, "iso unaligned");
    check_pattern_region(r, kSize - 100, 100, "iso tail");

    // Short read at EOF: asking past the end returns only what exists.
    std::vector<uint8_t> tail(500, 0xAA);
    const size_t n = r.read_at(kSize - 10, tail.data(), tail.size());
    CHECK(n == 10);
    // A read entirely past the end returns nothing.
    CHECK(r.read_at(kSize + 4096, tail.data(), tail.size()) == 0);

    // read_sector / read_range helpers.
    uint8_t sec[2048];
    CHECK(r.read_sector(0, sec));
    CHECK(sec[0] == pattern_byte(0));

    // Close before removing: Windows refuses to delete a file that still has an
    // open handle (POSIX would allow it).
    r.close();
    fs::remove(path);
}

void test_chd_backend(const std::string& fixture) {
    umdplay::DiscReader r;
    if (!r.open(fixture)) {
        std::printf("CHECK failed: cannot open CHD fixture %s (%s)\n",
                    fixture.c_str(), r.last_error().c_str());
        ++g_failures;
        return;
    }
    CHECK(std::string(r.backend_name()) == "chd");
    // The fixture is 512 KiB of 2048-byte units (256 units).
    const uint64_t size = r.size();
    CHECK(size == 512 * 1024);

    // Aligned, unaligned and hunk-boundary-spanning reads all match the pattern.
    check_pattern_region(r, 0, 2048, "chd sector 0");
    check_pattern_region(r, 2048 * 5, 2048, "chd sector 5");
    check_pattern_region(r, 100, 4000, "chd unaligned");
    // A read straddling a 4096-byte hunk boundary (hunk 1 -> 2).
    check_pattern_region(r, 4096 - 100, 200, "chd cross-hunk");
    // A long read spanning many hunks.
    check_pattern_region(r, 1234, 40000, "chd multi-hunk");

    // A read that starts mid-hunk and continues to EOF.
    const uint64_t tail_off = size - 3000;
    check_pattern_region(r, tail_off, 3000, "chd tail");

    // Short read at EOF.
    std::vector<uint8_t> buf(4096, 0xAA);
    CHECK(r.read_at(size - 5, buf.data(), buf.size()) == 5);
    CHECK(r.read_at(size + 1, buf.data(), buf.size()) == 0);

    // Repeated reads of the same hunk are served from the cache and stay exact.
    for (int i = 0; i < 8; ++i)
        check_pattern_region(r, 8192, 4096, "chd cached repeat");
}

void test_error_paths() {
    umdplay::DiscReader r;
    CHECK(!r.open("/nonexistent/definitely/missing.chd"));
    CHECK(!r.is_open());
    CHECK(!r.last_error().empty());
    CHECK(r.read_at(0, nullptr, 0) == 0);

    umdplay::DiscReader r2;
    CHECK(!r2.open("/nonexistent/definitely/missing.iso"));
    CHECK(!r2.is_open());
}

// When a real ISO is present and chdman is installed, compress its head into a
// CHD and confirm IsoReader discovers the same streams through both backends.
void test_real_iso_vs_chd(const std::string& iso) {
    std::error_code ec;
    if (!fs::exists(iso, ec)) {
        std::printf("[test_chd] no ISO at %s — skipping ISO/CHD equivalence\n",
                    iso.c_str());
        return;
    }

    // chdman createraw needs an uncompressed head; take the first 8 MiB.
    const std::string head = (fs::temp_directory_path() / "umdplay_head.iso").string();
    const std::string chd  = (fs::temp_directory_path() / "umdplay_head.chd").string();
    {
        std::ifstream in(iso, std::ios::binary);
        std::ofstream out(head, std::ios::binary | std::ios::trunc);
        std::vector<char> buf(1 << 20);
        size_t left = 8u << 20;
        while (left > 0 && in) {
            const size_t want = std::min<size_t>(left, buf.size());
            in.read(buf.data(), static_cast<std::streamsize>(want));
            const size_t got = static_cast<size_t>(in.gcount());
            if (got == 0) break;
            out.write(buf.data(), static_cast<std::streamsize>(got));
            left -= got;
        }
    }
    const std::string cmd = "chdman createraw -i '" + head + "' -o '" + chd +
                            "' -f -us 2048 >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0 || !fs::exists(chd, ec)) {
        std::printf("[test_chd] chdman unavailable — skipping ISO/CHD equivalence\n");
        fs::remove(head);
        return;
    }

    {
        // Scoped so both readers close their file handles before the removes
        // below (Windows will not delete a file with an open handle).
        IsoReader iso_reader(iso);
        IsoReader chd_reader(chd);
        const bool ok_iso = iso_reader.open();
        const bool ok_chd = chd_reader.open();
        CHECK(ok_iso);
        CHECK(ok_chd);

        if (ok_iso && ok_chd) {
            const auto a = iso_reader.get_mps_entries();
            const auto b = chd_reader.get_mps_entries();
            CHECK(a.size() == b.size());
            CHECK(a.size() == 4);
            for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
                CHECK(a[i].name == b[i].name);
                CHECK(a[i].lba == b[i].lba);
                CHECK(a[i].size_bytes == b[i].size_bytes);
            }
            // Metadata (PARAM.SFO title) must match too.
            CHECK(iso_reader.param().title == chd_reader.param().title);
        }
    }

    fs::remove(head);
    fs::remove(chd);
}

}  // namespace

int main() {
    test_path_helpers();

    const char* fixture_env = std::getenv("UMD_CHD_FIXTURE");
    const std::string fixture =
        fixture_env ? fixture_env : "tests/fixtures/pattern.chd";
    test_chd_backend(fixture);

    test_iso_backend();
    test_error_paths();

    const char* iso_env = std::getenv("UMD_ISO");
    test_real_iso_vs_chd(iso_env ? iso_env : "test_umd.iso");

    if (g_failures == 0) {
        std::printf("[test_chd] all checks passed\n");
        return 0;
    }
    std::printf("[test_chd] %d check(s) failed\n", g_failures);
    return 1;
}
