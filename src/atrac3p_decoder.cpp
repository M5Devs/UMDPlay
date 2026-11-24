// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Sony ATRAC3+ decoder driver, ported from FFmpeg libavcodec/atrac3plusdec.c
// (Copyright (c) 2010-2013 Maxim Poliakovski).  The frame loop, the residual
// spectrum reconstruction and the per-frame channel-unit orchestration follow
// that reference; only the FFmpeg plumbing (frame buffers, AVCodecContext) is
// replaced with plain C++.

#include "atrac3p_decoder.h"
#include "audio_decoder_backend.h"

#include <cstring>
#include <mutex>

#include "atrac3p/atrac3p_internal.h"

namespace umdplay {
namespace {

using namespace umdplay::atrac3p;

constexpr int kMaxChannelBlocks = 5;

// Output channel order for each supported layout (ATRAC3+ interleaves stereo
// and mono units in a fixed pattern; see the FFmpeg reference).
const uint8_t kChannelMap1[1] = {0};
const uint8_t kChannelMap2[2] = {0, 1};

std::once_flag g_init_once;

void init_static_tables()
{
    std::call_once(g_init_once, [] {
        at3p_init_vlcs();
        at3p_init_dsp_static();
    });
}

int16_t float_to_s16(float sample)
{
    const float v = sample * 32768.0f + 0.5f;
    if (!(v > -32769.0f))  // also catches NaN
        return -32768;
    if (v >= 32767.0f)
        return 32767;
    return static_cast<int16_t>(static_cast<int>(v));
}

}  // namespace

struct Atrac3PlusDecoder::Impl {
    BitReader gb;

    // Quantized MDCT spectrum, IMDCT output, gain-compensated signal and the
    // final QMF output.  mdct_buf needs one extra subband: the last subband's
    // 256-sample IMDCT starts at 15 * 128 = 1920.
    float samples[2][ATRAC3P_FRAME_SAMPLES]  = {{0}};
    float mdct_buf[2][ATRAC3P_FRAME_SAMPLES + ATRAC3P_SUBBAND_SAMPLES] = {{0}};
    float time_buf[2][ATRAC3P_FRAME_SAMPLES] = {{0}};
    float outp_buf[2][ATRAC3P_FRAME_SAMPLES] = {{0}};

    AtracGCContext gainc_ctx;
    MdctContext    mdct_ctx;      ///< 256-point IMDCT (one subband)
    MdctContext    ipqf_dct_ctx;  ///< 32-point half IMDCT (IPQF)

    Atrac3pChanUnitCtx ch_units[kMaxChannelBlocks];
    int                num_channel_blocks = 0;
    uint8_t            channel_blocks[kMaxChannelBlocks] = {0};
    const uint8_t*     channel_map = nullptr;

    int  channels    = 0;
    bool initialized = false;

    std::vector<float>   planar;       ///< scratch: per-channel float output
    std::vector<int16_t> interleaved;  ///< scratch: interleaved S16 output

    bool init(int ch);
    int  decode_frame(const uint8_t* data, int size, float* out_planar);
    void clear();

private:
    void decode_residual_spectrum(Atrac3pChanUnitCtx* ch_unit,
                                  float out[2][ATRAC3P_FRAME_SAMPLES],
                                  int num_channels);
    void reconstruct_frame(Atrac3pChanUnitCtx* ch_unit, int num_channels);
};

bool Atrac3PlusDecoder::Impl::init(int ch)
{
    init_static_tables();

    if (ch != 1 && ch != 2)
        return false;

    std::memset(ch_units, 0, sizeof(ch_units));
    std::memset(samples, 0, sizeof(samples));
    std::memset(mdct_buf, 0, sizeof(mdct_buf));
    std::memset(time_buf, 0, sizeof(time_buf));
    std::memset(outp_buf, 0, sizeof(outp_buf));

    // IPQF: 32-point inverse MDCT with scale 32/32768.
    mdct_init(&ipqf_dct_ctx, 5, 32.0 / 32768.0);
    // Subband IMDCT: 256-point inverse MDCT with unit scale.
    mdct_init(&mdct_ctx, 8, -1.0);

    atrac_init_gain_compensation(&gainc_ctx, 6, 2);

    std::memset(channel_blocks, 0, sizeof(channel_blocks));
    if (ch == 1) {
        num_channel_blocks = 1;
        channel_blocks[0]  = CH_UNIT_MONO;
        channel_map        = kChannelMap1;
    } else {
        num_channel_blocks = 1;
        channel_blocks[0]  = CH_UNIT_STEREO;
        channel_map        = kChannelMap2;
    }

    for (int i = 0; i < num_channel_blocks; i++) {
        Atrac3pChanUnitCtx& unit = ch_units[i];
        for (int c = 0; c < 2; c++) {
            Atrac3pChanParams& p = unit.channels[c];
            p.ch_num          = c;
            p.wnd_shape       = &p.wnd_shape_hist[0][0];
            p.wnd_shape_prev  = &p.wnd_shape_hist[1][0];
            p.gain_data       = &p.gain_data_hist[0][0];
            p.gain_data_prev  = &p.gain_data_hist[1][0];
            p.tones_info      = &p.tones_info_hist[0][0];
            p.tones_info_prev = &p.tones_info_hist[1][0];
        }
        unit.waves_info      = &unit.wave_synth_hist[0];
        unit.waves_info_prev = &unit.wave_synth_hist[1];
    }

    channels    = ch;
    initialized = true;
    return true;
}

void Atrac3PlusDecoder::Impl::clear()
{
    // Reset inter-frame history but keep the (immutable) table state.
    for (int i = 0; i < kMaxChannelBlocks; i++) {
        Atrac3pChanUnitCtx& unit = ch_units[i];
        std::memset(unit.prev_buf, 0, sizeof(unit.prev_buf));
        std::memset(unit.wave_synth_hist, 0, sizeof(unit.wave_synth_hist));
        for (int c = 0; c < 2; c++) {
            Atrac3pChanParams& p = unit.channels[c];
            std::memset(p.wnd_shape_hist, 0, sizeof(p.wnd_shape_hist));
            std::memset(p.gain_data_hist, 0, sizeof(p.gain_data_hist));
            std::memset(p.tones_info_hist, 0, sizeof(p.tones_info_hist));
            std::memset(&unit.ipqf_ctx[c], 0, sizeof(unit.ipqf_ctx[c]));
        }
    }
    std::memset(samples, 0, sizeof(samples));
    std::memset(mdct_buf, 0, sizeof(mdct_buf));
    std::memset(time_buf, 0, sizeof(time_buf));
    std::memset(outp_buf, 0, sizeof(outp_buf));
}

void Atrac3PlusDecoder::Impl::decode_residual_spectrum(
    Atrac3pChanUnitCtx* ch_unit, float out[2][ATRAC3P_FRAME_SAMPLES], int num_channels)
{
    int sb_RNG_index[ATRAC3P_SUBBANDS] = {0};

    if (ch_unit->mute_flag) {
        for (int ch = 0; ch < num_channels; ch++)
            std::memset(out[ch], 0, ATRAC3P_FRAME_SAMPLES * sizeof(out[ch][0]));
        return;
    }

    int RNG_index = 0;
    for (int qu = 0; qu < ch_unit->used_quant_units; qu++)
        RNG_index += ch_unit->channels[0].qu_sf_idx[qu] +
                     ch_unit->channels[1].qu_sf_idx[qu];

    for (int sb = 0; sb < ch_unit->num_coded_subbands; sb++, RNG_index += 128)
        sb_RNG_index[sb] = RNG_index & 0x3FC;

    for (int ch = 0; ch < num_channels; ch++) {
        std::memset(out[ch], 0, ATRAC3P_FRAME_SAMPLES * sizeof(out[ch][0]));

        for (int qu = 0; qu < ch_unit->used_quant_units; qu++) {
            const int16_t* src = &ch_unit->channels[ch].spectrum[qu_to_spec_pos[qu]];
            float* dst         = &out[ch][qu_to_spec_pos[qu]];
            const int nspeclines = qu_to_spec_pos[qu + 1] - qu_to_spec_pos[qu];

            if (ch_unit->channels[ch].qu_wordlen[qu] > 0) {
                const float q = sf_tab[ch_unit->channels[ch].qu_sf_idx[qu]] *
                                mant_tab[ch_unit->channels[ch].qu_wordlen[qu]];
                for (int i = 0; i < nspeclines; i++)
                    dst[i] = static_cast<float>(src[i]) * q;
            }
        }

        for (int sb = 0; sb < ch_unit->num_coded_subbands; sb++)
            at3p_power_compensation(ch_unit, ch, &out[ch][0], sb_RNG_index[sb], sb);
    }

    if (ch_unit->unit_type == CH_UNIT_STEREO) {
        for (int sb = 0; sb < ch_unit->num_coded_subbands; sb++) {
            if (ch_unit->swap_channels[sb]) {
                for (int i = 0; i < ATRAC3P_SUBBAND_SAMPLES; i++)
                    FFSWAP(float, out[0][sb * ATRAC3P_SUBBAND_SAMPLES + i],
                                  out[1][sb * ATRAC3P_SUBBAND_SAMPLES + i]);
            }
            if (ch_unit->negate_coeffs[sb]) {
                for (int i = 0; i < ATRAC3P_SUBBAND_SAMPLES; i++)
                    out[1][sb * ATRAC3P_SUBBAND_SAMPLES + i] =
                        -(out[1][sb * ATRAC3P_SUBBAND_SAMPLES + i]);
            }
        }
    }
}

void Atrac3PlusDecoder::Impl::reconstruct_frame(Atrac3pChanUnitCtx* ch_unit, int num_channels)
{
    for (int ch = 0; ch < num_channels; ch++) {
        for (int sb = 0; sb < ch_unit->num_subbands; sb++) {
            at3p_imdct(&mdct_ctx,
                       &samples[ch][sb * ATRAC3P_SUBBAND_SAMPLES],
                       &mdct_buf[ch][sb * ATRAC3P_SUBBAND_SAMPLES],
                       (ch_unit->channels[ch].wnd_shape_prev[sb] << 1) +
                           ch_unit->channels[ch].wnd_shape[sb],
                       sb);

            atrac_gain_compensation(&gainc_ctx,
                                    &mdct_buf[ch][sb * ATRAC3P_SUBBAND_SAMPLES],
                                    &ch_unit->prev_buf[ch][sb * ATRAC3P_SUBBAND_SAMPLES],
                                    &ch_unit->channels[ch].gain_data_prev[sb],
                                    &ch_unit->channels[ch].gain_data[sb],
                                    ATRAC3P_SUBBAND_SAMPLES,
                                    &time_buf[ch][sb * ATRAC3P_SUBBAND_SAMPLES]);
        }

        const int used = ch_unit->num_subbands * ATRAC3P_SUBBAND_SAMPLES;
        const int rest = ATRAC3P_FRAME_SAMPLES - used;
        if (rest > 0) {
            std::memset(&ch_unit->prev_buf[ch][used], 0, rest * sizeof(float));
            std::memset(&time_buf[ch][used], 0, rest * sizeof(float));
        }

        if (ch_unit->waves_info->tones_present ||
            ch_unit->waves_info_prev->tones_present) {
            for (int sb = 0; sb < ch_unit->num_subbands; sb++)
                if (ch_unit->channels[ch].tones_info[sb].num_wavs ||
                    ch_unit->channels[ch].tones_info_prev[sb].num_wavs)
                    at3p_generate_tones(ch_unit, ch, sb, &time_buf[ch][sb * 128]);
        }

        at3p_ipqf(&ipqf_dct_ctx, &ch_unit->ipqf_ctx[ch], &time_buf[ch][0],
                  &outp_buf[ch][0]);
    }

    for (int ch = 0; ch < num_channels; ch++) {
        FFSWAP(uint8_t*, ch_unit->channels[ch].wnd_shape, ch_unit->channels[ch].wnd_shape_prev);
        FFSWAP(AtracGainInfo*, ch_unit->channels[ch].gain_data, ch_unit->channels[ch].gain_data_prev);
        FFSWAP(Atrac3pWavesData*, ch_unit->channels[ch].tones_info, ch_unit->channels[ch].tones_info_prev);
    }

    FFSWAP(Atrac3pWaveSynthParams*, ch_unit->waves_info, ch_unit->waves_info_prev);
}

int Atrac3PlusDecoder::Impl::decode_frame(const uint8_t* data, int size, float* out)
{
    init_bits(&gb, data, size);

    if (get_bits1(&gb))  // invalid start bit
        return AT3P_ERR_INVALIDDATA;

    int ch_block     = 0;
    int out_ch_index = 0;

    while (bits_left(&gb) >= 2) {
        const int ch_unit_id = get_bits(&gb, 2);
        if (ch_unit_id == CH_UNIT_TERMINATOR)
            break;
        if (ch_unit_id == CH_UNIT_EXTENSION)
            return AT3P_ERR_PATCHWELCOME;
        if (ch_block >= num_channel_blocks || channel_blocks[ch_block] != ch_unit_id)
            return AT3P_ERR_INVALIDDATA;

        Atrac3pChanUnitCtx& unit = ch_units[ch_block];
        unit.unit_type = ch_unit_id;
        const int channels_to_process = ch_unit_id + 1;

        const int ret = at3p_decode_channel_unit(&gb, &unit, channels_to_process, nullptr);
        if (ret < 0)
            return ret;

        decode_residual_spectrum(&unit, samples, channels_to_process);
        reconstruct_frame(&unit, channels_to_process);

        for (int i = 0; i < channels_to_process; i++)
            std::memcpy(out + static_cast<size_t>(channel_map[out_ch_index + i]) * ATRAC3P_FRAME_SAMPLES,
                        outp_buf[i], ATRAC3P_FRAME_SAMPLES * sizeof(float));

        ch_block++;
        out_ch_index += channels_to_process;
    }

    // A frame with no channel unit carries no audio: emit silence rather than
    // stale state from the previous frame.
    if (ch_block == 0)
        std::memset(out, 0, static_cast<size_t>(channels) * ATRAC3P_FRAME_SAMPLES * sizeof(float));

    return ATRAC3P_FRAME_SAMPLES;
}

Atrac3PlusDecoder::Atrac3PlusDecoder() : m_impl(std::make_unique<Impl>()) {}

Atrac3PlusDecoder::~Atrac3PlusDecoder() = default;

bool Atrac3PlusDecoder::init(int channels) { return m_impl->init(channels); }

bool Atrac3PlusDecoder::initialized() const { return m_impl->initialized; }

int Atrac3PlusDecoder::channels() const { return m_impl->channels; }

void Atrac3PlusDecoder::reset() { m_impl->clear(); }

int Atrac3PlusDecoder::decode(const uint8_t* data, size_t size,
                              std::vector<int16_t>& pcm)
{
    if (!m_impl->initialized || !data || size == 0 || size > 0x7fffffff)
        return 0;

    // Decode into a per-channel float buffer, then interleave to S16.
    Impl& d = *m_impl;
    d.planar.assign(static_cast<size_t>(2) * ATRAC3P_FRAME_SAMPLES, 0.0f);

    const int ret = d.decode_frame(data, static_cast<int>(size), d.planar.data());
    if (ret <= 0)
        return 0;  // rejected frame: never propagate a negative code

    const int ch = d.channels;
    const size_t base = pcm.size();
    pcm.resize(base + static_cast<size_t>(ATRAC3P_FRAME_SAMPLES) * ch);

    if (ch == 1) {
        const float* l = d.planar.data();
        for (int i = 0; i < ATRAC3P_FRAME_SAMPLES; i++)
            pcm[base + i] = float_to_s16(l[i]);
    } else {
        const float* l = d.planar.data();
        const float* r = d.planar.data() + ATRAC3P_FRAME_SAMPLES;
        for (int i = 0; i < ATRAC3P_FRAME_SAMPLES; i++) {
            pcm[base + static_cast<size_t>(i) * 2 + 0] = float_to_s16(l[i]);
            pcm[base + static_cast<size_t>(i) * 2 + 1] = float_to_s16(r[i]);
        }
    }
    return ATRAC3P_FRAME_SAMPLES;
}

int Atrac3PlusDecoder::decode(const uint8_t* data, size_t size,
                              std::vector<uint8_t>& pcm_bytes)
{
    Impl& d = *m_impl;
    d.interleaved.clear();
    const int ret = decode(data, size, d.interleaved);
    if (ret <= 0)
        return ret;

    const size_t base = pcm_bytes.size();
    pcm_bytes.resize(base + d.interleaved.size() * sizeof(int16_t));
    for (size_t i = 0; i < d.interleaved.size(); i++) {
        const uint16_t v = static_cast<uint16_t>(d.interleaved[i]);
        pcm_bytes[base + i * 2 + 0] = static_cast<uint8_t>(v & 0xFF);
        pcm_bytes[base + i * 2 + 1] = static_cast<uint8_t>(v >> 8);
    }
    return ret;
}

namespace {

// Adapts the concrete decoder to the AudioDecoderBackend seam used by Decoder.
class NativeAtrac3pBackend final : public AudioDecoderBackend {
public:
    bool init(int channels) override { return m_dec.init(channels); }
    int decode(const uint8_t* data, size_t size, std::vector<uint8_t>& pcm) override {
        return m_dec.decode(data, size, pcm);
    }
    void reset() override { m_dec.reset(); }
    int sample_rate() const override { return Atrac3PlusDecoder::kSampleRate; }
    int channels() const override { return m_dec.channels(); }

private:
    Atrac3PlusDecoder m_dec;
};

}  // namespace

std::unique_ptr<AudioDecoderBackend> make_native_atrac3p_backend() {
    return std::make_unique<NativeAtrac3pBackend>();
}

}  // namespace umdplay
