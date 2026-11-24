#pragma once

// Native, single-pass MPEG-2 Program Stream demuxer for Sony UMD `.MPS`
// streams — the libavformat-free replacement for the AVIOContext +
// avformat_open_input path. It reads byte ranges straight from a DiscReader
// (so `.iso` and `.chd` behave identically) and walks MPEG-PS pack headers
// (`00 00 01 BA`) and PES packets in one forward pass, emitting:
//
//   * H.264 Annex-B access units from the video stream (0xE0) together with
//     their 33-bit PES PTS/DTS (90 kHz) and an IDR/SPS keyframe flag, and
//   * Sony ATRAC3+ frames from Private Stream 1 (0xBD) with their PES PTS,
//     using the dynamic `0F D0 48 <xx>` sync word and measured frame stride.
//
// Sony omits standard PSMF file headers on UMD Video, so libavformat cannot be
// used for the audio track; this demuxer replaces it for both tracks.
//
// It also estimates duration (first/last PES PTS) and supports time-based
// seeking by proportional byte offset snapped to a real pack boundary; the
// caller (Decoder) resyncs to the next IDR and aligns 0xBD audio PTS.

#include "disc_reader.h"

#include <cstdint>
#include <vector>

namespace umdplay {

// Presentation timestamp is absent from a PES header.
static constexpr int64_t PSMF_NOPTS = INT64_MIN;

// The common ATRAC3+ stereo frame stride (8-byte sync + payload). The real
// value is measured from the stream; this is only the fallback for a probe
// window too short to see two sync words.
static constexpr uint32_t PSMF_STRIDE_DEFAULT = 696;

// Stream properties discovered by probe().
struct PsmfStreamInfo {
    bool     has_video = false;
    bool     has_audio = false;
    double   fps = 0.0;             // video frame rate (0 = unknown)
    uint32_t audio_stride = PSMF_STRIDE_DEFAULT;  // ATRAC3+ sync + payload bytes
    int      audio_sample_rate = 44100;
    int      audio_channels = 2;
    double   duration = 0.0;        // seconds (0 = unknown)
    uint64_t size_bytes = 0;        // stream size
    // First SPS+PPS pair seen in the stream, packed as Annex-B (each NAL
    // preceded by a 00 00 00 01 start code) for AVCodecContext::extradata.
    // Annex-B — not an avcC record — because the demuxer hands the decoder
    // start-code-delimited access units, and avcC extradata would make
    // libavcodec expect length-prefixed NAL units instead. Empty when no
    // parameter sets were found in the probe window.
    std::vector<uint8_t> video_extradata;
};

// One demuxed access unit.
struct PsmfPacket {
    enum class Kind { None, Video, Audio };
    Kind    kind = Kind::None;
    bool    keyframe = false;       // video: payload contains an IDR or SPS
    int64_t pts = PSMF_NOPTS;       // 90 kHz
    int64_t dts = PSMF_NOPTS;       // 90 kHz
    std::vector<uint8_t> data;      // elementary payload (video Annex-B / pure ATRAC3+)
};

class PsmfDemuxer {
public:
    PsmfDemuxer() = default;
    ~PsmfDemuxer() = default;
    PsmfDemuxer(const PsmfDemuxer&) = delete;
    PsmfDemuxer& operator=(const PsmfDemuxer&) = delete;

    // Attach to the `size`-byte `.MPS` region of `reader` beginning at byte
    // `base_offset`. Returns false when the region is empty.
    bool open(const DiscReader* reader, uint64_t base_offset, uint64_t size);
    void close();

    bool is_open() const { return m_reader != nullptr && m_size > 0; }
    const PsmfStreamInfo& info() const { return m_info; }
    uint64_t size() const { return m_size; }

    // Sequential read: returns the next access unit in stream order, or false
    // at end of stream. `out` is reset on entry.
    bool read(PsmfPacket& out);

    // Scan the stream to fill info(): measured ATRAC3+ stride, video fps,
    // duration (first/last PES PTS), video/audio presence and the first
    // SPS+PPS. `window` bounds the leading scan in bytes (0 = 512 KiB). Leaves
    // the read cursor at the start of the stream.
    void probe(uint32_t window = 0);

    // Time-based seek. Repositions the read cursor on the MPEG-PS pack boundary
    // at or before the proportional byte target and arms a PES-PTS filter so
    // only packets at/after the target are delivered. Returns false when the
    // stream is not open.
    bool seek(double seconds);

    // Presentation time (seconds) of the most recently delivered packet, or the
    // seek target while packets are still being filtered. -1 before any PTS.
    double position() const { return m_position; }

    // --- pure helpers (unit-tested, no I/O) ---

    // Sony ATRAC3+ frame sync: 0F D0 48 <any size class> 00 00 00 00.
    static bool is_sony_frame_sync(const uint8_t* p, size_t avail);

    // Histogram 0F D0 48 <xx> sync gaps over a byte window; the dominant small
    // gap wins (large gaps from interleaved video/PES never do). Returns
    // `fallback` when fewer than two sync words are visible.
    static uint32_t detect_stride(const uint8_t* buf, size_t n, uint32_t fallback);

    // Decode a 33-bit PES timestamp (5 bytes, marker bits stripped). Returns
    // PSMF_NOPTS when fewer than 5 bytes are available.
    static int64_t decode_timestamp(const uint8_t* p, size_t avail);

    // True when a video payload contains an IDR (NAL type 5) or SPS (type 7)
    // unit — the sync points a post-seek resync may start on.
    static bool payload_is_keyframe(const uint8_t* p, size_t n);

    // Pack an SPS + PPS pair (Annex-B NAL bodies, no start codes) into Annex-B
    // extradata: 00 00 00 01 <sps> 00 00 00 01 <pps>. Empty when unusable.
    static std::vector<uint8_t> build_extradata(const std::vector<uint8_t>& sps,
                                                const std::vector<uint8_t>& pps);

private:
    // A parsed PES packet's header fields; payload offsets index into m_buf.
    struct PesView {
        uint8_t sid = 0;
        int64_t pts = PSMF_NOPTS;
        int64_t dts = PSMF_NOPTS;
        size_t  payload_off = 0;
        size_t  payload_len = 0;
    };

    enum class Step { Video, Audio, Eof };

    // Advance to the next PES packet (skipping pack/system headers). Returns
    // false at end of stream. The returned view points into m_buf and stays
    // valid until the next call.
    bool next_pes(PesView& v);

    // read() step: one video packet, one appended audio PES, or EOF.
    Step parse_next(PsmfPacket& out);

    // Emit one complete ATRAC3+ frame from m_audio_buf, or false when more data
    // is needed / nothing is buffered.
    bool emit_audio_frame(PsmfPacket& out);

    // Cut one access unit out of m_video_buf. PSMF splits an AU across several
    // PES packets, so the boundary is an AUD NAL (00 00 01 09), not a packet
    // edge. Returns false when the next boundary is not buffered yet (and not
    // at EOF).
    bool emit_video_au(PsmfPacket& out, bool at_eof);

    // Ensure at least `need` bytes are available at m_scan, reading more if
    // possible. Returns the bytes available (may be < need at EOF).
    size_t fill(size_t need);

    // Read `len` bytes at absolute stream offset `off` (used by probe's tail
    // scan). Returns bytes read.
    size_t read_at(uint64_t off, uint8_t* dst, size_t len) const;

    // Find the last MPEG-PS pack/system-header start code at or before `target`
    // (relative to the stream base) so a seek lands on a real packet boundary.
    uint64_t snap_to_pack(uint64_t target) const;

    // Reset the sliding buffer + audio buffer to stream offset `off`.
    void rewind_to(uint64_t off);

    const DiscReader* m_reader = nullptr;
    uint64_t m_base = 0;    // absolute byte offset of the stream in the disc
    uint64_t m_size = 0;    // stream size in bytes
    uint64_t m_read_end = 0;  // reads stop here (== m_size, or the probe window)

    PsmfStreamInfo m_info;

    // Sliding buffer of raw stream bytes: covers [m_buf_off, m_buf_off + size).
    std::vector<uint8_t> m_buf;
    uint64_t m_buf_off = 0;   // absolute stream offset of m_buf[0]
    size_t   m_scan = 0;      // parse cursor inside m_buf (a packet boundary)
    bool     m_eof = false;

    // Concatenated 0xE0 payloads awaiting access-unit splitting. `m_video_marks`
    // pairs each contributing PES's buffer offset with its PTS so a cut keeps
    // the timestamp of the packet that opened the AU.
    std::vector<uint8_t> m_video_buf;
    struct VideoMark {
        size_t  off;
        int64_t pts;
    };
    std::vector<VideoMark> m_video_marks;
    size_t m_video_scan = 1;  // skip the AUD at offset 0 of the pending AU

    // How video access units are delimited. UMD discs start every AU with an
    // AUD NAL and spread it over several PES packets; simpler streams keep one
    // AU per PES packet. Decided from the first 64 KiB of video data.
    enum class VideoFraming { Unknown, Aud, Pes };
    VideoFraming m_video_framing = VideoFraming::Unknown;

    // Concatenated 0xBD payloads awaiting ATRAC3+ frame splitting (frames span
    // PES boundaries).
    std::vector<uint8_t> m_audio_buf;
    size_t   m_audio_pos = 0;
    int64_t  m_audio_pts = PSMF_NOPTS;  // PTS of the PES currently feeding it

    // PES-PTS filter armed by seek(): packets whose PTS is before this are
    // skipped until the stream catches up to the seek target.
    int64_t  m_seek_pts = PSMF_NOPTS;
    bool     m_seeking = false;
    double   m_position = -1.0;   // last delivered PTS (seconds), or seek target

    uint32_t m_chunk = 1u << 20;  // read granularity in bytes
};

}  // namespace umdplay
