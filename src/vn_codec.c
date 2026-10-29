/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * vambed-neo codec layer on mbelib-neo (AMBE+, AMBE+2, IMBE) and codec2.
 *
 * Wire frames, as an xlxd reflector carries them:
 *   AMBE+   9 B  D-STAR DV voice bytes (72-bit channel frame, FEC included)
 *   AMBE+2  9 B  DMR 72-bit AMBE frame, MSB first, DMR dibit interleave
 *   Codec2  8 B  Codec2 3200 frame
 *   IMBE   11 B  the 88 IMBE voice bits u0..u7, MSB first, no FEC
 *   PCM   320 B  160 samples S16LE
 */
#include "vn_codec.h"

#include <stdlib.h>
#include <string.h>

#include <codec2/codec2.h>
#include <mbelib-neo/mbelib.h>

/* DMR AMBE dibit interleave tables as in DSD (rW, rX, rY, rZ; ISC licence,
 * see NOTICE): dibit i -> (plane, bit) of its high bit (W, X) and its low
 * bit (Y, Z). */
static const uint8_t dmr_w[36] = {
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2 };
static const uint8_t dmr_x[36] = {
    23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5, 17, 4, 16, 3, 15, 2,
    14, 1, 13, 0, 12, 10, 11, 9, 10, 8, 9, 7, 8, 6, 7, 5, 6, 4 };
static const uint8_t dmr_y[36] = {
    0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3, 1, 3, 1, 3, 1, 3,
    1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3 };
static const uint8_t dmr_z[36] = {
    5, 3, 4, 2, 3, 1, 2, 0, 1, 13, 0, 12, 22, 11, 21, 10, 20, 9,
    19, 8, 18, 7, 17, 6, 16, 5, 15, 4, 14, 3, 13, 2, 12, 1, 11, 0 };

typedef struct {
    mbe_parms cur, prev, enh;
} mbe_state;

/* codec2 draws its random phases from codec2_rand(), a single process-wide
 * generator, so a stream's audio would depend on every Codec2 frame decoded
 * before it. The library calls it through the PLT; this definition takes
 * its place and gives each stream its own generator, which keeps the output
 * of a stream a function of its own input (two instances given the same
 * stream produce the same bytes). The override needs libcodec2 as a shared
 * library that calls codec2_rand() through the PLT: linking it statically
 * duplicates the symbol, and a library linked with -Bsymbolic silently binds
 * its own copy, which `vambed-neo --check-rng` (a build gate) detects. */
static unsigned long *rng_state;
static unsigned long rng_spare = 1;
static unsigned long rng_calls;

int codec2_rand(void);
int codec2_rand(void)
{
    unsigned long *s = rng_state != NULL ? rng_state : &rng_spare;
    *s = *s * 1103515245ul + 12345ul;
    rng_calls++;
    return (int)((*s / 65536ul) % 32768ul);
}

struct vn_codec {
    int codec_in;
    /* decoder side */
    mbe_state dec;
    struct CODEC2 *c2_dec;
    /* encoder side, one slot per codec id */
    mbe_state enc[VN_CODEC_MAX + 1];
    mbe_ambe2400_encoder *ambep_enc;
    mbe_ambe2450_encoder *ambe2p_enc;
    mbe_imbe4400_encoder *imbe_enc;
    struct CODEC2 *c2_enc;
    unsigned long c2_rng;
};

int vn_codec_width(int codec)
{
    switch (codec) {
    case VN_CODEC_AMBEP:
    case VN_CODEC_AMBE2P: return 9;
    case VN_CODEC_CODEC2: return 8;
    case VN_CODEC_IMBE:   return 11;
    case VN_CODEC_PCM:    return 320;
    default:              return 0;
    }
}

uint32_t vn_codec_mask(void)
{
    return (1u << VN_CODEC_AMBEP) | (1u << VN_CODEC_AMBE2P) | (1u << VN_CODEC_CODEC2) |
           (1u << VN_CODEC_IMBE) | (1u << VN_CODEC_PCM);
}

static int get_bit(const uint8_t *bytes, int n)
{
    return (bytes[n >> 3] >> (7 - (n & 7))) & 1;
}

static void put_bit(uint8_t *bytes, int n, int bit)
{
    if (bit)
        bytes[n >> 3] |= (uint8_t)(0x80 >> (n & 7));
}

static void dmr_unpack(const uint8_t bytes[9], char fr[4][24])
{
    memset(fr, 0, 4 * 24);
    for (int i = 0; i < 36; i++) {
        fr[dmr_w[i]][dmr_x[i]] = (char)get_bit(bytes, 2 * i);
        fr[dmr_y[i]][dmr_z[i]] = (char)get_bit(bytes, 2 * i + 1);
    }
}

static void dmr_pack(char fr[4][24], uint8_t bytes[9])
{
    memset(bytes, 0, 9);
    for (int i = 0; i < 36; i++) {
        put_bit(bytes, 2 * i, fr[dmr_w[i]][dmr_x[i]]);
        put_bit(bytes, 2 * i + 1, fr[dmr_y[i]][dmr_z[i]]);
    }
}

static void mbe_state_init(mbe_state *s)
{
    mbe_initMbeParms(&s->cur, &s->prev, &s->enh);
}

vn_codec *vn_codec_open(int codec_in, const uint8_t *outs, int nouts)
{
    vn_codec *c = calloc(1, sizeof(*c));
    int ok = 1;

    if (c == NULL)
        return NULL;
    c->codec_in = codec_in;
    c->c2_rng = 1;
    mbe_state_init(&c->dec);
    if (codec_in == VN_CODEC_CODEC2)
        ok &= (c->c2_dec = codec2_create(CODEC2_MODE_3200)) != NULL;

    for (int i = 0; i < nouts; i++) {
        int codec = outs[i];
        if (codec < 1 || codec > VN_CODEC_MAX)
            continue;
        mbe_state_init(&c->enc[codec]);
        if (codec == VN_CODEC_AMBEP && c->ambep_enc == NULL)
            ok &= (c->ambep_enc = mbe_ambe2400EncoderAlloc()) != NULL;
        else if (codec == VN_CODEC_AMBE2P && c->ambe2p_enc == NULL)
            ok &= (c->ambe2p_enc = mbe_ambe2450EncoderAlloc()) != NULL;
        else if (codec == VN_CODEC_IMBE && c->imbe_enc == NULL)
            ok &= (c->imbe_enc = mbe_imbe4400EncoderAlloc()) != NULL;
        else if (codec == VN_CODEC_CODEC2 && c->c2_enc == NULL)
            ok &= (c->c2_enc = codec2_create(CODEC2_MODE_3200)) != NULL;
    }
    if (!ok) {
        vn_codec_close(c);
        return NULL;
    }
    return c;
}

void vn_codec_close(vn_codec *c)
{
    if (c == NULL)
        return;
    if (c->c2_dec != NULL)
        codec2_destroy(c->c2_dec);
    if (c->c2_enc != NULL)
        codec2_destroy(c->c2_enc);
    mbe_ambe2400EncoderFree(c->ambep_enc);
    mbe_ambe2450EncoderFree(c->ambe2p_enc);
    mbe_imbe4400EncoderFree(c->imbe_enc);
    free(c);
}

void vn_codec_decode(vn_codec *c, const uint8_t *frame, int16_t pcm[VN_PCM_SAMPLES])
{
    char fr[4][24];
    char d[88];
    int rc = 0;

    switch (c->codec_in) {
    case VN_CODEC_AMBEP:
        rc = mbe_decodeDStarDVData(frame, fr);
        if (rc >= 0)
            rc = mbe_processAmbe3600x2400Frame(pcm, NULL, (const char (*)[24])fr, d,
                                               &c->dec.cur, &c->dec.prev, &c->dec.enh);
        break;
    case VN_CODEC_AMBE2P:
        dmr_unpack(frame, fr);
        rc = mbe_processAmbe3600x2450Frame(pcm, NULL, (const char (*)[24])fr, d,
                                           &c->dec.cur, &c->dec.prev, &c->dec.enh);
        break;
    case VN_CODEC_IMBE:
        for (int i = 0; i < 88; i++)
            d[i] = (char)get_bit(frame, i);
        rc = mbe_processImbe4400Data(pcm, NULL, d, &c->dec.cur, &c->dec.prev, &c->dec.enh);
        break;
    case VN_CODEC_CODEC2:
        rng_state = &c->c2_rng;
        codec2_decode(c->c2_dec, pcm, frame);
        rng_state = NULL;
        break;
    case VN_CODEC_PCM:
        for (int i = 0; i < VN_PCM_SAMPLES; i++)
            pcm[i] = (int16_t)(uint16_t)(frame[2 * i] | (frame[2 * i + 1] << 8));
        break;
    default:
        rc = -1;
        break;
    }
    if (rc < 0)
        memset(pcm, 0, VN_PCM_SAMPLES * sizeof(pcm[0]));
}

void vn_codec_encode(vn_codec *c, int codec, const int16_t pcm[VN_PCM_SAMPLES], uint8_t *frame)
{
    char fr[4][24];
    char d[88];
    mbe_state *s = (codec >= 1 && codec <= VN_CODEC_MAX) ? &c->enc[codec] : NULL;
    int rc = 0;

    memset(frame, 0, (size_t)vn_codec_width(codec));
    switch (codec) {
    case VN_CODEC_AMBEP:
        rc = mbe_encodeAmbe2400ParmsShort(c->ambep_enc, pcm, d, &s->cur, &s->prev);
        if (rc >= 0)
            rc = mbe_encodeAmbe3600x2400Frame(d, fr);
        if (rc >= 0)
            rc = mbe_encodeDStarDVData((const char (*)[24])fr, frame);
        break;
    case VN_CODEC_AMBE2P:
        rc = mbe_encodeAmbe2450ParmsShort(c->ambe2p_enc, pcm, d, &s->cur, &s->prev);
        if (rc >= 0)
            rc = mbe_encodeAmbe3600x2450Frame(d, fr);
        if (rc >= 0)
            dmr_pack(fr, frame);
        break;
    case VN_CODEC_IMBE:
        rc = mbe_encodeImbe4400ParmsShort(c->imbe_enc, pcm, d, &s->cur, &s->prev);
        if (rc >= 0)
            for (int i = 0; i < 88; i++)
                put_bit(frame, i, d[i]);
        break;
    case VN_CODEC_CODEC2:
        rng_state = &c->c2_rng;
        codec2_encode(c->c2_enc, frame, (short *)pcm);
        rng_state = NULL;
        break;
    case VN_CODEC_PCM:
        for (int i = 0; i < VN_PCM_SAMPLES; i++) {
            frame[2 * i] = (uint8_t)((uint16_t)pcm[i] & 0xFF);
            frame[2 * i + 1] = (uint8_t)((uint16_t)pcm[i] >> 8);
        }
        break;
    default:
        break;
    }
    if (rc < 0)
        memset(frame, 0, (size_t)vn_codec_width(codec));
    else if (s != NULL && codec != VN_CODEC_CODEC2 && codec != VN_CODEC_PCM)
        mbe_moveMbeParms(&s->cur, &s->prev);
}

int vn_codec_rng_per_stream(void)
{
    struct CODEC2 *enc = codec2_create(CODEC2_MODE_3200);
    struct CODEC2 *dec = codec2_create(CODEC2_MODE_3200);
    unsigned long before = rng_calls, lcg = 7;
    short pcm[VN_PCM_SAMPLES];
    unsigned char bits[8];

    /* noise is coded unvoiced, whose synthesis draws random phases */
    for (int f = 0; f < 4 && enc != NULL && dec != NULL; f++) {
        for (int i = 0; i < VN_PCM_SAMPLES; i++) {
            lcg = lcg * 1103515245ul + 12345ul;
            pcm[i] = (short)((int)((lcg >> 16) & 0x3FFF) - 0x2000);
        }
        codec2_encode(enc, bits, pcm);
        codec2_decode(dec, pcm, bits);
    }
    if (enc != NULL)
        codec2_destroy(enc);
    if (dec != NULL)
        codec2_destroy(dec);
    return rng_calls != before;
}
