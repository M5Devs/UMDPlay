// Unit tests for the native PsmfDemuxer — the libavformat-free MPEG-2 Program
// Stream demuxer that reads `.MPS` byte ranges straight from a DiscReader.
//
// The tests build a synthetic MPEG-PS stream (pack headers + 0xE0 video and
// 0xBD ATRAC3+ PES packets) with known 33-bit PTS values, write it to a temp
// file, and demux it back: PTS decoding, Sony `0F D0 48 <xx>` sync + measured
// stride, keyframe detection, duration probing and time-based seeking. No UMD
// disc is required; when one is reachable (UMD_ISO) the suite additionally
// demuxes the real main stream to prove it parses end to end.
//
// Checks use CHECK() rather than assert() so they still run in Release builds.

#include "disc_reader.h"
#include "psmf_demuxer.h"
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

// --- synthetic MPEG-PS builder -------------------------------------------

void put_pts(std::vector<uint8_t>& out, int64_t ts) {
    out.push_back(static_cast<uint8_t>(0x20 | (((ts >> 30) & 0x07) << 1) | 1));
    out.push_back(static_cast<uint8_t>((ts >> 22) & 0xFF));
    out.push_back(static_cast<uint8_t>((((ts >> 15) & 0x7F) << 1) | 1));
    out.push_back(static_cast<uint8_t>((ts >> 7) & 0xFF));
    out.push_back(static_cast<uint8_t>((((ts >> 0) & 0x7F) << 1) | 1));
}

// 14-byte MPEG-2 pack header (top two bits of byte 4 are '01').
void put_pack(std::vector<uint8_t>& out) {
    const uint8_t hdr[14] = {0x00, 0x00, 0x01, 0xBA, 0x44, 0x00, 0x00, 0x00,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    out.insert(out.end(), hdr, hdr + sizeof(hdr));
}

// Append one PES packet. `substream` adds the 4-byte Sony 0xBD sub-header.
void put_pes(std::vector<uint8_t>& out, uint8_t sid, bool has_pts, int64_t pts,
             const std::vector<uint8_t>& payload, bool substream) {
    const size_t hdr_len = has_pts ? 5 : 0;
    const size_t pes_len = 3 + hdr_len + (substream ? 4 : 0) + payload.size();

    out.push_back(0x00); out.push_back(0x00); out.push_back(0x01);
    out.push_back(sid);
    out.push_back(static_cast<uint8_t>((pes_len >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(pes_len & 0xFF));
    out.push_back(0x80);  // '10' marker, no scrambling
    out.push_back(has_pts ? 0x80 : 0x00);  // PTS_DTS_flags
    out.push_back(static_cast<uint8_t>(hdr_len));
    if (has_pts) put_pts(out, pts);
    if (substream) { out.push_back(0x00); out.push_back(0x00); out.push_back(0x00); out.push_back(0x00); }
    out.insert(out.end(), payload.begin(), payload.end());
}

std::vector<uint8_t> h264_nal(uint8_t nal_type, size_t body) {
    std::vector<uint8_t> p = {0x00, 0x00, 0x00, 0x01, static_cast<uint8_t>(0x60 | nal_type)};
    for (size_t i = 0; i < body; ++i) p.push_back(static_cast<uint8_t>(i & 0xFF));
    return p;
}

// One ATRAC3+ frame: 8-byte 0F D0 48 <class> 00 00 00 00 + filler.
void put_atrac3p_frame(std::vector<uint8_t>& out, uint32_t stride, uint8_t cls) {
    out.push_back(0x0F); out.push_back(0xD0); out.push_back(0x48); out.push_back(cls);
    out.push_back(0x00); out.push_back(0x00); out.push_back(0x00); out.push_back(0x00);
    for (uint32_t i = 8; i < stride; ++i) out.push_back(0xA5);
}

// Build a synthetic stream: `frames` interleaved (video, audio) pairs at 29.97fps.
std::vector<uint8_t> build_stream(uint32_t frames, uint32_t stride,
                                  bool idr_every) {
    std::vector<uint8_t> out;
    int64_t pts = 0;  // content-relative, like real UMD discs
    for (uint32_t f = 0; f < frames; ++f) {
        put_pack(out);
        const bool idr = idr_every && (f % 15 == 0);
        put_pes(out, 0xE0, true, pts, h264_nal(idr ? 5 : 1, 32), false);

        put_pack(out);
        std::vector<uint8_t> af;
        put_atrac3p_frame(af, stride, stride == 1720 ? 0xD5 : 0x55);
        put_pes(out, 0xBD, true, pts, af, true);

        pts += 3003;  // 29.97 fps
    }
    put_pes(out, 0xB9, false, 0, {}, false);  // program end
    return out;
}


// Build a stream the way a UMD disc stores video: every access unit opens with
// an AUD and is spread over `parts` PES packets (the container cuts at sector
// boundaries, not at NAL edges). Only the first packet of an AU carries a PTS.
std::vector<uint8_t> build_aud_stream(uint32_t frames, uint32_t parts) {
    std::vector<uint8_t> out;
    int64_t pts = 0;
    for (uint32_t f = 0; f < frames; ++f) {
        std::vector<uint8_t> au = {0x00, 0x00, 0x00, 0x01, 0x09, 0x10};  // AUD
        const auto slice = h264_nal(f % 5 == 0 ? 5 : 1, 600);
        au.insert(au.end(), slice.begin(), slice.end());

        const size_t chunk = (au.size() + parts - 1) / parts;
        for (uint32_t k = 0; k < parts; ++k) {
            const size_t b = k * chunk;
            if (b >= au.size()) break;
            const size_t e = std::min(au.size(), b + chunk);
            put_pack(out);
            put_pes(out, 0xE0, k == 0, pts,
                    std::vector<uint8_t>(au.begin() + static_cast<std::ptrdiff_t>(b),
                                         au.begin() + static_cast<std::ptrdiff_t>(e)),
                    false);
        }
        pts += 3003;  // 29.97 fps
    }
    put_pes(out, 0xB9, false, 0, {}, false);  // program end
    return out;
}

std::string write_temp(const std::vector<uint8_t>& bytes, const char* name) {
    const fs::path p = fs::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    f.close();
    return p.string();
}

}  // namespace

int main() {
    using umdplay::PsmfDemuxer;
    using umdplay::PsmfPacket;

    // --- 33-bit PTS round trip ---
    {
        const int64_t vals[] = {0, 1, 90000, 123456789, (int64_t(1) << 33) - 1};
        for (int64_t v : vals) {
            std::vector<uint8_t> b;
            put_pts(b, v);
            CHECK(PsmfDemuxer::decode_timestamp(b.data(), b.size()) == v);
        }
        CHECK(PsmfDemuxer::decode_timestamp(nullptr, 0) == umdplay::PSMF_NOPTS);
    }

    // --- Sony frame sync (class byte agnostic) ---
    {
        const uint8_t s55[8] = {0x0F, 0xD0, 0x48, 0x55, 0, 0, 0, 0};
        const uint8_t sD5[8] = {0x0F, 0xD0, 0x48, 0xD5, 0, 0, 0, 0};
        const uint8_t bad[8] = {0x0F, 0xD0, 0x48, 0x55, 0, 0, 0, 1};
        CHECK(PsmfDemuxer::is_sony_frame_sync(s55, 8));
        CHECK(PsmfDemuxer::is_sony_frame_sync(sD5, 8));
        CHECK(!PsmfDemuxer::is_sony_frame_sync(bad, 8));
        CHECK(!PsmfDemuxer::is_sony_frame_sync(s55, 4));
    }

    // --- stride detection: 696, 1720, noisy, fallback ---
    {
        auto mk = [](uint32_t stride, uint8_t cls, size_t n) {
            std::vector<uint8_t> b;
            for (size_t i = 0; i < n; ++i) put_atrac3p_frame(b, stride, cls);
            return b;
        };
        auto a = mk(696, 0x55, 20);
        CHECK(PsmfDemuxer::detect_stride(a.data(), a.size(), 0) == 696);
        auto b = mk(1720, 0xD5, 20);
        CHECK(PsmfDemuxer::detect_stride(b.data(), b.size(), 0) == 1720);

        // A one-off 4096-byte gap must not win over the dominant 696 stride.
        auto noisy = mk(696, 0x55, 3);
        for (int i = 0; i < 4096; ++i) noisy.push_back(0x00);
        auto rest = mk(696, 0x55, 20);
        noisy.insert(noisy.end(), rest.begin(), rest.end());
        CHECK(PsmfDemuxer::detect_stride(noisy.data(), noisy.size(), 0) == 696);

        const uint8_t tiny[8] = {0x0F, 0xD0, 0x48, 0x55, 0, 0, 0, 0};
        CHECK(PsmfDemuxer::detect_stride(tiny, sizeof(tiny), 696) == 696);
    }

    // --- keyframe detection ---
    {
        auto idr = h264_nal(5, 8);
        auto p = h264_nal(1, 8);
        CHECK(PsmfDemuxer::payload_is_keyframe(idr.data(), idr.size()));
        CHECK(!PsmfDemuxer::payload_is_keyframe(p.data(), p.size()));
    }

    // --- Annex-B extradata from SPS + PPS ---
    {
        std::vector<uint8_t> sps = {0x67, 0x64, 0x00, 0x1E, 0xAA, 0xBB};
        std::vector<uint8_t> pps = {0x68, 0xEE, 0x3C, 0x80};
        auto ed = PsmfDemuxer::build_extradata(sps, pps);
        CHECK(ed.size() == 8 + sps.size() + pps.size());
        CHECK(ed[0] == 0 && ed[1] == 0 && ed[2] == 0 && ed[3] == 1);
        CHECK(ed[4] == 0x67);              // SPS first
        const size_t pps_off = 4 + sps.size() + 4;
        CHECK(ed[pps_off] == 0x68);        // PPS after the second start code
        CHECK(PsmfDemuxer::build_extradata({}, pps).empty());
        CHECK(PsmfDemuxer::build_extradata(sps, {}).empty());
    }

    // --- synthetic stream: demux, PTS, stride, seek ---
    {
        const uint32_t stride = 696;
        auto bytes = build_stream(60, stride, /*idr_every=*/true);
        const std::string path = write_temp(bytes, "umdplay_psmf_synth.mps");

        umdplay::DiscReader reader;
        CHECK(reader.open(path));
        CHECK(reader.is_open());

        PsmfDemuxer demux;
        CHECK(demux.open(&reader, 0, bytes.size()));
        demux.probe();

        const auto& info = demux.info();
        CHECK(info.has_video);
        CHECK(info.has_audio);
        CHECK(info.audio_stride == stride);
        CHECK(info.fps > 28.0 && info.fps < 31.0);
        CHECK(info.duration > 0.5);

        // First packet must be video (stream order), with the encoded PTS.
        PsmfPacket pkt;
        CHECK(demux.read(pkt));
        CHECK(pkt.kind == PsmfPacket::Kind::Video);
        CHECK(pkt.pts == 0);
        CHECK(pkt.keyframe);  // first frame is an IDR

        CHECK(demux.read(pkt));
        CHECK(pkt.kind == PsmfPacket::Kind::Audio);
        CHECK(pkt.pts == 0);
        CHECK(!pkt.data.empty());

        // Count a full pass: video + audio per frame, no desync.
        int video = 1, audio = 1, bad = 0;
        int64_t last_video_pts = 0;
        while (demux.read(pkt)) {
            if (pkt.kind == PsmfPacket::Kind::Video) {
                if (pkt.pts != umdplay::PSMF_NOPTS && pkt.pts < last_video_pts) ++bad;
                if (pkt.pts != umdplay::PSMF_NOPTS) last_video_pts = pkt.pts;
                ++video;
            } else if (pkt.kind == PsmfPacket::Kind::Audio) {
                ++audio;
            }
        }
        CHECK(video == 60);
        CHECK(audio == 60);
        CHECK(bad == 0);

        // Seek to ~1 s: the next delivered video PTS must be at/after it.
        CHECK(demux.seek(1.0));
        CHECK(demux.read(pkt));
        // Skip to the first video packet the filter lets through.
        while (pkt.kind != PsmfPacket::Kind::Video && demux.read(pkt)) {}
        CHECK(pkt.kind == PsmfPacket::Kind::Video);
        CHECK(pkt.pts != umdplay::PSMF_NOPTS);
        CHECK(pkt.pts >= 90000 - 3003);              // not before the target
        CHECK(pkt.pts <= 90000 + 15 * 3003);         // and reasonably near it

        demux.close();
        reader.close();
        std::remove(path.c_str());
        std::printf("[+] synthetic PSMF demux + seek ok\n");
    }

    // --- 1720-byte (high-bitrate) stride demuxes too ---
    {
        auto bytes = build_stream(10, 1720, false);
        const std::string path = write_temp(bytes, "umdplay_psmf_1720.mps");
        umdplay::DiscReader reader;
        CHECK(reader.open(path));
        PsmfDemuxer demux;
        CHECK(demux.open(&reader, 0, bytes.size()));
        demux.probe();
        CHECK(demux.info().audio_stride == 1720);

        int audio = 0;
        PsmfPacket pkt;
        while (demux.read(pkt)) if (pkt.kind == PsmfPacket::Kind::Audio) ++audio;
        CHECK(audio == 10);
        demux.close();
        reader.close();
        std::remove(path.c_str());
    }

    // --- real ISO (optional): demux the main stream end to end ---
    {
        std::string iso;
        if (const char* env = std::getenv("UMD_ISO")) iso = env;
        else {
            const char* cands[] = {"../test_umd.iso", "test_umd.iso"};
            for (const char* c : cands) {
                std::ifstream f(c, std::ios::binary);
                if (f.good()) { iso = c; break; }
            }
        }
        if (iso.empty()) {
            std::printf("[skip] No ISO found (set UMD_ISO); synthetic checks passed.\n");
        } else {
            IsoReader ir(iso);
            if (!ir.open() || ir.get_mps_entries().empty()) {
                std::printf("[skip] ISO present but no .MPS streams.\n");
            } else {
                MpsEntry main_entry = ir.get_mps_entries()[0];
                for (const auto& e : ir.get_mps_entries())
                    if (e.size_bytes > main_entry.size_bytes) main_entry = e;

                umdplay::DiscReader reader;
                CHECK(reader.open(iso));
                PsmfDemuxer demux;
                CHECK(demux.open(&reader, static_cast<uint64_t>(main_entry.lba) * UMD_SECTOR_SIZE,
                                 main_entry.size_bytes));
                demux.probe();
                std::printf("[*] real %s: video=%d audio=%d stride=%u fps=%.3f dur=%.2f\n",
                            main_entry.name.c_str(), demux.info().has_video,
                            demux.info().has_audio, demux.info().audio_stride,
                            demux.info().fps, demux.info().duration);
                CHECK(demux.info().has_video);

                int video = 0, audio = 0;
                PsmfPacket pkt;
                while ((video + audio) < 600 && demux.read(pkt)) {
                    if (pkt.kind == PsmfPacket::Kind::Video) ++video;
                    else if (pkt.kind == PsmfPacket::Kind::Audio) ++audio;
                }
                CHECK(video > 0);
                demux.close();
                reader.close();
            }
        }
    }


    // --- access-unit reassembly: AUD-delimited AUs spanning PES packets ---
    {
        const uint32_t frames = 12;
        auto bytes = build_aud_stream(frames, /*parts=*/3);
        const std::string path = write_temp(bytes, "umdplay_psmf_aud.mps");

        umdplay::DiscReader reader;
        CHECK(reader.open(path));
        PsmfDemuxer demux;
        CHECK(demux.open(&reader, 0, bytes.size()));

        PsmfPacket pkt;
        uint32_t video = 0;
        int bad_pts = 0;
        size_t first_size = 0;
        while (demux.read(pkt)) {
            if (pkt.kind != PsmfPacket::Kind::Video) continue;
            if (video == 0) {
                first_size = pkt.data.size();
                CHECK(pkt.keyframe);
                CHECK(pkt.pts == 0);
                // The whole AU, not just the first PES chunk.
                CHECK(pkt.data.size() > 600);
                CHECK(pkt.data[0] == 0 && pkt.data[1] == 0 && pkt.data[2] == 0 &&
                      pkt.data[3] == 1 && pkt.data[4] == 0x09);
            } else {
                if (pkt.data.size() != first_size) ++bad_pts;  // uniform AUs
                if (pkt.pts != umdplay::PSMF_NOPTS &&
                    pkt.pts != static_cast<int64_t>(video) * 3003) {
                    ++bad_pts;
                }
            }
            ++video;
        }
        CHECK(video == frames);
        CHECK(bad_pts == 0);
        demux.close();
        reader.close();
        std::remove(path.c_str());
    }

    if (g_failures != 0) {
        std::printf("[-] %d PsmfDemuxer check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("All PsmfDemuxer tests passed!\n");
    return 0;
}
