/**
 * jlpp.h — JLPP (Jerk-Limited Phase Packet)
 *
 * DPCM + Golomb-Rice para telemetría de vibración.
 */
#ifndef TUXRT_JLPP_H
#define TUXRT_JLPP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jlpp_ctx jlpp_ctx_t;

jlpp_ctx_t* tuxrt_jlpp_create(uint8_t rice_k);
void tuxrt_jlpp_destroy(jlpp_ctx_t* ctx);

int tuxrt_jlpp_encode(jlpp_ctx_t* ctx, const int16_t* samples, size_t n,
                      uint8_t* out, size_t out_max, size_t* out_len);

int tuxrt_jlpp_decode(jlpp_ctx_t* ctx, const uint8_t* in, size_t in_len,
                      int16_t* out, size_t out_max, size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_JLPP_H */
