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

class Decoder {
public:
    Decoder();
    ~Decoder();

    bool open_stream(const std::string& iso_path, const MpsEntry& entry);
    void close();

    bool test_decode(int max_frames = 100, const std::string& dump_frame_path = "output_frame.ppm");

    bool has_video() const { return m_video.stream_index != -1; }
    bool has_audio() const { return m_audio.stream_index != -1; }

private:
    struct StreamIoState {
        std::ifstream iso_file;
        uint64_t      base_offset = 0;   // entry.lba * 2048
        uint64_t      file_size   = 0;   // entry.size_bytes
        uint64_t      cursor      = 0;   // offset relative to base
    };

    static int     read_packet(void* opaque, uint8_t* buf, int buf_size);
    static int64_t seek_packet(void* opaque, int64_t offset, int whence);

    void process_audio_packet(AVPacket* pkt, AVFrame* audio_frame, int& audio_frames_decoded);

    StreamIoState    m_io;
    uint8_t*         m_avio_buffer = nullptr;
    AVIOContext*     m_avio_ctx    = nullptr;
    AVFormatContext* m_fmt_ctx     = nullptr;

    StreamInfo       m_video;
    StreamInfo       m_audio;
    std::vector<uint8_t> m_audio_buffer;

    static void save_ppm(AVFrame* frame, int width, int height, const std::string& filename);
};
