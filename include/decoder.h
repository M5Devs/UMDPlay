#pragma once

#include "iso_reader.h"
#include <string>
#include <fstream>
#include <cstdint>
#include <iostream>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#if defined(UMDPLAY_HAVE_AVFILTER)
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#endif
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
}

// Sony PSMF ATRAC3+ framing default: an 8-byte sync header (0F D0 48 55 …)
// followed by a payload; the 696-byte stride is the common stereo case but the
// real value is detected per-stream (see Decoder::psmf_detect_stride).
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
    // Flushes codec buffers and re-anchors the PSMF audio demuxer so no stale
    // audio/video survives the jump.
    bool seek_absolute(double seconds);

    // Current playback clock in seconds, taken from the last decoded video
    // frame's PTS (falls back to the frame count / fps when PTS is missing).
    double current_time() const { return m_clock; }

    // Pure helper: derive the ATRAC3+ frame stride from a raw byte window by
    // histogramming gaps between 0F D0 48 55 sync words. Returns `fallback`
    // when fewer than two syncs are visible. Exposed for unit testing.
    static size_t detect_stride_from(const uint8_t* buf, size_t n, size_t fallback);

    bool has_video() const { return m_video.stream_index != -1; }
    bool has_audio() const { return m_audio.stream_index != -1 || m_psmf_audio; }
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
    // Uses the codec id (stable API) rather than AVCodecContext::codec, which
    // newer FFmpeg releases deprecate.
    const char* video_codec_name() const {
        if (!m_video.codec_ctx) return "";
        const char* n = avcodec_get_name(m_video.codec_ctx->codec_id);
        return n ? n : "";
    }

    // Detected ATRAC3+ frame stride in bytes (SONY_FRAME_STRIDE_DEFAULT when
    // the stream uses the FFmpeg demuxer rather than the hand PSMF path).
    size_t audio_stride() const { return m_psmf_stride; }
    // True when audio is being demuxed by the hand PSMF (ATRAC3+) fallback.
    bool using_psmf_audio() const { return m_psmf_audio; }
    int  audio_sample_rate() const {
        return m_audio.codec_ctx ? m_audio.codec_ctx->sample_rate : 0;
    }

    // Total stream duration in seconds (0 if unknown).
    double get_duration() const;

private:
    struct StreamIoState {
        // Non-owning: the disc image lives in Decoder::m_reader so the AVIOContext
        // and the hand PSMF demuxer share one ISO/CHD backend.
        const umdplay::DiscReader* reader = nullptr;
        uint64_t      base_offset = 0;   // entry.lba * 2048
        uint64_t      file_size   = 0;   // entry.size_bytes
        uint64_t      cursor      = 0;   // offset relative to base
    };

    static int     read_packet(void* opaque, uint8_t* buf, int buf_size);
    static int64_t seek_packet(void* opaque, int64_t offset, int whence);

    void process_audio_packet(AVPacket* pkt, AVFrame* audio_frame, std::vector<uint8_t>* out_pcm = nullptr);

    // Sony PSMF ATRAC3+ fallback. FFmpeg's MPEG-PS demuxer does not expose the
    // private stream 1 (0xBD) audio track, so when no audio stream is found the
    // ATRAC3+ frames are demuxed by hand: 0xBD PES payloads are concatenated
    // (frames span PES boundaries), split on the fixed 696-byte stride (8-byte
    // Sony sync header + 688-byte frame) and the sync header is stripped.
    bool init_psmf_audio();
    bool psmf_read_more();
    void process_psmf_audio(AVFrame* audio_frame, std::vector<uint8_t>& out_pcm, bool flush);
    bool resample_to_s16(AVFrame* audio_frame, std::vector<uint8_t>& out_pcm);
    void reset_psmf_audio(uint64_t scan_offset);
    // Trim/pad the first PCM chunk so audio starts on the same timeline as the
    // video's first post-seek frame (PES PTS vs frame PTS). One-shot per anchor.
    void apply_psmf_align(std::vector<uint8_t>& pcm);
    // Find the MPEG-PS pack/system-header offset nearest to a byte target so a
    // seek can re-anchor the hand demuxer on a real packet boundary.
    uint64_t psmf_find_scan_offset(uint64_t target);

    // Detect the ATRAC3+ frame stride (8-byte sync + payload) by measuring the
    // distance between consecutive 0F D0 48 55 sync words in the stream. UMD
    // Video discs are not all 696 bytes (that is the 176.4 KB/s stereo case);
    // reading the real spacing keeps every bitrate working.
    size_t psmf_detect_stride();

    // Decode the next video frame from the FFmpeg demuxer, discarding packets
    // that belong to no tracked stream. Used by the PSMF interleave path.
    FrameType decode_video_frame(AVFrame* out);

    // Deinterlacing: 480i UMD Video frames carry AV_FRAME_FLAG_INTERLACED, so
    // they are run through a yadif filter graph to remove combing. The graph is
    // rebuilt when the frame geometry/format changes and flushed on seek.
    bool ensure_deinterlace_graph(const AVFrame* f);
    bool maybe_deinterlace(AVFrame* f);
    void free_deinterlace_graph();

    StreamIoState    m_io;
    uint8_t*         m_avio_buffer = nullptr;
    AVIOContext*     m_avio_ctx    = nullptr;
    AVFormatContext* m_fmt_ctx     = nullptr;

    StreamInfo       m_video;
    StreamInfo       m_audio;
    std::vector<uint8_t> m_audio_buffer;

    // PSMF ATRAC3+ fallback state
    bool                 m_psmf_audio   = false;
    bool                 m_psmf_eof     = false;
    uint64_t             m_psmf_cursor  = 0;   // next MPEG-PS packet offset (relative to base)
    std::vector<uint8_t> m_psmf_pes;           // buffered concatenated 0xBD payloads
    size_t               m_psmf_pos     = 0;   // parse cursor inside m_psmf_pes
    size_t               m_psmf_stride  = SONY_FRAME_STRIDE_DEFAULT; // detected stride
    int64_t              m_psmf_samples = 0;   // PCM samples emitted so far
    // PES PTS (90 kHz) of the first audio frame after the current anchor; used
    // to trim audio so it starts on the same timeline as the first video frame.
    int64_t              m_psmf_next_pts = AV_NOPTS_VALUE;
    int64_t              m_psmf_pts_skip_samples = 0; // PCM frames still to drop
    bool                 m_psmf_align_done = false;   // skip/pad computed for this anchor
    bool                 m_psmf_pts_started = true;   // accepting audio PES (>= anchor)
    int64_t              m_vcount       = 0;   // video frames emitted (A/V pacing)
    std::vector<uint8_t> m_psmf_scan;          // reusable read buffer
    AVPacket*            m_psmf_pkt     = nullptr;
    AVFrame*             m_psmf_video   = nullptr; // stashed frame for A/V interleave
    bool                 m_psmf_video_pending = false;
    std::string          m_iso_path;
    umdplay::DiscReader  m_reader;             // ISO or CHD disc image (read_at)

    SwrContext*      m_swr_ctx     = nullptr;
    AVFrame*         m_temp_audio_frame = nullptr;
    AVPacket*        m_demux_pkt   = nullptr;
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
