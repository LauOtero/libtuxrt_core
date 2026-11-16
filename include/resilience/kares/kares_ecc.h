/**
 * kares_ecc.h — ECC BCH(1024,996) para KARES
 *
 * Código BCH que corrige hasta 3 bits de error en 1024 bits.
 * Usado para proteger checkpoints contra corrupción de datos.
 *
 * © 2026 TuxedoRT-Mojo Project. Licencia: Apache-2.0
 */

#ifndef TUXRT_KARES_ECC_H
#define TUXRT_KARES_ECC_H

#include "../../sdk/include/tuxrt_sdk.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Tamaño de datos para ECC (996 bits ≈ 124 bytes)
 */
#define KARES_ECC_DATA_SIZE 124
#define KARES_ECC_PARITY_SIZE 4  /* 28 bits de paridad en 4 bytes */

/**
 * Codificar datos con ECC BCH(1024,996)
 *
 * @param data Datos a proteger (124 bytes)
 * @param data_len Longitud de datos (debe ser 124)
 * @param ecc_out Paridad calculada (4 bytes)
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_ecc_encode(
    const uint8_t* data,
    size_t data_len,
    uint8_t* ecc_out
);

/**
 * Decodificar y detectar errores usando ECC
 *
 * @param data Datos recibidos (124 bytes)
 * @param data_len Longitud de datos (debe ser 124)
 * @param ecc ECC recibido (4 bytes)
 * @param out_error_bits Salida: número de bits con error
 * @param out_correctable Salida: true si es correctable
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_kares_ecc_decode(
    const uint8_t* data,
    size_t data_len,
    const uint8_t* ecc,
    uint32_t* out_error_bits,
    bool* out_correctable
);

/**
 * Corregir errores detectados usando ECC
 *
 * @param data Datos recibidos (se modifican in-place)
 * @param data_len Longitud de datos (debe ser 124)
 * @param ecc ECC recibido (4 bytes)
 * @return TUXRT_OK o error si no es correctable
 */
tuxrt_err_t tuxrt_kares_ecc_correct(
    uint8_t* data,
    size_t data_len,
    const uint8_t* ecc
);

/**
 * Obtener tamaño de paridad ECC
 *
 * @return Tamaño en bytes
 */
size_t tuxrt_kares_ecc_get_parity_size(void);

/**
 * Obtener tamaño de datos para ECC
 *
 * @return Tamaño en bytes
 */
size_t tuxrt_kares_ecc_get_data_size(void);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_KARES_ECC_H */
