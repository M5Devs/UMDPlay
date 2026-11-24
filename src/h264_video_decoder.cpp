// SPDX-License-Identifier: GPL-2.0-only
//
// VideoDecoderBackend over third_party/h264dec (MIT, xesf/umd-player).

#include "video_decoder_backend.h"

#include <cstring>

extern "C" {
#include "h264.h"
#include "h264dec.h"
}

namespace umdplay {
namespace {

// h264dec reports the PTS it was given for the frame's first picture. Values
// far above any real 90 kHz timestamp mean "not a picture" (the parameter-set
// priming call), so they map to "unknown".
constexpr uint64_t kPtsSentinelFloor = uint64_t(1) << 62;

class H264Backend final : public VideoDecoderBackend {
public:
    H264Backend() : m_dec(h264dec_open()) { h264_parser_init(&m_parser); }
    ~H264Backend() override { h264dec_close(m_dec); }

    H264Backend(const H264Backend&)            = delete;
    H264Backend& operator=(const H264Backend&) = delete;

    bool init(const uint8_t* extradata, size_t size) override {
        if (!m_dec) return false;
        if (!extradata || size == 0) return true;
        if (size > UINT32_MAX) return false;
        // Seeds the SPS (size, aspect) so the stream's geometry is known before
        // the first picture, then primes the decoder with the same bytes.
        parse_headers(extradata, size);
        h264dec_decode(m_dec, extradata, static_cast<uint32_t>(size), kPtsSentinelFloor);
        return true;
    }

    bool decode(const uint8_t* au, size_t size, int64_t pts90k) override {
        if (!m_dec || !au || size == 0 || size > UINT32_MAX) return false;
        parse_headers(au, size);
        const uint64_t pts = (pts90k == kPtsUnknown || pts90k < 0)
                                 ? kPtsSentinelFloor
                                 : static_cast<uint64_t>(pts90k);
        if (h264dec_decode(m_dec, au, static_cast<uint32_t>(size), pts) < 0) {
            if (m_errors < 8) {
                ++m_errors;
                m_last_error = h264dec_error(m_dec);
            }
            return false;
        }
        return true;
    }

    bool receive(VideoFrame& out) override {
        if (!m_dec) return false;
        h264dec_frame f;
        if (!h264dec_output(m_dec, &f)) return false;
        if (f.width <= 0 || f.height <= 0) return false;

        out.allocate(f.width, f.height);
        for (int p = 0; p < 3; ++p) {
            const int pw = p ? out.chroma_width() : out.width;
            const int ph = p ? out.chroma_height() : out.height;
            const int src_stride = f.stride[p];
            if (!f.plane[p] || src_stride < pw) {
                out.clear();
                return false;
            }
            for (int y = 0; y < ph; ++y) {
                std::memcpy(out.plane(p) + static_cast<size_t>(y) * out.stride[p],
                            f.plane[p] + static_cast<size_t>(y) * src_stride,
                            static_cast<size_t>(pw));
            }
        }
        out.interlaced      = f.interlaced != 0;
        out.top_field_first = f.top_field_first != 0;
        out.pts             = f.pts >= kPtsSentinelFloor
                                  ? -1.0
                                  : static_cast<double>(f.pts) / 90000.0;

        m_width  = out.width;
        m_height = out.height;
        return true;
    }

    void flush() override {
        if (m_dec) h264dec_flush(m_dec);
    }

    void reset() override {
        if (m_dec) h264dec_reset(m_dec);
    }

    int width() const override {
        return m_width > 0 ? m_width : m_parser.sps.width;
    }
    int height() const override {
        return m_height > 0 ? m_height : m_parser.sps.height;
    }
    float par() const override {
        const h264_sps& sps = m_parser.sps;
        if (sps.sar_w <= 0 || sps.sar_h <= 0) return 1.0f;
        return static_cast<float>(sps.sar_w) / static_cast<float>(sps.sar_h);
    }
    const char* name() const override { return "h264"; }
    const char* backend_name() const override { return "h264dec"; }
    bool native() const override { return true; }

    const char* last_error() const { return m_last_error; }

private:
    void parse_headers(const uint8_t* au, size_t size) {
        if (size > UINT32_MAX) return;
        h264_au_info info;
        h264_parse_au(&m_parser, au, static_cast<uint32_t>(size), &info);
    }

    h264dec*    m_dec = nullptr;
    h264_parser m_parser{};
    int         m_width = 0;
    int         m_height = 0;
    int         m_errors = 0;
    const char* m_last_error = "";
};

}  // namespace

std::unique_ptr<VideoDecoderBackend> make_native_h264_backend() {
    auto backend = std::make_unique<H264Backend>();
    if (!backend->init(nullptr, 0)) return nullptr;
    return backend;
}

}  // namespace umdplay
