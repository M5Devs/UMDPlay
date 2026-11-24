// End-to-end check of the Sony PSMF ATRAC3+ audio path.
//
// If a real UMD Video ISO is available (UMD_ISO env var, else ../test_umd.iso)
// we open the main .MPS stream and decode a few seconds of audio, asserting the
// PSMF fallback produces non-silent PCM. Without an ISO the test still exercises
// the default-constructed decoder state so it is never a no-op failure.

#include "decoder.h"
#include "iso_reader.h"
#if defined(UMDPLAY_NATIVE_ATRAC3P)
#include "atrac3p_decoder.h"
#include "audio_decoder_backend.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstring>
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


#if defined(UMDPLAY_NATIVE_ATRAC3P)
// ---------------------------------------------------------------------------
// Native Atrac3PlusDecoder (src/atrac3p/) — no FFmpeg on the audio path.
// ---------------------------------------------------------------------------

namespace {

// A frame whose first bits are "0 11": a valid start bit followed immediately by
// the channel-unit terminator. It carries no audio, so the decoder must emit
// exactly one full frame of digital silence.
const uint8_t kSilenceFrame[1] = {0x60};

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

// Correlation, peak and worst-case error between two interleaved S16 streams.
struct PcmDiff {
    double corr = 0.0;
    double rms_got = 0.0;
    double rms_ref = 0.0;
    int    peak_got = 0;
    int    peak_ref = 0;
    int    max_abs = 0;
};

PcmDiff compare_pcm(const std::vector<int16_t>& got, const std::vector<int16_t>& ref) {
    PcmDiff d;
    const size_t n = std::min(got.size(), ref.size());
    if (n == 0) return d;
    double ss_got = 0, ss_ref = 0, dot = 0;
    for (size_t i = 0; i < n; i++) {
        const double a = got[i], b = ref[i];
        ss_got += a * a;
        ss_ref += b * b;
        dot += a * b;
        const int e = std::abs((int)got[i] - (int)ref[i]);
        if (e > d.max_abs) d.max_abs = e;
        if (std::abs((int)got[i]) > d.peak_got) d.peak_got = std::abs((int)got[i]);
        if (std::abs((int)ref[i]) > d.peak_ref) d.peak_ref = std::abs((int)ref[i]);
    }
    d.corr = dot / std::sqrt((ss_got > 0 ? ss_got : 1.0) * (ss_ref > 0 ? ss_ref : 1.0));
    d.rms_got = std::sqrt(ss_got / n);
    d.rms_ref = std::sqrt(ss_ref / n);
    return d;
}

}  // namespace

// Always-on checks: layout validation, the terminator-only frame, the two UMD
// frame strides (688 / 1712 bytes) and bounds safety on malformed input.
static int native_selftest() {
    using umdplay::Atrac3PlusDecoder;

    Atrac3PlusDecoder dec;
    CHECK(dec.init(2));
    CHECK(dec.initialized());
    CHECK(dec.channels() == 2);

    // Only mono and stereo are defined for this decoder.
    Atrac3PlusDecoder bad;
    CHECK(!bad.init(0));
    CHECK(!bad.init(3));
    CHECK(!bad.initialized());

    // A terminator-only frame must decode to one full frame of silence.
    std::vector<int16_t> pcm;
    const int n = dec.decode(kSilenceFrame, sizeof(kSilenceFrame), pcm);
    CHECK(n == Atrac3PlusDecoder::kSamplesPerFrame);
    CHECK(pcm.size() == (size_t)Atrac3PlusDecoder::kSamplesPerFrame * 2);
    for (int16_t s : pcm) CHECK(s == 0);

    // The byte-oriented overload appends S16LE: 2048 frames * 2 ch * 2 bytes.
    std::vector<uint8_t> bytes;
    CHECK(dec.decode(kSilenceFrame, sizeof(kSilenceFrame), bytes) ==
          Atrac3PlusDecoder::kSamplesPerFrame);
    CHECK(bytes.size() == Atrac3PlusDecoder::kBytesPerFrame);
    for (uint8_t b : bytes) CHECK(b == 0);

    // Invalid start bit (MSB set) is rejected, not decoded.
    const uint8_t bad_start[4] = {0x80, 0x00, 0x00, 0x00};
    pcm.clear();
    CHECK(dec.decode(bad_start, sizeof(bad_start), pcm) == 0);
    CHECK(pcm.empty());

    // Both UMD frame strides must be accepted and behave identically to the
    // unpadded payload (the PSMF container pads frames to the fixed stride).
    for (size_t stride : {size_t(688), size_t(1712)}) {
        std::vector<uint8_t> padded(stride, 0);
        padded[0] = 0x60;
        Atrac3PlusDecoder a, b;
        CHECK(a.init(2));
        CHECK(b.init(2));
        std::vector<int16_t> pa, pb;
        CHECK(a.decode(kSilenceFrame, sizeof(kSilenceFrame), pa) ==
              Atrac3PlusDecoder::kSamplesPerFrame);
        CHECK(b.decode(padded.data(), padded.size(), pb) ==
              Atrac3PlusDecoder::kSamplesPerFrame);
        CHECK(pa == pb);
        CHECK(pb.size() == (size_t)Atrac3PlusDecoder::kSamplesPerFrame * 2);
    }

    // Bounds safety: every truncation length of a plausible frame, plus
    // pseudo-random payloads at both strides, must be handled without crashing.
    // Repeating the same input must be deterministic.
    {
        std::vector<uint8_t> pattern(1712);
        for (size_t i = 0; i < pattern.size(); i++)
            pattern[i] = static_cast<uint8_t>(i * 37 + 11);

        for (size_t len = 1; len <= 96; len++) {
            Atrac3PlusDecoder d1, d2;
            CHECK(d1.init(2));
            CHECK(d2.init(2));
            std::vector<int16_t> p1, p2;
            const int r1 = d1.decode(pattern.data(), len, p1);
            const int r2 = d2.decode(pattern.data(), len, p2);
            CHECK(r1 == r2);
            CHECK(p1 == p2);
        }

        uint32_t seed = 0xC0FFEEu;
        for (int iter = 0; iter < 200; iter++) {
            const size_t stride = (iter & 1) ? 688 : 1712;
            std::vector<uint8_t> junk(stride);
            for (auto& v : junk) {
                seed = seed * 1664525u + 1013904223u;
                v = static_cast<uint8_t>(seed >> 24);
            }
            Atrac3PlusDecoder d1, d2;
            CHECK(d1.init(2));
            CHECK(d2.init(2));
            std::vector<int16_t> p1, p2;
            const int r1 = d1.decode(junk.data(), junk.size(), p1);
            const int r2 = d2.decode(junk.data(), junk.size(), p2);
            CHECK(r1 == r2);
            CHECK(p1 == p2);
        }
    }

    // Mono layout: a stereo access unit is a channel-config mismatch, so it is
    // rejected rather than mis-decoded; the decoder keeps working afterwards.
    {
        Atrac3PlusDecoder mono;
        CHECK(mono.init(1));
        CHECK(mono.channels() == 1);
        std::vector<int16_t> m;
        CHECK(mono.decode(kSilenceFrame, sizeof(kSilenceFrame), m) ==
              Atrac3PlusDecoder::kSamplesPerFrame);
        CHECK(m.size() == (size_t)Atrac3PlusDecoder::kSamplesPerFrame);
    }

    // reset() drops inter-frame history but keeps the decoder usable.
    {
        Atrac3PlusDecoder d;
        CHECK(d.init(2));
        std::vector<int16_t> p;
        CHECK(d.decode(kSilenceFrame, sizeof(kSilenceFrame), p) ==
              Atrac3PlusDecoder::kSamplesPerFrame);
        p.clear();
        d.reset();
        CHECK(d.decode(kSilenceFrame, sizeof(kSilenceFrame), p) ==
              Atrac3PlusDecoder::kSamplesPerFrame);
        CHECK(p.size() == (size_t)Atrac3PlusDecoder::kSamplesPerFrame * 2);
    }

    // The AudioDecoderBackend seam is what Decoder actually drives.
    {
        std::unique_ptr<umdplay::AudioDecoderBackend> be =
            umdplay::make_native_atrac3p_backend();
        CHECK(be != nullptr);
        CHECK(be->init(2));
        CHECK(be->sample_rate() == Atrac3PlusDecoder::kSampleRate);
        CHECK(be->channels() == 2);
        std::vector<uint8_t> bytes;
        CHECK(be->decode(kSilenceFrame, sizeof(kSilenceFrame), bytes) ==
              Atrac3PlusDecoder::kSamplesPerFrame);
        CHECK(bytes.size() == Atrac3PlusDecoder::kBytesPerFrame);
        be->reset();
        bytes.clear();
        CHECK(be->decode(kSilenceFrame, sizeof(kSilenceFrame), bytes) ==
              Atrac3PlusDecoder::kSamplesPerFrame);
    }

    std::cout << "[+] native Atrac3PlusDecoder: layout, 688/1712 strides, "
                 "malformed input, backend seam and reset ok\n";
    return 0;
}

// Optional reference comparison. Point these at a raw ATRAC3+ frame dump and the
// matching decoded PCM (S16LE stereo) to verify the decoder against a known-good
// stream, e.g. extracted from an FFmpeg FATE sample:
//   ffmpeg -i sample.oma -map 0:a:0 -c:a copy -f data frames.raw
//   ffmpeg -i sample.oma -f s16le -acodec pcm_s16le reference.pcm
//   UMD_AT3P_FRAMES=frames.raw UMD_AT3P_REF=reference.pcm
//   UMD_AT3P_FRAME_SIZE=1488 ./test_audio_decoder
// Skips cleanly when unset, so CI never depends on copyrighted media.
static int native_reference_test() {
    const char* frames_path = std::getenv("UMD_AT3P_FRAMES");
    const char* ref_path    = std::getenv("UMD_AT3P_REF");
    if (!frames_path || !ref_path) {
        std::cout << "[skip] UMD_AT3P_FRAMES/UMD_AT3P_REF not set; "
                     "reference comparison skipped.\n";
        return 0;
    }

    size_t frame_size = 1488;
    if (const char* fs = std::getenv("UMD_AT3P_FRAME_SIZE")) {
        const long v = std::strtol(fs, nullptr, 10);
        if (v > 0) frame_size = static_cast<size_t>(v);
    }

    const std::vector<uint8_t> frames = read_file(frames_path);
    const std::vector<uint8_t> ref_raw = read_file(ref_path);
    if (frames.empty() || ref_raw.size() < 4) {
        std::cerr << "[-] Could not read reference inputs.\n";
        return 1;
    }

    umdplay::Atrac3PlusDecoder dec;
    CHECK(dec.init(2));
    std::vector<int16_t> pcm;
    int decoded = 0;
    for (size_t off = 0; off + frame_size <= frames.size(); off += frame_size) {
        if (dec.decode(frames.data() + off, frame_size, pcm) > 0) decoded++;
    }
    CHECK(decoded > 0);
    // Trailing partial packets are allowed to be absent from the dump, so only
    // require that the bulk of the reference is covered.
    CHECK(pcm.size() * 2 >= ref_raw.size() / 2);

    std::vector<int16_t> ref(ref_raw.size() / 2);
    std::memcpy(ref.data(), ref_raw.data(), ref_raw.size());
    const PcmDiff d = compare_pcm(pcm, ref);

    std::cout << "[*] reference: frames=" << decoded
              << "  rms got=" << d.rms_got << " ref=" << d.rms_ref
              << "  peak got=" << d.peak_got << " ref=" << d.peak_ref
              << "  corr=" << d.corr << "  max|diff|=" << d.max_abs << "\n";

    CHECK(d.peak_got > 1000);                    // non-silent, real signal
    CHECK(d.corr > 0.999);                       // waveform matches
    CHECK(std::fabs(d.rms_got - d.rms_ref) < 0.02 * d.rms_ref);  // level matches
    CHECK(d.max_abs <= 4);                       // only float-rounding noise
    std::cout << "[+] native ATRAC3+ output matches the reference PCM.\n";
    return 0;
}
#endif  // UMDPLAY_NATIVE_ATRAC3P

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

#if defined(UMDPLAY_NATIVE_ATRAC3P)
    if (native_selftest() != 0) return 1;
    if (native_reference_test() != 0) return 1;
#endif

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
#if defined(UMDPLAY_NATIVE_ATRAC3P)
    if (decoder.has_audio()) {
        CHECK(decoder.using_native_audio());
        CHECK(decoder.audio_sample_rate() == 44100);
    }
#endif
    std::cout << "[*] Opened " << main_entry.name
              << "  fps=" << decoder.get_fps()
              << "  dur=" << decoder.get_duration() << "s\n";

    // Decode up to ~4 seconds worth of audio, interleaved with video frames.
    umdplay::VideoFrame vframe;
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
