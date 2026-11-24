// Unit tests for the native H.264 video path: the VideoDecoderBackend over
// third_party/h264dec, the in-memory deinterlacer and the YUV converter.
//
// The decoder test runs on a committed synthetic Annex-B stream (720x480 Main
// profile, CABAC, 11 frames) so CI needs no disc and no copyrighted media.
//
// CHECK always evaluates (assert() is compiled out under NDEBUG/Release).
#include "av_sync.h"
#include "deinterlace.h"
#include "video_decoder_backend.h"
#include "video_frame.h"
#include "yuv_convert.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

std::vector<uint8_t> load_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    if (n <= 0) return {};
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    in.read(reinterpret_cast<char*>(buf.data()), n);
    return buf;
}

// Split an Annex-B stream into access units: a new AU starts at an access unit
// delimiter, or at a slice whose first_mb_in_slice is 0 (the reference
// player's h264dectest does the same).
std::vector<std::pair<size_t, size_t>> split_access_units(const std::vector<uint8_t>& d) {
    std::vector<std::pair<size_t, size_t>> aus;
    long start = -1;
    for (size_t i = 0; i + 4 <= d.size(); ++i) {
        if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) continue;
        const int type = d[i + 3] & 0x1F;
        const bool au_start =
            type == 9 || ((type == 1 || type == 5) && (d[i + 4] & 0x80) != 0);
        if (au_start && start >= 0) {
            bool has_slice = false;
            for (size_t k = static_cast<size_t>(start); k + 3 < i; ++k) {
                if (d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1 &&
                    ((d[k + 3] & 0x1F) == 1 || (d[k + 3] & 0x1F) == 5)) {
                    has_slice = true;
                    break;
                }
            }
            if (has_slice) {
                aus.emplace_back(static_cast<size_t>(start), i - static_cast<size_t>(start));
                start = static_cast<long>(i);
            }
        }
        if (start < 0) start = static_cast<long>(i);
        i += 2;
    }
    if (start >= 0 && static_cast<size_t>(start) < d.size()) {
        aus.emplace_back(static_cast<size_t>(start), d.size() - static_cast<size_t>(start));
    }
    return aus;
}

// The demuxer hands Decoder an Annex-B SPS+PPS blob as extradata; build the
// same thing here so the priming path is covered without decoding a picture.
std::vector<uint8_t> extract_parameter_sets(const std::vector<uint8_t>& d) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 4 <= d.size(); ++i) {
        if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) continue;
        const int type = d[i + 3] & 0x1F;
        if (type != 7 && type != 8) continue;
        size_t end = i + 3;
        while (end + 3 < d.size()) {
            if (d[end] == 0 && d[end + 1] == 0 && d[end + 2] == 1) break;
            ++end;
        }
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), d.begin() + static_cast<std::ptrdiff_t>(i + 3),
                   d.begin() + static_cast<std::ptrdiff_t>(end));
        if (out.size() > 512) break;
    }
    return out;
}

double plane_variance(const umdplay::VideoFrame& f) {
    const size_t n = static_cast<size_t>(f.width) * f.height;
    if (!n) return 0.0;
    double sum = 0.0, sum2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double v = f.plane(0)[i];
        sum += v;
        sum2 += v * v;
    }
    const double mean = sum / static_cast<double>(n);
    return sum2 / static_cast<double>(n) - mean * mean;
}

uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// --- decoder ---------------------------------------------------------------

void test_native_decoder(const std::string& fixture) {
    const std::vector<uint8_t> stream = load_file(fixture);
    if (stream.empty()) {
        std::printf("[skip] H.264 fixture not found (%s)\n", fixture.c_str());
        return;
    }

    const auto aus = split_access_units(stream);
    CHECK(aus.size() >= 5);

    auto backend = umdplay::make_native_h264_backend();
    CHECK(backend != nullptr);
    if (!backend) return;

    CHECK(std::strcmp(backend->name(), "h264") == 0);
    CHECK(std::strcmp(backend->backend_name(), "h264dec") == 0);
    CHECK(backend->native());

    // Parameter sets travel inside the stream, but priming with them is what
    // Decoder does from the demuxer's extradata; both paths must work. Prime
    // with SPS+PPS only: re-feeding a slice would decode that picture twice.
    const std::vector<uint8_t> extradata = extract_parameter_sets(stream);
    CHECK(!extradata.empty());
    CHECK(backend->init(extradata.data(), extradata.size()));

    int frames = 0;
    double last_pts = -1.0;
    bool pts_monotonic = true;
    uint64_t first_hash = 0;
    for (size_t i = 0; i < aus.size(); ++i) {
        CHECK(backend->decode(stream.data() + aus[i].first, aus[i].second,
                              static_cast<int64_t>(i) * 3003));
        umdplay::VideoFrame f;
        while (backend->receive(f)) {
            CHECK(f.valid());
            CHECK(f.width == 720);
            CHECK(f.height == 480);
            CHECK(f.stride[0] == 720);
            CHECK(f.stride[1] == 360);
            CHECK(f.data.size() == 720u * 480u * 3u / 2u);
            CHECK(!f.interlaced);  // this fixture is progressive
            if (frames == 0) first_hash = fnv1a(f.data.data(), f.data.size());
            if (f.pts >= 0.0) {
                if (last_pts >= 0.0 && f.pts < last_pts - 1e-9) pts_monotonic = false;
                last_pts = f.pts;
            }
            ++frames;
        }
    }
    backend->flush();
    umdplay::VideoFrame f;
    while (backend->receive(f)) ++frames;

    std::printf("[*] native h264: %d access units -> %d frames, first hash %016llx\n",
                static_cast<int>(aus.size()), frames,
                static_cast<unsigned long long>(first_hash));
    CHECK(frames == 11);
    CHECK(pts_monotonic);
    CHECK(first_hash != 0);

    // Geometry is known from the SPS before any picture is decoded.
    CHECK(backend->width() == 720);
    CHECK(backend->height() == 480);
    CHECK(backend->par() > 0.0f);

    // A seek forgets everything; the next IDR decodes again.
    backend->reset();
    CHECK(backend->decode(stream.data() + aus.front().first, aus.front().second, 0));
    umdplay::VideoFrame again;
    bool got = backend->receive(again);
    for (int i = 1; i < 4 && !got; ++i) {
        backend->decode(stream.data() + aus[i].first, aus[i].second,
                        static_cast<int64_t>(i) * 3003);
        got = backend->receive(again);
    }
    CHECK(got);
    CHECK(again.valid());
    CHECK(plane_variance(again) > 1.0);  // a real picture, not a flat placeholder
}

// Truncated and corrupt NAL units must be rejected, never crash.
void test_decoder_bounds(const std::string& fixture) {
    const std::vector<uint8_t> stream = load_file(fixture);
    if (stream.empty()) {
        std::printf("[skip] H.264 fixture not found (bounds)\n");
        return;
    }
    auto backend = umdplay::make_native_h264_backend();
    CHECK(backend != nullptr);
    if (!backend) return;

    // Every truncation of the first access unit.
    const auto aus = split_access_units(stream);
    const size_t first_len = aus.empty() ? 0 : aus.front().second;
    for (size_t n = 1; n <= first_len; n += 7) {
        backend->decode(stream.data(), n, 0);
        umdplay::VideoFrame f;
        backend->receive(f);
    }
    backend->reset();

    // Empty and null inputs are refused, not dereferenced.
    CHECK(!backend->decode(nullptr, 0, 0));
    CHECK(!backend->decode(stream.data(), 0, 0));

    // Deterministic pseudo-random payloads at both UMD-ish sizes.
    uint32_t state = 0x12345678u;
    auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<uint8_t>(state >> 24);
    };
    for (size_t size : {size_t(688), size_t(1712)}) {
        for (int iter = 0; iter < 64; ++iter) {
            std::vector<uint8_t> junk(size);
            for (auto& b : junk) b = next();
            // Give it a start code half the time so the NAL path is exercised.
            if (iter % 2 == 0) {
                junk[0] = 0; junk[1] = 0; junk[2] = 1; junk[3] = 0x09;
            }
            backend->decode(junk.data(), junk.size(), 0);
            umdplay::VideoFrame f;
            backend->receive(f);
        }
    }
    backend->flush();
    umdplay::VideoFrame f;
    while (backend->receive(f)) { /* drain */ }
    backend->reset();
    std::printf("[+] decoder bounds: truncations + 128 junk payloads ok\n");
}

// --- deinterlacer ----------------------------------------------------------

void fill_plane(umdplay::VideoFrame& f, int p, uint8_t value) {
    const int w = p ? f.chroma_width() : f.width;
    const int h = p ? f.chroma_height() : f.height;
    for (int y = 0; y < h; ++y)
        std::memset(f.plane(p) + static_cast<size_t>(y) * f.stride[p], value, w);
}

void test_deinterlace() {
    // A woven frame: even lines from one field, odd lines from the other. The
    // blend must collapse the alternating pattern.
    umdplay::VideoFrame f;
    f.allocate(64, 48);
    for (int y = 0; y < f.height; ++y)
        std::memset(f.plane(0) + static_cast<size_t>(y) * f.stride[0],
                    (y & 1) ? 200 : 40, f.width);
    fill_plane(f, 1, 128);
    fill_plane(f, 2, 128);

    auto adjacent_delta = [](const umdplay::VideoFrame& v) {
        long total = 0;
        for (int y = 0; y + 1 < v.height; ++y) {
            const uint8_t* a = v.plane(0) + static_cast<size_t>(y) * v.stride[0];
            const uint8_t* b = v.plane(0) + static_cast<size_t>(y + 1) * v.stride[0];
            for (int x = 0; x < v.width; ++x)
                total += std::abs(static_cast<int>(a[x]) - static_cast<int>(b[x]));
        }
        return total;
    };
    const long before = adjacent_delta(f);
    umdplay::deinterlace_yuv420p(f);
    const long after = adjacent_delta(f);
    std::printf("[*] deinterlace: adjacent-line delta %ld -> %ld\n", before, after);
    CHECK(before > 0);
    CHECK(after * 4 < before);        // combing is gone
    CHECK(f.plane(0)[0] > 40);        // and the picture is not flattened to black

    // Chroma planes are blended too.
    umdplay::VideoFrame c;
    c.allocate(32, 32);
    fill_plane(c, 0, 128);
    for (int y = 0; y < c.chroma_height(); ++y)
        std::memset(c.plane(1) + static_cast<size_t>(y) * c.stride[1],
                    (y & 1) ? 60 : 200, c.chroma_width());
    fill_plane(c, 2, 128);
    umdplay::deinterlace_yuv420p(c);
    CHECK(c.plane(1)[0] > 60);
    CHECK(c.plane(1)[0] < 200);

    // --- Smart: motion-adaptive ------------------------------------------
    // A progressive frame with a hard horizontal edge (a cartoon line) must
    // come out bit-identical: that is the anti-ghosting guarantee.
    umdplay::VideoFrame sharp;
    sharp.allocate(64, 32);
    for (int y = 0; y < sharp.height; ++y)
        std::memset(sharp.plane(0) + static_cast<size_t>(y) * sharp.stride[0],
                    y < sharp.height / 2 ? 30 : 220, sharp.width);
    fill_plane(sharp, 1, 128);
    fill_plane(sharp, 2, 128);

    umdplay::VideoFrame blended = sharp;
    umdplay::deinterlace_yuv420p(sharp, umdplay::DeinterlaceMode::Smart);
    umdplay::deinterlace_yuv420p(blended, umdplay::DeinterlaceMode::Blend);
    bool smart_identical = true;
    for (int y = 0; y < sharp.height && smart_identical; ++y) {
        const uint8_t* a = sharp.plane(0) + static_cast<size_t>(y) * sharp.stride[0];
        for (int x = 0; x < sharp.width; ++x) {
            const uint8_t want = (y < sharp.height / 2) ? 30 : 220;
            if (a[x] != want) { smart_identical = false; break; }
        }
    }
    CHECK(smart_identical);
    CHECK(blended.plane(0)[static_cast<size_t>(sharp.height / 2) * blended.stride[0]] != 220);

    // The same Smart mode still repairs a woven frame (a moving edge does comb).
    umdplay::VideoFrame woven;
    woven.allocate(64, 48);
    for (int y = 0; y < woven.height; ++y)
        std::memset(woven.plane(0) + static_cast<size_t>(y) * woven.stride[0],
                    (y & 1) ? 200 : 40, woven.width);
    fill_plane(woven, 1, 128);
    fill_plane(woven, 2, 128);
    const long woven_before = adjacent_delta(woven);
    umdplay::deinterlace_yuv420p(woven, umdplay::DeinterlaceMode::Smart);
    const long woven_after = adjacent_delta(woven);
    std::printf("[*] smart deinterlace: comb delta %ld -> %ld\n", woven_before, woven_after);
    CHECK(woven_after * 4 < woven_before);

    // Off leaves the woven frame exactly as coded.
    umdplay::VideoFrame raw;
    raw.allocate(64, 48);
    for (int y = 0; y < raw.height; ++y)
        std::memset(raw.plane(0) + static_cast<size_t>(y) * raw.stride[0],
                    (y & 1) ? 200 : 40, raw.width);
    const long raw_before = adjacent_delta(raw);
    umdplay::deinterlace_yuv420p(raw, umdplay::DeinterlaceMode::Off);
    CHECK(adjacent_delta(raw) == raw_before);

    // A flat field has no comb to repair even in Smart mode.
    umdplay::VideoFrame flat;
    flat.allocate(32, 32);
    fill_plane(flat, 0, 128);
    fill_plane(flat, 1, 128);
    fill_plane(flat, 2, 128);
    umdplay::deinterlace_yuv420p(flat, umdplay::DeinterlaceMode::Smart);
    CHECK(flat.plane(0)[0] == 128);

    // Degenerate sizes are no-ops, not crashes.
    umdplay::VideoFrame tiny;
    tiny.allocate(2, 2);
    umdplay::deinterlace_yuv420p(tiny);
    umdplay::VideoFrame empty;
    umdplay::deinterlace_yuv420p(empty);
    umdplay::deinterlace_yuv420p(tiny, umdplay::DeinterlaceMode::Smart);
    umdplay::deinterlace_yuv420p(empty, umdplay::DeinterlaceMode::Off);
    std::printf("[+] deinterlace: combing removed, edges kept, odd sizes safe\n");
}

// --- A/V sync --------------------------------------------------------------

void test_av_sync() {
    const umdplay::AvSyncParams p;  // 20 ms early, 80 ms late

    // In step, slightly ahead (one refresh) and slightly behind all present.
    CHECK(umdplay::av_decide(10.000, 10.000, p) == umdplay::AvAction::Present);
    CHECK(umdplay::av_decide(10.015, 10.000, p) == umdplay::AvAction::Present);
    CHECK(umdplay::av_decide(9.950, 10.000, p) == umdplay::AvAction::Present);

    // Far ahead is held for a later tick; far behind is dropped.
    CHECK(umdplay::av_decide(10.300, 10.000, p) == umdplay::AvAction::Hold);
    CHECK(umdplay::av_decide(9.700, 10.000, p) == umdplay::AvAction::Drop);

    // Boundaries: exactly at the window edges still presents.
    CHECK(umdplay::av_decide(10.000 + p.early, 10.000, p) == umdplay::AvAction::Present);
    CHECK(umdplay::av_decide(10.000 - p.late, 10.000, p) == umdplay::AvAction::Present);

    // No PTS / no clock: trust decode order rather than stalling.
    CHECK(umdplay::av_decide(-1.0, 10.0, p) == umdplay::AvAction::Present);
    CHECK(umdplay::av_decide(10.0, -1.0, p) == umdplay::AvAction::Present);

    // The clock only moves with played audio.
    CHECK(umdplay::av_audio_clock(5.0, 0) == 5.0);
    CHECK(std::fabs(umdplay::av_audio_clock(5.0, 44100) - 6.0) < 1e-9);
    CHECK(std::fabs(umdplay::av_audio_clock(5.0, 22050) - 5.5) < 1e-9);
    CHECK(umdplay::av_audio_clock(-1.0, 1000) < 0.0);   // unanchored

    // A 90-minute stream stays exact in double precision.
    const double late = umdplay::av_audio_clock(0.0, 90 * 60 * 44100LL);
    CHECK(std::fabs(late - 5400.0) < 1e-6);
    std::printf("[+] av_sync: hold/drop/present windows and audio clock ok\n");
}

// --- YUV -> RGB ------------------------------------------------------------

void test_yuv_convert() {
    umdplay::VideoFrame f;
    f.allocate(16, 16);

    // BT.601 limited range: (16,128,128) black, (235,128,128) white.
    fill_plane(f, 0, 16);
    fill_plane(f, 1, 128);
    fill_plane(f, 2, 128);
    std::vector<uint8_t> rgb(static_cast<size_t>(f.width) * f.height * 3);
    umdplay::yuv420p_to_rgb24(f, rgb.data(), f.width * 3);
    CHECK(rgb[0] <= 1 && rgb[1] <= 1 && rgb[2] <= 1);

    fill_plane(f, 0, 235);
    umdplay::yuv420p_to_rgb24(f, rgb.data(), f.width * 3);
    CHECK(rgb[0] >= 254 && rgb[1] >= 254 && rgb[2] >= 254);

    // Red: Y=81, U=90, V=240.
    fill_plane(f, 0, 81);
    fill_plane(f, 1, 90);
    fill_plane(f, 2, 240);
    umdplay::yuv420p_to_rgb24(f, rgb.data(), f.width * 3);
    CHECK(rgb[0] >= 250 && rgb[1] <= 5 && rgb[2] <= 5);

    // 32-bit output: RGBA and BGRA differ only in the R/B order, alpha opaque.
    std::vector<uint8_t> rgba(static_cast<size_t>(f.width) * f.height * 4);
    umdplay::yuv420p_to_rgba32(f, rgba.data(), f.width * 4, false);
    CHECK(rgba[0] >= 250 && rgba[2] <= 5 && rgba[3] == 255);
    umdplay::yuv420p_to_rgba32(f, rgba.data(), f.width * 4, true);
    CHECK(rgba[2] >= 250 && rgba[0] <= 5 && rgba[3] == 255);

    // Null/invalid input is ignored.
    umdplay::yuv420p_to_rgb24(umdplay::VideoFrame{}, rgb.data(), 0);
    umdplay::yuv420p_to_rgb24(f, nullptr, 0);
    std::printf("[+] yuv420p -> RGB/RGBA conversion matches BT.601\n");
}

}  // namespace

int main() {
    const char* env = std::getenv("UMD_H264_FIXTURE");
    const std::string fixture =
        env ? env : "tests/fixtures/h264/testsrc2_720x480.h264";

    test_native_decoder(fixture);
    test_decoder_bounds(fixture);
    test_deinterlace();
    test_av_sync();
    test_yuv_convert();

    if (g_failures == 0) std::printf("[+] test_video_decoder: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
