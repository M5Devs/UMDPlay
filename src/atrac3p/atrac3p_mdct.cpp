// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Inverse MDCT used by the ATRAC3+ decoder, ported from FFmpeg's split-radix
// FFT + MDCT (libavcodec/fft_template.c, mdct_template.c).
// Copyright (c) 2000-2007 Fabrice Bellard, 2007 Alex Beregszaszi,
//               2007-2010 Michael Niedermayer, 2008 Loren Merritt.
//
// ATRAC3+ only ever needs the inverse transform, so this file implements just
// imdct_half()/imdct_calc() for N = 32 (IPQF) and N = 256 (subband IMDCT).

#include "atrac3p_internal.h"

namespace umdplay::atrac3p {
namespace {

constexpr float kSqrtHalf = 0.70710678118654752440f;

float ff_cos_16[16 / 2];
float ff_cos_32[32 / 2];
float ff_cos_64[64 / 2];
float ff_cos_128[128 / 2];
float ff_cos_256[256 / 2];
float ff_cos_512[512 / 2];

void init_ff_cos_tabs(float* tab, int m)
{
    const double freq = 2 * M_PI / m;
    for (int i = 0; i <= m / 4; i++)
        tab[i] = static_cast<float>(cos(i * freq));
    for (int i = 1; i < m / 4; i++)
        tab[m / 2 - i] = tab[i];
}

int split_radix_permutation(int i, int n, int inverse)
{
    if (n <= 2)
        return i & 1;
    int m = n >> 1;
    if ((i & m) == 0)
        return split_radix_permutation(i, m, inverse) * 2;
    m >>= 1;
    const int bit = ((i & m) == 0) ? 1 : 0;
    return split_radix_permutation(i, m, inverse) * 4 + (inverse == bit ? 1 : -1);
}

void fft_init(MdctContext* s, int nbits)
{
    const int n = 1 << nbits;
    s->nbits = nbits;

    init_ff_cos_tabs(ff_cos_16, 16);
    init_ff_cos_tabs(ff_cos_32, 32);
    init_ff_cos_tabs(ff_cos_64, 64);
    init_ff_cos_tabs(ff_cos_128, 128);
    init_ff_cos_tabs(ff_cos_256, 256);
    init_ff_cos_tabs(ff_cos_512, 512);

    for (int i = 0; i < n; i++)
        s->revtab[-split_radix_permutation(i, n, 1) & (n - 1)] = i;
}

inline void cmul(float* d, int dre, int dim, float are, float aim, float bre, float bim)
{
    d[dre] = are * bre - aim * bim;
    d[dim] = are * bim + aim * bre;
}

void fft4(float* z)
{
    const float t3 = z[0] - z[2];
    const float t1 = z[0] + z[2];
    const float t8 = z[6] - z[4];
    const float t6 = z[6] + z[4];
    z[4] = t1 - t6;
    z[0] = t1 + t6;
    const float t4 = z[1] - z[3];
    const float t2 = z[1] + z[3];
    const float t7 = z[5] - z[7];
    const float t5 = z[5] + z[7];
    z[7] = t4 - t8;
    z[3] = t4 + t8;
    z[6] = t3 - t7;
    z[2] = t3 + t7;
    z[5] = t2 - t5;
    z[1] = t2 + t5;
}

void fft8(float* z)
{
    fft4(z);

    float t1 = z[8] + z[10];
    z[10] = z[8] - z[10];
    float t2 = z[9] + z[11];
    z[11] = z[9] - z[11];
    float t5 = z[12] + z[14];
    z[14] = z[12] - z[14];
    float t6 = z[13] + z[15];
    z[15] = z[13] - z[15];

    float t3 = t5 - t1;
    t5 = t5 + t1;
    z[8] = z[0] - t5;
    z[0] = z[0] + t5;
    z[13] = z[5] - t3;
    z[5] = z[5] + t3;
    float t4 = t2 - t6;
    t6 = t2 + t6;
    z[12] = z[4] - t4;
    z[4] = z[4] + t4;
    z[9] = z[1] - t6;
    z[1] = z[1] + t6;

    t1 = z[10] * kSqrtHalf + z[11] * kSqrtHalf;
    t2 = -z[10] * kSqrtHalf + z[11] * kSqrtHalf;
    t5 = z[14] * kSqrtHalf - z[15] * kSqrtHalf;
    t6 = z[14] * kSqrtHalf + z[15] * kSqrtHalf;
    t3 = t5 - t1;
    t5 = t5 + t1;
    z[10] = z[2] - t5;
    z[2] = z[2] + t5;
    z[15] = z[7] - t3;
    z[7] = z[7] + t3;
    t4 = t2 - t6;
    t6 = t2 + t6;
    z[14] = z[6] - t4;
    z[6] = z[6] + t4;
    z[11] = z[3] - t6;
    z[3] = z[3] + t6;
}

void pass(float* z, const float* cos_tab, int n)
{
    int o0 = 0;
    int o1 = 2 * n * 2;
    int o2 = 4 * n * 2;
    int o3 = 6 * n * 2;
    int wre = 0;
    int wim = 2 * n;
    n--;

    float t1 = z[o2 + 0];
    float t2 = z[o2 + 1];
    float t5 = z[o3 + 0];
    float t6 = z[o3 + 1];
    float t3 = t5 - t1;
    t5 = t5 + t1;
    z[o2 + 0] = z[o0 + 0] - t5;
    z[o0 + 0] = z[o0 + 0] + t5;
    z[o3 + 1] = z[o1 + 1] - t3;
    z[o1 + 1] = z[o1 + 1] + t3;
    float t4 = t2 - t6;
    t6 = t2 + t6;
    z[o3 + 0] = z[o1 + 0] - t4;
    z[o1 + 0] = z[o1 + 0] + t4;
    z[o2 + 1] = z[o0 + 1] - t6;
    z[o0 + 1] = z[o0 + 1] + t6;

    t1 = z[o2 + 2] * cos_tab[wre + 1] + z[o2 + 3] * cos_tab[wim - 1];
    t2 = -z[o2 + 2] * cos_tab[wim - 1] + z[o2 + 3] * cos_tab[wre + 1];
    t5 = z[o3 + 2] * cos_tab[wre + 1] - z[o3 + 3] * cos_tab[wim - 1];
    t6 = z[o3 + 2] * cos_tab[wim - 1] + z[o3 + 3] * cos_tab[wre + 1];
    t3 = t5 - t1;
    t5 = t5 + t1;
    z[o2 + 2] = z[o0 + 2] - t5;
    z[o0 + 2] = z[o0 + 2] + t5;
    z[o3 + 3] = z[o1 + 3] - t3;
    z[o1 + 3] = z[o1 + 3] + t3;
    t4 = t2 - t6;
    t6 = t2 + t6;
    z[o3 + 2] = z[o1 + 2] - t4;
    z[o1 + 2] = z[o1 + 2] + t4;
    z[o2 + 3] = z[o0 + 3] - t6;
    z[o0 + 3] = z[o0 + 3] + t6;

    do {
        o0 += 4;
        o1 += 4;
        o2 += 4;
        o3 += 4;
        wre += 2;
        wim -= 2;

        t1 = z[o2 + 0] * cos_tab[wre] + z[o2 + 1] * cos_tab[wim];
        t2 = -z[o2 + 0] * cos_tab[wim] + z[o2 + 1] * cos_tab[wre];
        t5 = z[o3 + 0] * cos_tab[wre] - z[o3 + 1] * cos_tab[wim];
        t6 = z[o3 + 0] * cos_tab[wim] + z[o3 + 1] * cos_tab[wre];
        t3 = t5 - t1;
        t5 = t5 + t1;
        z[o2 + 0] = z[o0 + 0] - t5;
        z[o0 + 0] = z[o0 + 0] + t5;
        z[o3 + 1] = z[o1 + 1] - t3;
        z[o1 + 1] = z[o1 + 1] + t3;
        t4 = t2 - t6;
        t6 = t2 + t6;
        z[o3 + 0] = z[o1 + 0] - t4;
        z[o1 + 0] = z[o1 + 0] + t4;
        z[o2 + 1] = z[o0 + 1] - t6;
        z[o0 + 1] = z[o0 + 1] + t6;

        t1 = z[o2 + 2] * cos_tab[wre + 1] + z[o2 + 3] * cos_tab[wim - 1];
        t2 = -z[o2 + 2] * cos_tab[wim - 1] + z[o2 + 3] * cos_tab[wre + 1];
        t5 = z[o3 + 2] * cos_tab[wre + 1] - z[o3 + 3] * cos_tab[wim - 1];
        t6 = z[o3 + 2] * cos_tab[wim - 1] + z[o3 + 3] * cos_tab[wre + 1];
        t3 = t5 - t1;
        t5 = t5 + t1;
        z[o2 + 2] = z[o0 + 2] - t5;
        z[o0 + 2] = z[o0 + 2] + t5;
        z[o3 + 3] = z[o1 + 3] - t3;
        z[o1 + 3] = z[o1 + 3] + t3;
        t4 = t2 - t6;
        t6 = t2 + t6;
        z[o3 + 2] = z[o1 + 2] - t4;
        z[o1 + 2] = z[o1 + 2] + t4;
        z[o2 + 3] = z[o0 + 3] - t6;
        z[o0 + 3] = z[o0 + 3] + t6;
    } while (--n != 0);
}

void fft16(float* z) { fft8(z); fft4(z + 16); fft4(z + 24); pass(z, ff_cos_16, 2); }
void fft32(float* z) { fft16(z); fft8(z + 32); fft8(z + 48); pass(z, ff_cos_32, 4); }
void fft64(float* z) { fft32(z); fft16(z + 64); fft16(z + 96); pass(z, ff_cos_64, 8); }
void fft128(float* z) { fft64(z); fft32(z + 128); fft32(z + 192); pass(z, ff_cos_128, 16); }
void fft256(float* z) { fft128(z); fft64(z + 256); fft64(z + 384); pass(z, ff_cos_256, 32); }
void fft512(float* z) { fft256(z); fft128(z + 512); fft128(z + 768); pass(z, ff_cos_512, 64); }

void fft_calc(MdctContext* s, float* z)
{
    switch (s->nbits) {
    case 2: fft4(z); break;
    case 3: fft8(z); break;
    case 4: fft16(z); break;
    case 5: fft32(z); break;
    case 6: fft64(z); break;
    case 7: fft128(z); break;
    case 8: fft256(z); break;
    case 9: fft512(z); break;
    default: AT3P_ASSERT(0); break;
    }
}

}  // namespace

int mdct_init(MdctContext* s, int nbits, double scale)
{
    const int n = 1 << nbits;
    const int n4 = n >> 2;

    s->mdct_bits = nbits;
    fft_init(s, nbits - 2);

    const double theta = 1.0 / 8.0 + (scale < 0 ? n4 : 0);
    scale = sqrt(fabs(scale));
    for (int i = 0; i < n4; i++) {
        const double alpha = 2 * M_PI * (i + theta) / n;
        s->tcos[i] = static_cast<float>(-cos(alpha) * scale);
        s->tsin[i] = static_cast<float>(-sin(alpha) * scale);
    }
    return 0;
}

void imdct_half(MdctContext* s, float* output, const float* input)
{
    const int n = 1 << s->mdct_bits;
    const int n2 = n >> 1;
    const int n4 = n >> 2;
    const int n8 = n >> 3;

    /* pre rotation */
    int in1 = 0;
    int in2 = n2 - 1;
    for (int k = 0; k < n4; k++) {
        const int j = s->revtab[k];
        cmul(output, j * 2, j * 2 + 1, input[in2], input[in1], s->tcos[k], s->tsin[k]);
        in1 += 2;
        in2 -= 2;
    }

    fft_calc(s, output);

    /* post rotation + reordering */
    float r[4];
    for (int k = 0; k < n8; k++) {
        cmul(r, 0, 3, output[(n8 - k - 1) * 2 + 1], output[(n8 - k - 1) * 2 + 0],
             s->tsin[n8 - k - 1], s->tcos[n8 - k - 1]);
        cmul(r, 2, 1, output[(n8 + k) * 2 + 1], output[(n8 + k) * 2 + 0],
             s->tsin[n8 + k], s->tcos[n8 + k]);
        output[(n8 - k - 1) * 2 + 0] = r[0];
        output[(n8 - k - 1) * 2 + 1] = r[1];
        output[(n8 + k) * 2 + 0] = r[2];
        output[(n8 + k) * 2 + 1] = r[3];
    }
}

void imdct_calc(MdctContext* s, float* output, const float* input)
{
    const int n = 1 << s->mdct_bits;
    const int n2 = n >> 1;
    const int n4 = n >> 2;

    imdct_half(s, output + n4, input);

    for (int k = 0; k < n4; k++) {
        output[k] = -output[n2 - k - 1];
        output[n - k - 1] = output[n2 + k];
    }
}

}  // namespace umdplay::atrac3p
