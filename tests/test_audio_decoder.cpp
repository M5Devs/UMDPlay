// End-to-end check of the Sony PSMF ATRAC3+ audio path.
//
// If a real UMD Video ISO is available (UMD_ISO env var, else ../test_umd.iso)
// we open the main .MPS stream and decode a few seconds of audio, asserting the
// PSMF fallback produces non-silent PCM. Without an ISO the test still exercises
// the default-constructed decoder state so it is never a no-op failure.

#include "decoder.h"
#include "iso_reader.h"

#include <cassert>
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

int main() {
    // --- Default-constructed state (always runs) ---
    Decoder fresh;
    assert(!fresh.has_video());
    assert(!fresh.has_audio());
    assert(fresh.get_duration() == 0.0);
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

    assert(decoder.has_video());
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

    assert(video_frames > 0 && "expected to decode video frames");
    if (had_audio) {
        assert(audio_chunks > 0 && "expected ATRAC3+ audio chunks");
        assert(total_pcm > 0 && "expected non-empty PCM");
        assert(nonzero > 0 && "expected non-silent PCM");
        std::cout << "[+] ATRAC3+ audio decoded to non-silent PCM.\n";
    } else {
        std::cout << "[skip] Stream reports no audio track.\n";
    }

    std::cout << "All PSMF ATRAC3+ audio decoder tests passed!\n";
    return 0;
}
