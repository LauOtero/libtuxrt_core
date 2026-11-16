/**
 * hwdet.h — Hardware Detection (HWD)
 *
 * API para detección de features de hardware usando /proc/self/auxv
 * y other mechanisms. Permite el dispatch de kernels SIMD y
 * optimizaciones basadas en capacidades del hardware.
 *
 * Design: TRT-20 - HWD /proc/self/auxv (P1)
 */

#ifndef TUXRT_HWDET_H
#define TUXRT_HWDET_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Hardware Feature Flags
// =============================================================================

typedef enum {
    TUXRT_FEAT_AVX2 = 0x0001,
    TUXRT_FEAT_SSE42 = 0x0002,
    TUXRT_FEAT_NEON = 0x0004,
    TUXRT_FEAT_ARMV8_CRC = 0x0008,
    TUXRT_FEAT_FMA = 0x0010,
    TUXRT_FEAT_BMI2 = 0x0020,
    TUXRT_FEAT_RDRAND = 0x0040,
    TUXRT_FEAT_RDSEED = 0x0080,
    TUXRT_FEAT_PCLMUL = 0x0100,
    TUXRT_FEAT_SSE3 = 0x0200,
    TUXRT_FEAT_SSSE3 = 0x0400,
    TUXRT_FEAT_SSE41 = 0x0800,
    TUXRT_FEAT_SSE4A = 0x1000,
    TUXRT_FEAT_XOP = 0x2000,
    TUXRT_FEAT_F16C = 0x4000,
    TUXRT_FEAT_CMOV = 0x8000,
    TUXRT_FEAT_MMX = 0x10000,
    TUXRT_FEAT_3DNOW = 0x20000,
    TUXRT_FEAT_3DNOWE = 0x40000,
    TUXRT_FEAT_E3DNOW = 0x80000,
    TUXRT_FEAT_LZCNT = 0x100000,
    TUXRT_FEAT_POPCNT = 0x200000,
    TUXRT_FEAT_ABM = 0x400000,
    TUXRT_FEAT_TBM = 0x800000,
    TUXRT_FEAT_AES = 0x1000000,
    TUXRT_FEAT_SHA = 0x2000000,
    TUXRT_FEAT_AVX512F = 0x4000000,
    TUXRT_FEAT_AVX512BW = 0x8000000,
    TUXRT_FEAT_AVX512DQ = 0x10000000,
    TUXRT_FEAT_AVX512VL = 0x20000000,
    TUXRT_FEAT_AVX512CD = 0x40000000,
    TUXRT_FEAT_AVX512ER = 0x80000000,
    TUXRT_FEAT_AVX512PF = 0x100000000ULL,
    // Legacy / missing features used by detection code
    TUXRT_FEAT_FPU = 0x1000000000ULL,
    TUXRT_FEAT_TSC = 0x2000000000ULL,
    TUXRT_FEAT_SSE = 0x4000000000ULL,
    TUXRT_FEAT_SSE2 = 0x8000000000ULL,
    TUXRT_FEAT_TXT = 0x10000000000ULL,
    TUXRT_FEAT_AVX = 0x20000000000ULL,
} tuxrt_hw_feature_t;

// =============================================================================
// Initialization and Query
// =============================================================================

/**
 * Inicializar el detector de hardware.
 *
 * Lee /proc/self/auxv y other sources para detectar features.
 * Esta función es thread-safe y puede ser llamada múltiples veces.
 *
 * @return TUXRT_OK en éxito, error en caso contrario
 */
int tuxrt_hwdet_init(void);

/**
 * Verificar si un feature está disponible.
 *
 * @param feature Feature a verificar
 * @return true si el feature está disponible, false en caso contrario
 */
bool tuxrt_hwdet_has_feature(tuxrt_hw_feature_t feature);

/**
 * Obtener el número de CPUs físicas disponibles.
 *
 * @return Número de CPUs físicas
 */
int tuxrt_hwdet_get_cpu_count(void);

/**
 * Obtener el número de hilos lógicos disponibles.
 *
 * @return Número de hilos lógicos
 */
int tuxrt_hwdet_get_logical_count(void);

/**
 * Obtener la frecuencia del CPU en MHz.
 *
 * @return Frecuencia del CPU en MHz (0 si no disponible)
 */
uint32_t tuxrt_hwdet_get_cpu_freq_mhz(void);

/**
 * Verificar si el sistema está ejecutando en una máquina virtual.
 *
 * @return true si es una VM, false si es hardware físico
 */
bool tuxrt_hwdet_is_virtual(void);

/**
 * Obtener el fabricante del CPU.
 *
 * @param out_buf Buffer de salida para el nombre del fabricante
 * @param buf_len  Tamaño del buffer
 * @return TUXRT_OK en éxito, error en caso contrario
 */
int tuxrt_hwdet_get_manufacturer(char* out_buf, size_t buf_len);

/**
 * Obtener el modelo del CPU.
 *
 * @param out_buf Buffer de salida para el modelo del CPU
 * @param buf_len  Tamaño del buffer
 * @return TUXRT_OK en éxito, error en caso contrario
 */
int tuxrt_hwdet_get_model(char* out_buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif // TUXRT_HWDET_H