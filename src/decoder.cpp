#include "decoder.h"
#include <algorithm>
#include <cstring>
#include <sstream>

static constexpr size_t AVIO_BUF_SIZE = 64 * 1024;

// Sony PSMF ATRAC3+ framing: each frame is prefixed by an 8-byte sync header and
// the stride between consecutive frames is fixed at 696 bytes (688-byte payload).
static const uint8_t SONY_FRAME_HEADER[8] = { 0x0F, 0xD0, 0x48, 0x55, 0x00, 0x00, 0x00, 0x00 };
static constexpr size_t SONY_FRAME_STRIDE = 696;
static constexpr size_t PSMF_CHUNK        = 4 * 1024 * 1024;  // read granularity for the 0xBD walk

// Best-effort presentation time of a decoded frame, in seconds (-1 if unknown).
static double frame_time(const AVFrame* f, const AVStream* st) {
    if (!f || !st) return -1.0;
    const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE
                           ? f->best_effort_timestamp : f->pts;
    if (ts == AV_NOPTS_VALUE) return -1.0;
    return static_cast<double>(ts) * av_q2d(st->time_base);
}

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

    m_iso_path = iso_path;
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
                if (st->duration != AV_NOPTS_VALUE && st->duration > 0) {
                    m_duration = st->duration * av_q2d(st->time_base);
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

    // FFmpeg's MPEG-PS demuxer does not surface the Sony private stream, so if
    // no audio stream was detected fall back to hand-demuxing the ATRAC3+ track.
    if (m_audio.stream_index == -1) {
        init_psmf_audio();
    }

    return (m_video.stream_index != -1 || m_audio.stream_index != -1 || m_psmf_audio);
}

double Decoder::get_duration() const {
    if (m_duration > 0.0) return m_duration;
    if (m_fmt_ctx && m_fmt_ctx->duration != AV_NOPTS_VALUE && m_fmt_ctx->duration > 0) {
        return static_cast<double>(m_fmt_ctx->duration) / AV_TIME_BASE;
    }
    return 0.0;
}

bool Decoder::init_psmf_audio() {

    m_psmf_file.open(m_iso_path, std::ios::binary);
    if (!m_psmf_file.is_open()) return false;

    const AVCodec* audio_decoder = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
    if (!audio_decoder) return false;

    m_audio.codec = audio_decoder;
    m_audio.codec_ctx = avcodec_alloc_context3(audio_decoder);
    m_audio.codec_ctx->sample_rate = 44100;
    m_audio.codec_ctx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    m_audio.codec_ctx->block_align = static_cast<int>(SONY_FRAME_STRIDE);
    m_audio.codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    if (avcodec_open2(m_audio.codec_ctx, audio_decoder, nullptr) < 0) {
        avcodec_free_context(&m_audio.codec_ctx);
        m_audio.codec = nullptr;
        return false;
    }

    if (!m_swr_ctx) {
        swr_alloc_set_opts2(
            &m_swr_ctx,
            &m_audio.codec_ctx->ch_layout, AV_SAMPLE_FMT_S16, 44100,
            &m_audio.codec_ctx->ch_layout, m_audio.codec_ctx->sample_fmt, 44100,
            0, nullptr);
        if (m_swr_ctx) swr_init(m_swr_ctx);
    }

    m_psmf_pkt = av_packet_alloc();
    m_psmf_video = av_frame_alloc();
    m_psmf_audio = true;
    m_psmf_eof = false;
    m_psmf_cursor = 0;
    m_psmf_pes.clear();
    m_psmf_pos = 0;
    m_psmf_samples = 0;
    m_psmf_video_pending = false;
    return true;
}

// Read forward from the PSMF cursor, walking MPEG-PS packets and appending the
// payload of every private-stream-1 (0xBD) packet to m_psmf_pes. The MPEG-PS
// pack/system headers are parsed generically so video packets are skipped.
bool Decoder::psmf_read_more() {
    if (m_psmf_eof) return false;
    if (!m_psmf_file.is_open()) return false;

    while (m_psmf_cursor < m_io.file_size) {
        size_t want = static_cast<size_t>(
            std::min<uint64_t>(PSMF_CHUNK, m_io.file_size - m_psmf_cursor));
        m_psmf_scan.resize(want);
        m_psmf_file.clear();
        m_psmf_file.seekg(static_cast<std::streamoff>(m_io.base_offset + m_psmf_cursor),
                          std::ios::beg);
        m_psmf_file.read(reinterpret_cast<char*>(m_psmf_scan.data()),
                         static_cast<std::streamsize>(want));
        size_t got = static_cast<size_t>(m_psmf_file.gcount());
        if (got == 0) { m_psmf_eof = true; return false; }

        const uint8_t* d = m_psmf_scan.data();
        size_t p = 0;
        size_t last_end = 0;        // end offset of the last fully consumed packet
        bool any_parsed = false;    // parsed >= 1 complete packet/pack header
        bool consumed_something = false;

        while (p + 6 <= got) {
            // Locate the next start code.
            size_t sc = p;
            while (sc + 4 <= got && !(d[sc] == 0 && d[sc + 1] == 0 && d[sc + 2] == 1)) ++sc;
            if (sc + 4 > got) break;

            uint8_t sid = d[sc + 3];
            if (sid == 0xBA) {  // MPEG-2 pack header (14 bytes)
                p = sc + 14;
                last_end = p;
                any_parsed = true;
                continue;
            }
            if (sid == 0xB9) { m_psmf_eof = true; break; }  // program end

            if (sc + 6 > got) break;
            uint16_t plen = static_cast<uint16_t>((d[sc + 4] << 8) | d[sc + 5]);
            if (plen == 0) break;
            size_t pes_end = sc + 6 + plen;
            if (pes_end > got) break;  // packet spans the chunk boundary; stop here

            if (sid == 0xBD) {
                uint8_t hdrlen = d[sc + 8];
                size_t data_off = sc + 9 + hdrlen + 4;  // skip PES opts + 4-byte substream hdr
                if (data_off < pes_end) {
                    m_psmf_pes.insert(m_psmf_pes.end(), d + data_off, d + pes_end);
                    consumed_something = true;
                }
            }
            p = pes_end;
            last_end = p;
            any_parsed = true;
        }

        if (any_parsed) {
            m_psmf_cursor += last_end;
            if (consumed_something) return true;
            continue;  // only video/pack packets in this chunk; read the next one
        }

        // A packet straddles the chunk start (only possible at true EOF with a
        // 4 MiB read window) or no start code was found: stop.
        m_psmf_eof = true;
        return false;
    }
    m_psmf_eof = true;
    return false;
}

// Decode the next available ATRAC3+ frame into out_pcm (S16 stereo).
void Decoder::process_psmf_audio(AVFrame* audio_frame, std::vector<uint8_t>& out_pcm, bool flush) {
    if (!m_audio.codec_ctx) return;

    while (true) {
        // Ensure we have at least one full frame buffered.
        while (m_psmf_pos + SONY_FRAME_STRIDE <= m_psmf_pes.size()) {
            const uint8_t* unit = m_psmf_pes.data() + m_psmf_pos;
            if (std::memcmp(unit, SONY_FRAME_HEADER, 8) != 0) {
                // Lost sync: rescan for the next Sony header.
                ++m_psmf_pos;
                continue;
            }
            av_packet_unref(m_psmf_pkt);
            m_psmf_pkt->data = const_cast<uint8_t*>(unit + 8);
            m_psmf_pkt->size = static_cast<int>(SONY_FRAME_STRIDE - 8);
            m_psmf_pos += SONY_FRAME_STRIDE;

            if (avcodec_send_packet(m_audio.codec_ctx, m_psmf_pkt) < 0) continue;
            bool got = false;
            while (avcodec_receive_frame(m_audio.codec_ctx, audio_frame) >= 0) {
                resample_to_s16(audio_frame, out_pcm);
                m_psmf_samples += audio_frame->nb_samples;
                got = true;
            }
            if (got) return;
        }

        if (flush) {
            // Drain the decoder.
            av_packet_unref(m_psmf_pkt);
            m_psmf_pkt->data = nullptr;
            m_psmf_pkt->size = 0;
            if (avcodec_send_packet(m_audio.codec_ctx, m_psmf_pkt) >= 0) {
                while (avcodec_receive_frame(m_audio.codec_ctx, audio_frame) >= 0) {
                    resample_to_s16(audio_frame, out_pcm);
                    m_psmf_samples += audio_frame->nb_samples;
                }
            }
            return;
        }

        // Need more data: drop the parsed prefix and pull another chunk.
        if (m_psmf_pos > 0) {
            m_psmf_pes.erase(m_psmf_pes.begin(), m_psmf_pes.begin() + m_psmf_pos);
            m_psmf_pos = 0;
        }
        if (!psmf_read_more()) {
            flush = true;
        }
    }
}

bool Decoder::resample_to_s16(AVFrame* audio_frame, std::vector<uint8_t>& out_pcm) {
    if (!m_swr_ctx || !audio_frame) return false;
    int out_samples = audio_frame->nb_samples;
    int dst_bufsize = av_samples_get_buffer_size(nullptr, 2, out_samples, AV_SAMPLE_FMT_S16, 0);
    if (dst_bufsize <= 0) return false;

    size_t orig = out_pcm.size();
    out_pcm.resize(orig + static_cast<size_t>(dst_bufsize));
    uint8_t* out_ptr = out_pcm.data() + orig;

    const uint8_t* in_data[AV_NUM_DATA_POINTERS] = { nullptr };
    for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i) in_data[i] = audio_frame->data[i];

    int converted = swr_convert(m_swr_ctx, &out_ptr, out_samples, in_data, audio_frame->nb_samples);
    if (converted < 0) {
        out_pcm.resize(orig);
        return false;
    }
    out_pcm.resize(orig + static_cast<size_t>(converted) * 2 * sizeof(int16_t));
    return true;
}

void Decoder::reset_psmf_audio(uint64_t scan_offset) {
    m_psmf_cursor = scan_offset;
    m_psmf_pes.clear();
    m_psmf_pos = 0;
    m_psmf_eof = false;
    m_psmf_samples = 0;
    m_psmf_video_pending = false;
    if (m_audio.codec_ctx) avcodec_flush_buffers(m_audio.codec_ctx);
}

// Decode one video frame straight from the FFmpeg demuxer, skipping audio.
FrameType Decoder::decode_video_frame(AVFrame* out) {
    if (!m_video.codec_ctx || !out) return FrameType::Eof;
    while (true) {
        if (avcodec_receive_frame(m_video.codec_ctx, out) >= 0) {
            m_resync_video = false;
            return FrameType::Video;
        }
        int ret = av_read_frame(m_fmt_ctx, m_demux_pkt);
        if (ret < 0) return FrameType::Eof;
        if (m_video.stream_index != -1 && m_demux_pkt->stream_index == m_video.stream_index) {
            // After a seek, drop packets until the next keyframe so the decoder
            // never starts on a partial GOP (avoids corrupt macroblocks).
            if (m_resync_video && !(m_demux_pkt->flags & AV_PKT_FLAG_KEY)) {
                av_packet_unref(m_demux_pkt);
                continue;
            }
            int r = avcodec_send_packet(m_video.codec_ctx, m_demux_pkt);
            av_packet_unref(m_demux_pkt);
            if (r >= 0) {
                if (avcodec_receive_frame(m_video.codec_ctx, out) >= 0) {
                    m_resync_video = false;
                    return FrameType::Video;
                }
            }
        } else {
            av_packet_unref(m_demux_pkt);
        }
    }
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

    // Hand-demuxed ATRAC3+ path: pace audio against the video frame clock so the
    // caller receives interleaved Video/Audio events in roughly real time.
    if (m_psmf_audio) {
        if (!m_psmf_video_pending) {
            if (decode_video_frame(m_psmf_video) != FrameType::Video) {
                return FrameType::Eof;
            }
            m_psmf_video_pending = true;
        }

        const double fps = (m_fps > 0.0 && m_fps < 240.0) ? m_fps : 29.97;
        const int64_t target = static_cast<int64_t>((m_vcount + 1) * (44100.0 / fps));
        while (m_psmf_samples < target) {
            process_psmf_audio(m_temp_audio_frame, audio_pcm, false);
            if (audio_pcm.empty()) break;  // no more audio available
        }
        if (!audio_pcm.empty()) return FrameType::Audio;

        if (video_frame) {
            av_frame_unref(video_frame);
            av_frame_ref(video_frame, m_psmf_video);
        }
        m_psmf_video_pending = false;
        m_vcount++;
        // Advance the wall clock from the stashed frame's PTS.
        const double pts = frame_time(m_psmf_video, m_fmt_ctx->streams[m_video.stream_index]);
        if (pts >= 0.0) m_clock = pts;
        else m_clock += 1.0 / ((m_fps > 0.0 && m_fps < 240.0) ? m_fps : 29.97);
        return FrameType::Video;
    }

    while (true) {
        // First check if video codec context has any buffered frames available
        if (m_video.codec_ctx && video_frame) {
            if (avcodec_receive_frame(m_video.codec_ctx, video_frame) >= 0) {
                const double pts = frame_time(video_frame, m_fmt_ctx->streams[m_video.stream_index]);
                if (pts >= 0.0) m_clock = pts;
                else m_clock += 1.0 / ((m_fps > 0.0 && m_fps < 240.0) ? m_fps : 29.97);
                return FrameType::Video;
            }
        }

        int ret = av_read_frame(m_fmt_ctx, m_demux_pkt);
        if (ret < 0) {
            return FrameType::Eof;
        }

        if (m_video.stream_index != -1 && m_demux_pkt->stream_index == m_video.stream_index) {
            // After a seek, drop packets until the next keyframe so the decoder
            // never starts on a partial GOP (avoids corrupt macroblocks).
            if (m_resync_video && !(m_demux_pkt->flags & AV_PKT_FLAG_KEY)) {
                av_packet_unref(m_demux_pkt);
                continue;
            }
            if (avcodec_send_packet(m_video.codec_ctx, m_demux_pkt) >= 0) {
                av_packet_unref(m_demux_pkt);
                if (video_frame && avcodec_receive_frame(m_video.codec_ctx, video_frame) >= 0) {
                    m_resync_video = false;
                    const double pts = frame_time(video_frame, m_fmt_ctx->streams[m_video.stream_index]);
                    if (pts >= 0.0) m_clock = pts;
                    else m_clock += 1.0 / ((m_fps > 0.0 && m_fps < 240.0) ? m_fps : 29.97);
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
    return seek_absolute(m_clock + seconds);
}

// Scan for the MPEG-PS pack / system-header start code nearest at-or-before
// `target`, so the hand demuxer re-anchors on a real packet boundary rather
// than mid-payload. Returns `target` unchanged if no start code is found.
uint64_t Decoder::psmf_find_scan_offset(uint64_t target) {
    if (!m_psmf_file.is_open()) return target;
    if (target >= m_io.file_size) return target;

    const uint64_t window = 64 * 1024;
    const uint64_t begin = target > window ? target - window : 0;
    const size_t want = static_cast<size_t>(target - begin);
    if (want == 0) return target;

    m_psmf_scan.resize(want);
    m_psmf_file.clear();
    m_psmf_file.seekg(static_cast<std::streamoff>(m_io.base_offset + begin), std::ios::beg);
    m_psmf_file.read(reinterpret_cast<char*>(m_psmf_scan.data()),
                     static_cast<std::streamsize>(want));
    const size_t got = static_cast<size_t>(m_psmf_file.gcount());

    // Walk backwards for the last 00 00 01 BA/BB (pack or system header).
    for (size_t i = got; i >= 4; --i) {
        const uint8_t* p = m_psmf_scan.data() + (i - 4);
        if (p[0] == 0 && p[1] == 0 && p[2] == 1 &&
            (p[3] == 0xBA || p[3] == 0xBB)) {
            return begin + (i - 4);
        }
    }
    return target;
}

bool Decoder::seek_absolute(double seconds) {
    if (!m_fmt_ctx) return false;

    int primary_stream = m_video.stream_index != -1 ? m_video.stream_index : m_audio.stream_index;
    if (primary_stream == -1) return false;

    AVStream* st = m_fmt_ctx->streams[primary_stream];

    const double dur = get_duration();
    if (seconds < 0.0) seconds = 0.0;
    if (dur > 0.0 && seconds > dur) seconds = dur;

    const int64_t seek_offset = static_cast<int64_t>(seconds * AV_TIME_BASE);
    const int flags = AVSEEK_FLAG_BACKWARD;

    if (m_video.codec_ctx) avcodec_flush_buffers(m_video.codec_ctx);
    if (m_audio.codec_ctx) avcodec_flush_buffers(m_audio.codec_ctx);
    m_audio_buffer.clear();
    m_psmf_video_pending = false;
    m_resync_video = true;

    int ret = av_seek_frame(m_fmt_ctx, -1, seek_offset, flags);
    if (ret < 0) {
        ret = av_seek_frame(m_fmt_ctx, primary_stream,
                            static_cast<int64_t>(seconds / av_q2d(st->time_base)), flags);
    }

    if (m_avio_ctx) avio_flush(m_avio_ctx);

    if (m_psmf_audio) {
        // Re-anchor the manual ATRAC3+ demuxer. Prefer the byte position the
        // FFmpeg demuxer landed on; fall back to a proportional estimate of the
        // MPEG-PS offset, snapped to a pack header.
        int64_t pos = m_avio_ctx ? avio_tell(m_avio_ctx) : 0;
        if (pos <= 0 && dur > 0.0 && m_io.file_size > 0) {
            pos = static_cast<int64_t>((seconds / dur) * m_io.file_size);
        }
        if (pos < 0) pos = 0;
        reset_psmf_audio(psmf_find_scan_offset(static_cast<uint64_t>(pos)));
        m_vcount = 0;
    }

    m_clock = seconds;
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
    if (m_psmf_pkt) { av_packet_free(&m_psmf_pkt); }
    if (m_psmf_video) { av_frame_free(&m_psmf_video); }
    if (m_psmf_file.is_open()) m_psmf_file.close();
    m_psmf_audio = false;
    m_psmf_pes.clear();
    m_psmf_scan.clear();
    m_psmf_pos = 0;
    m_psmf_cursor = 0;
    m_psmf_samples = 0;
    m_psmf_video_pending = false;
    m_vcount = 0;
    m_clock = 0.0;
    m_resync_video = false;
    if (m_swr_ctx) { swr_free(&m_swr_ctx); }
    if (m_temp_audio_frame) { av_frame_free(&m_temp_audio_frame); }
    if (m_demux_pkt) { av_packet_free(&m_demux_pkt); }
    if (m_video.codec_ctx) { avcodec_free_context(&m_video.codec_ctx); m_video = {}; }
    if (m_audio.codec_ctx) { avcodec_free_context(&m_audio.codec_ctx); m_audio = {}; }
    if (m_fmt_ctx) { avformat_close_input(&m_fmt_ctx); m_fmt_ctx = nullptr; }
    if (m_avio_ctx) { av_freep(&m_avio_ctx->buffer); avio_context_free(&m_avio_ctx); }
    if (m_io.iso_file.is_open()) m_io.iso_file.close();
}
