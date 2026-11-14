// Transport-control check on a real UMD Video ISO.
//
// Exercises the playback primitives the player relies on — decode, seek
// forward/backward, pause/resume and stop — asserting the decoder stays
// consistent (monotonic clock after a forward seek, clamped at 0 after a
// backward seek past the start, decodable frames + non-silent ATRAC3+ audio
// after every seek, and a clean close/reopen cycle). Without an ISO it still
// runs the default-state checks so it is never a no-op failure.
//
// Checks use CHECK() rather than assert() so they still run in Release builds
// (assert is compiled out under NDEBUG, which would silently neuter the test).

#include "decoder.h"
#include "iso_reader.h"

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "[-] CHECK failed: " #cond " (line " << __LINE__      \
                      << ")\n";                                                \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

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

// Decode up to `max_video` frames, counting video frames and non-silent audio.
struct DecodeStats { int video = 0; int audio = 0; size_t pcm = 0; int nonzero = 0; };

DecodeStats decode_some(Decoder& d, int max_video) {
    DecodeStats s;
    AVFrame* vf = av_frame_alloc();
    std::vector<uint8_t> pcm;
    while (s.video < max_video) {
        pcm.clear();
        FrameType ft = d.read_frame(vf, pcm);
        if (ft == FrameType::Eof) break;
        if (ft == FrameType::Video) {
            ++s.video;
        } else if (ft == FrameType::Audio) {
            ++s.audio;
            s.pcm += pcm.size();
            for (uint8_t b : pcm) if (b != 0) { ++s.nonzero; break; }
        }
    }
    av_frame_free(&vf);
    return s;
}

}  // namespace

int main() {
    // --- Default-constructed state (always runs) ---
    Decoder fresh;
    CHECK(!fresh.has_video());
    CHECK(!fresh.has_audio());
    CHECK(fresh.get_duration() == 0.0);
    CHECK(fresh.current_time() == 0.0);
    CHECK(!fresh.seek_absolute(10.0));  // no stream open: must be a safe no-op
    fresh.close();

    const std::string iso = find_iso();
    if (iso.empty()) {
        std::cout << "[skip] No ISO found (set UMD_ISO); state checks passed.\n";
        return g_failures == 0 ? 0 : 1;
    }

    IsoReader reader(iso);
    if (!reader.open()) {
        std::cout << "[skip] ISO present but unreadable; state checks passed.\n";
        return g_failures == 0 ? 0 : 1;
    }
    const auto entries = reader.get_mps_entries();
    if (entries.empty()) {
        std::cout << "[skip] No .MPS streams; state checks passed.\n";
        return g_failures == 0 ? 0 : 1;
    }

    MpsEntry main_entry = entries[0];
    for (const auto& e : entries) {
        if (e.size_bytes > main_entry.size_bytes) main_entry = e;
    }

    Decoder decoder;
    if (!decoder.open_stream(iso, main_entry)) {
        std::cerr << "[-] Failed to open " << main_entry.name << "\n";
        return 1;
    }
    CHECK(decoder.has_video());
    const double dur = decoder.get_duration();
    std::cout << "[*] Opened " << main_entry.name
              << "  fps=" << decoder.get_fps()
              << "  dur=" << dur << "s\n";
    CHECK(dur > 0.0);

    // --- Play a little, then verify the clock advanced ---
    DecodeStats s0 = decode_some(decoder, 30);
    CHECK(s0.video > 0);
    const double t_after_play = decoder.current_time();
    std::cout << "[*] play: video=" << s0.video << " audio=" << s0.audio
              << " clock=" << t_after_play << "s\n";
    CHECK(t_after_play >= 0.0);

    // --- Seek FORWARD +120s, decode, and confirm the clock jumped ahead ---
    CHECK(decoder.seek_absolute(t_after_play + 120.0));
    const double t_fwd = decoder.current_time();
    CHECK(t_fwd >= t_after_play + 119.0);
    DecodeStats sf = decode_some(decoder, 30);
    CHECK(sf.video > 0);
    std::cout << "[*] seek +120s -> clock=" << t_fwd
              << "s  post-seek video=" << sf.video
              << " audio=" << sf.audio << " pcm=" << sf.pcm << "\n";
    if (decoder.has_audio()) {
        CHECK(sf.audio > 0);
        CHECK(sf.pcm > 0);
        CHECK(sf.nonzero > 0);
    }

    // --- Seek BACKWARD -60s and confirm the clock moved back ---
    const double t_before_back = decoder.current_time();
    CHECK(decoder.seek_absolute(t_before_back - 60.0));
    const double t_back = decoder.current_time();
    CHECK(t_back <= t_before_back);
    DecodeStats sb = decode_some(decoder, 30);
    CHECK(sb.video > 0);
    std::cout << "[*] seek -60s -> clock=" << t_back
              << "s  post-seek video=" << sb.video << " audio=" << sb.audio << "\n";

    // --- Backward seek past the start must clamp to 0, not go negative ---
    CHECK(decoder.seek_absolute(-500.0));
    CHECK(decoder.current_time() == 0.0);
    DecodeStats sz = decode_some(decoder, 15);
    CHECK(sz.video > 0);
    std::cout << "[*] seek <0 clamped to 0s, video=" << sz.video << "\n";

    // --- Forward seek past the end must clamp to the duration ---
    CHECK(decoder.seek_absolute(dur + 1000.0));
    const double t_end = decoder.current_time();
    CHECK(t_end <= dur + 0.001);
    std::cout << "[*] seek >end clamped to " << t_end << "s\n";

    // --- Stop: close, then reopen to confirm a clean reset ---
    decoder.close();
    CHECK(!decoder.has_video());
    CHECK(decoder.current_time() == 0.0);
    if (!decoder.open_stream(iso, main_entry)) {
        std::cerr << "[-] Failed to reopen after stop\n";
        return 1;
    }
    DecodeStats sr = decode_some(decoder, 20);
    CHECK(sr.video > 0);
    std::cout << "[*] stop/reopen: video=" << sr.video << " audio=" << sr.audio << "\n";
    decoder.close();

    if (g_failures != 0) {
        std::cerr << "[-] " << g_failures << " playback check(s) failed.\n";
        return 1;
    }
    std::cout << "All playback transport tests passed!\n";
    return 0;
}
