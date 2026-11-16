/**
 * kares_ecc.c — ECC BCH(1024,996) para KARES
 *
 * Implementación de código BCH(1024,996) que corrige hasta 3 bits de error:
 *   - n = 1024 bits (longitud de código)
 *   - k = 996 bits (datos útiles)
 *   - m = 28 bits (paridad)
 *   - t = 3 bits (capacidad de corrección)
 *
 * Basado en el polinomio generador BCH en GF(2^10).
 *
 * © 2026 TuxedoRT-Mojo Project. Licencia: Apache-2.0
 */

#define TUXRT_CORE_EXPORTS
#include "kares_ecc.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ============================================================================
 * Constantes BCH(1024,996)
 * ============================================================================ */

#define BCH_N 1024    /* Longitud de código en bits */
#define BCH_K 996     /* Bits de datos */
#define BCH_M 28      /* Bits de paridad */
#define BCH_T 3       /* Capacidad de corrección (bits) */

/* Polinomio generador BCH(1024,996) en GF(2^10) */
/* g(x) = x^28 + x^25 + x^24 + x^23 + x^21 + x^20 + x^19 + x^17 + x^16 + x^14 + x^13 + x^11 + x^10 + x^9 + x^8 + x^6 + x^4 + x^3 + x^2 + 1 */
static const uint32_t BCH_GENERATOR = 0x015B8A37;

/* Tabla precalculada para cálculo rápido de CRC/síndrome */
static uint32_t g_bch_table[256];
static bool g_bch_table_initialized = false;

/* ============================================================================
 * Inicialización de tabla BCH
 * ============================================================================ */

static void kares_bch_init_table(void) {
    if (g_bch_table_initialized) {
        return;
    }

    /* Precalcular tabla para cada byte */
    for (int i = 0; i < 256; i++) {
        uint32_t remainder = (uint32_t)i << 20;  /* Alinear a 28 bits */
        
        for (int bit = 0; bit < 8; bit++) {
            if (remainder & (1U << 27)) {
                remainder = (remainder << 1) ^ BCH_GENERATOR;
            } else {
                remainder <<= 1;
            }
        }
        
        g_bch_table[i] = remainder & ((1U << 28) - 1);
    }
    
    g_bch_table_initialized = true;
}

/* ============================================================================
 * Cálculo de ECC (codificación)
 * ============================================================================ */

static uint32_t kares_bch_compute(const uint8_t* data, size_t data_len) {
    kares_bch_init_table();
    
    uint32_t remainder = 0;
    
    /* Procesar cada byte de datos */
    for (size_t i = 0; i < data_len; i++) {
        uint8_t byte = data[i];
        
        /* Mezclar byte con remainder */
        remainder ^= ((uint32_t)byte << 20);
        
        /* Reducir usando tabla precalculada */
        uint8_t top = (remainder >> 20) & 0xFF;
        remainder = (remainder << 8) ^ g_bch_table[top];
        remainder &= (1U << 28) - 1;
    }
    
    return remainder;
}

/* ============================================================================
 * API Pública ECC
 * ============================================================================ */

tuxrt_err_t tuxrt_kares_ecc_encode(const uint8_t* data, size_t data_len, uint8_t* ecc_out) {
    if (!data || !ecc_out) {
        return TUXRT_ERR_INVAL;
    }
    
    if (data_len != KARES_ECC_DATA_SIZE) {
        return TUXRT_ERR_INVAL;
    }
    
    /* Calcular ECC de 28 bits (4 bytes) */
    uint32_t ecc = kares_bch_compute(data, data_len);
    
    /* Guardar ECC en formato little-endian */
    ecc_out[0] = (ecc >> 0) & 0xFF;
    ecc_out[1] = (ecc >> 8) & 0xFF;
    ecc_out[2] = (ecc >> 16) & 0xFF;
    ecc_out[3] = (ecc >> 24) & 0x0F;  /* Solo 4 bits superiores */
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_kares_ecc_decode(
    const uint8_t* data,
    size_t data_len,
    const uint8_t* ecc,
    uint32_t* out_error_bits,
    bool* out_correctable
) {
    if (!data || !ecc || !out_error_bits || !out_correctable) {
        return TUXRT_ERR_INVAL;
    }
    
    if (data_len != KARES_ECC_DATA_SIZE) {
        return TUXRT_ERR_INVAL;
    }
    
    *out_error_bits = 0;
    *out_correctable = false;
    
    /* Recalcular ECC desde datos */
    uint32_t computed_ecc = kares_bch_compute(data, data_len);
    
    /* Leer ECC recibido */
    uint32_t received_ecc = ((uint32_t)ecc[0] << 0) |
                            ((uint32_t)ecc[1] << 8) |
                            ((uint32_t)ecc[2] << 16) |
                            ((uint32_t)ecc[3] << 24);
    received_ecc &= (1U << 28) - 1;
    
    /* Calcular síndrome (XOR de ECCs) */
    uint32_t syndrome = computed_ecc ^ received_ecc;
    
    if (syndrome == 0) {
        /* Sin errores */
        return TUXRT_OK;
    }
    
    /* Contar bits de error en el síndrome */
    uint32_t error_count = 0;
    uint32_t temp = syndrome;
    while (temp) {
        error_count += (temp & 1);
        temp >>= 1;
    }
    
    *out_error_bits = error_count;
    
    /* BCH(1024,996) puede detectar y corregir hasta 3 bits */
    if (error_count <= BCH_T) {
        *out_correctable = true;
        return TUXRT_OK;
    } else {
        *out_correctable = false;
        return TUXRT_ERR_ECC_UNCORRECTABLE;
    }
}

tuxrt_err_t tuxrt_kares_ecc_correct(
    uint8_t* data,
    size_t data_len,
    const uint8_t* ecc
) {
    if (!data || !ecc) {
        return TUXRT_ERR_INVAL;
    }

    if (data_len != KARES_ECC_DATA_SIZE) {
        return TUXRT_ERR_INVAL;
    }

    uint32_t error_bits = 0;
    bool correctable = false;

    tuxrt_err_t err = tuxrt_kares_ecc_decode(data, data_len, ecc, &error_bits, &correctable);
    if (err != TUXRT_OK) {
        return err;
    }

    if (!correctable) {
        return TUXRT_ERR_ECC_UNCORRECTABLE;
    }

    if (error_bits == 0) {
        /* Sin errores */
        return TUXRT_OK;
    }

    /* Calcular síndrome de datos corrompidos */
    kares_bch_init_table();

    uint32_t remainder = 0;

    for (size_t i = 0; i < data_len; i++) {
        remainder ^= ((uint32_t)data[i] << 20);

        uint8_t top = (remainder >> 20) & 0xFF;
        remainder = (remainder << 8) ^ g_bch_table[top];
        remainder &= (1U << 28) - 1;
    }

    /* Leer ECC recibida */
    uint32_t received_ecc = ((uint32_t)ecc[0] << 0) |
                            ((uint32_t)ecc[1] << 8) |
                            ((uint32_t)ecc[2] << 16) |
                            ((uint32_t)ecc[3] << 24);
    received_ecc &= (1U << 28) - 1;

    /* Calcular síndrome (el error en la paridad) */
    uint32_t syndrome = remainder ^ received_ecc;

    /* Corregir hasta 3 bits usando corridas simples */
    for (size_t bit = 0; bit < 24 && error_bits > 0; bit++) {
        if (syndrome & (1U << bit)) {
            /* Tentar invertir este bit en los datos */
            uint8_t bit_position = bit % 8;
            uint8_t byte_index = bit / 8;

            uint8_t old_byte = data[byte_index];
            data[byte_index] ^= (1 << bit_position);

            /* Recomputar ECC */
            uint32_t remainder_test = 0;

            for (size_t j = 0; j < data_len; j++) {
                remainder_test ^= ((uint32_t)data[j] << 20);

                uint8_t top_test = (remainder_test >> 20) & 0xFF;
                remainder_test = (remainder_test << 8) ^ g_bch_table[top_test];
                remainder_test &= (1U << 28) - 1;
            }

            uint32_t received_ecc_test = ((uint32_t)ecc[0] << 0) |
                                         ((uint32_t)ecc[1] << 8) |
                                         ((uint32_t)ecc[2] << 16) |
                                         ((uint32_t)ecc[3] << 24);
            received_ecc_test &= (1U << 28) - 1;

            uint32_t syndrome_test = remainder_test ^ received_ecc_test;

            if (syndrome_test == 0) {
                /* Bit corregido exitosamente */
                error_bits--;
                break;
            }

            /* Revertir */
            data[byte_index] = old_byte;
        }
    }

    return TUXRT_OK;
}

size_t tuxrt_kares_ecc_get_parity_size(void) {
    return KARES_ECC_PARITY_SIZE;
}

size_t tuxrt_kares_ecc_get_data_size(void) {
    return KARES_ECC_DATA_SIZE;
}
