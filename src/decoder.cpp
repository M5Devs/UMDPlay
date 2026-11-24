#include "decoder.h"

#include "deinterlace.h"
#include "yuv_convert.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

// The video elementary stream is timestamped on the PES 90 kHz clock.
static constexpr double PSMF_TIMESCALE = 90000.0;

// Largest PCM chunk handed back per read_frame (a couple of ATRAC3+ frames).
static constexpr size_t PCM_CHUNK_CAP = 16384;

Decoder::Decoder() { m_psmf_next_pts = umdplay::PSMF_NOPTS; }
Decoder::~Decoder() { close(); }

// --- helpers --------------------------------------------------------------

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

    // --- video (H.264) ---
    if (info.has_video) {
#if defined(UMDPLAY_NATIVE_VIDEO)
        m_video_backend = umdplay::make_native_h264_backend();
#elif defined(UMDPLAY_FFMPEG_BACKENDS)
        m_video_backend = umdplay::make_ffmpeg_h264_backend();
#endif
        if (m_video_backend && !info.video_extradata.empty() &&
            !m_video_backend->init(info.video_extradata.data(),
                                   info.video_extradata.size())) {
            m_video_backend.reset();
        }
    }

    m_fps = info.fps > 0.0 ? info.fps : 29.97;
    if (m_fps <= 0.0 || m_fps > 240.0) m_fps = 29.97;
    m_duration = info.duration;

    // --- audio (Sony ATRAC3+) ---
    if (info.has_audio) {
#if defined(UMDPLAY_NATIVE_ATRAC3P)
        m_audio_backend = umdplay::make_native_atrac3p_backend();
#elif defined(UMDPLAY_FFMPEG_BACKENDS)
        m_audio_backend = umdplay::make_ffmpeg_atrac3p_backend();
#endif
        if (m_audio_backend && !m_audio_backend->init(2)) m_audio_backend.reset();
    }

    if (!m_video_backend && !m_audio_backend) return false;
    m_psmf_next_pts = umdplay::PSMF_NOPTS;
    return true;
}

void Decoder::close() {
    m_pcm_ready.clear();
    m_pcm_pos = 0;
    m_have_pushback = false;
    m_video_backend.reset();
    m_audio_backend.reset();
    m_stashed_video.clear();
    m_video_pending = false;
    m_video_eof = m_audio_eof = false;
    m_video_flushed = false;
    m_align_done = false;
    m_psmf_next_pts = umdplay::PSMF_NOPTS;
    m_psmf_skip_samples = 0;
    m_audio_samples = 0;
    m_vcount = 0;
    m_clock = 0.0;
    m_duration = 0.0;
    m_fps = 29.97;
    m_resync_video = false;
    m_pts_anchor = -1.0;
    m_last_interlaced = false;
    m_demux.close();
    m_reader.close();
}

// --- video decode ---------------------------------------------------------

bool Decoder::send_video(const umdplay::PsmfPacket& pkt) {
    if (!m_video_backend || pkt.data.empty()) return false;
    const int64_t pts = pkt.pts == umdplay::PSMF_NOPTS ? umdplay::kPtsUnknown : pkt.pts;
    return m_video_backend->decode(pkt.data.data(), pkt.data.size(), pts);
}

bool Decoder::receive_video(umdplay::VideoFrame& out) {
    if (!m_video_backend) return false;
    if (!m_video_backend->receive(out)) return false;
    m_last_interlaced = out.interlaced;
    if (m_deinterlace && out.interlaced) umdplay::deinterlace_yuv420p(out);
    return true;
}

void Decoder::flush_video() {
    if (m_video_backend) m_video_backend->flush();
}

bool Decoder::decode_next_video(umdplay::VideoFrame& out) {
    if (!m_video_backend) return false;

    // A frame may already be buffered from the previous receive pass.
    if (receive_video(out)) return true;

    while (!m_video_eof) {
        umdplay::PsmfPacket pkt;
        if (!pull(pkt)) { m_video_eof = true; break; }

        if (pkt.kind == umdplay::PsmfPacket::Kind::Video) {
            if (m_resync_video && !pkt.keyframe) continue;
            m_resync_video = false;
            send_video(pkt);
            if (receive_video(out)) return true;
        } else if (pkt.kind == umdplay::PsmfPacket::Kind::Audio) {
            decode_audio_packet(pkt);
        }
    }

    // Drain the decoder so its reordered tail is not lost.
    if (!m_video_flushed) {
        m_video_flushed = true;
        flush_video();
        if (receive_video(out)) return true;
    }
    return false;
}

void Decoder::pump_audio(int64_t target_samples) {
    while (m_audio_backend && !m_audio_eof && m_audio_samples < target_samples) {
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
    if (!m_audio_backend || pkt.data.empty()) return;

    if (pkt.pts != umdplay::PSMF_NOPTS && m_psmf_next_pts == umdplay::PSMF_NOPTS) {
        m_psmf_next_pts = pkt.pts;
    }

    const size_t before = m_pcm_ready.size();
    m_audio_backend->decode(pkt.data.data(), pkt.data.size(), m_pcm_ready);
    const size_t added = m_pcm_ready.size() - before;
    m_audio_samples += static_cast<int64_t>(added / (2 * sizeof(int16_t)));
    if (added && !m_align_done) apply_align();
}

// Post-seek alignment: m_pts_anchor is the video PTS (seconds) of the first
// decoded frame after the seek; m_psmf_next_pts is the 90 kHz PES PTS of the
// first audio frame. The player places both at t=0, so the audio must be
// shifted to start on the video frame's content time: skip leading audio when
// it starts early, or prepend silence when it starts late.
void Decoder::apply_align() {
    if (m_align_done) return;
    if (m_pts_anchor < 0.0 || m_psmf_next_pts == umdplay::PSMF_NOPTS) return;
    if (m_pcm_ready.empty()) return;

    constexpr int    kRate    = 44100;
    constexpr size_t kFrameSz = sizeof(int16_t) * 2;   // bytes per stereo frame

    const double audio_pts = static_cast<double>(m_psmf_next_pts) / PSMF_TIMESCALE;
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

FrameType Decoder::read_frame(umdplay::VideoFrame& video_frame,
                              std::vector<uint8_t>& audio_pcm) {
    audio_pcm.clear();
    if (!m_demux.is_open()) return FrameType::Eof;
    if (!m_video_backend && !m_audio_backend) return FrameType::Eof;

    // Audio-only stream: decode and hand back PCM directly.
    if (!m_video_backend) {
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
        m_video_pending = true;
        if (m_pts_anchor < 0.0 && m_stashed_video.pts >= 0.0) {
            m_pts_anchor = m_stashed_video.pts;
        }
    }

    // Pace audio to the video clock: by the end of frame N roughly N/fps
    // seconds of PCM should have been decoded.
    if (m_audio_backend && !m_audio_eof) {
        const double fps = (m_fps > 0.0 && m_fps < 240.0) ? m_fps : 29.97;
        const int64_t target = static_cast<int64_t>((m_vcount + 1) * (44100.0 / fps));
        pump_audio(target);
    }

    if (drain_pcm(audio_pcm, PCM_CHUNK_CAP)) return FrameType::Audio;

    video_frame = std::move(m_stashed_video);
    m_stashed_video.clear();
    m_video_pending = false;
    m_vcount++;
    const double pts = video_frame.pts;
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

    if (m_video_backend) m_video_backend->reset();
    if (m_audio_backend) m_audio_backend->reset();

    m_pcm_ready.clear();
    m_pcm_pos = 0;
    m_pushback = umdplay::PsmfPacket{};
    m_have_pushback = false;
    m_video_eof = m_audio_eof = false;
    m_video_flushed = false;
    m_video_pending = false;
    m_stashed_video.clear();
    m_resync_video = true;
    m_pts_anchor = -1.0;
    m_psmf_next_pts = umdplay::PSMF_NOPTS;
    m_psmf_skip_samples = 0;
    m_align_done = false;
    m_audio_samples = 0;
    m_vcount = 0;

    m_demux.seek(seconds);
    m_clock = seconds;
    return true;
}

// --- options / introspection ----------------------------------------------

void Decoder::set_deinterlace(bool on) {
    m_deinterlace = on;
}

int Decoder::video_width() const {
    return m_video_backend ? m_video_backend->width() : 0;
}

int Decoder::video_height() const {
    return m_video_backend ? m_video_backend->height() : 0;
}

float Decoder::video_par() const {
    return m_video_backend ? m_video_backend->par() : 1.0f;
}

const char* Decoder::video_codec_name() const {
    return m_video_backend ? m_video_backend->name() : "";
}

const char* Decoder::video_backend_name() const {
    return m_video_backend ? m_video_backend->backend_name() : "";
}

int Decoder::audio_sample_rate() const {
    return m_audio_backend ? m_audio_backend->sample_rate() : 0;
}

bool Decoder::using_native_audio() const {
    return m_audio_backend && m_audio_backend->native();
}

bool Decoder::using_native_video() const {
    return m_video_backend && m_video_backend->native();
}

// --- test / dump helpers --------------------------------------------------

bool Decoder::test_decode(int max_frames, const std::string& dump_frame_path) {
    if (!has_video() && !has_audio()) return false;

    umdplay::VideoFrame frame;
    std::vector<uint8_t> pcm;
    int video_frames = 0, audio_chunks = 0;

    while (video_frames < max_frames) {
        pcm.clear();
        const FrameType ft = read_frame(frame, pcm);
        if (ft == FrameType::Eof) break;
        if (ft == FrameType::Video) {
            if (video_frames == 0) save_ppm(frame, dump_frame_path);
            ++video_frames;
        } else if (ft == FrameType::Audio) {
            ++audio_chunks;
        }
    }
    if (audio_chunks > 0) {
        std::cout << "[+] Decoded " << audio_chunks << " audio chunks in memory.\n";
    }
    return has_video() ? video_frames > 0 : audio_chunks > 0;
}

void Decoder::save_ppm(const umdplay::VideoFrame& frame, const std::string& filename) {
    if (!frame.valid()) return;
    const int width = frame.width;
    const int height = frame.height;

    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    umdplay::yuv420p_to_rgb24(frame, rgb.data(), width * 3);

    std::ofstream out(filename, std::ios::binary);
    if (!out) return;
    out << "P6\n" << width << " " << height << "\n255\n";
    out.write(reinterpret_cast<char*>(rgb.data()),
              static_cast<std::streamsize>(rgb.size()));
}
