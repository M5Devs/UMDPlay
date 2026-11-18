#include "decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

// The video elementary stream is timestamped on the PES 90 kHz clock, so the
// codec is primed with that packet time base and frame PTS stays in 90 kHz.
static constexpr int PSMF_TIMESCALE = 90000;

// Largest PCM chunk handed back per read_frame (a couple of ATRAC3+ frames).
static constexpr size_t PCM_CHUNK_CAP = 16384;

Decoder::Decoder() {}
Decoder::~Decoder() { close(); }

// --- helpers --------------------------------------------------------------

double Decoder::frame_seconds(const AVFrame* f) {
    if (!f) return -1.0;
    const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE
                           ? f->best_effort_timestamp : f->pts;
    if (ts == AV_NOPTS_VALUE) return -1.0;
    return static_cast<double>(ts) / static_cast<double>(PSMF_TIMESCALE);
}

size_t Decoder::detect_stride_from(const uint8_t* buf, size_t n, size_t fallback) {
    return umdplay::PsmfDemuxer::detect_stride(
        buf, n, static_cast<uint32_t>(fallback));
}

bool Decoder::pull(umdplay::PsmfPacket& out) {
    if (m_have_pushback) {
        out = std::move(m_pushback);
        m_have_pushback = false;
        return true;
    }
    return m_demux.read(out);
}

void Decoder::push_back(umdplay::PsmfPacket&& pkt) {
    m_pushback = std::move(pkt);
    m_have_pushback = true;
}

// --- open / close ---------------------------------------------------------

bool Decoder::open_stream(const std::string& iso_path, const MpsEntry& entry) {
    close();

    if (!m_reader.open(iso_path)) return false;

    const uint64_t base = static_cast<uint64_t>(entry.lba) * UMD_SECTOR_SIZE;
    const uint64_t size = entry.size_bytes > 0
                              ? entry.size_bytes
                              : m_reader.size() - base;
    if (!m_demux.open(&m_reader, base, size)) return false;

    m_demux.probe();
    const umdplay::PsmfStreamInfo& info = m_demux.info();

    m_pkt = av_packet_alloc();
    m_temp_audio_frame = av_frame_alloc();
    m_stashed_video = av_frame_alloc();

    // --- video (H.264) ---
    if (info.has_video) {
        const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (dec) {
            m_video.stream_index = 0;
            m_video.codec = dec;
            m_video.codec_ctx = avcodec_alloc_context3(dec);
            m_video.codec_ctx->pkt_timebase = AVRational{1, PSMF_TIMESCALE};
            if (!info.video_extradata.empty()) {
                m_video.codec_ctx->extradata = static_cast<uint8_t*>(
                    av_mallocz(info.video_extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
                if (m_video.codec_ctx->extradata) {
                    std::memcpy(m_video.codec_ctx->extradata,
                                info.video_extradata.data(),
                                info.video_extradata.size());
                    m_video.codec_ctx->extradata_size =
                        static_cast<int>(info.video_extradata.size());
                }
            }
            if (avcodec_open2(m_video.codec_ctx, dec, nullptr) < 0) {
                avcodec_free_context(&m_video.codec_ctx);
                m_video = {};
            }
        }
    }

    m_fps = info.fps > 0.0 ? info.fps : 29.97;
    if (m_fps <= 0.0 || m_fps > 240.0) m_fps = 29.97;
    m_duration = info.duration;

    // --- audio (Sony ATRAC3+) ---
    if (info.has_audio) {
        const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
        if (dec) {
            m_audio.stream_index = 1;
            m_audio.codec = dec;
            m_audio.codec_ctx = avcodec_alloc_context3(dec);
            m_audio.codec_ctx->sample_rate = info.audio_sample_rate;
            m_audio.codec_ctx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
            m_audio.block_align = static_cast<int>(info.audio_stride);
            m_audio.codec_ctx->block_align = static_cast<int>(info.audio_stride);
            m_audio.codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
            if (avcodec_open2(m_audio.codec_ctx, dec, nullptr) < 0) {
                avcodec_free_context(&m_audio.codec_ctx);
                m_audio = {};
            } else {
                swr_alloc_set_opts2(&m_swr_ctx,
                                    &m_audio.codec_ctx->ch_layout, AV_SAMPLE_FMT_S16,
                                    m_audio.codec_ctx->sample_rate,
                                    &m_audio.codec_ctx->ch_layout,
                                    m_audio.codec_ctx->sample_fmt,
                                    m_audio.codec_ctx->sample_rate, 0, nullptr);
                if (m_swr_ctx) swr_init(m_swr_ctx);
            }
        }
    }

    if (m_video.stream_index == -1 && m_audio.stream_index == -1) return false;

    // Warm-up: decode the first video frame so the coded dimensions (and any
    // other stream properties) are known at open time — the player and
    // clip_export read video_width()/video_height() immediately. The frame is
    // kept stashed so read_frame() still delivers it first.
    if (m_video.stream_index != -1) {
        if (decode_next_video(m_stashed_video)) {
            maybe_deinterlace(m_stashed_video);
            m_video_pending = true;
            const double vpts = frame_seconds(m_stashed_video);
            if (vpts >= 0.0) m_pts_anchor = vpts;
        }
    }

    return true;
}

void Decoder::close() {
    free_deinterlace_graph();
    m_pcm_ready.clear();
    m_pcm_pos = 0;
    m_pushback = umdplay::PsmfPacket{};
    m_have_pushback = false;
    m_psmf_next_pts = umdplay::PSMF_NOPTS;
    m_psmf_skip_samples = 0;
    m_align_done = false;
    m_audio_samples = 0;
    m_vcount = 0;
    m_video_eof = m_audio_eof = false;
    m_video_flushed = m_audio_flushed = false;
    m_video_pending = false;
    m_clock = 0.0;
    m_pts_anchor = -1.0;
    m_resync_video = false;
    m_duration = 0.0;
    m_fps = 29.97;
    m_demux.close();
    if (m_swr_ctx) { swr_free(&m_swr_ctx); }
    if (m_temp_audio_frame) { av_frame_free(&m_temp_audio_frame); }
    if (m_stashed_video) { av_frame_free(&m_stashed_video); }
    if (m_pkt) { av_packet_free(&m_pkt); }
    if (m_video.codec_ctx) { avcodec_free_context(&m_video.codec_ctx); m_video = {}; }
    if (m_audio.codec_ctx) { avcodec_free_context(&m_audio.codec_ctx); m_audio = {}; }
    m_reader.close();
}

// --- video decode ---------------------------------------------------------

bool Decoder::send_video(const umdplay::PsmfPacket& pkt, AVFrame* out) {
    if (!m_video.codec_ctx || pkt.data.empty()) return false;

    av_packet_unref(m_pkt);
    m_pkt->data = const_cast<uint8_t*>(pkt.data.data());
    m_pkt->size = static_cast<int>(pkt.data.size());
    m_pkt->pts = pkt.pts != umdplay::PSMF_NOPTS ? pkt.pts : AV_NOPTS_VALUE;
    m_pkt->dts = pkt.dts != umdplay::PSMF_NOPTS ? pkt.dts : m_pkt->pts;
    m_pkt->stream_index = 0;
    if (pkt.keyframe) m_pkt->flags |= AV_PKT_FLAG_KEY;

    if (avcodec_send_packet(m_video.codec_ctx, m_pkt) < 0) return false;

    bool got = false;
    while (true) {
        AVFrame* f = av_frame_alloc();
        if (!f) break;
        if (avcodec_receive_frame(m_video.codec_ctx, f) < 0) { av_frame_free(&f); break; }
        if (!got && out) {
            av_frame_move_ref(out, f);
            got = true;
        } else {
            // Extra reordered frames are not expected on UMD (one AU per PES);
            // drop rather than leak so a malformed stream cannot grow memory.
        }
        av_frame_free(&f);
    }
    return got;
}

void Decoder::flush_video() {
    if (m_video.codec_ctx) avcodec_send_packet(m_video.codec_ctx, nullptr);
}

void Decoder::flush_audio() {
    if (m_audio.codec_ctx) avcodec_send_packet(m_audio.codec_ctx, nullptr);
}

bool Decoder::decode_next_video(AVFrame* out) {
    if (!m_video.codec_ctx) return false;

    // A frame may already be buffered from the previous receive pass.
    if (avcodec_receive_frame(m_video.codec_ctx, out) >= 0) return true;

    while (!m_video_eof) {
        umdplay::PsmfPacket pkt;
        if (!pull(pkt)) { m_video_eof = true; break; }

        if (pkt.kind == umdplay::PsmfPacket::Kind::Video) {
            if (m_resync_video && !pkt.keyframe) continue;
            m_resync_video = false;
            if (send_video(pkt, out)) return true;
        } else if (pkt.kind == umdplay::PsmfPacket::Kind::Audio) {
            decode_audio_packet(pkt);
        }
    }

    // Drain the decoder so its reordered tail is not lost.
    if (!m_video_flushed) {
        m_video_flushed = true;
        flush_video();
        if (avcodec_receive_frame(m_video.codec_ctx, out) >= 0) return true;
    }
    return false;
}

void Decoder::pump_audio(int64_t target_samples) {
    while (m_audio.stream_index != -1 && !m_audio_eof && m_audio_samples < target_samples) {
        umdplay::PsmfPacket pkt;
        if (!pull(pkt)) { m_audio_eof = true; break; }
        if (pkt.kind == umdplay::PsmfPacket::Kind::Video) {
            push_back(std::move(pkt));  // leave video for decode_next_video
            break;
        }
        if (pkt.kind == umdplay::PsmfPacket::Kind::Audio) decode_audio_packet(pkt);
    }
}

// --- audio decode ---------------------------------------------------------

void Decoder::decode_audio_packet(const umdplay::PsmfPacket& pkt) {
    if (!m_audio.codec_ctx || pkt.data.empty()) return;

    if (pkt.pts != umdplay::PSMF_NOPTS && m_psmf_next_pts == umdplay::PSMF_NOPTS) {
        m_psmf_next_pts = pkt.pts;
    }

    av_packet_unref(m_pkt);
    m_pkt->data = const_cast<uint8_t*>(pkt.data.data());
    m_pkt->size = static_cast<int>(pkt.data.size());
    m_pkt->pts = pkt.pts != umdplay::PSMF_NOPTS ? pkt.pts : AV_NOPTS_VALUE;
    m_pkt->dts = m_pkt->pts;
    m_pkt->stream_index = 1;

    if (avcodec_send_packet(m_audio.codec_ctx, m_pkt) < 0) return;
    while (avcodec_receive_frame(m_audio.codec_ctx, m_temp_audio_frame) >= 0) {
        resample_to_s16(m_temp_audio_frame);
    }
}

void Decoder::resample_to_s16(AVFrame* audio_frame) {
    if (!m_swr_ctx || !audio_frame) return;
    const int out_samples = audio_frame->nb_samples;
    int dst_bufsize = av_samples_get_buffer_size(nullptr, 2, out_samples, AV_SAMPLE_FMT_S16, 0);
    if (dst_bufsize <= 0) return;

    const size_t orig = m_pcm_ready.size();
    m_pcm_ready.resize(orig + static_cast<size_t>(dst_bufsize));
    uint8_t* out_ptr = m_pcm_ready.data() + orig;

    const uint8_t* in_data[AV_NUM_DATA_POINTERS] = {nullptr};
    for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i) in_data[i] = audio_frame->data[i];

    const int converted = swr_convert(m_swr_ctx, &out_ptr, out_samples,
                                      in_data, audio_frame->nb_samples);
    if (converted < 0) {
        m_pcm_ready.resize(orig);
        return;
    }
    m_pcm_ready.resize(orig + static_cast<size_t>(converted) * 2 * sizeof(int16_t));
    m_audio_samples += converted;

    if (!m_align_done) apply_align();
}

// Post-seek alignment: `m_pts_anchor` is the video PTS (seconds) of the first
// decoded frame after the seek; `m_psmf_next_pts` is the 90 kHz PES PTS of the
// first audio frame. The player places both at t=0, so the audio must be
// shifted to start on the video frame's content time: skip leading audio when
// it starts early, or prepend silence when it starts late.
void Decoder::apply_align() {
    if (m_align_done) return;
    if (m_pts_anchor < 0.0 || m_psmf_next_pts == umdplay::PSMF_NOPTS) return;
    if (m_pcm_ready.empty()) return;

    constexpr int    kRate    = 44100;
    constexpr size_t kFrameSz = sizeof(int16_t) * 2;   // bytes per stereo frame

    const double audio_pts = static_cast<double>(m_psmf_next_pts) / 90000.0;
    const double lead      = m_pts_anchor - audio_pts;   // >0: audio is early
    const int64_t frames   = static_cast<int64_t>(std::llround(lead * kRate));
    m_align_done = true;

    // Ignore sub-frame noise; never shift more than a few seconds (bad PTS).
    if (std::llabs(frames) < 2 || std::llabs(frames) > kRate * 5) return;

    if (frames > 0) {
        const size_t drop = static_cast<size_t>(frames) * kFrameSz;
        if (drop < m_pcm_ready.size()) {
            m_pcm_ready.erase(m_pcm_ready.begin(),
                              m_pcm_ready.begin() + static_cast<std::ptrdiff_t>(drop));
        } else {
            m_psmf_skip_samples = frames - static_cast<int64_t>(m_pcm_ready.size() / kFrameSz);
            m_pcm_ready.clear();
        }
    } else {
        m_pcm_ready.insert(m_pcm_ready.begin(), static_cast<size_t>(-frames) * kFrameSz, 0);
    }
}

bool Decoder::drain_pcm(std::vector<uint8_t>& pcm, size_t max_bytes) {
    // Honour any residual skip left by a chunk that was entirely early.
    if (m_psmf_skip_samples > 0) {
        constexpr size_t kFrameSz = sizeof(int16_t) * 2;
        const size_t avail = (m_pcm_ready.size() - m_pcm_pos) / kFrameSz;
        const size_t drop = std::min<size_t>(avail, static_cast<size_t>(m_psmf_skip_samples));
        m_pcm_pos += drop * kFrameSz;
        m_psmf_skip_samples -= static_cast<int64_t>(drop);
    }

    const size_t avail = m_pcm_ready.size() - m_pcm_pos;
    if (avail == 0) {
        // Reclaim the whole buffer once drained so it does not grow forever.
        if (m_pcm_pos > 0) { m_pcm_ready.clear(); m_pcm_pos = 0; }
        return false;
    }

    const size_t take = std::min(avail, max_bytes);
    pcm.insert(pcm.end(), m_pcm_ready.begin() + static_cast<std::ptrdiff_t>(m_pcm_pos),
               m_pcm_ready.begin() + static_cast<std::ptrdiff_t>(m_pcm_pos + take));
    m_pcm_pos += take;
    if (m_pcm_pos >= m_pcm_ready.size()) { m_pcm_ready.clear(); m_pcm_pos = 0; }
    return !pcm.empty();
}

// --- public read ----------------------------------------------------------

FrameType Decoder::read_frame(AVFrame* video_frame, std::vector<uint8_t>& audio_pcm) {
    audio_pcm.clear();
    if (!m_demux.is_open()) return FrameType::Eof;
    if (m_video.stream_index == -1 && m_audio.stream_index == -1) return FrameType::Eof;

    // Audio-only stream: decode and hand back PCM directly.
    if (m_video.stream_index == -1) {
        if (drain_pcm(audio_pcm, PCM_CHUNK_CAP)) return FrameType::Audio;
        while (!m_audio_eof) {
            umdplay::PsmfPacket pkt;
            if (!pull(pkt)) { m_audio_eof = true; break; }
            if (pkt.kind == umdplay::PsmfPacket::Kind::Audio) decode_audio_packet(pkt);
            if (drain_pcm(audio_pcm, PCM_CHUNK_CAP)) return FrameType::Audio;
        }
        if (drain_pcm(audio_pcm, PCM_CHUNK_CAP)) return FrameType::Audio;
        return FrameType::Eof;
    }

    // Decode the next video frame when none is stashed, buffering the audio
    // interleaved ahead of it.
    if (!m_video_pending) {
        if (!decode_next_video(m_stashed_video)) {
            if (drain_pcm(audio_pcm, PCM_CHUNK_CAP)) return FrameType::Audio;
            return FrameType::Eof;
        }
        maybe_deinterlace(m_stashed_video);
        m_video_pending = true;
        if (m_pts_anchor < 0.0) {
            const double vpts = frame_seconds(m_stashed_video);
            if (vpts >= 0.0) m_pts_anchor = vpts;
        }
    }

    // Pace audio to the video clock: by the end of frame N roughly N/fps
    // seconds of PCM should have been decoded.
    if (m_audio.stream_index != -1 && !m_audio_eof) {
        const double fps = (m_fps > 0.0 && m_fps < 240.0) ? m_fps : 29.97;
        const int64_t target = static_cast<int64_t>((m_vcount + 1) * (44100.0 / fps));
        pump_audio(target);
    }

    if (drain_pcm(audio_pcm, PCM_CHUNK_CAP)) return FrameType::Audio;

    if (video_frame) {
        av_frame_unref(video_frame);
        av_frame_ref(video_frame, m_stashed_video);
    }
    m_video_pending = false;
    m_vcount++;
    const double pts = frame_seconds(m_stashed_video);
    if (pts >= 0.0) m_clock = pts;
    else m_clock += 1.0 / ((m_fps > 0.0 && m_fps < 240.0) ? m_fps : 29.97);
    return FrameType::Video;
}

// --- seek -----------------------------------------------------------------

bool Decoder::seek_relative(double seconds) {
    return seek_absolute(m_clock + seconds);
}

bool Decoder::seek_absolute(double seconds) {
    if (!m_demux.is_open()) return false;

    const double dur = get_duration();
    if (seconds < 0.0) seconds = 0.0;
    if (dur > 0.0 && seconds > dur) seconds = dur;

    if (m_video.codec_ctx) avcodec_flush_buffers(m_video.codec_ctx);
    if (m_audio.codec_ctx) avcodec_flush_buffers(m_audio.codec_ctx);

    m_pcm_ready.clear();
    m_pcm_pos = 0;
    m_pushback = umdplay::PsmfPacket{};
    m_have_pushback = false;
    m_video_eof = m_audio_eof = false;
    m_video_flushed = m_audio_flushed = false;
    m_video_pending = false;
    m_resync_video = true;
    m_pts_anchor = -1.0;
    m_psmf_next_pts = umdplay::PSMF_NOPTS;
    m_psmf_skip_samples = 0;
    m_align_done = false;
    m_audio_samples = 0;
    m_vcount = 0;

    // Flush the deinterlace graph so no pre-seek frames leak into the new GOP.
#if defined(UMDPLAY_HAVE_AVFILTER)
    if (m_filter_src) {
        const int flush_ret = av_buffersrc_add_frame_flags(m_filter_src, nullptr, 0);
        (void)flush_ret;
    }
    if (m_filter_sink) {
        av_frame_unref(m_filter_out);
        while (av_buffersink_get_frame(m_filter_sink, m_filter_out) >= 0) {
            av_frame_unref(m_filter_out);
        }
    }
#endif

    m_demux.seek(seconds);
    m_clock = seconds;
    return true;
}

// --- deinterlacing (yadif) ------------------------------------------------

void Decoder::set_deinterlace(bool on) {
    if (m_deinterlace == on) return;
    m_deinterlace = on;
    free_deinterlace_graph();
}

void Decoder::free_deinterlace_graph() {
#if defined(UMDPLAY_HAVE_AVFILTER)
    if (m_filter_graph) avfilter_graph_free(&m_filter_graph);
    m_filter_graph = nullptr;
    m_filter_src = nullptr;
    m_filter_sink = nullptr;
    if (m_filter_out) av_frame_free(&m_filter_out);
    m_filter_w = m_filter_h = 0;
    m_filter_fmt = -1;
#endif
}

#if defined(UMDPLAY_HAVE_AVFILTER)

bool Decoder::ensure_deinterlace_graph(const AVFrame* f) {
    if (m_filter_graph && m_filter_w == f->width && m_filter_h == f->height &&
        m_filter_fmt == f->format) {
        return true;
    }
    free_deinterlace_graph();

    const AVFilter* src = avfilter_get_by_name("buffer");
    const AVFilter* sink = avfilter_get_by_name("buffersink");
    const AVFilter* yadif = avfilter_get_by_name("yadif");
    if (!src || !sink || !yadif) return false;

    m_filter_graph = avfilter_graph_alloc();
    if (!m_filter_graph) return false;

    char args[256];
    std::snprintf(args, sizeof(args),
                  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:"
                  "range=%s:colorspace=%s",
                  f->width, f->height, f->format,
                  static_cast<int>(m_fps > 0.0 ? m_fps * 1000 : 30000), 1000,
                  f->sample_aspect_ratio.num ? f->sample_aspect_ratio.num : 1,
                  f->sample_aspect_ratio.den ? f->sample_aspect_ratio.den : 1,
                  f->color_range == AVCOL_RANGE_JPEG ? "pc" : "tv",
                  av_color_space_name(f->colorspace) ? av_color_space_name(f->colorspace)
                                                     : "bt470bg");

    if (avfilter_graph_create_filter(&m_filter_src, src, "in", args, nullptr,
                                     m_filter_graph) < 0) {
        free_deinterlace_graph();
        return false;
    }
    if (avfilter_graph_create_filter(&m_filter_sink, sink, "out", nullptr, nullptr,
                                     m_filter_graph) < 0) {
        free_deinterlace_graph();
        return false;
    }
    // yadif mode=0 emits one frame per input (1-in/1-out), keeping the fixed
    // frame clock the PSMF pacing relies on.
    AVFilterContext* yadif_ctx = nullptr;
    if (avfilter_graph_create_filter(&yadif_ctx, yadif, "yadif", "mode=0", nullptr,
                                     m_filter_graph) < 0) {
        free_deinterlace_graph();
        return false;
    }
    if (avfilter_link(m_filter_src, 0, yadif_ctx, 0) < 0 ||
        avfilter_link(yadif_ctx, 0, m_filter_sink, 0) < 0 ||
        avfilter_graph_config(m_filter_graph, nullptr) < 0) {
        free_deinterlace_graph();
        return false;
    }

    m_filter_out = av_frame_alloc();
    if (!m_filter_out) {
        free_deinterlace_graph();
        return false;
    }
    m_filter_w = f->width;
    m_filter_h = f->height;
    m_filter_fmt = f->format;
    return true;
}

bool Decoder::maybe_deinterlace(AVFrame* f) {
    if (!m_deinterlace || !f) return false;
    const bool interlaced = (f->flags & AV_FRAME_FLAG_INTERLACED) != 0;
    m_last_interlaced = interlaced;
    if (!interlaced) return false;

    if (!ensure_deinterlace_graph(f)) return false;

    if (av_buffersrc_add_frame_flags(m_filter_src, f, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
        return false;
    }
    av_frame_unref(m_filter_out);
    if (av_buffersink_get_frame(m_filter_sink, m_filter_out) < 0) return false;

    m_filter_out->pts = f->pts;
    m_filter_out->best_effort_timestamp = f->best_effort_timestamp;

    av_frame_unref(f);
    if (av_frame_ref(f, m_filter_out) < 0) return false;
    return true;
}

#else  // !UMDPLAY_HAVE_AVFILTER

bool Decoder::ensure_deinterlace_graph(const AVFrame*) { return false; }

bool Decoder::maybe_deinterlace(AVFrame* f) {
    if (!m_deinterlace || !f) return false;
    m_last_interlaced = (f->flags & AV_FRAME_FLAG_INTERLACED) != 0;
    return false;  // no filter available; pass the frame through
}

#endif

// --- test / dump helpers --------------------------------------------------

bool Decoder::test_decode(int max_frames, const std::string& dump_frame_path) {
    if (!has_video() && !has_audio()) return false;

    AVFrame* frame = av_frame_alloc();
    std::vector<uint8_t> pcm;
    int video_frames = 0, audio_chunks = 0;

    while (video_frames < max_frames) {
        pcm.clear();
        const FrameType ft = read_frame(frame, pcm);
        if (ft == FrameType::Eof) break;
        if (ft == FrameType::Video) {
            if (video_frames == 0 && m_video.codec_ctx) {
                save_ppm(frame, m_video.codec_ctx->width, m_video.codec_ctx->height,
                         dump_frame_path);
            }
            ++video_frames;
        } else if (ft == FrameType::Audio) {
            ++audio_chunks;
        }
    }
    if (audio_chunks > 0) {
        std::cout << "[+] Decoded " << audio_chunks << " audio chunks in memory.\n";
    }
    av_frame_free(&frame);
    return has_video() ? video_frames > 0 : audio_chunks > 0;
}

void Decoder::save_ppm(AVFrame* frame, int width, int height, const std::string& filename) {
    SwsContext* sws = sws_getContext(
        width, height, static_cast<AVPixelFormat>(frame->format),
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) return;

    std::vector<uint8_t> rgb_buffer(static_cast<size_t>(width) * height * 3);
    uint8_t* dst_data[1] = {rgb_buffer.data()};
    int dst_linesize[1] = {3 * width};

    sws_scale(sws, frame->data, frame->linesize, 0, height, dst_data, dst_linesize);
    sws_freeContext(sws);

    std::ofstream out(filename, std::ios::binary);
    if (!out) return;
    out << "P6\n" << width << " " << height << "\n255\n";
    out.write(reinterpret_cast<char*>(rgb_buffer.data()),
              static_cast<std::streamsize>(rgb_buffer.size()));
}
