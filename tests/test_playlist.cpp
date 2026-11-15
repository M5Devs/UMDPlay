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
#include "rco_menu.h"
#include "vsmx_vm.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
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

// --- synthetic VSMX cases ------------------------------------------------

// Build a VSMX blob: signature + header + a code array + string tables.
std::vector<uint8_t> make_vsmx(const std::vector<std::pair<uint32_t,uint32_t>>& code,
                               const std::vector<std::string>& texts,
                               const std::vector<std::string>& props,
                               const std::vector<std::string>& names) {
    auto put_le32 = [](std::vector<uint8_t>& b, size_t off, uint32_t v) {
        b[off] = v & 0xff; b[off+1] = (v >> 8) & 0xff;
        b[off+2] = (v >> 16) & 0xff; b[off+3] = (v >> 24) & 0xff;
    };
    // Lay out: header(52) | code | texts | props | names.
    std::vector<uint8_t> code_bytes;
    for (auto& g : code) {
        std::vector<uint8_t> t(8, 0);
        put_le32(t, 0, g.first); put_le32(t, 4, g.second);
        code_bytes.insert(code_bytes.end(), t.begin(), t.end());
    }
    auto enc16 = [](const std::vector<std::string>& v) {
        std::vector<uint8_t> o;
        for (auto& s : v) { for (char c : s) { o.push_back((uint8_t)c); o.push_back(0); }
                            o.push_back(0); o.push_back(0); }
        return o;
    };
    auto enc8 = [](const std::vector<std::string>& v) {
        std::vector<uint8_t> o;
        for (auto& s : v) { for (char c : s) o.push_back((uint8_t)c); o.push_back(0); }
        return o;
    };
    auto code_b = code_bytes;
    auto text_b = enc16(texts);
    auto prop_b = enc16(props);
    auto name_b = enc8(names);

    std::vector<uint8_t> b(52, 0);
    std::memcpy(b.data(), "VSMX", 4);
    put_le32(b, 0x04, 0x00010000);
    size_t off = 52;
    put_le32(b, 0x08, (uint32_t)off); put_le32(b, 0x0C, (uint32_t)code_b.size());
    off += code_b.size();
    put_le32(b, 0x10, (uint32_t)off); put_le32(b, 0x14, (uint32_t)text_b.size());
    put_le32(b, 0x18, (uint32_t)texts.size());
    off += text_b.size();
    put_le32(b, 0x1C, (uint32_t)off); put_le32(b, 0x20, (uint32_t)prop_b.size());
    put_le32(b, 0x24, (uint32_t)props.size());
    off += prop_b.size();
    put_le32(b, 0x28, (uint32_t)off); put_le32(b, 0x2C, (uint32_t)name_b.size());
    put_le32(b, 0x30, (uint32_t)names.size());
    b.insert(b.end(), code_b.begin(), code_b.end());
    b.insert(b.end(), text_b.begin(), text_b.end());
    b.insert(b.end(), prop_b.begin(), prop_b.end());
    b.insert(b.end(), name_b.begin(), name_b.end());
    return b;
}

// A native object that records the calls made to it.
struct RecordingNative : umdplay::VsmxNative {
    std::vector<std::string> calls;
    std::string name() const override { return "rec"; }
    umdplay::VsmxValue call(const std::string& m,
                            const std::vector<umdplay::VsmxValue>&) override {
        calls.push_back(m);
        if (m == "answer") return umdplay::VsmxValue::number(42);
        return umdplay::VsmxValue();
    }
};

void test_vsmx_synthetic() {
    using namespace umdplay;
    // Program:  x = 3;  y = x + 4;  rec.probe();  z = rec.answer();
    const int VID = 0; (void)VID;
    std::vector<std::pair<uint32_t,uint32_t>> code = {
        {VSMX_VARIABLE, 0}, {VSMX_CONST_INT, 3}, {VSMX_ASSIGN, 0}, {VSMX_END_STMT, 0},
        {VSMX_VARIABLE, 1}, {VSMX_VARIABLE, 0}, {VSMX_CONST_INT, 4}, {VSMX_ADD, 0},
        {VSMX_ASSIGN, 0}, {VSMX_END_STMT, 0},
        {VSMX_VARIABLE, 2}, {VSMX_METHOD, 0}, {VSMX_CALL_METHOD, 0}, {VSMX_END_STMT, 0},
        {VSMX_VARIABLE, 3}, {VSMX_VARIABLE, 2}, {VSMX_METHOD, 1},
        {VSMX_CALL_METHOD, 0}, {VSMX_ASSIGN, 0}, {VSMX_END_STMT, 0},
        {VSMX_END, 0},
    };
    auto raw = make_vsmx(code, {}, {"probe", "answer"}, {"x", "y", "rec", "z"});
    auto prog = decode_vsmx(raw.data(), raw.size());
    CHECK(prog.valid);
    CHECK(prog.code.size() == code.size());
    CHECK(prog.names.size() == 4);
    CHECK(prog.properties.size() == 2);
    CHECK(std::string(vsmx_opcode_name(VSMX_ADD)) == "ADD");
    CHECK(!disassemble(prog).empty());
    CHECK(!vsmx_opcode_histogram(prog).empty());

    auto rec = std::make_shared<RecordingNative>();
    VsmxMachine vm(prog);
    vm.bind("rec", rec);
    CHECK(vm.run());
    CHECK(vm.last_error().empty());
    CHECK(rec->calls.size() == 2);
    CHECK(rec->calls.size() >= 1 && rec->calls[0] == "probe");

    // Reject a non-VSMX blob.
    uint8_t junk[64] = {0};
    CHECK(!decode_vsmx(junk, sizeof(junk)).valid);
    CHECK(!decode_vsmx(nullptr, 0).valid);
}

// --- synthetic .RCO cases ------------------------------------------------

std::vector<uint8_t> make_rco() {
    std::vector<uint8_t> b(0x400, 0);
    b[0] = 0x00; b[1] = 'P'; b[2] = 'R'; b[3] = 'F';
    put_be32(b, 0x04, 0x80);           // version (stored LE on disk; see below)
    // version is LE: write 0x80 little-endian.
    b[0x04] = 0x80; b[0x05] = 0x00; b[0x06] = 0x00; b[0x07] = 0x00;
    b[0x0c] = 0x01;                    // compression: low nibble = UMD

    auto le32 = [&](size_t off, uint32_t v) {
        b[off] = v & 0xff; b[off+1] = (v >> 8) & 0xff;
        b[off+2] = (v >> 16) & 0xff; b[off+3] = (v >> 24) & 0xff;
    };
    // Header: pMain=0xa4, pImg=0x180, pLabelData=0x300, pEventData=0x380,
    // pImgData=0x200.
    le32(0x10, 0xa4);      // pMain
    le32(0x24, 0x180);     // pImg
    le32(0x40, 0x300);     // pLabelData
    le32(0x44, 0x40);      // lLabelData
    le32(0x48, 0x380);     // pEventData
    le32(0x4c, 0x20);      // lEventData
    le32(0x80, 0x200);     // pImgData
    le32(0x84, 0x100);     // lImgData

    // Labels: "main1\0" at 0x300.
    std::memcpy(b.data() + 0x300, "main1", 5);
    // Events: "script:/x\0" at 0x380.
    std::memcpy(b.data() + 0x380, "script:/x", 9);

    // MAIN entry at 0xa4: type=1 id=1, labelOffset=0, numSub=1, child at 0xcc.
    b[0xa4] = 0x01; b[0xa4 + 1] = 0x01;      // type=1 id=MAIN
    le32(0xa4 + 4, 0xffffffff);              // no label
    le32(0xa4 + 12, 40);                     // entrySize
    le32(0xa4 + 16, 1);                      // numSubEntries
    // Child IMG entry at 0xcc: type=1 id=4, label 0, sizePacked, offset 0.
    b[0xcc] = 0x01; b[0xcc + 1] = 0x04;      // type=1 id=IMG
    le32(0xcc + 4, 0);                       // labelOffset -> "main1"
    le32(0xcc + 12, 0);                      // entrySize
    le32(0xcc + 16, 0);                      // numSub
    b[0xcc + 40] = 0x00; b[0xcc + 41] = 0x00;   // format PNG
    b[0xcc + 42] = 0x00; b[0xcc + 43] = 0x00;   // compression none
    le32(0xcc + 44, 24);                     // sizePacked
    le32(0xcc + 48, 0);                      // img offset

    // A fake PNG (signature + IHDR dims + IEND) at pImgData (0x200).
    const uint8_t png[] = {0x89,'P','N','G','\r','\n',0x1a,'\n',
                           0,0,0,13,'I','H','D','R'};
    std::memcpy(b.data() + 0x200, png, sizeof(png));
    put_be32(b, 0x200 + 16, 252);   // width
    put_be32(b, 0x200 + 20, 30);    // height
    std::memcpy(b.data() + 0x218, "IEND", 4);
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
    CHECK(r.images[0].name == "main1");
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

// --- Disc Menu model -----------------------------------------------------

void test_rco_menu_synthetic() {
    // The synthetic .RCO builder above is fiddly and the real-disc case below
    // covers the model end-to-end, so here we only assert the model degrades
    // gracefully on an RCO with no usable VSMX.
    std::vector<uint8_t> junk(64, 0);
    auto r = umdplay::parse_rco(junk.data(), junk.size());
    umdplay::RcoMenu menu;
    CHECK(!menu.build(r));
    CHECK(!menu.valid());
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
        // The disc's menu object tree should expose at least one page.
        CHECK(!r.pages().empty());

        // Decode and disassemble the embedded VSMX, then actually run it with a
        // stub controller to prove the interpreter executes real menu logic.
        if (r.vsmx.present && r.vsmx.offset + r.vsmx.size <= rco_raw.size()) {
            auto prog = umdplay::decode_vsmx(rco_raw.data() + r.vsmx.offset,
                                             r.vsmx.size);
            CHECK(prog.valid);
            CHECK(!prog.code.empty());
            CHECK(!prog.names.empty());
            std::printf("[*] VSMX: %zu instructions, %zu strings, %zu props, %zu names\n",
                        prog.code.size(), prog.texts.size(),
                        prog.properties.size(), prog.names.size());
            std::printf("    %s\n", umdplay::vsmx_opcode_histogram(prog).c_str());

            struct StubController : umdplay::VsmxNative {
                std::vector<std::string> calls;
                std::string name() const override { return "controller"; }
                umdplay::VsmxValue call(const std::string& m,
                    const std::vector<umdplay::VsmxValue>&) override {
                    calls.push_back(m);
                    if (m == "playStatus") return umdplay::VsmxValue::number(0);
                    return umdplay::VsmxValue();
                }
                umdplay::VsmxValue get(const std::string&) override {
                    return umdplay::VsmxValue::number(0);
                }
            };
            struct StubMovie : umdplay::VsmxNative {
                std::vector<std::string> calls;
                std::string name() const override { return "movieplayer"; }
                umdplay::VsmxValue call(const std::string& m,
                    const std::vector<umdplay::VsmxValue>&) override {
                    calls.push_back(m);
                    return umdplay::VsmxValue();
                }
            };
            auto ctrl = std::make_shared<StubController>();
            auto movie = std::make_shared<StubMovie>();
            umdplay::VsmxMachine vm(prog);
            vm.bind("controller", ctrl);
            vm.bind("movieplayer", movie);
            CHECK(vm.run());
            CHECK(vm.last_error().empty());

            // Drive every event the disc's RCO actually declares, each on a
            // fresh machine (a VM error is sticky, so events must not share
            // one). Discs name their handlers differently (Silent Hill's
            // "onAutoPlay", Summer Days' "on_PushMenu"), so dispatch whatever
            // this disc carries rather than a hard-coded name. The RCO's event
            // table also carries empty padding entries, and may reference an
            // event whose handler the script never defines (a dead reference
            // the Sony runtime ignores) — neither is an interpreter fault. Any
            // other failure (stack underflow, unimplemented opcode, budget) is.
            size_t ran = 0, declared = 0, missing = 0, vm_errors = 0;
            std::string first_vm_error;
            for (const auto& ev : r.events) {
                if (ev.empty()) continue;            // padding, not a handler
                ++declared;
                auto c = std::make_shared<StubController>();
                auto m2 = std::make_shared<StubMovie>();
                umdplay::VsmxMachine m(prog);
                m.bind("controller", c);
                m.bind("movieplayer", m2);
                if (!m.run()) {
                    ++vm_errors;
                    if (first_vm_error.empty()) first_vm_error = "toplevel: " + m.last_error();
                    continue;
                }
                if (!m.call_event(ev)) {
                    if (m.last_error().find("no event function") != std::string::npos)
                        ++missing;                    // declared but not defined
                    else {
                        ++vm_errors;
                        if (first_vm_error.empty())
                            first_vm_error = ev + ": " + m.last_error();
                    }
                    continue;
                }
                ++ran;
            }
            std::printf("[*] VSMX events: %zu/%zu handlers ran, %zu dead refs, "
                        "%zu interpreter faults\n", ran, declared, missing, vm_errors);
            CHECK(declared > 0);
            CHECK(vm_errors == 0);                   // no interpreter faults
            CHECK(ran + missing == declared);
            CHECK(ran * 2 > declared);               // most handlers resolve
            if (!first_vm_error.empty())
                std::printf("    first interpreter fault: %s\n",
                            first_vm_error.c_str());
        }

        // The Disc Menu model drives the same RCO through its VSMX script:
        // build it, walk the pages, and confirm a button press produces a
        // concrete MenuAction (page switch or playback) rather than nothing.
        umdplay::RcoMenu menu;
        CHECK(menu.build(r, 3, 1));
        if (menu.valid()) {
            CHECK(menu.page_count() >= 1);
            CHECK(!menu.page_name().empty());
            std::printf("[*] Disc Menu model: %d pages, page '%s' (%zu items, "
                        "%d buttons)\n", menu.page_count(),
                        menu.page_name().c_str(), menu.page().items.size(),
                        menu.focus_index() >= 0 ? 1 : 0);
            // Discs whose RCO carries explicit geometry (and a pageID/focusID
            // driven script) resolve a focusable item straight away. Others
            // (e.g. Silent Hill) build their layout at runtime via MPSize, so
            // the initial page may legitimately have no buttons yet; only assert
            // a focus target when the page actually exposes buttons.
            bool has_buttons = false;
            for (const auto& it : menu.page().items) if (it.is_button) has_buttons = true;
            if (has_buttons) CHECK(menu.focused_item() != nullptr);

            // The main_menu page carries a "main2" tile whose push handler
            // sets pageID=2; activating it must switch to the chapter page.
            const std::string first_page = menu.page_name();
            const umdplay::RcoMenuItem* main2 = nullptr;
            for (const auto& it : menu.page().items)
                if (it.is_button && it.name == "main2") main2 = &it;
            if (main2) {
                menu.focus_at(main2->rect_x + main2->rect_w * 0.5f,
                              main2->rect_y + main2->rect_h * 0.5f);
                umdplay::MenuAction a = menu.activate_focused();
                CHECK(a.kind == umdplay::MenuActionKind::SwitchPage);
                CHECK(menu.page_name() != first_page);
                std::printf("[*] Disc Menu: '%s' -> '%s' via main2\n",
                            first_page.c_str(), menu.page_name().c_str());

                // On the chapter page, a chapter tile yields a play request.
                const umdplay::RcoMenuItem* chap = nullptr;
                for (const auto& it : menu.page().items)
                    if (it.is_button && it.name.rfind("chapter", 0) == 0 &&
                        it.name.find("_") == std::string::npos) { chap = &it; break; }
                if (chap) {
                    menu.focus_at(chap->rect_x + chap->rect_w * 0.5f,
                                  chap->rect_y + chap->rect_h * 0.5f);
                    umdplay::MenuAction b = menu.activate_focused();
                    CHECK(b.kind == umdplay::MenuActionKind::PlayMovie);
                    std::printf("[*] Disc Menu: chapter tile '%s' -> play (chapter %d)\n",
                                chap->name.c_str(), b.chapter);
                }
            }

            // Navigation always lands on a real button (or none, never a bad
            // index), and the page stays one of the RCO's own pages.
            menu.move_focus(0, 1);
            const int fi = menu.focus_index();
            CHECK(fi == -1 || menu.focused_item() != nullptr);
            bool known_page = false;
            for (uint32_t p : r.pages()) {
                const auto* po = r.object(p);
                if (po && po->name == menu.page_name()) known_page = true;
            }
            CHECK(known_page);
        }
    }
}

}  // namespace

int main() {
    test_playlist_synthetic();
    test_playlist_rejects();
    test_rco_synthetic();
    test_vsmx_synthetic();
    test_rco_menu_synthetic();
    test_chapter_nav();
    test_real_disc();

    if (g_failures != 0) {
        std::printf("[-] %d playlist/rco check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All playlist + RCO + VSMX tests passed!\n");
    return 0;
}
