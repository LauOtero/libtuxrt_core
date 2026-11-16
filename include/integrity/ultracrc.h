/**
 * ultracrc.h — Ultra Fast CRC Dispatch with SIMD
 *
 * CRC dispatch con aceleración por hardware/SIMD para integridad de datos.
 *
 * API interna (protocolo):
 *   - CRC_I   : CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, xorout 0x0000)
 *   - CRC_II  : CRC-32/MPEG-2     (poly 0x04C11DB7, init 0xFFFFFFFF, xorout 0x00000000)
 *   - CRC_III : CRC-64/ECMA-182   (poly 0x42F0E1EBA9EA3693, init 0x0, xorout 0x0)
 *
 * En todas ellas `prev` es el estado CRC acumulado (valor de arranque en la
 * primera llamada; el resultado de una llamada previa para encadenar chunks).
 *
 * La API pública de integridad (CRC-32C hardware-accelerated) se declara en
 * `sdk/include/tuxrt_sdk.h` (`tuxrt_ultracrc32`).
 *
 * Design: TRT-24 - UCRC SIMD dispatch (P1)
 */

#ifndef ULTRACRC_H
#define ULTRACRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// CRC Algorithm Enumeration
// =============================================================================

typedef enum {
    CRC_I = 0,    // CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)
    CRC_II = 1,   // CRC-32/MPEG-2     (poly 0x04C11DB7, init 0xFFFFFFFF)
    CRC_III = 2,  // CRC-64/ECMA-182   (poly 0x42F0E1EBA9EA3693, init 0x0)
} tuxrc_crc_algo_e;

// Valores de arranque por algoritmo (para `prev` en la primera llamada).
#define ULTRACRC_INIT_I   0xFFFFu
#define ULTRACRC_INIT_II  0xFFFFFFFFu
#define ULTRACRC_INIT_III 0x0000000000000000ull

// =============================================================================
// CRC Functions
// =============================================================================

/**
 * Calcular CRC-I (CRC-16/CCITT-FALSE).
 *
 * @param buf   Buffer de datos
 * @param len   Longitud en bytes
 * @param prev  Valor previo para encadenamiento (ULTRACRC_INIT_I al iniciar)
 * @return CRC-16 result
 */
uint16_t ultracrc_crc_i(const uint8_t* buf, size_t len, uint16_t prev);

/**
 * Calcular CRC-II (CRC-32/MPEG-2).
 *
 * @param buf   Buffer de datos
 * @param len   Longitud en bytes
 * @param prev  Valor previo para encadenamiento (ULTRACRC_INIT_II al iniciar)
 * @return CRC-32/MPEG-2 result
 */
uint32_t ultracrc_crc_ii(const uint8_t* buf, size_t len, uint32_t prev);

/**
 * Calcular CRC-III (CRC-64/ECMA-182).
 *
 * @param buf   Buffer de datos
 * @param len   Longitud en bytes
 * @param prev  Valor previo para encadenamiento (ULTRACRC_INIT_III al iniciar)
 * @return CRC-64 result
 */
uint64_t ultracrc_crc_iii(const uint8_t* buf, size_t len, uint64_t prev);

/**
 * Calcular CRC genérico según algoritmo seleccionado.
 *
 * @param algo  Algoritmo de CRC (CRC_I, CRC_II, CRC_III)
 * @param buf   Buffer de datos
 * @param len   Longitud en bytes
 * @param prev  Valor previo para encadenamiento
 * @return CRC result del tamaño apropiado según el algoritmo
 *         (uint16_t para CRC_I, uint32_t para CRC_II, uint64_t para CRC_III)
 */
uint64_t ultracrc_compute(tuxrc_crc_algo_e algo, const uint8_t* buf, size_t len, uint64_t prev);

/**
 * CRC-32C incremental (Castagnoli) SIN inversión inicial/final.
 *
 * `prev` es el estado acumulado (0xFFFFFFFFu al iniciar; el resultado de una
 * llamada previa para encadenar bloques). Usa la instrucción CRC32C si está
 * disponible (SSE4.2 en x86-64, ARMv8 CRC en aarch64) y slice-by-8 si no.
 *
 * Es la primitiva que consume la HAL (`tuxrt_crc32c_raw`) para calcular el CRC
 * de las tramas EtherTux por partes (cabecera + carga útil).
 *
 * @param buf   Buffer de datos (puede ser NULL si len == 0)
 * @param len   Longitud en bytes
 * @param prev  Estado CRC previo
 * @return CRC-32C acumulado (sin xorout)
 */
uint32_t ultracrc_crc32c_raw(const void* buf, size_t len, uint32_t prev);

/**
 * Nombre del backend CRC-32C seleccionado en runtime.
 *
 * @return Cadena estática: "sse42", "armv8" o "sw".
 */
const char* ultracrc_backend_name_get(void);

#ifdef __cplusplus
}
#endif

#endif // ULTRACRC_H
