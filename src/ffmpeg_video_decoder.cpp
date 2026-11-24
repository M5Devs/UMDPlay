// SPDX-License-Identifier: GPL-2.0-only
//
// Optional libavcodec H.264 backend. Only compiled for A/B builds
// (-DUMDPLAY_NATIVE_VIDEO=OFF); the default player uses third_party/h264dec.

#include "video_decoder_backend.h"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

namespace umdplay {
namespace {

class FfmpegH264Backend final : public VideoDecoderBackend {
public:
    ~FfmpegH264Backend() override {
        if (m_frame) av_frame_free(&m_frame);
        if (m_pkt) av_packet_free(&m_pkt);
        if (m_ctx) avcodec_free_context(&m_ctx);
    }

    bool init(const uint8_t* extradata, size_t size) override {
        const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!dec) return false;
        m_ctx = avcodec_alloc_context3(dec);
        if (!m_ctx) return false;
        m_ctx->pkt_timebase = AVRational{1, 90000};
        if (extradata && size > 0) {
            m_ctx->extradata = static_cast<uint8_t*>(
                av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!m_ctx->extradata) return false;
            std::memcpy(m_ctx->extradata, extradata, size);
            m_ctx->extradata_size = static_cast<int>(size);
        }
        if (avcodec_open2(m_ctx, dec, nullptr) < 0) return false;
        m_pkt = av_packet_alloc();
        m_frame = av_frame_alloc();
        return m_pkt && m_frame;
    }

    bool decode(const uint8_t* au, size_t size, int64_t pts90k) override {
        if (!m_ctx || !au || size == 0) return false;
        av_packet_unref(m_pkt);
        m_pkt->data = const_cast<uint8_t*>(au);
        m_pkt->size = static_cast<int>(size);
        m_pkt->pts = pts90k == kPtsUnknown ? AV_NOPTS_VALUE : pts90k;
        m_pkt->dts = m_pkt->pts;
        return avcodec_send_packet(m_ctx, m_pkt) >= 0;
    }

    bool receive(VideoFrame& out) override {
        if (!m_ctx) return false;
        av_frame_unref(m_frame);
        if (avcodec_receive_frame(m_ctx, m_frame) < 0) return false;
        if (m_frame->format != AV_PIX_FMT_YUV420P || m_frame->width <= 0 ||
            m_frame->height <= 0) {
            return false;
        }
        out.allocate(m_frame->width, m_frame->height);
        for (int p = 0; p < 3; ++p) {
            const int pw = p ? out.chroma_width() : out.width;
            const int ph = p ? out.chroma_height() : out.height;
            for (int y = 0; y < ph; ++y) {
                std::memcpy(out.plane(p) + static_cast<size_t>(y) * out.stride[p],
                            m_frame->data[p] + static_cast<size_t>(y) * m_frame->linesize[p],
                            static_cast<size_t>(pw));
            }
        }
        out.interlaced = (m_frame->flags & AV_FRAME_FLAG_INTERLACED) != 0;
        out.top_field_first = (m_frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0;
        const int64_t ts = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
                               ? m_frame->best_effort_timestamp
                               : m_frame->pts;
        out.pts = ts == AV_NOPTS_VALUE ? -1.0 : static_cast<double>(ts) / 90000.0;
        m_width = out.width;
        m_height = out.height;
        return true;
    }

    void flush() override {
        if (m_ctx) avcodec_send_packet(m_ctx, nullptr);
    }

    void reset() override {
        if (m_ctx) avcodec_flush_buffers(m_ctx);
    }

    const char* backend_name() const override { return "ffmpeg"; }
    bool native() const override { return false; }
    int width() const override { return m_ctx ? m_ctx->width : 0; }
    int height() const override { return m_ctx ? m_ctx->height : 0; }
    float par() const override {
        if (!m_ctx) return 1.0f;
        const AVRational r = m_ctx->sample_aspect_ratio;
        if (r.num <= 0 || r.den <= 0) return 1.0f;
        return static_cast<float>(r.num) / static_cast<float>(r.den);
    }
    const char* name() const override { return "h264"; }

private:
    AVCodecContext* m_ctx = nullptr;
    AVPacket*       m_pkt = nullptr;
    AVFrame*        m_frame = nullptr;
    int             m_width = 0;
    int             m_height = 0;
};

}  // namespace

std::unique_ptr<VideoDecoderBackend> make_ffmpeg_h264_backend() {
    auto backend = std::make_unique<FfmpegH264Backend>();
    if (!backend->init(nullptr, 0)) return nullptr;
    return backend;
}

}  // namespace umdplay
