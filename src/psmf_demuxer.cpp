#include "psmf_demuxer.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace umdplay {

namespace {

// MPEG-2 pack header is 14 bytes; the older MPEG-1 pack header is 12. The top
// two bits of the first byte after the start code are '01' for MPEG-2.
constexpr size_t PACK_HEADER_MPEG2 = 14;
constexpr size_t PACK_HEADER_MPEG1 = 12;

// Only accept a frame stride within this window when matching a sync word; the
// common ATRAC3+ strides are 696 and 1720 bytes.
constexpr size_t STRIDE_MIN = 64;
constexpr size_t STRIDE_MAX = 8192;

bool find_start_code(const uint8_t* p, size_t n, size_t from, size_t* at) {
    for (size_t i = from; i + 4 <= n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) { *at = i; return true; }
    }
    return false;
}

// Walk an Annex-B payload for the first SPS (NAL type 7) and PPS (type 8).
void extract_parameter_sets(const uint8_t* p, size_t n,
                            std::vector<uint8_t>& sps,
                            std::vector<uint8_t>& pps) {
    size_t i = 0;
    while (i + 4 <= n) {
        size_t sc = 0;
        if (!find_start_code(p, n, i, &sc)) break;
        const size_t nal = sc + 3;
        size_t end = nal;
        while (end + 3 <= n && !(p[end] == 0 && p[end + 1] == 0 && p[end + 2] == 1)) ++end;
        if (end + 3 > n) end = n;
        if (nal < end) {
            const uint8_t type = p[nal] & 0x1F;
            if (type == 7 && sps.empty()) sps.assign(p + nal, p + end);
            else if (type == 8 && pps.empty()) pps.assign(p + nal, p + end);
        }
        i = end > sc ? end : sc + 4;
    }
}

// Frame rate from consecutive video PES PTS deltas: the most common plausible
// delta wins (a PES packet normally carries exactly one access unit).
double estimate_fps(const std::vector<int64_t>& pts) {
    if (pts.size() < 2) return 0.0;
    std::map<int64_t, int> hist;
    for (size_t i = 1; i < pts.size(); ++i) {
        const int64_t d = pts[i] - pts[i - 1];
        if (d >= 700 && d <= 6000) ++hist[d];
    }
    int64_t best = 0;
    int best_count = 0;
    for (const auto& kv : hist) {
        if (kv.second > best_count) { best = kv.first; best_count = kv.second; }
    }
    if (best <= 0) return 0.0;
    return 90000.0 / static_cast<double>(best);
}

}  // namespace

// --- pure helpers ---------------------------------------------------------

bool PsmfDemuxer::is_sony_frame_sync(const uint8_t* p, size_t avail) {
    return avail >= 8 && p[0] == 0x0F && p[1] == 0xD0 && p[2] == 0x48 &&
           p[4] == 0x00 && p[5] == 0x00 && p[6] == 0x00 && p[7] == 0x00;
}

uint32_t PsmfDemuxer::detect_stride(const uint8_t* buf, size_t n, uint32_t fallback) {
    if (!buf || n < 8) return fallback;

    std::vector<size_t> syncs;
    for (size_t i = 0; i + 8 <= n; ++i) {
        if (is_sony_frame_sync(buf + i, n - i)) syncs.push_back(i);
    }
    if (syncs.size() < 2) return fallback;

    std::map<size_t, size_t> hist;
    for (size_t k = 1; k < syncs.size(); ++k) {
        const size_t gap = syncs[k] - syncs[k - 1];
        if (gap >= STRIDE_MIN && gap <= STRIDE_MAX) ++hist[gap];
    }
    if (hist.empty()) return fallback;

    size_t best = fallback;
    size_t best_count = 0;
    for (const auto& kv : hist) {
        if (kv.second > best_count) { best = kv.first; best_count = kv.second; }
    }
    return static_cast<uint32_t>(best);
}

int64_t PsmfDemuxer::decode_timestamp(const uint8_t* p, size_t avail) {
    if (!p || avail < 5) return PSMF_NOPTS;
    return (static_cast<int64_t>((p[0] >> 1) & 0x07) << 30) |
           (static_cast<int64_t>(p[1]) << 22) |
           (static_cast<int64_t>((p[2] >> 1) & 0x7F) << 15) |
           (static_cast<int64_t>(p[3]) << 7) |
           (static_cast<int64_t>((p[4] >> 1) & 0x7F));
}

bool PsmfDemuxer::payload_is_keyframe(const uint8_t* p, size_t n) {
    if (!p) return false;
    for (size_t i = 0; i + 4 <= n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            const uint8_t type = p[i + 3] & 0x1F;
            if (type == 5 || type == 7) return true;  // IDR or SPS
        }
    }
    return false;
}

std::vector<uint8_t> PsmfDemuxer::build_extradata(const std::vector<uint8_t>& sps,
                                                 const std::vector<uint8_t>& pps) {
    std::vector<uint8_t> out;
    if (sps.empty() || pps.empty()) return out;
    out.reserve(8 + sps.size() + pps.size());
    const uint8_t sc[4] = {0x00, 0x00, 0x00, 0x01};
    out.insert(out.end(), sc, sc + 4);
    out.insert(out.end(), sps.begin(), sps.end());
    out.insert(out.end(), sc, sc + 4);
    out.insert(out.end(), pps.begin(), pps.end());
    return out;
}

// --- lifecycle ------------------------------------------------------------

bool PsmfDemuxer::open(const DiscReader* reader, uint64_t base_offset, uint64_t size) {
    close();
    if (!reader || !reader->is_open() || size == 0) return false;
    m_reader = reader;
    m_base = base_offset;
    m_size = size;
    m_read_end = size;
    rewind_to(0);
    return true;
}

void PsmfDemuxer::close() {
    m_reader = nullptr;
    m_base = m_size = m_read_end = 0;
    m_info = PsmfStreamInfo{};
    m_buf.clear();
    m_buf_off = 0;
    m_scan = 0;
    m_eof = false;
    m_audio_buf.clear();
    m_audio_pos = 0;
    m_audio_pts = PSMF_NOPTS;
    m_seek_pts = PSMF_NOPTS;
    m_seeking = false;
    m_position = -1.0;
}

void PsmfDemuxer::rewind_to(uint64_t off) {
    m_buf.clear();
    m_buf_off = off;
    m_scan = 0;
    m_eof = (off >= m_read_end);
    m_audio_buf.clear();
    m_audio_pos = 0;
    m_audio_pts = PSMF_NOPTS;
}

size_t PsmfDemuxer::read_at(uint64_t off, uint8_t* dst, size_t len) const {
    if (!m_reader || off >= m_size) return 0;
    const uint64_t clamped = std::min<uint64_t>(len, m_size - off);
    return m_reader->read_at(m_base + off, dst, static_cast<size_t>(clamped));
}

size_t PsmfDemuxer::fill(size_t need) {
    if (m_buf.size() - m_scan >= need) return m_buf.size() - m_scan;
    if (m_eof) return m_buf.size() - m_scan;

    // Compact consumed bytes so the buffer never grows without bound.
    if (m_scan > 0) {
        m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(m_scan));
        m_buf_off += m_scan;
        m_scan = 0;
    }

    while (m_buf.size() < need && !m_eof) {
        uint64_t off = m_buf_off + m_buf.size();
        if (off >= m_read_end) { m_eof = true; break; }
        size_t want = std::max<size_t>(m_chunk, need - m_buf.size());
        want = static_cast<size_t>(std::min<uint64_t>(want, m_read_end - off));
        const size_t old = m_buf.size();
        m_buf.resize(old + want);
        const size_t got = m_reader->read_at(m_base + off, m_buf.data() + old, want);
        m_buf.resize(old + got);
        if (got == 0) { m_eof = true; break; }
    }
    return m_buf.size() - m_scan;
}

// --- PES walk -------------------------------------------------------------

bool PsmfDemuxer::next_pes(PesView& v) {
    while (true) {
        size_t avail = fill(6);
        if (avail < 4) return false;

        size_t sc = 0;
        if (!find_start_code(m_buf.data(), m_buf.size(), m_scan, &sc)) {
            // No start code in the buffer: keep the last 3 bytes (a start code
            // may straddle the boundary) and read on.
            const size_t remaining = m_buf.size() - m_scan;
            const size_t keep = remaining >= 3 ? 3 : remaining;
            m_scan = m_buf.size() - keep;
            if (m_eof) return false;
            fill(m_buf.size() - m_scan + 1);
            continue;
        }
        m_scan = sc;

        // fill() may compact (moving the start code to index 0), so always
        // re-index from m_scan after every fill call.
        avail = fill(6);
        if (avail < 6) return false;
        const uint8_t sid = m_buf[m_scan + 3];

        if (sid == 0xB9) return false;  // program end

        if (sid == 0xBA) {  // pack header
            const uint8_t b4 = m_buf[m_scan + 4];
            m_scan += (b4 & 0xC0) == 0x40 ? PACK_HEADER_MPEG2 : PACK_HEADER_MPEG1;
            continue;
        }
        if (sid == 0xBB) {  // system header
            const uint16_t plen = static_cast<uint16_t>((m_buf[m_scan + 4] << 8) |
                                                        m_buf[m_scan + 5]);
            m_scan += 6 + plen;
            continue;
        }

        const uint16_t plen = static_cast<uint16_t>((m_buf[m_scan + 4] << 8) |
                                                    m_buf[m_scan + 5]);
        if (plen == 0) return false;  // unbounded PES: not produced by UMD
        const size_t total = 6 + plen;
        if (fill(total) < total) return false;  // truncated at EOF

        if (sid != 0xBD && !(sid >= 0xE0 && sid <= 0xEF)) {
            m_scan += total;  // unknown stream: skip wholesale
            continue;
        }

        const uint8_t* d = m_buf.data() + m_scan;
        const uint8_t flags = d[7];
        const size_t hdr_len = d[8];

        v = PesView{};
        v.sid = sid;
        if (flags & 0x80) {  // PTS present (5 bytes at offset 9)
            v.pts = decode_timestamp(d + 9, total > 9 ? total - 9 : 0);
        }
        if (flags & 0x40) {  // DTS present (5 bytes at offset 14)
            v.dts = decode_timestamp(d + 14, total > 14 ? total - 14 : 0);
        }

        size_t payload = 9 + hdr_len;  // hdr_len covers PTS/DTS + extensions
        if (payload > total) payload = total;
        v.payload_off = m_scan + payload;
        v.payload_len = total - payload;

        // Private Stream 1 carries a 4-byte Sony substream header (00 00 ..).
        if (sid == 0xBD) {
            if (v.payload_len >= 4 && m_buf[v.payload_off] == 0x00) {
                v.payload_off += 4;
                v.payload_len -= 4;
            } else {
                v.payload_len = 0;
            }
        }

        m_scan += total;
        return true;
    }
}

// --- ATRAC3+ framing ------------------------------------------------------

bool PsmfDemuxer::emit_audio_frame(PsmfPacket& out) {
    const size_t stride = m_info.audio_stride;
    while (true) {
        const size_t avail = m_audio_buf.size() - m_audio_pos;
        if (avail < 8) break;

        const uint8_t* base = m_audio_buf.data() + m_audio_pos;
        if (!is_sony_frame_sync(base, avail)) { ++m_audio_pos; continue; }

        // Prefer the distance to the next sync word (exact even when the frame
        // size differs from the measured stride); else use the detected stride.
        size_t frame_len = stride;
        if (!(avail >= stride + 8 && is_sony_frame_sync(base + stride, avail - stride))) {
            for (size_t cand = STRIDE_MIN; cand <= STRIDE_MAX && cand + 8 <= avail; ++cand) {
                if (is_sony_frame_sync(base + cand, avail - cand)) { frame_len = cand; break; }
            }
        }
        if (avail < frame_len) break;  // incomplete frame: need more data

        out.kind = PsmfPacket::Kind::Audio;
        out.keyframe = false;
        out.pts = m_audio_pts;
        out.dts = PSMF_NOPTS;
        out.data.assign(base + 8, base + frame_len);
        m_audio_pos += frame_len;
        m_info.has_audio = true;
        return true;
    }
    return false;
}

PsmfDemuxer::Step PsmfDemuxer::parse_next(PsmfPacket& out) {
    if (emit_audio_frame(out)) return Step::Audio;

    while (true) {
        PesView v;
        if (!next_pes(v)) return Step::Eof;

        // PTS filter armed by a seek: drop packets that precede the target.
        if (m_seeking) {
            const int64_t ts = v.pts != PSMF_NOPTS ? v.pts : v.dts;
            if (ts != PSMF_NOPTS && ts < m_seek_pts) continue;
            m_seeking = false;
        }

        if (v.sid >= 0xE0 && v.sid <= 0xEF) {
            out.kind = PsmfPacket::Kind::Video;
            out.pts = v.pts;
            out.dts = v.dts;
            out.data.assign(m_buf.begin() + static_cast<std::ptrdiff_t>(v.payload_off),
                            m_buf.begin() + static_cast<std::ptrdiff_t>(v.payload_off + v.payload_len));
            out.keyframe = payload_is_keyframe(out.data.data(), out.data.size());
            m_info.has_video = true;
            if (v.pts != PSMF_NOPTS) m_position = static_cast<double>(v.pts) / 90000.0;
            return Step::Video;
        }

        // 0xBD: append the payload to the audio buffer (frames span packets).
        if (v.pts != PSMF_NOPTS) m_audio_pts = v.pts;
        m_audio_buf.insert(m_audio_buf.end(),
                           m_buf.begin() + static_cast<std::ptrdiff_t>(v.payload_off),
                           m_buf.begin() + static_cast<std::ptrdiff_t>(v.payload_off + v.payload_len));
        if (emit_audio_frame(out)) return Step::Audio;

        // Bound the audio buffer: drop consumed frames once enough pile up.
        if (m_audio_pos >= (1u << 20)) {
            m_audio_buf.erase(m_audio_buf.begin(),
                              m_audio_buf.begin() + static_cast<std::ptrdiff_t>(m_audio_pos));
            m_audio_pos = 0;
        }
    }
}

bool PsmfDemuxer::read(PsmfPacket& out) {
    out = PsmfPacket{};
    if (!is_open()) return false;
    return parse_next(out) != Step::Eof;
}

// --- probe / seek ---------------------------------------------------------

void PsmfDemuxer::probe(uint32_t window) {
    m_info = PsmfStreamInfo{};
    m_info.size_bytes = m_size;
    if (!is_open()) return;

    const uint64_t win = window ? window : (512u * 1024u);

    // Leading walk: stream presence, fps, first SPS+PPS, and a window of
    // concatenated 0xBD payloads for stride measurement. Concatenating the
    // payloads makes ATRAC3+ frames contiguous even when each PES carries only
    // one frame, so the histogram sees the real stride rather than PES gaps.
    std::vector<uint8_t> audio_probe;
    {
        const uint64_t saved_end = m_read_end;
        rewind_to(0);
        m_read_end = std::min<uint64_t>(m_size, win);

        std::vector<int64_t> video_pts;
        std::vector<uint8_t> sps, pps;
        PesView v;
        while (next_pes(v)) {
            if (v.sid >= 0xE0 && v.sid <= 0xEF) {
                m_info.has_video = true;
                if (v.pts != PSMF_NOPTS) video_pts.push_back(v.pts);
                if (sps.empty() || pps.empty()) {
                    extract_parameter_sets(m_buf.data() + v.payload_off, v.payload_len,
                                           sps, pps);
                }
            } else if (v.sid == 0xBD) {
                m_info.has_audio = true;
                if (audio_probe.size() < win) {
                    const size_t take = std::min<size_t>(v.payload_len, win - audio_probe.size());
                    audio_probe.insert(audio_probe.end(),
                                       m_buf.begin() + static_cast<std::ptrdiff_t>(v.payload_off),
                                       m_buf.begin() + static_cast<std::ptrdiff_t>(v.payload_off + take));
                }
            }
        }
        m_info.fps = estimate_fps(video_pts);
        if (!sps.empty() && !pps.empty()) m_info.video_extradata = build_extradata(sps, pps);

        m_read_end = saved_end;
        rewind_to(0);
    }

    // Stride: prefer the concatenated 0xBD payloads; fall back to a raw window.
    if (audio_probe.size() >= 16) {
        m_info.audio_stride = detect_stride(audio_probe.data(), audio_probe.size(),
                                            PSMF_STRIDE_DEFAULT);
    } else {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(win, m_size));
        std::vector<uint8_t> raw(want);
        const size_t got = read_at(0, raw.data(), want);
        m_info.audio_stride = detect_stride(raw.data(), got, PSMF_STRIDE_DEFAULT);
    }

    // Duration from the last PES PTS in a tail window.
    {
        const uint64_t tail = std::min<uint64_t>(m_size, 4u << 20);
        const uint64_t off = m_size - tail;
        const uint64_t saved_end = m_read_end;
        rewind_to(off);
        m_read_end = m_size;

        int64_t last = PSMF_NOPTS;
        PesView v;
        while (next_pes(v)) {
            const int64_t ts = v.pts != PSMF_NOPTS ? v.pts : v.dts;
            if (ts != PSMF_NOPTS && ts > last) last = ts;
        }
        m_read_end = saved_end;
        rewind_to(0);
        if (last != PSMF_NOPTS) m_info.duration = static_cast<double>(last) / 90000.0;
    }
}

uint64_t PsmfDemuxer::snap_to_pack(uint64_t target) const {
    if (!m_reader || target == 0) return 0;
    if (target >= m_size) return m_size;

    const uint64_t window = 64 * 1024;
    const uint64_t begin = target > window ? target - window : 0;
    const size_t want = static_cast<size_t>(target - begin);
    if (want == 0) return target;

    std::vector<uint8_t> buf(want);
    const size_t got = read_at(begin, buf.data(), want);

    for (size_t i = got; i >= 4; --i) {
        const uint8_t* p = buf.data() + (i - 4);
        if (p[0] == 0 && p[1] == 0 && p[2] == 1 && (p[3] == 0xBA || p[3] == 0xBB)) {
            return begin + (i - 4);
        }
    }
    return target;
}

bool PsmfDemuxer::seek(double seconds) {
    if (!is_open()) return false;
    if (seconds < 0.0) seconds = 0.0;

    const double dur = m_info.duration;
    if (dur > 0.0 && seconds > dur) seconds = dur;

    uint64_t target = 0;
    if (dur > 0.0) {
        target = static_cast<uint64_t>((seconds / dur) * static_cast<double>(m_size));
    }
    if (target >= m_size) target = m_size > 1 ? m_size - 1 : 0;
    target = snap_to_pack(target);

    rewind_to(target);
    m_seeking = (seconds > 0.0);
    m_seek_pts = m_seeking ? static_cast<int64_t>(seconds * 90000.0) : PSMF_NOPTS;
    m_position = seconds;
    return true;
}

}  // namespace umdplay
