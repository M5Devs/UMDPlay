// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Self-contained ATRAC3+ decoder internals.
//
// This is a faithful C++17 port of FFmpeg's ATRAC3+ decoder
// (libavcodec/atrac3plus.c, atrac3plusdec.c, atrac3plusdsp.c, atrac3plus_data.h,
// atrac.c) by Maxim Poliakovski <max_poliakovski@users.sourceforge.net>.
// Copyright (c) 2010-2013 Maxim Poliakovski.  The bit reader, the VLC table
// builder, the FFT/MDCT and the float DSP helpers are likewise reimplemented
// from FFmpeg's corresponding units (get_bits.h, vlc.c, fft/mdct, float_dsp.c).
// No FFmpeg library is linked: everything needed lives in src/atrac3p/.
//
// The algorithm's identifiers are kept close to the reference so the port can
// be audited line by line against upstream.

#pragma once

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace umdplay::atrac3p {

/** Global unit sizes */
constexpr int ATRAC3P_SUBBANDS        = 16;   ///< number of PQF subbands
constexpr int ATRAC3P_SUBBAND_SAMPLES = 128;  ///< number of samples per subband
constexpr int ATRAC3P_FRAME_SAMPLES   = ATRAC3P_SUBBAND_SAMPLES * ATRAC3P_SUBBANDS;
constexpr int ATRAC3P_PQF_FIR_LEN     = 12;   ///< length of the prototype FIR of the PQF
constexpr int ATRAC3P_POWER_COMP_OFF  = 15;   ///< disable power compensation

/** ATRAC3+ channel unit types */
enum Atrac3pChannelUnitTypes {
    CH_UNIT_MONO       = 0,
    CH_UNIT_STEREO     = 1,
    CH_UNIT_EXTENSION  = 2,
    CH_UNIT_TERMINATOR = 3
};

enum {
    AT3P_ERR_INVALIDDATA  = -1,
    AT3P_ERR_PATCHWELCOME = -2
};

#define AT3P_ASSERT(cond) assert(cond)
#define AT3P_UNUSED(x) ((void)(x))
#define FF_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))
#define AT3P_COLD
#define FFMIN(a, b) ((a) > (b) ? (b) : (a))
#define FFMAX(a, b) ((a) > (b) ? (a) : (b))
#define FFABS(a) ((a) >= 0 ? (a) : (-(a)))
#define FFSWAP(type, a, b) do { type SWAP_tmp = b; b = a; a = SWAP_tmp; } while (0)
#define DECLARE_ALIGNED(align, type, name) type name
#define LOCAL_ALIGNED_32(type, name, size) type name size
#define LOCAL_ALIGNED(align, type, name, size) type name size

// ---------------------------------------------------------------------------
// Bit reader (MSB-first), matching FFmpeg's get_bits.h semantics closely enough
// for the parser: reads past the end yield zero bits and never index outside
// the buffer.
// ---------------------------------------------------------------------------
struct BitReader {
    const uint8_t* buf      = nullptr;
    int            size_bits = 0;  ///< total bits available
    int            index     = 0;  ///< current bit position
};

inline int init_bits(BitReader* br, const uint8_t* buf, int size_bytes)
{
    br->buf       = buf;
    br->size_bits = size_bytes > 0 ? size_bytes * 8 : 0;
    br->index     = 0;
    return 0;
}

inline int bits_left(const BitReader* br)
{
    return br->size_bits - br->index;
}

inline uint32_t get_bits(BitReader* br, int n)
{
    if (n <= 0)
        return 0;
    uint32_t v = 0;
    int idx = br->index;
    const int end = br->size_bits;
    for (int i = 0; i < n; i++, idx++) {
        uint32_t bit = 0;
        if (idx < end)
            bit = (br->buf[idx >> 3] >> (7 - (idx & 7))) & 1u;
        v = (v << 1) | bit;
    }
    br->index = idx < end ? idx : end;  // clamp so overread cannot loop forever
    return v;
}

inline int get_bits1(BitReader* br)
{
    return static_cast<int>(get_bits(br, 1));
}

inline uint32_t get_bitsz(BitReader* br, int n)
{
    return n ? get_bits(br, n) : 0;
}

inline void skip_bits(BitReader* br, int n)
{
    br->index += n;
    if (br->index < 0)
        br->index = 0;
    if (br->index > br->size_bits)
        br->index = br->size_bits;
}

/** Peek without consuming; missing bits read as zero (FFmpeg pads its cache). */
inline uint32_t show_bits(const BitReader* br, int n)
{
    uint32_t v = 0;
    int idx = br->index;
    const int end = br->size_bits;
    for (int i = 0; i < n; i++, idx++) {
        uint32_t bit = 0;
        if (idx < end)
            bit = (br->buf[idx >> 3] >> (7 - (idx & 7))) & 1u;
        v = (v << 1) | bit;
    }
    return v;
}

inline int sign_extend(int val, int bits)
{
    const int shift = 32 - bits;
    return (val << shift) >> shift;  // NOLINT: intentional arithmetic shift
}

inline uint32_t zero_extend(uint32_t val, int bits)
{
    return val & ((1u << bits) - 1u);
}

inline int at3p_log2(int n)
{
    if (n <= 0)
        return 0;
    return 31 - __builtin_clz(static_cast<unsigned>(n));
}

// Logging / diagnostics: the port has no logger. These keep the reference call
// sites intact and mark their context parameter used (avoids -Wunused-param).
struct AVCodecContext {};
enum { AV_LOG_ERROR = 0, AV_LOG_WARNING = 1, AV_LOG_INFO = 2 };
#define at3p_log(ctx, level, ...) do { (void)(ctx); } while (0)
#define at3p_missing_feature(ctx, ...) do { (void)(ctx); } while (0)

// ---------------------------------------------------------------------------
// VLC (single-level canonical Huffman lookup tables, as used by ATRAC3+)
// ---------------------------------------------------------------------------
struct VLCElem {
    int16_t sym;
    int8_t  len;
};

struct VLC {
    VLCElem* table            = nullptr;
    int      table_allocated  = 0;
    int      table_size       = 0;
    int      bits             = 0;
};

/**
 * Build a canonical Huffman table from per-symbol code lengths, assigning codes
 * in the given order (identical to FFmpeg's ff_vlc_init_from_lengths, which the
 * ATRAC3+ codebooks rely on).  `lens[i]` may be zero to skip a symbol.
 */
int vlc_init_from_lengths(VLC* vlc, int nb_bits, int nb_codes,
                          const uint8_t* lens, const uint8_t* symbols);

/** Decode one symbol; `max_depth` is accepted for source compatibility. */
inline int get_vlc2(BitReader* br, const VLCElem* table, int bits, int /*max_depth*/)
{
    const unsigned index = show_bits(br, bits);
    const VLCElem e = table[index];
    skip_bits(br, e.len);
    return e.sym;
}

// ---------------------------------------------------------------------------
// Float DSP helpers (subset used by ATRAC3+)
// ---------------------------------------------------------------------------
struct FloatDsp {
    static void vector_fmul(float* dst, const float* src0, const float* src1, int len)
    {
        for (int i = 0; i < len; i++)
            dst[i] = src0[i] * src1[i];
    }
    static void vector_fmac_scalar(float* dst, const float* src, float mul, int len)
    {
        for (int i = 0; i < len; i++)
            dst[i] += src[i] * mul;
    }
    static void vector_fmul_scalar(float* dst, const float* src, float mul, int len)
    {
        for (int i = 0; i < len; i++)
            dst[i] = src[i] * mul;
    }
    static void vector_fmul_reverse(float* dst, const float* src0, const float* src1, int len)
    {
        for (int i = 0; i < len; i++)
            dst[i] = src0[i] * src1[len - 1 - i];
    }
};

// ---------------------------------------------------------------------------
// Gain compensation (atrac.c)
// ---------------------------------------------------------------------------
struct AtracGainInfo {
    int num_points;
    int lev_code[7];
    int loc_code[7];
};

struct AtracGCContext {
    float gain_tab1[16];
    float gain_tab2[31];
    int   id2exp_offset;
    int   loc_scale;
    int   loc_size;
};

void atrac_init_gain_compensation(AtracGCContext* gctx, int id2exp_offset, int loc_scale);
void atrac_gain_compensation(AtracGCContext* gctx, float* in, float* prev,
                             AtracGainInfo* gc_now, AtracGainInfo* gc_next,
                             int num_samples, float* out);

// ---------------------------------------------------------------------------
// MDCT / IMDCT (port of FFmpeg's fft/mdct, via the same algorithm Jpcsp uses)
// ---------------------------------------------------------------------------
struct MdctContext {
    int   mdct_bits;   ///< log2 of the MDCT size N
    int   nbits;       ///< log2 of the underlying FFT size (N/4)
    int   revtab[256];
    float tcos[128];
    float tsin[128];
};

/** `nbits` is log2(N); `scale` follows ff_mdct_init (negative flips theta). */
int mdct_init(MdctContext* s, int nbits, double scale);

/** Middle half of the inverse MDCT: N/2 outputs from N/2 inputs. */
void imdct_half(MdctContext* s, float* output, const float* input);

/** Full inverse MDCT: N outputs from N/2 inputs. */
void imdct_calc(MdctContext* s, float* output, const float* input);

// ---------------------------------------------------------------------------
// Data shared between the parser and the DSP
// ---------------------------------------------------------------------------
extern const uint16_t qu_to_spec_pos[33];
extern const float    sf_tab[64];
extern const float    mant_tab[8];
extern const float    sine_win_64[64];
extern const float    sine_win_128[128];

// ---------------------------------------------------------------------------
// Per-channel / per-unit state (mirrors atrac3plus.h)
// ---------------------------------------------------------------------------
struct Atrac3pIPQFChannelCtx {
    float buf1[ATRAC3P_PQF_FIR_LEN * 2][8];
    float buf2[ATRAC3P_PQF_FIR_LEN * 2][8];
    int   pos;
};

struct Atrac3pWaveEnvelope {
    int has_start_point;
    int has_stop_point;
    int start_pos;
    int stop_pos;
};

struct Atrac3pWavesData {
    Atrac3pWaveEnvelope pend_env;
    Atrac3pWaveEnvelope curr_env;
    int num_wavs;
    int start_index;
};

struct Atrac3pWaveParam {
    int freq_index;
    int amp_sf;
    int amp_index;
    int phase_index;
};

struct Atrac3pChanParams {
    int     ch_num;
    int     num_coded_vals;
    int     fill_mode;
    int     split_point;
    int     table_type;
    int     qu_wordlen[32];
    int     qu_sf_idx[32];
    int     qu_tab_idx[32];
    int16_t spectrum[2048];
    uint8_t power_levs[5];

    uint8_t  wnd_shape_hist[2][ATRAC3P_SUBBANDS];
    uint8_t* wnd_shape;
    uint8_t* wnd_shape_prev;

    AtracGainInfo  gain_data_hist[2][ATRAC3P_SUBBANDS];
    AtracGainInfo* gain_data;
    AtracGainInfo* gain_data_prev;
    int            num_gain_subbands;

    Atrac3pWavesData  tones_info_hist[2][ATRAC3P_SUBBANDS];
    Atrac3pWavesData* tones_info;
    Atrac3pWavesData* tones_info_prev;
};

struct Atrac3pWaveSynthParams {
    int     tones_present;
    int     amplitude_mode;
    int     num_tone_bands;
    uint8_t tone_sharing[ATRAC3P_SUBBANDS];
    uint8_t tone_master[ATRAC3P_SUBBANDS];
    uint8_t invert_phase[ATRAC3P_SUBBANDS];
    int     tones_index;
    Atrac3pWaveParam waves[48];
};

struct Atrac3pChanUnitCtx {
    int     unit_type;
    int     num_quant_units;
    int     num_subbands;
    int     used_quant_units;
    int     num_coded_subbands;
    int     mute_flag;
    int     use_full_table;
    int     noise_present;
    int     noise_level_index;
    int     noise_table_index;
    uint8_t swap_channels[ATRAC3P_SUBBANDS];
    uint8_t negate_coeffs[ATRAC3P_SUBBANDS];
    Atrac3pChanParams channels[2];

    Atrac3pWaveSynthParams  wave_synth_hist[2];
    Atrac3pWaveSynthParams* waves_info;
    Atrac3pWaveSynthParams* waves_info_prev;

    Atrac3pIPQFChannelCtx ipqf_ctx[2];
    float prev_buf[2][ATRAC3P_FRAME_SAMPLES];
};

// ---------------------------------------------------------------------------
// Module entry points
// ---------------------------------------------------------------------------
void at3p_init_vlcs();
void at3p_init_dsp_static();
int  at3p_decode_channel_unit(BitReader* br, Atrac3pChanUnitCtx* ctx, int num_channels,
                             AVCodecContext* avctx);
void at3p_generate_tones(Atrac3pChanUnitCtx* ch_unit, int ch_num, int sb, float* out);
void at3p_power_compensation(Atrac3pChanUnitCtx* ctx, int ch_index, float* sp,
                             int rng_index, int sb_num);
void at3p_imdct(MdctContext* mdct_ctx, float* in, float* out, int wind_id, int sb);
void at3p_ipqf(MdctContext* dct_ctx, Atrac3pIPQFChannelCtx* hist,
               const float* in, float* out);

}  // namespace umdplay::atrac3p
