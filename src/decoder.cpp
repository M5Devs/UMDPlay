#include "decoder.h"
#include <algorithm>
#include <cstring>
#include <sstream>

static constexpr size_t AVIO_BUF_SIZE = 64 * 1024;
static const uint8_t SONY_FRAME_HEADER[8] = { 0x0F, 0xD0, 0x48, 0x55, 0x00, 0x00, 0x00, 0x00 };

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

                if (st->avg_frame_rate.den > 0 && st->avg_frame_rate.num > 0) {
                    m_fps = av_q2d(st->avg_frame_rate);
                } else if (st->r_frame_rate.den > 0 && st->r_frame_rate.num > 0) {
                    m_fps = av_q2d(st->r_frame_rate);
                }
            }
        } else if ((codecpar->codec_type == AVMEDIA_TYPE_AUDIO || st->id == 0x1BD || codecpar->codec_id == AV_CODEC_ID_ATRAC3P) && m_audio.stream_index == -1) {
            m_audio.stream_index = i;
        }
    }

    if (m_audio.stream_index == -1) {
        // Fallback: check streams by ID if not recognized as AVMEDIA_TYPE_AUDIO
        for (unsigned int i = 0; i < m_fmt_ctx->nb_streams; i++) {
            AVStream* st = m_fmt_ctx->streams[i];
            if (st->id == 0x1BD || st->id == 0xBD) {
                m_audio.stream_index = i;
                break;
            }
        }
    }

    if (m_audio.stream_index != -1) {
        // Calculate block_align by scanning first audio packets for 8-byte Sony frame sync headers
        avio_seek(m_avio_ctx, 0, SEEK_SET);
        av_seek_frame(m_fmt_ctx, m_audio.stream_index, 0, AVSEEK_FLAG_BACKWARD);

        AVPacket* probe_pkt = av_packet_alloc();
        std::vector<size_t> header_offsets;
        int packets_read = 0;

        while (av_read_frame(m_fmt_ctx, probe_pkt) >= 0 && packets_read < 50) {
            if (probe_pkt->stream_index == m_audio.stream_index) {
                packets_read++;
                const uint8_t* data = probe_pkt->data;
                int size = probe_pkt->size;

                // Strip PES sub-header if present
                if (size > 4 && data[0] == 0x00) {
                    data += 4;
                    size -= 4;
                }

                for (int pos = 0; pos <= size - 8; pos++) {
                    if (std::memcmp(data + pos, SONY_FRAME_HEADER, 8) == 0) {
                        header_offsets.push_back(pos);
                    }
                }

                if (header_offsets.size() >= 2) break;
            }
            av_packet_unref(probe_pkt);
        }
        av_packet_free(&probe_pkt);

        int calculated_block_align = 0;
        if (header_offsets.size() >= 2) {
            calculated_block_align = static_cast<int>(header_offsets[1] - header_offsets[0] - 8);
        } else {
            // Default fallback block align for Sony PSMF ATRAC3+ stereo if probing single frame
            calculated_block_align = 368;
        }

        const AVCodec* audio_decoder = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
        if (audio_decoder) {
            m_audio.codec = audio_decoder;
            m_audio.codec_ctx = avcodec_alloc_context3(audio_decoder);
            m_audio.block_align = calculated_block_align;

            m_audio.codec_ctx->sample_rate = 44100;
            m_audio.codec_ctx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
            m_audio.codec_ctx->block_align = calculated_block_align;
            m_audio.codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;

            if (avcodec_open2(m_audio.codec_ctx, audio_decoder, nullptr) < 0) {
                avcodec_free_context(&m_audio.codec_ctx);
                m_audio.stream_index = -1;
            } else {
                // Initialize SwrContext for audio resampling (FLTP -> S16 44100Hz stereo)
                swr_alloc_set_opts2(
                    &m_swr_ctx,
                    &m_audio.codec_ctx->ch_layout,
                    AV_SAMPLE_FMT_S16,
                    44100,
                    &m_audio.codec_ctx->ch_layout,
                    m_audio.codec_ctx->sample_fmt,
                    m_audio.codec_ctx->sample_rate,
                    0, nullptr
                );
                if (m_swr_ctx) {
                    swr_init(m_swr_ctx);
                }
            }
        } else {
            m_audio.stream_index = -1;
        }

        // Reset stream read positions
        avio_flush(m_avio_ctx);
        avio_seek(m_avio_ctx, 0, SEEK_SET);
    }

    m_temp_audio_frame = av_frame_alloc();
    m_demux_pkt = av_packet_alloc();

    return (m_video.stream_index != -1 || m_audio.stream_index != -1);
}

void Decoder::process_audio_packet(AVPacket* pkt, AVFrame* audio_frame, std::vector<uint8_t>* out_pcm) {
    if (!m_audio.codec_ctx || pkt->size <= 0) return;

    const uint8_t* data = pkt->data;
    int size = pkt->size;

    if (size > 4 && data[0] == 0x00) {
        data += 4;
        size -= 4;
    }

    m_audio_buffer.insert(m_audio_buffer.end(), data, data + size);

    while (m_audio_buffer.size() >= 8) {
        auto it = std::search(m_audio_buffer.begin(), m_audio_buffer.end(),
                              SONY_FRAME_HEADER, SONY_FRAME_HEADER + 8);
        if (it == m_audio_buffer.end()) {
            if (m_audio_buffer.size() > 7) {
                m_audio_buffer.erase(m_audio_buffer.begin(), m_audio_buffer.end() - 7);
            }
            break;
        }

        size_t header_idx = std::distance(m_audio_buffer.begin(), it);
        size_t pure_frame_size = m_audio.block_align > 0 ? m_audio.block_align : 368;
        size_t total_frame_bytes = 8 + pure_frame_size;

        if (m_audio_buffer.size() - header_idx < total_frame_bytes) {
            if (header_idx > 0) {
                m_audio_buffer.erase(m_audio_buffer.begin(), m_audio_buffer.begin() + header_idx);
            }
            break;
        }

        const uint8_t* pure_atrac3p_data = m_audio_buffer.data() + header_idx + 8;

        AVPacket* audio_pkt = av_packet_alloc();
        audio_pkt->data = const_cast<uint8_t*>(pure_atrac3p_data);
        audio_pkt->size = static_cast<int>(pure_frame_size);

        if (avcodec_send_packet(m_audio.codec_ctx, audio_pkt) >= 0) {
            while (avcodec_receive_frame(m_audio.codec_ctx, audio_frame) >= 0) {
                if (out_pcm && m_swr_ctx) {
                    int out_samples = audio_frame->nb_samples;
                    int dst_bufsize = av_samples_get_buffer_size(nullptr, 2, out_samples, AV_SAMPLE_FMT_S16, 0);
                    if (dst_bufsize > 0) {
                        size_t orig_size = out_pcm->size();
                        out_pcm->resize(orig_size + dst_bufsize);
                        uint8_t* out_ptr = out_pcm->data() + orig_size;

                        const uint8_t* in_data[AV_NUM_DATA_POINTERS] = { nullptr };
                        for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
                            in_data[i] = audio_frame->data[i];
                        }

                        swr_convert(m_swr_ctx, &out_ptr, out_samples, in_data, audio_frame->nb_samples);
                    }
                }
            }
        }

        av_packet_free(&audio_pkt);
        m_audio_buffer.erase(m_audio_buffer.begin(), m_audio_buffer.begin() + header_idx + total_frame_bytes);
    }
}

FrameType Decoder::read_frame(AVFrame* video_frame, std::vector<uint8_t>& audio_pcm) {
    if (!m_fmt_ctx) return FrameType::Eof;

    audio_pcm.clear();

    while (true) {
        // First check if video codec context has any buffered frames available
        if (m_video.codec_ctx && video_frame) {
            if (avcodec_receive_frame(m_video.codec_ctx, video_frame) >= 0) {
                return FrameType::Video;
            }
        }

        int ret = av_read_frame(m_fmt_ctx, m_demux_pkt);
        if (ret < 0) {
            return FrameType::Eof;
        }

        if (m_video.stream_index != -1 && m_demux_pkt->stream_index == m_video.stream_index) {
            if (avcodec_send_packet(m_video.codec_ctx, m_demux_pkt) >= 0) {
                av_packet_unref(m_demux_pkt);
                if (video_frame && avcodec_receive_frame(m_video.codec_ctx, video_frame) >= 0) {
                    return FrameType::Video;
                }
            } else {
                av_packet_unref(m_demux_pkt);
            }
        } else if (m_audio.stream_index != -1 && m_demux_pkt->stream_index == m_audio.stream_index) {
            process_audio_packet(m_demux_pkt, m_temp_audio_frame, &audio_pcm);
            av_packet_unref(m_demux_pkt);
            if (!audio_pcm.empty()) {
                return FrameType::Audio;
            }
        } else {
            av_packet_unref(m_demux_pkt);
        }
    }
}

bool Decoder::seek_relative(double seconds) {
    if (!m_fmt_ctx) return false;

    int primary_stream = m_video.stream_index != -1 ? m_video.stream_index : m_audio.stream_index;
    if (primary_stream == -1) return false;

    AVStream* st = m_fmt_ctx->streams[primary_stream];

    int64_t seek_offset = static_cast<int64_t>(seconds * AV_TIME_BASE);
    int flags = seconds < 0 ? AVSEEK_FLAG_BACKWARD : 0;

    if (m_video.codec_ctx) {
        avcodec_flush_buffers(m_video.codec_ctx);
    }
    if (m_audio.codec_ctx) {
        avcodec_flush_buffers(m_audio.codec_ctx);
    }
    m_audio_buffer.clear();

    int ret = av_seek_frame(m_fmt_ctx, -1, seek_offset, flags);
    if (ret < 0) {
        ret = av_seek_frame(m_fmt_ctx, primary_stream, static_cast<int64_t>(seconds / av_q2d(st->time_base)), flags);
    }

    if (m_avio_ctx) {
        avio_flush(m_avio_ctx);
    }

    return ret >= 0;
}

bool Decoder::test_decode(int max_frames, const std::string& dump_frame_path) {
    if (!m_video.codec_ctx && !m_audio.codec_ctx) return false;

    avio_flush(m_avio_ctx);
    avio_seek(m_avio_ctx, 0, SEEK_SET);

    int primary_index = m_video.stream_index != -1 ? m_video.stream_index : m_audio.stream_index;
    av_seek_frame(m_fmt_ctx, primary_index, 0, AVSEEK_FLAG_BACKWARD);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* video_frame = av_frame_alloc();
    AVFrame* audio_frame = av_frame_alloc();
    int decoded_video_frames = 0;
    int decoded_audio_frames = 0;

    m_audio_buffer.clear();

    while (av_read_frame(m_fmt_ctx, pkt) >= 0) {
        if (m_video.stream_index != -1 && pkt->stream_index == m_video.stream_index) {
            if (avcodec_send_packet(m_video.codec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(m_video.codec_ctx, video_frame) >= 0) {
                    decoded_video_frames++;
                    if (decoded_video_frames == 1) {
                        save_ppm(video_frame, m_video.codec_ctx->width, m_video.codec_ctx->height, dump_frame_path);
                    }
                    if (decoded_video_frames >= max_frames) break;
                }
            }
        } else if (m_audio.stream_index != -1 && pkt->stream_index == m_audio.stream_index) {
            process_audio_packet(pkt, audio_frame, nullptr);
            decoded_audio_frames++;
        }
        av_packet_unref(pkt);
        if (m_video.stream_index != -1 && decoded_video_frames >= max_frames) break;
    }

    if (decoded_audio_frames > 0) {
        std::cout << "[+] Decoded " << decoded_audio_frames << " audio frames in memory.\n";
    }

    av_frame_free(&audio_frame);
    av_frame_free(&video_frame);
    av_packet_free(&pkt);
    return (m_video.stream_index != -1 ? decoded_video_frames > 0 : decoded_audio_frames > 0);
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
    m_audio_buffer.clear();
    if (m_swr_ctx) { swr_free(&m_swr_ctx); }
    if (m_temp_audio_frame) { av_frame_free(&m_temp_audio_frame); }
    if (m_demux_pkt) { av_packet_free(&m_demux_pkt); }
    if (m_video.codec_ctx) { avcodec_free_context(&m_video.codec_ctx); m_video = {}; }
    if (m_audio.codec_ctx) { avcodec_free_context(&m_audio.codec_ctx); m_audio = {}; }
    if (m_fmt_ctx) { avformat_close_input(&m_fmt_ctx); m_fmt_ctx = nullptr; }
    if (m_avio_ctx) { av_freep(&m_avio_ctx->buffer); avio_context_free(&m_avio_ctx); }
    if (m_io.iso_file.is_open()) m_io.iso_file.close();
}
