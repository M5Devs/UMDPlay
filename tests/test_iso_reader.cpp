// Regression tests for IsoReader's disc-layout discovery.
//
// A UMD Video disc's content root (UMD_VIDEO/) and its STREAM/ sub-directory do
// NOT sit at a fixed LBA: real discs place UMD_VIDEO at 23 or 24 and STREAM/ at
// 28 or 30, and a disc with many streams (90+) spills STREAM/ across several
// 2048-byte sectors. The reader must therefore locate both from the ISO 9660
// directory tree rather than a hard-coded sector.
//
// These tests build a tiny synthetic ISO 9660 image in memory (written to a
// temp file so the real FileBackend is exercised) whose UMD_VIDEO/ is at LBA 23
// and whose STREAM/ is at LBA 37 — deliberately away from the legacy fixed
// sectors (24 / 28) — and whose STREAM/ holds 300 entries spanning three
// sectors. A reader that reads one fixed sector, or the wrong sector, cannot
// pass.
//
// CHECK always evaluates (assert() is compiled out under NDEBUG/Release).

#include "iso_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

constexpr uint32_t SECTOR = 2048;

struct Ent {
    std::string name;
    bool        dir = false;
    uint32_t    lba = 0;
    uint32_t    size = 0;
};

// Append one ISO 9660 directory record, padding to the next sector first when
// it would otherwise straddle a 2048-byte boundary (records may not span
// sectors; the reader treats a 0 length byte as "skip to next sector").
void add_record(std::vector<uint8_t>& out, const std::string& name,
                uint8_t flags, uint32_t lba, uint32_t size) {
    const size_t nlen = name.size();
    const size_t rec_len = 33 + nlen + ((nlen % 2) ? 1 : 0);

    const size_t sector_off = out.size() % SECTOR;
    if (sector_off + rec_len > SECTOR)
        out.resize(out.size() + (SECTOR - sector_off), 0);

    std::vector<uint8_t> r(rec_len, 0);
    r[0] = static_cast<uint8_t>(rec_len);
    r[2] = static_cast<uint8_t>(lba);
    r[3] = static_cast<uint8_t>(lba >> 8);
    r[4] = static_cast<uint8_t>(lba >> 16);
    r[5] = static_cast<uint8_t>(lba >> 24);
    r[10] = static_cast<uint8_t>(size);
    r[11] = static_cast<uint8_t>(size >> 8);
    r[12] = static_cast<uint8_t>(size >> 16);
    r[13] = static_cast<uint8_t>(size >> 24);
    r[25] = flags;
    r[32] = static_cast<uint8_t>(nlen);
    std::memcpy(&r[33], name.data(), nlen);
    out.insert(out.end(), r.begin(), r.end());
}

std::vector<uint8_t> build_dir(const std::vector<Ent>& ents) {
    std::vector<uint8_t> out;
    add_record(out, std::string(1, '\0'), 0x02, 0, SECTOR);   // "."
    add_record(out, std::string(1, '\1'), 0x02, 0, SECTOR);   // ".."
    for (const auto& e : ents)
        add_record(out, e.name, e.dir ? 0x02 : 0x00, e.lba, e.size);
    if (out.size() % SECTOR) out.resize(out.size() + (SECTOR - out.size() % SECTOR), 0);
    return out;
}

// Build a synthetic UMD Video image with STREAM/ away from the legacy LBAs.
std::vector<uint8_t> build_iso(uint32_t& stream_count_out) {
    constexpr uint32_t LBA_ROOT    = 20;
    constexpr uint32_t LBA_VIDEO   = 23;   // not 24
    constexpr uint32_t LBA_STREAM  = 37;   // not 28
    constexpr uint32_t LBA_RES     = 60;
    constexpr uint32_t LBA_SFO     = 100;
    constexpr uint32_t LBA_PLAYL   = 101;
    constexpr uint32_t LBA_MPS0    = 200;

    const uint32_t kStreams = 300;         // forces STREAM/ across 3 sectors

    std::vector<Ent> stream_ents;
    for (uint32_t i = 0; i < kStreams; ++i) {
        char nm[16];
        std::snprintf(nm, sizeof(nm), "%05u.MPS", i + 1);
        stream_ents.push_back({nm, false, LBA_MPS0 + i, 4096});
    }
    auto stream_dir = build_dir(stream_ents);

    std::vector<Ent> res_ents = {{"JA100000.RCO", false, 150, 4096}};
    auto res_dir = build_dir(res_ents);

    std::vector<Ent> video_ents = {
        {"STREAM", true, LBA_STREAM, static_cast<uint32_t>(stream_dir.size())},
        {"RESOURCE", true, LBA_RES, static_cast<uint32_t>(res_dir.size())},
        {"PARAM.SFO", false, LBA_SFO, 64},
        {"PLAYLIST.UMD", false, LBA_PLAYL, 128},
    };
    auto video_dir = build_dir(video_ents);

    std::vector<Ent> root_ents = {
        {"UMD_VIDEO", true, LBA_VIDEO, static_cast<uint32_t>(video_dir.size())},
    };
    auto root_dir = build_dir(root_ents);

    const uint32_t total_sectors = LBA_MPS0 + kStreams + 4;
    std::vector<uint8_t> img(static_cast<size_t>(total_sectors) * SECTOR, 0);

    auto put = [&](uint32_t lba, const std::vector<uint8_t>& bytes) {
        std::memcpy(img.data() + static_cast<size_t>(lba) * SECTOR,
                    bytes.data(), bytes.size());
    };

    // Primary Volume Descriptor at sector 16 with a root directory record.
    std::vector<uint8_t> pvd(SECTOR, 0);
    pvd[0] = 1;                                  // volume descriptor type
    std::memcpy(&pvd[1], "CD001", 5);
    pvd[6] = 1;                                  // version
    uint8_t* rde = pvd.data() + 156;             // root directory record
    rde[0] = 34;
    rde[2] = static_cast<uint8_t>(LBA_ROOT);
    rde[3] = static_cast<uint8_t>(LBA_ROOT >> 8);
    const uint32_t rsz = static_cast<uint32_t>(root_dir.size());
    rde[10] = static_cast<uint8_t>(rsz);
    rde[11] = static_cast<uint8_t>(rsz >> 8);
    rde[12] = static_cast<uint8_t>(rsz >> 16);
    rde[13] = static_cast<uint8_t>(rsz >> 24);
    rde[25] = 0x02;
    rde[32] = 1;
    rde[33] = 0;
    put(16, pvd);

    put(LBA_ROOT, root_dir);
    put(LBA_VIDEO, video_dir);
    put(LBA_STREAM, stream_dir);
    put(LBA_RES, res_dir);

    stream_count_out = kStreams;
    return img;
}

}  // namespace

int main() {
    uint32_t stream_count = 0;
    std::vector<uint8_t> img = build_iso(stream_count);

    const fs::path tmp = fs::temp_directory_path() / "umdplay_test_iso_reader.iso";
    {
        std::ofstream out(tmp, std::ios::binary);
        out.write(reinterpret_cast<const char*>(img.data()),
                  static_cast<std::streamsize>(img.size()));
    }

    IsoReader reader(tmp.string());
    const bool opened = reader.open();

    // The disc must open and the content root must be UMD_VIDEO.
    CHECK(opened);
    CHECK(reader.root() == UmdRoot::Video);
    CHECK(reader.root_name() == "UMD_VIDEO");

    // Every .MPS entry in STREAM/ must be found — including those past the
    // first sector, which a single fixed-sector read would miss.
    auto entries = reader.get_mps_entries();
    CHECK(entries.size() == stream_count);
    if (entries.size() == stream_count) {
        CHECK(entries.front().name == "00001.MPS");
        CHECK(entries.front().lba == 200);
        CHECK(entries.back().name == "00300.MPS");
        CHECK(entries.back().lba == 200 + stream_count - 1);
    }
    std::printf("[*] synthetic ISO: %zu stream(s), root=%s\n",
                entries.size(), reader.root_name().c_str());

    // Metadata at the content-root level is discovered via the tree too.
    CHECK(reader.has_metadata("PARAM.SFO"));
    CHECK(!reader.assets_in("RESOURCE").empty());
    CHECK(reader.find_asset("PLAYLIST.UMD") != nullptr);
    CHECK(reader.find_asset("JA100000.RCO") != nullptr);

    std::error_code ec;
    fs::remove(tmp, ec);

    if (g_failures != 0) {
        std::printf("[-] %d iso_reader check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All IsoReader layout tests passed!\n");
    return 0;
}
