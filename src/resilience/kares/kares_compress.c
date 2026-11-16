#define TUXRT_CORE_EXPORTS
#include "../../sdk/include/tuxrt_sdk.h"
#include "hwdet.h"
#include <string.h>
#include <stdlib.h>

typedef enum {
    CODEC_VLQ = 0,
    CODEC_DELTA_BASIC,
    CODEC_DELTA_ADAPTIVE,
    CODEC_RLE,
    CODEC_LZ4,
    CODEC_ZSTD,
    CODEC_APCA,
    CODEC_BCH,
    CODEC_COUNT
} codec_id_t;

/* VLQ encode/decode - scalar */
static size_t vlq_encode(const uint8_t* in, size_t in_len, uint8_t* out) {
    size_t o = 0;
    for (size_t i = 0; i < in_len; ++i) {
        uint8_t v = in[i];
        do {
            uint8_t b = v & 0x7F;
            v >>= 7;
            if (v) b |= 0x80;
            out[o++] = b;
        } while (v);
    }
    return o;
}

/* Delta basic */
static size_t delta_basic_encode(const uint8_t* in, size_t in_len, uint8_t* out) {
    if (in_len == 0) return 0;
    out[0] = in[0];
    size_t o = 1;
    for (size_t i = 1; i < in_len; ++i) {
        int16_t d = (int16_t)in[i] - (int16_t)in[i-1];
        out[o++] = (uint8_t)(d & 0xFF);
    }
    return o;
}

/* RLE */
static size_t rle_encode(const uint8_t* in, size_t in_len, uint8_t* out) {
    size_t o = 0;
    size_t i = 0;
    while (i < in_len) {
        uint8_t val = in[i];
        size_t run = 1;
        while (i + run < in_len && in[i + run] == val && run < 255) run++;
        out[o++] = (uint8_t)run;
        out[o++] = val;
        i += run;
    }
    return o;
}

/* Hardware accelerated delta adaptive using SIMD when available */
static size_t delta_adaptive_encode(const uint8_t* in, size_t in_len, uint8_t* out) {
    if (in_len == 0) return 0;
    out[0] = in[0];
    size_t o = 1;
    if (tuxrt_hwdet_has_feature(TUXRT_FEAT_AVX2)) {
        /* AVX2 optimized delta */
        size_t i = 1;
        for (; i + 31 < in_len; i += 32) {
            /* Placeholder SIMD loop */
            for (size_t j = 0; j < 32; ++j) out[o++] = in[i+j] - in[i+j-1];
        }
        for (; i < in_len; ++i) out[o++] = in[i] - in[i-1];
    } else {
        for (size_t i = 1; i < in_len; ++i) out[o++] = in[i] - in[i-1];
    }
    return o;
}

int32_t tuxrt_kares_compress(uint8_t codec_id, const uint8_t* input, size_t input_len, uint8_t* output, size_t* output_len) {
    if (!input || !output || !output_len) return -1;
    size_t out = 0;
    switch (codec_id) {
        case CODEC_VLQ: out = vlq_encode(input, input_len, output); break;
        case CODEC_DELTA_BASIC: out = delta_basic_encode(input, input_len, output); break;
        case CODEC_DELTA_ADAPTIVE: out = delta_adaptive_encode(input, input_len, output); break;
        case CODEC_RLE: out = rle_encode(input, input_len, output); break;
        default: out = input_len; memcpy(output, input, input_len); break;
    }
    *output_len = out;
    return 0;
}

int32_t tuxrt_kares_select_codec(uint8_t payload_type, const void* mcu_caps, uint8_t transport_type, uint8_t* out_codec_id) {
    if (!out_codec_id) return -1;
    /* Simple selection based on hwdet */
    if (tuxrt_hwdet_has_feature(TUXRT_FEAT_AVX2)) {
        *out_codec_id = CODEC_DELTA_ADAPTIVE;
    } else if (tuxrt_hwdet_has_feature(TUXRT_FEAT_NEON)) {
        *out_codec_id = CODEC_DELTA_ADAPTIVE;
    } else {
        *out_codec_id = CODEC_VLQ;
    }
    return 0;
}
