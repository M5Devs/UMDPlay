#pragma once

#include "iso_reader.h"
#include "psmf_demuxer.h"
#include <memory>
#include <string>
#include <cstdint>
#include <vector>

// Native ATRAC3+ decoder (no libavcodec / libswresample on the audio path).
#if defined(UMDPLAY_NATIVE_ATRAC3P)
#include "atrac3p_decoder.h"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#if defined(UMDPLAY_HAVE_AVFILTER)
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#endif
#include <libswscale/swscale.h>
#if !defined(UMDPLAY_NATIVE_ATRAC3P)
#include <libswresample/swresample.h>
#endif
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
}

// Sony PSMF ATRAC3+ framing default: an 8-byte sync header (0F D0 48 55 …)
// followed by a payload; the 696-byte stride is the common stereo case but the
// real value is detected per-stream (see PsmfDemuxer::detect_stride).
static constexpr size_t SONY_FRAME_STRIDE_DEFAULT = 696;

struct StreamInfo {
    int             stream_index = -1;
    AVCodecContext* codec_ctx    = nullptr;
    const AVCodec*  codec        = nullptr;
    int             block_align  = 0;
};

enum class FrameType {
    None,
    Video,
    Audio,
    Eof
};

// Decodes an UMD `.MPS` stream by demuxing it with the native PsmfDemuxer
// (no libavformat) and feeding the resulting elementary packets to libavcodec
// (H.264 video, Sony ATRAC3+ audio). Deinterlacing (yadif) and PCM resampling
// (swresample) are unchanged.
class Decoder {
public:
    Decoder();
    ~Decoder();

    bool open_stream(const std::string& iso_path, const MpsEntry& entry);
    void close();

    bool test_decode(int max_frames = 100, const std::string& dump_frame_path = "output_frame.ppm");

    // Read next decoded frame or audio packet chunk
    FrameType read_frame(AVFrame* video_frame, std::vector<uint8_t>& audio_pcm);

    // Seek relative in seconds (e.g., +5.0 or -5.0)
    bool seek_relative(double seconds);

    // Seek to an absolute position in seconds, clamping to [0, duration].
    // Flushes codec buffers and re-anchors the PSMF demuxer so no stale
    // audio/video survives the jump.
    bool seek_absolute(double seconds);

    // Current playback clock in seconds, taken from the last decoded video
    // frame's PTS (falls back to the frame count / fps when PTS is missing).
    double current_time() const { return m_clock; }

    // Pure helper: derive the ATRAC3+ frame stride from a raw byte window by
    // histogramming gaps between 0F D0 48 <xx> sync words. Returns `fallback`
    // when fewer than two syncs are visible. Exposed for unit testing.
    static size_t detect_stride_from(const uint8_t* buf, size_t n, size_t fallback);

    bool has_video() const { return m_video.stream_index != -1; }
    bool has_audio() const { return m_audio.stream_index != -1; }
    double get_fps() const { return m_fps; }

    // Enable/disable deinterlacing of decoded video frames. 480i NTSC UMD Video
    // carries AV_FRAME_FLAG_INTERLACED frames; when enabled (default) they are
    // run through a yadif filter so fast motion renders without combing.
    void set_deinterlace(bool on);
    bool deinterlace_enabled() const { return m_deinterlace; }
    // True if the most recently returned video frame was interlaced.
    bool last_frame_interlaced() const { return m_last_interlaced; }

    // Coded video dimensions (0 if no video stream is open).
    int video_width() const {
        return m_video.codec_ctx ? m_video.codec_ctx->width : 0;
    }
    int video_height() const {
        return m_video.codec_ctx ? m_video.codec_ctx->height : 0;
    }

    // Pixel aspect ratio of the video (1.0 when square/unknown). Used by the
    // player's Zoom modes so anamorphic 720x480 renders at its true shape.
    float video_par() const {
        if (!m_video.codec_ctx) return 1.0f;
        const AVRational r = m_video.codec_ctx->sample_aspect_ratio;
        if (r.num <= 0 || r.den <= 0) return 1.0f;
        return static_cast<float>(r.num) / static_cast<float>(r.den);
    }

    // Human-readable video codec name ("h264", ...), or "" when unknown.
    const char* video_codec_name() const {
        if (!m_video.codec_ctx) return "";
        const char* n = avcodec_get_name(m_video.codec_ctx->codec_id);
        return n ? n : "";
    }

    // Detected ATRAC3+ frame stride in bytes.
    size_t audio_stride() const { return m_demux.info().audio_stride; }
    // True when audio is present and demuxed by the native PSMF path.
    bool using_psmf_audio() const { return m_audio.stream_index != -1; }
    int  audio_sample_rate() const {
#if defined(UMDPLAY_NATIVE_ATRAC3P)
        if (m_native_audio) return umdplay::Atrac3PlusDecoder::kSampleRate;
#endif
        return m_audio.codec_ctx ? m_audio.codec_ctx->sample_rate : 0;
    }
    // True when the built-in ATRAC3+ decoder (not libavcodec) is in use.
    bool using_native_audio() const {
#if defined(UMDPLAY_NATIVE_ATRAC3P)
        return m_native_audio != nullptr;
#else
        return false;
#endif
    }

    // Total stream duration in seconds (0 if unknown).
    double get_duration() const { return m_duration; }

private:
    // Presentation time of a decoded frame in seconds (-1 if unknown). The
    // video decoder is primed with a 1/90000 packet time base, so frame PTS is
    // in the same 90 kHz clock as the PES timestamps.
    static double frame_seconds(const AVFrame* f);

    // Next demuxer packet, honouring the one-packet pushback slot.
    bool pull(umdplay::PsmfPacket& out);
    void push_back(umdplay::PsmfPacket&& pkt);

    // Send a video access unit to the H.264 decoder and try to receive a frame.
    // Returns true when a frame was produced into `out`.
    bool send_video(const umdplay::PsmfPacket& pkt, AVFrame* out);

    // Read demuxer packets until the next video frame is decoded (buffering any
    // audio PCM encountered on the way). Returns false at end of stream.
    bool decode_next_video(AVFrame* out);

    // Decode demuxer packets into m_pcm_ready until the stream reaches
    // `target_samples` (or a video packet turns up, which is pushed back).
    void pump_audio(int64_t target_samples);

    void flush_video();
    void flush_audio();

    // Decode one ATRAC3+ access unit to S16 stereo PCM, appending to
    // m_pcm_ready and recording the first post-anchor PES PTS.
    void decode_audio_packet(const umdplay::PsmfPacket& pkt);

    // Resample a decoded ATRAC3+ frame (FLTP) to interleaved S16 stereo and
    // append it to m_pcm_ready.
    void resample_to_s16(AVFrame* audio_frame);
    // Trim/pad the buffered PCM so audio starts on the same timeline as the
    // video's first post-seek frame (PES PTS vs frame PTS). One-shot per anchor.
    void apply_align();

    // Move up to `max_bytes` of buffered PCM into `pcm` (honouring any pending
    // post-seek skip). Returns true when any bytes were produced.
    bool drain_pcm(std::vector<uint8_t>& pcm, size_t max_bytes);

    // Deinterlacing: 480i UMD Video frames carry AV_FRAME_FLAG_INTERLACED, so
    // they are run through a yadif filter graph to remove combing. The graph is
    // rebuilt when the frame geometry/format changes and flushed on seek.
    bool ensure_deinterlace_graph(const AVFrame* f);
    bool maybe_deinterlace(AVFrame* f);
    void free_deinterlace_graph();

    umdplay::DiscReader  m_reader;             // ISO or CHD disc image (read_at)
    umdplay::PsmfDemuxer m_demux;              // native MPEG-PS demuxer

    StreamInfo       m_video;
    StreamInfo       m_audio;
    AVPacket*        m_pkt = nullptr;          // reusable packet handed to codecs

    // Decoded audio PCM awaiting delivery to the caller, drained from the front.
    std::vector<uint8_t> m_pcm_ready;
    size_t               m_pcm_pos = 0;

    // One-packet pushback so pumping audio never loses the next video packet.
    umdplay::PsmfPacket  m_pushback;
    bool                 m_have_pushback = false;

    // Post-seek A/V alignment: PES PTS (90 kHz) of the first audio frame after
    // the anchor and the residual sample skip left by a fully-early chunk.
    int64_t              m_psmf_next_pts = umdplay::PSMF_NOPTS;
    int64_t              m_psmf_skip_samples = 0;
    bool                 m_align_done = false;
    int64_t              m_audio_samples = 0;  // PCM samples decoded since anchor
    int64_t              m_vcount = 0;         // video frames emitted (A/V pacing)

    AVFrame*         m_stashed_video = nullptr; // pending frame for A/V interleave
    bool             m_video_pending = false;

    bool             m_video_eof = false;      // demuxer exhausted
    bool             m_audio_eof = false;
    bool             m_video_flushed = false;  // decoder drain packet sent
    bool             m_audio_flushed = false;

#if defined(UMDPLAY_NATIVE_ATRAC3P)
    std::unique_ptr<umdplay::Atrac3PlusDecoder> m_native_audio;  // zero-FFmpeg audio
#else
    SwrContext*      m_swr_ctx     = nullptr;
#endif
    AVFrame*         m_temp_audio_frame = nullptr;
    double           m_fps         = 29.97;
    double           m_duration    = 0.0;   // seconds (0 = unknown)
    double           m_clock       = 0.0;   // last decoded video PTS (seconds)
    bool             m_resync_video = false; // drop to next keyframe after a seek
    double           m_pts_anchor  = -1.0;  // video PTS of the first post-seek frame

    // Deinterlacing (yadif) filter graph, rebuilt when the frame layout changes.
    bool             m_deinterlace      = true;
    bool             m_last_interlaced  = false;
#if defined(UMDPLAY_HAVE_AVFILTER)
    AVFilterGraph*   m_filter_graph     = nullptr;
    AVFilterContext* m_filter_src       = nullptr;
    AVFilterContext* m_filter_sink      = nullptr;
    AVFrame*         m_filter_out       = nullptr;
    int              m_filter_w = 0, m_filter_h = 0, m_filter_fmt = -1;
#endif

    static void save_ppm(AVFrame* frame, int width, int height, const std::string& filename);
};
