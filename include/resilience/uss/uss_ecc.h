/**
 * uss_ecc.h — ECC BCH(32768,32640) para USS Checkpoint
 *
 * Código BCH que corrige hasta 16 bits de error en 32768 bits (4 KB).
 * Usado para proteger Universal State Snapshot contra corrupción de disco.
 *
 * Especificación:
 *   - Bloque: 32768 bits (4096 bytes)
 *   - Datos: 32640 bits (4080 bytes)
 *   - Paridad: 128 bits (16 bytes)
 *   - Capacidad correctora: hasta 16 errores de bit
 *   - Campo de Galois: GF(2^13) con polinomio primitivo p(x) = x^13 + x^4 + x^3 + x + 1
 *
 * © 2026 TuxedoRT-Mojo Project. Licencia: Apache-2.0
 */

#ifndef TUXRT_USS_ECC_H
#define TUXRT_USS_ECC_H

#include "../../sdk/include/tuxrt_sdk.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Tamaño de datos para ECC BCH(32768,32640)
 * 32640 bits = 4080 bytes de datos útiles
 */
#define USS_ECC_DATA_SIZE 4080

/**
 * Tamaño de paridad ECC
 * 128 bits = 16 bytes de paridad
 */
#define USS_ECC_PARITY_SIZE 16

/**
 * Tamaño total del bloque codificado
 * 32768 bits = 4096 bytes
 */
#define USS_ECC_BLOCK_SIZE 4096

/**
 * Capacidad correctora del código BCH
 * Hasta 16 errores de bit por bloque
 */
#define USS_ECC_CORRECTABLE_BITS 16

/**
 * Polinomio generador BCH en GF(2^13)
 * Grado 128 (16 bytes)
 */
#define USS_ECC_GEN_POLY_DEGREE 128

/**
 * Contexto para operaciones ECC
 * Contiene tablas precalculadas para acelerar codificación/decodificación
 */
typedef struct uss_ecc_ctx {
    uint16_t gf13_log[8192];    /* Tabla log para GF(2^13) */
    uint16_t gf13_exp[16384];   /* Tabla exp extendida (2 × orden 8191) */
    uint8_t gen_poly[USS_ECC_PARITY_SIZE]; /* Polinomio generador */
    bool initialized;
} uss_ecc_ctx_t;

/**
 * Inicializar contexto ECC BCH
 * Precalcula tablas de Galois Field y polinomio generador
 *
 * @param ctx Contexto a inicializar
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_uss_ecc_init(uss_ecc_ctx_t* ctx);

/**
 * Liberar contexto ECC
 *
 * @param ctx Contexto a liberar
 */
void tuxrt_uss_ecc_destroy(uss_ecc_ctx_t* ctx);

/**
 * Codificar datos con ECC BCH(32768,32640)
 *
 * @param ctx Contexto ECC inicializado
 * @param data Datos a proteger (4080 bytes)
 * @param data_len Longitud de datos (debe ser 4080)
 * @param ecc_out Paridad calculada (16 bytes)
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_uss_ecc_encode(
    uss_ecc_ctx_t* ctx,
    const uint8_t* data,
    size_t data_len,
    uint8_t* ecc_out
);

/**
 * Calcular síndrome para detección de errores
 *
 * @param ctx Contexto ECC
 * @param data Datos recibidos (4080 bytes)
 * @param data_len Longitud de datos
 * @param ecc ECC recibido (16 bytes)
 * @param syndrome Síndrome calculado (16 bytes)
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_uss_ecc_syndrome(
    uss_ecc_ctx_t* ctx,
    const uint8_t* data,
    size_t data_len,
    const uint8_t* ecc,
    uint8_t* syndrome
);

/**
 * Decodificar y detectar errores usando ECC
 *
 * @param ctx Contexto ECC
 * @param data Datos recibidos (4080 bytes)
 * @param data_len Longitud de datos
 * @param ecc ECC recibido (16 bytes)
 * @param out_error_bits Salida: número de bits con error detectados
 * @param out_correctable Salida: true si es correctable (≤16 bits)
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_uss_ecc_decode(
    uss_ecc_ctx_t* ctx,
    const uint8_t* data,
    size_t data_len,
    const uint8_t* ecc,
    uint32_t* out_error_bits,
    bool* out_correctable
);

/**
 * Corregir errores detectados usando ECC
 *
 * @param ctx Contexto ECC
 * @param data Datos recibidos (se modifican in-place, 4080 bytes)
 * @param data_len Longitud de datos
 * @param ecc ECC recibido (16 bytes)
 * @param out_bits_corrected Salida: número de bits corregidos
 * @return TUXRT_OK si se corrigió, TUXRT_ERR_ECC_UNCORRECTABLE si no es correctable
 */
tuxrt_err_t tuxrt_uss_ecc_correct(
    uss_ecc_ctx_t* ctx,
    uint8_t* data,
    size_t data_len,
    const uint8_t* ecc,
    uint32_t* out_bits_corrected
);

/**
 * Verificar integridad de bloque USS
 * Combina CRC32 y ECC para validación completa
 *
 * @param ctx Contexto ECC
 * @param block Bloque completo (4096 bytes: 4080 datos + 16 ECC)
 * @param block_len Longitud del bloque (debe ser 4096)
 * @param out_valid_crc Salida: true si CRC32 es válido
 * @param out_valid_ecc Salida: true si ECC es válido
 * @return TUXRT_OK o error
 */
tuxrt_err_t tuxrt_uss_ecc_verify_block(
    uss_ecc_ctx_t* ctx,
    const uint8_t* block,
    size_t block_len,
    bool* out_valid_crc,
    bool* out_valid_ecc
);

/**
 * Obtener tamaño de paridad ECC
 *
 * @return Tamaño en bytes (16)
 */
size_t tuxrt_uss_ecc_get_parity_size(void);

/**
 * Obtener tamaño de datos para ECC
 *
 * @return Tamaño en bytes (4080)
 */
size_t tuxrt_uss_ecc_get_data_size(void);

/**
 * Obtener tamaño de bloque completo
 *
 * @return Tamaño en bytes (4096)
 */
size_t tuxrt_uss_ecc_get_block_size(void);

/**
 * Obtener capacidad correctora
 *
 * @return Número de bits corregibles (16)
 */
uint32_t tuxrt_uss_ecc_get_correctable_bits(void);

#ifdef __cplusplus
}
#endif

#endif /* TUXRT_USS_ECC_H */
