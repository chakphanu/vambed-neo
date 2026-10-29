/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * vambed-neo codec layer: one frame of any supported codec to 8 kHz PCM and
 * back, with per-stream decoder and encoder state.
 */
#ifndef VN_CODEC_H
#define VN_CODEC_H

#include <stdint.h>

#define VN_CODEC_NONE   0
#define VN_CODEC_AMBEP  1   /* AMBE+ 3600x2400, D-STAR, 9 B */
#define VN_CODEC_AMBE2P 2   /* AMBE+2 3600x2450, DMR/YSF, 9 B */
#define VN_CODEC_CODEC2 3   /* Codec2 3200, M17, 8 B */
#define VN_CODEC_IMBE   4   /* IMBE 4400 voice bits, P25, 11 B */
#define VN_CODEC_PCM    5   /* S16LE, 160 samples, 320 B */
#define VN_CODEC_MAX    5

#define VN_PCM_SAMPLES  160
#define VN_FRAME_MAX    320

typedef struct vn_codec vn_codec;

/* Frame bytes of a codec, 0 for none or unknown. */
int vn_codec_width(int codec);

/* Codecs this build serves, CAPS mask layout (bit n = codec n). */
uint32_t vn_codec_mask(void);

/* 1 when codec2 uses the per-stream generator of this module (the library
 * resolves codec2_rand() through the PLT), 0 when it keeps its own. */
int vn_codec_rng_per_stream(void);

/* State for one stream: decoder of codec_in, encoders of the listed outputs. */
vn_codec *vn_codec_open(int codec_in, const uint8_t *outs, int nouts);
void vn_codec_close(vn_codec *c);

/* Decode one input frame. Bad frames give silence. */
void vn_codec_decode(vn_codec *c, const uint8_t *frame, int16_t pcm[VN_PCM_SAMPLES]);

/* Encode one frame of PCM to codec (must be one of the opened outputs). */
void vn_codec_encode(vn_codec *c, int codec, const int16_t pcm[VN_PCM_SAMPLES], uint8_t *frame);

#endif
