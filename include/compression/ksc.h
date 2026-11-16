/**
 * ksc.h — KSC (Klipper Stream Compressor)
 *
 * Compresor adaptativo por tipo de mensaje con 7 algoritmos.
 * Implementa la API especificada en documento13 §3.5.
 */
#ifndef TUXRT_KSC_H
#define TUXRT_KSC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ksc_ctx ksc_ctx_t;

ksc_ctx_t* tuxrt_ksc_create(void);
void tuxrt_ksc_destroy(ksc_ctx_t* ctx);

int tuxrt_ksc_encode(ksc_ctx_t* ctx, uint8_t msg_type,
                     const uint8_t* in, size_t in_len,
                     uint8_t* out, size_t out_max, size_t* out_len);

int tuxrt_ksc_decode(ksc_ctx_t* ctx, uint8_t msg_type,
                     const uint8_t* in, size_t in_len,
                     uint8_t* out, size_t out_max, size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_KSC_H */
