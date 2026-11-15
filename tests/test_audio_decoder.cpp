// End-to-end check of the Sony PSMF ATRAC3+ audio path.
//
// If a real UMD Video ISO is available (UMD_ISO env var, else ../test_umd.iso)
// we open the main .MPS stream and decode a few seconds of audio, asserting the
// PSMF fallback produces non-silent PCM. Without an ISO the test still exercises
// the default-constructed decoder state so it is never a no-op failure.

#include "decoder.h"
#include "iso_reader.h"

#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

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

}  // namespace

// Compiled-out under NDEBUG, so use a macro that always evaluates.
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "CHECK failed: " #cond " (" << __FILE__ << ":"        \
                      << __LINE__ << ")\n";                                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

// Build a synthetic ATRAC3+ byte stream: N frames, each an 8-byte 0F D0 48 55
// sync followed by `stride-8` filler bytes, with optional larger gaps that
// simulate interleaved video packets. Returns the buffer.
static std::vector<uint8_t> synth_atrac3p(size_t stride, size_t frames,
                                          const std::vector<size_t>& extra_gaps) {
    std::vector<uint8_t> buf;
    auto push_frame = [&]() {
        buf.push_back(0x0F); buf.push_back(0xD0);
        buf.push_back(0x48); buf.push_back(0x55);
        buf.push_back(0x00); buf.push_back(0x00);
        buf.push_back(0x00); buf.push_back(0x00);
        for (size_t i = 8; i < stride; ++i) buf.push_back(0xA5);
    };
    for (size_t f = 0; f < frames; ++f) {
        push_frame();
        if (f < extra_gaps.size()) {
            for (size_t g = 0; g < extra_gaps[f]; ++g) buf.push_back(0x00);
        }
    }
    return buf;
}

int main() {
    // --- ATRAC3+ stride detection (always runs, no ISO needed) ---
    {
        // The common 696-byte stereo stride.
        auto s696 = synth_atrac3p(696, 20, {});
        CHECK(Decoder::detect_stride_from(s696.data(), s696.size(), 0) == 696);

        // A non-696 bitrate (e.g. a hypothetical 384-byte frame) must be read
        // from the data, not hardcoded.
        auto s384 = synth_atrac3p(384, 20, {});
        CHECK(Decoder::detect_stride_from(s384.data(), s384.size(), 696) == 384);

        // Interleaved large gaps (video packets) must not win the vote.
        auto noisy = synth_atrac3p(696, 20, {4096, 0, 2048, 0, 8000});
        CHECK(Decoder::detect_stride_from(noisy.data(), noisy.size(), 0) == 696);

        // Too little data -> fall back to the caller's default.
        uint8_t tiny[8] = {0x0F, 0xD0, 0x48, 0x55, 0, 0, 0, 0};
        CHECK(Decoder::detect_stride_from(tiny, sizeof(tiny), 696) == 696);
        std::cout << "[+] ATRAC3+ stride detection: 696 / 384 / noisy / fallback ok\n";
    }

    // --- Default-constructed state (always runs) ---
    Decoder fresh;
    CHECK(!fresh.has_video());
    CHECK(!fresh.has_audio());
    CHECK(fresh.get_duration() == 0.0);
    fresh.close();  // must be safe on an unopened decoder

    const std::string iso = find_iso();
    if (iso.empty()) {
        std::cout << "[skip] No ISO found (set UMD_ISO); state checks passed.\n";
        return 0;
    }

    IsoReader reader(iso);
    if (!reader.open()) {
        std::cout << "[skip] ISO present but unreadable; state checks passed.\n";
        return 0;
    }
    const auto entries = reader.get_mps_entries();
    if (entries.empty()) {
        std::cout << "[skip] No .MPS streams; state checks passed.\n";
        return 0;
    }

    // Pick the largest (main feature) stream.
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
    std::cout << "[*] Opened " << main_entry.name
              << "  fps=" << decoder.get_fps()
              << "  dur=" << decoder.get_duration() << "s\n";

    // Decode up to ~4 seconds worth of audio, interleaved with video frames.
    AVFrame* vframe = av_frame_alloc();
    std::vector<uint8_t> pcm;
    size_t total_pcm = 0;
    int nonzero = 0, video_frames = 0, audio_chunks = 0;
    const int max_video = 120;   // ~4s at 29.97fps

    while (video_frames < max_video) {
        pcm.clear();
        FrameType ft = decoder.read_frame(vframe, pcm);
        if (ft == FrameType::Eof) break;
        if (ft == FrameType::Video) {
            ++video_frames;
        } else if (ft == FrameType::Audio) {
            ++audio_chunks;
            total_pcm += pcm.size();
            for (uint8_t b : pcm) {
                if (b != 0) { ++nonzero; break; }
            }
        }
    }
    av_frame_free(&vframe);
    const bool had_audio = decoder.has_audio();
    decoder.close();

    std::cout << "[*] video frames=" << video_frames
              << "  audio chunks=" << audio_chunks
              << "  pcm bytes=" << total_pcm << "\n";

    CHECK(video_frames > 0);
    if (had_audio) {
        CHECK(audio_chunks > 0);
        CHECK(total_pcm > 0);
        CHECK(nonzero > 0);
        std::cout << "[+] ATRAC3+ audio decoded to non-silent PCM.\n";
    } else {
        std::cout << "[skip] Stream reports no audio track.\n";
    }

    std::cout << "All PSMF ATRAC3+ audio decoder tests passed!\n";
    return 0;
}
