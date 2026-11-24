#pragma once

#include "audio_decoder_backend.h"
#include "iso_reader.h"
#include "psmf_demuxer.h"
#include "video_decoder_backend.h"
#include "video_frame.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Sony PSMF ATRAC3+ framing default: an 8-byte sync header (0F D0 48 55 ...)
// followed by a payload; the 696-byte stride is the common stereo case but the
// real value is detected per-stream (see PsmfDemuxer::detect_stride).
static constexpr size_t SONY_FRAME_STRIDE_DEFAULT = 696;

enum class FrameType {
    None,
    Video,
    Audio,
    Eof
};

// Decodes an UMD .MPS stream: the native PsmfDemuxer splits the MPEG-PS into
// H.264 access units and Sony ATRAC3+ frames, and each is handed to a decoder
// backend. Both backends are UMDPlay's own code by default, so the player needs
// no FFmpeg at all; see video_decoder_backend.h / audio_decoder_backend.h.
class Decoder {
public:
    Decoder();
    ~Decoder();

    bool open_stream(const std::string& iso_path, const MpsEntry& entry);
    void close();

    bool test_decode(int max_frames = 100, const std::string& dump_frame_path = "output_frame.ppm");

    // Read the next decoded video frame or audio PCM chunk. video_frame is only
    // touched when FrameType::Video is returned.
    FrameType read_frame(umdplay::VideoFrame& video_frame, std::vector<uint8_t>& audio_pcm);

    // Seek relative in seconds (e.g., +5.0 or -5.0)
    bool seek_relative(double seconds);

    // Seek to an absolute position in seconds, clamping to [0, duration].
    // Flushes decoder state and re-anchors the PSMF demuxer so no stale
    // audio/video survives the jump.
    bool seek_absolute(double seconds);

    // Current playback clock in seconds, taken from the last decoded video
    // frame's PTS (falls back to the frame count / fps when PTS is missing).
    double current_time() const { return m_clock; }

    // Pure helper: derive the ATRAC3+ frame stride from a raw byte window by
    // histogramming gaps between 0F D0 48 <xx> sync words. Returns fallback
    // when fewer than two syncs are visible. Exposed for unit testing.
    static size_t detect_stride_from(const uint8_t* buf, size_t n, size_t fallback);

    bool has_video() const { return m_video_backend != nullptr; }
    bool has_audio() const { return m_audio_backend != nullptr; }
    double get_fps() const { return m_fps; }

    // Enable/disable deinterlacing of decoded video frames. 480i NTSC UMD Video
    // carries field-coded pictures; when enabled (default) they are blended
    // vertically so fast motion renders without combing.
    void set_deinterlace(bool on);
    bool deinterlace_enabled() const { return m_deinterlace; }
    // True if the most recently returned video frame was coded interlaced.
    bool last_frame_interlaced() const { return m_last_interlaced; }

    // Coded video dimensions (0 while unknown).
    int video_width() const;
    int video_height() const;

    // Pixel aspect ratio of the video (1.0 when square/unknown). Used by the
    // player's Zoom modes so anamorphic 720x480 renders at its true shape.
    float video_par() const;

    // Human-readable video codec name ("h264", ...), or "" when unknown.
    const char* video_codec_name() const;
    // Which implementation is decoding video ("h264dec", "ffmpeg").
    const char* video_backend_name() const;

    // Detected ATRAC3+ frame stride in bytes.
    size_t audio_stride() const { return m_demux.info().audio_stride; }
    // True when audio is present and demuxed by the native PSMF path.
    bool using_psmf_audio() const { return m_audio_backend != nullptr; }
    int audio_sample_rate() const;
    // True when the built-in decoders (not libav*) are in use.
    bool using_native_audio() const;
    bool using_native_video() const;

    // Total stream duration in seconds (0 if unknown).
    double get_duration() const { return m_duration; }

private:
    // Next demuxer packet, honouring the one-packet pushback slot.
    bool pull(umdplay::PsmfPacket& out);
    void push_back(umdplay::PsmfPacket&& pkt);

    // Hand one H.264 access unit to the video backend.
    bool send_video(const umdplay::PsmfPacket& pkt);
    // Pull the next picture from the backend (applying the deinterlace option).
    bool receive_video(umdplay::VideoFrame& out);

    // Read demuxer packets until the next video frame is decoded (buffering any
    // audio PCM encountered on the way). Returns false at end of stream.
    bool decode_next_video(umdplay::VideoFrame& out);

    // Decode demuxer packets into m_pcm_ready until the stream reaches
    // target_samples (or a video packet turns up, which is pushed back).
    void pump_audio(int64_t target_samples);

    void flush_video();

    // Decode one ATRAC3+ access unit to S16 stereo PCM, appending to
    // m_pcm_ready and recording the first post-anchor PES PTS.
    void decode_audio_packet(const umdplay::PsmfPacket& pkt);

    // Trim/pad the buffered PCM so audio starts on the same timeline as the
    // video's first post-seek frame (PES PTS vs frame PTS). One-shot per anchor.
    void apply_align();

    // Move up to max_bytes of buffered PCM into pcm (honouring any pending
    // post-seek skip). Returns true when any bytes were produced.
    bool drain_pcm(std::vector<uint8_t>& pcm, size_t max_bytes);

    void save_ppm(const umdplay::VideoFrame& frame, const std::string& filename);

    umdplay::DiscReader  m_reader;             // ISO or CHD disc image (read_at)
    umdplay::PsmfDemuxer m_demux;              // native MPEG-PS demuxer

    std::unique_ptr<umdplay::VideoDecoderBackend> m_video_backend;
    std::unique_ptr<umdplay::AudioDecoderBackend> m_audio_backend;

    // Decoded audio PCM awaiting delivery to the caller, drained from the front.
    std::vector<uint8_t> m_pcm_ready;
    size_t               m_pcm_pos = 0;

    // One-packet pushback so pumping audio never loses the next video packet.
    umdplay::PsmfPacket  m_pushback;
    bool                 m_have_pushback = false;

    // Post-seek A/V alignment: PES PTS (90 kHz) of the first audio frame after
    // the anchor and the residual sample skip left by a fully-early chunk.
    int64_t              m_psmf_next_pts = 0;
    int64_t              m_psmf_skip_samples = 0;
    bool                 m_align_done = false;
    int64_t              m_audio_samples = 0;  // PCM samples decoded since anchor
    int64_t              m_vcount = 0;         // video frames emitted (A/V pacing)

    umdplay::VideoFrame  m_stashed_video;      // pending frame for A/V interleave
    bool                 m_video_pending = false;

    bool             m_video_eof = false;      // demuxer exhausted
    bool             m_audio_eof = false;
    bool             m_video_flushed = false;  // decoder drain requested

    double           m_fps         = 29.97;
    double           m_duration    = 0.0;   // seconds (0 = unknown)
    double           m_clock       = 0.0;   // last decoded video PTS (seconds)
    bool             m_resync_video = false; // drop to next keyframe after a seek
    double           m_pts_anchor  = -1.0;  // video PTS of the first post-seek frame

    bool             m_deinterlace      = true;
    bool             m_last_interlaced  = false;
};
