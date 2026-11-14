// Unit tests for the PLAYLIST.UMD parser and the .RCO container probe.
//
// The synthetic cases build a byte buffer from scratch so the layout logic is
// checked deterministically (bounds, truncation, bad magic, empty). When a real
// ISO is reachable (UMD_ISO env var, else ./test_umd.iso) the suite also parses
// the disc's own PLAYLIST.UMD / .RCO and asserts on the discovered structure.
//
// CHECK always evaluates (assert() is compiled out under NDEBUG/Release).

#include "iso_reader.h"
#include "playlist_parser.h"
#include "rco_parser.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

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

// --- little helpers to synthesise a PLAYLIST.UMD -------------------------

void put_be16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
    if (off + 2 > b.size()) b.resize(off + 2, 0);
    b[off]     = static_cast<uint8_t>(v >> 8);
    b[off + 1] = static_cast<uint8_t>(v & 0xff);
}
void put_be32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    if (off + 4 > b.size()) b.resize(off + 4, 0);
    b[off]     = static_cast<uint8_t>(v >> 24);
    b[off + 1] = static_cast<uint8_t>((v >> 16) & 0xff);
    b[off + 2] = static_cast<uint8_t>((v >> 8) & 0xff);
    b[off + 3] = static_cast<uint8_t>(v & 0xff);
}
void put_str(std::vector<uint8_t>& b, size_t off, const std::string& s) {
    if (off + s.size() > b.size()) b.resize(off + s.size(), 0);
    std::memcpy(b.data() + off, s.data(), s.size());
}
void put_u8(std::vector<uint8_t>& b, size_t off, uint8_t v) {
    if (off + 1 > b.size()) b.resize(off + 1, 0);
    b[off] = v;
}

// Build a minimal but valid PLAYLIST.UMD with `tracks` tracks, each having
// `markers_per_track` markers. Track 0 gets a real name + clip id.
std::vector<uint8_t> make_playlist(int tracks, int markers_per_track) {
    const size_t kTrackSize = 0x338;
    const size_t kMarkSize  = 0x28;
    std::vector<uint8_t> b(0x136, 0);
    // Header
    put_str(b, 0, "UMDV0088");
    put_be32(b, 0x08, 0x130);
    put_be16(b, 0x2A, 0x0104);
    put_be32(b, 0x2C, 1);
    put_u8(b, 0x30, 7);
    put_str(b, 0x31, "MYUMDVD");

    size_t off = 0x136;
    for (int t = 0; t < tracks; ++t) {
        const size_t ent_len = kTrackSize - 4 + static_cast<size_t>(markers_per_track) * kMarkSize;
        put_be32(b, off, static_cast<uint32_t>(ent_len));
        put_be16(b, off + 0x04, 0x0310);
        put_be16(b, off + 0x06, 0x0332);
        put_be16(b, off + 0x26, 0x02E8);
        put_be32(b, off + 0x32, 0x00060900);
        std::string nm = "Track" + std::to_string(t);
        put_u8(b, off + 0x36, static_cast<uint8_t>(nm.size()));
        put_str(b, off + 0x37, nm);
        put_be16(b, off + 0x312, 0x1E00);
        put_be32(b, off + 0x316, 0x00010000);
        put_be32(b, off + 0x31A, 0x00160005);
        std::string clip = "0000" + std::to_string(t + 1);
        put_str(b, off + 0x31E, clip);
        put_be32(b, off + 0x328, 90000);                 // start = 1.00s
        put_be32(b, off + 0x32E, 90000 * (30 + t * 10)); // end
        put_be32(b, off + 0x332, static_cast<uint32_t>(kMarkSize * markers_per_track + 2));
        put_be16(b, off + 0x336, static_cast<uint16_t>(markers_per_track));
        size_t mo = off + kTrackSize;
        for (int m = 0; m < markers_per_track; ++m) {
            put_u8(b, mo, 0x05);
            std::string mn = "Mark" + std::to_string((m + 1) * 4500);
            put_u8(b, mo + 1, static_cast<uint8_t>(mn.size()));
            put_be32(b, mo + 6, static_cast<uint32_t>(90000 * (m + 1) * 3));
            put_str(b, mo + 0x10, mn);
            mo += kMarkSize;
        }
        off += 4 + ent_len;
    }
    b.resize(off, 0);
    // Fix up the file-data table: offset-to-EOF (from 0x134) + track count.
    put_be32(b, 0x130, static_cast<uint32_t>(b.size() - 0x134));
    put_be16(b, 0x134, static_cast<uint16_t>(tracks));
    return b;
}

std::string find_iso() {
    if (const char* env = std::getenv("UMD_ISO")) return env;
    const char* candidates[] = {"../test_umd.iso", "test_umd.iso",
                                "../../test_umd.iso"};
    for (const char* c : candidates) {
        std::ifstream f(c, std::ios::binary);
        if (f.good()) return c;
    }
    return {};
}

// --- synthetic PLAYLIST.UMD cases ----------------------------------------

void test_playlist_synthetic() {
    auto raw = make_playlist(3, 2);
    auto pl = umdplay::parse_playlist(raw.data(), raw.size());
    CHECK(pl.valid);
    CHECK(pl.magic == "UMDV0088");
    CHECK(pl.version == 88);
    CHECK(pl.title == "MYUMDVD");
    CHECK(pl.tracks.size() == 3);
    CHECK(pl.tracks[0].name == "Track0");
    CHECK(pl.tracks[0].clip_name == "00001");
    CHECK(pl.tracks[2].clip_name == "00003");
    CHECK(pl.tracks[0].markers.size() == 2);
    CHECK(pl.tracks[0].markers[0].name == "Mark4500");
    CHECK(pl.tracks[0].markers[0].seconds > 2.99 &&
          pl.tracks[0].markers[0].seconds < 3.01);
    // Chapters: empty labels become "Chapter N".
    auto ch = pl.chapters(0);
    CHECK(ch.size() == 2);
    CHECK(ch[0].index == 0);
    CHECK(ch[1].label == "Mark9000");
    // main_track picks the one with the most markers (tie -> longest span).
    CHECK(pl.main_track() != nullptr);

    // No tracks -> still valid, but no main track.
    auto empty = make_playlist(0, 0);
    auto ple = umdplay::parse_playlist(empty.data(), empty.size());
    CHECK(ple.valid);
    CHECK(ple.tracks.empty());
    CHECK(ple.main_track() == nullptr);
    CHECK(ple.chapters(0).empty());

    // Summary is non-empty for both.
    CHECK(!umdplay::playlist_summary(pl).empty());
}

void test_playlist_rejects() {
    // Null / too small.
    CHECK(!umdplay::parse_playlist(nullptr, 0).valid);
    uint8_t tiny[8] = {0};
    CHECK(!umdplay::parse_playlist(tiny, sizeof(tiny)).valid);

    // Bad magic.
    auto raw = make_playlist(1, 1);
    std::memcpy(raw.data(), "XXXX0088", 8);
    auto bad = umdplay::parse_playlist(raw.data(), raw.size());
    CHECK(!bad.valid);
    CHECK(!bad.error.empty());

    // Bad version digits.
    auto raw2 = make_playlist(1, 1);
    std::memcpy(raw2.data(), "UMDVabcd", 8);
    CHECK(!umdplay::parse_playlist(raw2.data(), raw2.size()).valid);

    // Truncated mid-track (cut the buffer short) must not read OOB.
    auto raw3 = make_playlist(2, 4);
    raw3.resize(0x200);
    auto trunc = umdplay::parse_playlist(raw3.data(), raw3.size());
    CHECK(!trunc.valid);  // track entry claims more bytes than remain

    // Implausible track-entry length.
    auto raw4 = make_playlist(1, 1);
    put_be32(raw4, 0x136, 0x7FFFFFFF);
    CHECK(!umdplay::parse_playlist(raw4.data(), raw4.size()).valid);

    // Implausible marker count.
    auto raw5 = make_playlist(1, 1);
    put_be16(raw5, 0x136 + 0x336, 0xFFFF);
    CHECK(!umdplay::parse_playlist(raw5.data(), raw5.size()).valid);
}

// --- synthetic .RCO cases ------------------------------------------------

std::vector<uint8_t> make_rco() {
    std::vector<uint8_t> b(0x200, 0);
    b[0] = 0x00; b[1] = 'P'; b[2] = 'R'; b[3] = 'F';
    put_be32(b, 0x04, 0x80);           // version (stored LE on disk; see below)
    // version is LE: write 0x80 little-endian.
    b[0x04] = 0x80; b[0x05] = 0x00; b[0x06] = 0x00; b[0x07] = 0x00;
    // TOC entry 0: offset 0xa4, size 0xcc (little-endian).
    b[0x10] = 0xa4; b[0x11] = 0x00; b[0x12] = 0x00; b[0x13] = 0x00;
    b[0x14] = 0xcc; b[0x15] = 0x00; b[0x16] = 0x00; b[0x17] = 0x00;
    // TOC entry 1: absent (0xFFFFFFFF / 0xFFFFFFFF).
    for (int i = 0x18; i < 0x20; ++i) b[i] = 0xff;
    // A fake PNG (signature + IHDR dims + IEND) at 0x100.
    const uint8_t png[] = {0x89,'P','N','G','\r','\n',0x1a,'\n',
                           0,0,0,13,'I','H','D','R'};
    std::memcpy(b.data() + 0x100, png, sizeof(png));
    put_be32(b, 0x100 + 16, 252);   // width
    put_be32(b, 0x100 + 20, 30);    // height
    std::memcpy(b.data() + 0x130, "IEND", 4);
    // A VSMX block at 0x180.
    std::memcpy(b.data() + 0x180, "VSMX", 4);
    return b;
}

void test_rco_synthetic() {
    auto raw = make_rco();
    auto r = umdplay::parse_rco(raw.data(), raw.size());
    CHECK(r.valid);
    CHECK(r.magic == "PRF");
    CHECK(r.version == 0x80);
    CHECK(r.images.size() == 1);
    CHECK(r.images[0].width == 252);
    CHECK(r.images[0].height == 30);
    CHECK(r.has_vsmx);
    CHECK(r.vsmx_offset == 0x180);
    CHECK(!r.toc.empty());
    CHECK(!umdplay::rco_summary(r).empty());

    // Rejections.
    CHECK(!umdplay::parse_rco(nullptr, 0).valid);
    uint8_t tiny[8] = {0};
    CHECK(!umdplay::parse_rco(tiny, sizeof(tiny)).valid);
    auto bad = raw;
    bad[1] = 'X';
    CHECK(!umdplay::parse_rco(bad.data(), bad.size()).valid);
}

// --- chapter navigation math (pure templates) ----------------------------

void test_chapter_nav() {
    struct Ch { double seconds; std::string label; };
    std::vector<Ch> ch = {{0.0, "a"}, {30.0, "b"}, {90.0, "c"}, {300.0, "d"}};

    // chapter_at: last start <= t.
    CHECK(umdplay::chapter_at(ch, -1.0) == -1);   // before the first
    CHECK(umdplay::chapter_at(ch, 0.0) == 0);
    CHECK(umdplay::chapter_at(ch, 29.9) == 0);
    CHECK(umdplay::chapter_at(ch, 30.0) == 1);
    CHECK(umdplay::chapter_at(ch, 89.999) == 1);
    CHECK(umdplay::chapter_at(ch, 90.0) == 2);
    CHECK(umdplay::chapter_at(ch, 10000.0) == 3);
    std::vector<Ch> none;
    CHECK(umdplay::chapter_at(none, 5.0) == -1);

    // next: from before the first -> chapter 0; from a mid chapter -> the next.
    CHECK(umdplay::chapter_step(ch, -5.0, +1) == 0);
    CHECK(umdplay::chapter_step(ch, 0.0, +1) == 1);
    CHECK(umdplay::chapter_step(ch, 45.0, +1) == 2);
    CHECK(umdplay::chapter_step(ch, 300.0, +1) == -1);   // last chapter
    CHECK(umdplay::chapter_step(none, 0.0, +1) == -1);

    // prev: restarts the current chapter if we're past its start + eps, else
    // steps back one.
    CHECK(umdplay::chapter_step(ch, 45.0, -1) == 1);     // 15s into chapter 1 -> restart it
    CHECK(umdplay::chapter_step(ch, 30.5, -1) == 0);     // <eps into chapter 1 -> back to 0
    CHECK(umdplay::chapter_step(ch, 0.5, -1) == -1);     // <eps into chapter 0 -> nothing
    CHECK(umdplay::chapter_step(ch, 0.0, -1) == -1);
    CHECK(umdplay::chapter_step(none, 10.0, -1) == -1);

    // A custom restart epsilon is honoured.
    CHECK(umdplay::chapter_step(ch, 45.0, -1, 60.0) == 0);
}

// --- real disc (when present) --------------------------------------------

void test_real_disc() {
    const std::string iso = find_iso();
    if (iso.empty()) {
        std::printf("[skip] No ISO found (set UMD_ISO); synthetic cases passed.\n");
        return;
    }
    IsoReader reader(iso);
    if (!reader.open()) {
        std::printf("[skip] ISO present but unreadable; synthetic cases passed.\n");
        return;
    }

    // Root + asset discovery.
    CHECK(reader.root() != UmdRoot::None);
    const auto* pl_asset = reader.find_asset("PLAYLIST.UMD");
    if (!pl_asset) {
        std::printf("[skip] No PLAYLIST.UMD on this disc; synthetic cases passed.\n");
        return;
    }
    std::printf("[*] Content root: %s  assets=%zu\n",
                reader.root_name().c_str(), reader.assets().size());

    auto raw = reader.read_asset(*pl_asset);
    CHECK(!raw.empty());
    auto pl = umdplay::parse_playlist(raw.data(), raw.size());
    CHECK(pl.valid);
    CHECK(!pl.tracks.empty());
    std::printf("%s", umdplay::playlist_summary(pl).c_str());

    // The real disc has at least one track with chapter markers.
    size_t total_markers = 0;
    for (const auto& t : pl.tracks) total_markers += t.markers.size();
    CHECK(total_markers > 0);
    const auto* main = pl.main_track();
    CHECK(main != nullptr);
    if (main) {
        auto ch = pl.chapters(static_cast<int>(main - &pl.tracks[0]));
        std::printf("[*] main track '%s' clip=%s chapters=%zu\n",
                    main->name.c_str(), main->clip_name.c_str(), ch.size());
        CHECK(!ch.empty());
        // Navigation math over the real markers: from the start, "next" lands
        // on chapter 1; "prev" from just inside a chapter restarts it.
        if (ch.size() >= 2) {
            CHECK(umdplay::chapter_at(ch, ch[0].seconds) == 0);
            CHECK(umdplay::chapter_step(ch, ch[0].seconds, +1) == 1);
            const double mid = ch[1].seconds + 5.0;
            CHECK(umdplay::chapter_at(ch, mid) == 1);
            CHECK(umdplay::chapter_step(ch, mid, -1) == 1);   // restart current
        }
    }

    // CLIPINF/*.CLP assets exist and start with the CLPI0088 magic.
    auto clps = reader.assets_in("CLIPINF");
    CHECK(!clps.empty());
    if (!clps.empty()) {
        auto clp = reader.read_asset(clps.front());
        CHECK(clp.size() >= 8);
        CHECK(std::memcmp(clp.data(), "CLPI0088", 8) == 0);
    }

    // RESOURCE/*.RCO assets exist and probe cleanly.
    auto rcos = reader.assets_in("RESOURCE");
    if (!rcos.empty()) {
        auto rco_raw = reader.read_asset(rcos.front());
        auto r = umdplay::parse_rco(rco_raw.data(), rco_raw.size());
        CHECK(r.valid);
        std::printf("%s", umdplay::rco_summary(r).c_str());
    }
}

}  // namespace

int main() {
    test_playlist_synthetic();
    test_playlist_rejects();
    test_rco_synthetic();
    test_chapter_nav();
    test_real_disc();

    if (g_failures != 0) {
        std::printf("[-] %d playlist/rco check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All playlist + RCO tests passed!\n");
    return 0;
}
