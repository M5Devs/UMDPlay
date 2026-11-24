// SPDX-License-Identifier: GPL-2.0-only
//
// Optional libavcodec + libswresample ATRAC3+ backend. Only compiled for A/B
// builds (-DUMDPLAY_NATIVE_ATRAC3P=OFF); the default player uses src/atrac3p/.

#include "audio_decoder_backend.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace umdplay {
namespace {

class FfmpegAtrac3pBackend final : public AudioDecoderBackend {
public:
    ~FfmpegAtrac3pBackend() override {
        if (m_swr) swr_free(&m_swr);
        if (m_frame) av_frame_free(&m_frame);
        if (m_pkt) av_packet_free(&m_pkt);
        if (m_ctx) avcodec_free_context(&m_ctx);
    }

    bool init(int channels) override {
        const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
        if (!dec) return false;
        m_ctx = avcodec_alloc_context3(dec);
        if (!m_ctx) return false;
        if (avcodec_open2(m_ctx, dec, nullptr) < 0) return false;

        AVChannelLayout in{};
        av_channel_layout_default(&in, channels);
        AVChannelLayout out{};
        av_channel_layout_default(&out, 2);
        const int rc = swr_alloc_set_opts2(&m_swr, &out, AV_SAMPLE_FMT_S16, 44100,
                                           &in, AV_SAMPLE_FMT_FLTP, 44100, 0, nullptr);
        av_channel_layout_uninit(&in);
        av_channel_layout_uninit(&out);
        if (rc < 0 || swr_init(m_swr) < 0) return false;

        m_channels = channels;
        m_pkt = av_packet_alloc();
        m_frame = av_frame_alloc();
        return m_pkt && m_frame;
    }

    int decode(const uint8_t* data, size_t size, std::vector<uint8_t>& pcm) override {
        if (!m_ctx || !data || size == 0) return 0;
        av_packet_unref(m_pkt);
        m_pkt->data = const_cast<uint8_t*>(data);
        m_pkt->size = static_cast<int>(size);
        if (avcodec_send_packet(m_ctx, m_pkt) < 0) return 0;

        int produced = 0;
        while (true) {
            av_frame_unref(m_frame);
            if (avcodec_receive_frame(m_ctx, m_frame) < 0) break;
            const int out_samples = m_frame->nb_samples;
            if (out_samples <= 0) continue;
            const size_t base = pcm.size();
            pcm.resize(base + static_cast<size_t>(out_samples) * 2 * sizeof(int16_t));
            uint8_t* dst = pcm.data() + base;
            const int converted = swr_convert(m_swr, &dst, out_samples,
                                              const_cast<const uint8_t**>(m_frame->data),
                                              m_frame->nb_samples);
            if (converted < 0) {
                pcm.resize(base);
                break;
            }
            pcm.resize(base + static_cast<size_t>(converted) * 2 * sizeof(int16_t));
            produced += converted;
        }
        return produced;
    }

    void reset() override {
        if (m_ctx) avcodec_flush_buffers(m_ctx);
        if (m_swr) swr_close(m_swr), swr_init(m_swr);
    }

    int sample_rate() const override { return m_ctx ? m_ctx->sample_rate : 0; }
    int channels() const override { return m_channels; }
    bool native() const override { return false; }

private:
    AVCodecContext* m_ctx = nullptr;
    AVPacket*       m_pkt = nullptr;
    AVFrame*        m_frame = nullptr;
    SwrContext*     m_swr = nullptr;
    int             m_channels = 2;
};

}  // namespace

std::unique_ptr<AudioDecoderBackend> make_ffmpeg_atrac3p_backend() {
    auto backend = std::make_unique<FfmpegAtrac3pBackend>();
    return backend;
}

}  // namespace umdplay
