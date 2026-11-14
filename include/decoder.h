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
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
}

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

    bool has_video() const { return m_video.stream_index != -1; }
    bool has_audio() const { return m_audio.stream_index != -1 || m_psmf_audio; }
    double get_fps() const { return m_fps; }

    // Coded video dimensions (0 if no video stream is open).
    int video_width() const {
        return m_video.codec_ctx ? m_video.codec_ctx->width : 0;
    }
    int video_height() const {
        return m_video.codec_ctx ? m_video.codec_ctx->height : 0;
    }

    // Total stream duration in seconds (0 if unknown).
    double get_duration() const;

private:
    struct StreamIoState {
        std::ifstream iso_file;
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
    // Find the MPEG-PS pack/system-header offset nearest to a byte target so a
    // seek can re-anchor the hand demuxer on a real packet boundary.
    uint64_t psmf_find_scan_offset(uint64_t target);

    // Decode the next video frame from the FFmpeg demuxer, discarding packets
    // that belong to no tracked stream. Used by the PSMF interleave path.
    FrameType decode_video_frame(AVFrame* out);

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
    int64_t              m_psmf_samples = 0;   // PCM samples emitted so far
    int64_t              m_vcount       = 0;   // video frames emitted (A/V pacing)
    std::vector<uint8_t> m_psmf_scan;          // reusable read buffer
    std::ifstream        m_psmf_file;
    AVPacket*            m_psmf_pkt     = nullptr;
    AVFrame*             m_psmf_video   = nullptr; // stashed frame for A/V interleave
    bool                 m_psmf_video_pending = false;
    std::string          m_iso_path;

    SwrContext*      m_swr_ctx     = nullptr;
    AVFrame*         m_temp_audio_frame = nullptr;
    AVPacket*        m_demux_pkt   = nullptr;
    double           m_fps         = 29.97;
    double           m_duration    = 0.0;   // seconds (0 = unknown)
    double           m_clock       = 0.0;   // last decoded video PTS (seconds)
    bool             m_resync_video = false; // drop to next keyframe after a seek

    static void save_ppm(AVFrame* frame, int width, int height, const std::string& filename);
};
