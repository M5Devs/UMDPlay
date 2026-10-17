#include "decoder.h"
#include <algorithm>
#include <cstring>
#include <sstream>

static constexpr size_t AVIO_BUF_SIZE = 64 * 1024;

Decoder::Decoder() {}
Decoder::~Decoder() { close(); }

int Decoder::read_packet(void* opaque, uint8_t* buf, int buf_size) {
    auto* io = static_cast<StreamIoState*>(opaque);
    if (!io->iso_file.is_open()) return AVERROR_EOF;

    if (io->file_size > 0 && io->cursor >= io->file_size) return AVERROR_EOF;

    uint64_t bytes_to_read = buf_size;
    if (io->file_size > 0) {
        bytes_to_read = std::min(bytes_to_read, io->file_size - io->cursor);
    }
    if (bytes_to_read == 0) return AVERROR_EOF;

    io->iso_file.clear();
    io->iso_file.seekg(io->base_offset + io->cursor, std::ios::beg);
    io->iso_file.read(reinterpret_cast<char*>(buf), bytes_to_read);
    std::streamsize bytes_read = io->iso_file.gcount();

    if (bytes_read <= 0) return AVERROR_EOF;
    io->cursor += bytes_read;
    return static_cast<int>(bytes_read);
}

int64_t Decoder::seek_packet(void* opaque, int64_t offset, int whence) {
    auto* io = static_cast<StreamIoState*>(opaque);
    if (!io->iso_file.is_open()) return -1;

    if (whence == AVSEEK_SIZE) {
        return io->file_size > 0 ? io->file_size : -1;
    }

    int64_t new_cursor = 0;
    switch (whence) {
        case SEEK_SET: new_cursor = offset; break;
        case SEEK_CUR: new_cursor = io->cursor + offset; break;
        case SEEK_END:
            if (io->file_size == 0) return -1;
            new_cursor = io->file_size + offset;
            break;
        default: return -1;
    }

    if (new_cursor < 0) return -1;
    io->iso_file.clear();
    io->cursor = new_cursor;
    return io->cursor;
}

bool Decoder::open_stream(const std::string& iso_path, const MpsEntry& entry) {
    close();

    m_io.iso_file.open(iso_path, std::ios::binary);
    if (!m_io.iso_file.is_open()) return false;

    m_io.base_offset = static_cast<uint64_t>(entry.lba) * UMD_SECTOR_SIZE;
    m_io.file_size   = entry.size_bytes;
    m_io.cursor      = 0;

    m_avio_buffer = static_cast<uint8_t*>(av_malloc(AVIO_BUF_SIZE));
    if (!m_avio_buffer) return false;

    m_avio_ctx = avio_alloc_context(
        m_avio_buffer, AVIO_BUF_SIZE, 0, &m_io,
        Decoder::read_packet, nullptr, Decoder::seek_packet
    );
    if (!m_avio_ctx) return false;

    m_fmt_ctx = avformat_alloc_context();
    m_fmt_ctx->pb = m_avio_ctx;

    if (avformat_open_input(&m_fmt_ctx, nullptr, nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(m_fmt_ctx, nullptr) < 0) return false;

    for (unsigned int i = 0; i < m_fmt_ctx->nb_streams; i++) {
        AVStream* st = m_fmt_ctx->streams[i];
        AVCodecParameters* codecpar = st->codecpar;

        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO && m_video.stream_index == -1) {
            const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
            if (decoder) {
                m_video.stream_index = i;
                m_video.codec = decoder;
                m_video.codec_ctx = avcodec_alloc_context3(decoder);
                avcodec_parameters_to_context(m_video.codec_ctx, codecpar);
                avcodec_open2(m_video.codec_ctx, decoder, nullptr);
            }
        }
    }

    return (m_video.stream_index != -1);
}

bool Decoder::test_decode(int max_frames, const std::string& dump_frame_path) {
    if (!m_video.codec_ctx) return false;

    avio_flush(m_avio_ctx);
    avio_seek(m_avio_ctx, 0, SEEK_SET);
    av_seek_frame(m_fmt_ctx, m_video.stream_index, 0, AVSEEK_FLAG_BACKWARD);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int decoded_frames = 0;

    while (av_read_frame(m_fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == m_video.stream_index) {
            if (avcodec_send_packet(m_video.codec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(m_video.codec_ctx, frame) >= 0) {
                    decoded_frames++;
                    if (decoded_frames == 1) {
                        save_ppm(frame, m_video.codec_ctx->width, m_video.codec_ctx->height, dump_frame_path);
                    }
                    if (decoded_frames >= max_frames) break;
                }
            }
        }
        av_packet_unref(pkt);
        if (decoded_frames >= max_frames) break;
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    return decoded_frames > 0;
}

void Decoder::save_ppm(AVFrame* frame, int width, int height, const std::string& filename) {
    SwsContext* sws = sws_getContext(
        width, height, static_cast<AVPixelFormat>(frame->format),
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!sws) return;

    std::vector<uint8_t> rgb_buffer(width * height * 3);
    uint8_t* dst_data[1] = { rgb_buffer.data() };
    int dst_linesize[1] = { 3 * width };

    sws_scale(sws, frame->data, frame->linesize, 0, height, dst_data, dst_linesize);
    sws_freeContext(sws);

    std::ofstream out(filename, std::ios::binary);
    if (!out) return;
    out << "P6\n" << width << " " << height << "\n255\n";
    out.write(reinterpret_cast<char*>(rgb_buffer.data()), rgb_buffer.size());
}

void Decoder::close() {
    if (m_video.codec_ctx) { avcodec_free_context(&m_video.codec_ctx); m_video = {}; }
    if (m_audio.codec_ctx) { avcodec_free_context(&m_audio.codec_ctx); m_audio = {}; }
    if (m_fmt_ctx) { avformat_close_input(&m_fmt_ctx); m_fmt_ctx = nullptr; }
    if (m_avio_ctx) { av_freep(&m_avio_ctx->buffer); avio_context_free(&m_avio_ctx); }
    if (m_io.iso_file.is_open()) m_io.iso_file.close();
}
