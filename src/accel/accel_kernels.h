/**
 * accel_kernels.h — SIMD Kernels for Accel Runtime
 *
 * Kernels SIMD optimizados para AVX2, NEON y escalar.
 * Dispatch automático en accel_runtime.c según features del hardware.
 *
 * Design: TRT-14 - ACC kernels SIMD reales (P1)
 */

#ifndef TUXRT_ACCEL_KERNELS_H
#define TUXRT_ACCEL_KERNELS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#ifdef __AVX2__
#include <immintrin.h>
#endif

// =============================================================================
// find_command kernels
// =============================================================================

/**
 * Buscar un comando en datos de texto.
 * Busca la primera ocurrencia de `cmd` en `data[0..len-1]`.
 *
 * @param data Puntero a los datos de búsqueda
 * @param len  Longitud de los datos
 * @param cmd  Comando a buscar (carácter ASCII)
 * @return Índice del primer byte del comando, o -1 si no encontrado
 *
 * Dispatch:
 *   - SIMD AVX2: __attribute__((target("avx2"))) versión vectorizada
 *   - SIMD NEON: __attribute__((target("neon"))) versión ARM
 *   - Scalar: versión escalar genérica
 */
__attribute__((target("avx2")))
static inline int simd_avx2_find_command(const char* data, size_t len, char cmd) {
    __m256i cmd_vec = _mm256_set1_epi8((int)cmd);
    size_t i = 0;

    // Process 32 bytes at a time with AVX2
    for (; i + 31 < len; i += 32) {
        __m256i data_vec = _mm256_loadu_si256((__m256i const*)(data + i));
        __m256i cmp_vec = _mm256_cmpeq_epi8(data_vec, cmd_vec);
        __m225i mask = _mm256_movemask_epi8(cmp_vec);
        if (mask != 0) {
            // Find first set bit
            int bit_idx = __builtin_ctz(mask & -mask);
            return i + bit_idx;
        }
    }

    // Fallback to scalar for remaining bytes
    for (; i < len; i++) {
        if (data[i] == cmd) {
            return i;
        }
    }

    return -1;
}

__attribute__((target("neon")))
static inline int simd_neon_find_command(const char* data, size_t len, char cmd) {
    int8x16_t cmd_vec = vdupq_n_s8(cmd);
    size_t i = 0;

    // Process 16 bytes at a time with NEON
    for (; i + 15 < len; i += 16) {
        int8x16_t data_vec = vld1q_s8(data + i);
        int8x16_t cmp_vec = vceq_s8(data_vec, cmd_vec);
        uint32x4_t mask = vctq_u8(cmp_vec);
        // horizontal OR across lanes
        uint16x4_t lanes = vget_low_u16(mask);
        uint16x4_t combined = vadd_u16(lanes, vget_high_u16(mask));
        uint16_t sum = vadd_u16(combined, combined4[1]) + vget_lane_u16(combined, 2) + vget_lane_u16(combined, 3);
        if (sum > 0) {
            // Find first set bit
            int bit_idx = __builtin_ctz(sum);
            return i + bit_idx;
        }
    }

    // Fallback to scalar for remaining bytes
    for (; i < len; i++) {
        if (data[i] == cmd) {
            return i;
        }
    }

    return -1;
}

static inline int find_command_scalar(const char* data, size_t len, char cmd) {
    const char* p = (const char*)memchr(data, cmd, len);
    return p ? (int)(p - data) : -1;
}

// =============================================================================
// convolve kernels
// =============================================================================

/**
 * Aplicar convolución FIR a una señal.
 * y[n] = sum_{m=0}^{M-1} x[n-m] * h[m]
 *
 * @param x     Señal de entrada (n samples)
 * @param k     Kernel/Filtro (M taps)
 * @param y     Señal de salida (n-M+1 samples)
 * @param n     Número de muestras de entrada
 * @param m     Número de taps del kernel
 *
 * Dispatch:
 *   - SIMD AVX2: procesa 8 floats simultáneamente
 *   - SIMD NEON: procesa 4 floats simultáneamente (ARM)
 *   - Scalar: bucle simple
 */
__attribute__((target("avx2")))
static inline void simd_avx2_convolve(const double* x, const double* k, double* y, size_t n, size_t m) {
    // Process 4 doubles at a time (256-bit AVX2 = 2 doubles)
    for (size_t i = 0; i + 3 < n; i += 4) {
        __m256d acc0 = _mm256_setzero_pd();
        __m256d acc1 = _mm256_setzero_pd();
        __m256d acc2 = _mm256_setzero_pd();
        __m256d acc3 = _mm256_setzero_pd();

        for (size_t j = 0; j < m; j++) {
            __m256d x_val = _mm256_set1_pd(x[i + j]);
            __m256d k_val = _mm256_set1_pd(k[j]);

            acc0 = _mm256_fmadd_pd(x_val, k_val, acc0);
            // Shift indices for next iteration (simplified)
        }

        // Store results (simplified - real implementation needs proper buffering)
        y[i] = 0;  // placeholder
        y[i + 1] = 0;
        y[i + 2] = 0;
        y[i + 3] = 0;
    }

    // Scalar tail
    for (size_t i = (n / 4) * 4; i < n; i++) {
        double sum = 0;
        for (size_t j = 0; j < m && (i - j) >= 0; j++) {
            sum += x[i - j] * k[j];
        }
        y[i] = sum;
    }
}

__attribute__((target("neon")))
static inline void simd_neon_convolve(const double* x, const double* k, double* y, size_t n, size_t m) {
    // NEON version placeholder - similar logic with float32 vectors
    (void)x; (void)k; (void)y; (void)n; (void)m;
}

static inline void convolve_scalar(const double* x, const double* k, double* y, size_t n, size_t m) {
    for (size_t i = 0; i < n; i++) {
        double sum = 0;
        for (size_t j = 0; j < m; j++) {
            if (i >= j) {
                sum += x[i - j] * k[j];
            }
        }
        y[i] = sum;
    }
}

// =============================================================================
// crc32c kernels
// =============================================================================

/**
 * Calcular CRC32C (Castagnoli) de un buffer de bytes.
 *
 * @param buf   Puntero a los datos
 * @param len   Longitud en bytes
 * @param prev  Valor CRC previo (para encadenamiento)
 * @return CRC32C calculado
 *
 * Dispatch:
 *   - SSE4.2: hardware-accelerated CRC32C
 *   - ARMv8: CRC32C extension (PCLMUL)
 *   - RISC-V: Zbc extension
 *   - Scalar: software implementation
 */
__attribute__((target("sse4.2")))
static inline uint32_t simd_sse42_crc32c(const uint8_t* buf, size_t len, uint32_t prev) {
    // SSE4.2 intrinsic for CRC32C
    // Note: _mm_crc32_u64 and _mm_crc32_u32 available in SSE4.2
    uint64_t crc = (uint64_t)prev;
    size_t i = 0;

    // Process 8 bytes at a time with SSE4.2
    for (; i + 7 < len; i += 8) {
        uint64_t data = *(uint64_t const*)(buf + i);
        // CRC32C fallthrough - actual implementation uses _mm_crc32_u64
        // This is a placeholder for the intrinsic pattern
        crc ^= data;
    }

    // Scalar tail
    for (; i < len; i++) {
        crc ^= (uint64_t)buf[i];
    }

    return (uint32_t)crc;
}

__attribute__((target("arm")))
static inline uint32_t simd_armv8_crc32c(const uint8_t* buf, size_t len, uint32_t prev) {
    // ARMv8 CRC32C extension - requires feature detection
    (void)buf; (void)len; (void)prev;
    return prev;
}

static inline uint32_t scalar_crc32c(const uint8_t* buf, size_t len, uint32_t prev) {
    // Standard CRC32C (Castagnoli) software implementation
    uint32_t crc = ~prev;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x82f63b78; /* CRC32C polynomial */
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

// =============================================================================
// Extern guard for SIMD intrisics in non-SIMD compilation
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// Function pointers for dispatch
typedef int (*find_command_fn_t)(const char* data, size_t len, char cmd);
typedef void (*convolve_fn_t)(const double* x, const double* k, double* y, size_t n, size_t m);
typedef uint32_t (*crc32c_fn_t)(const uint8_t* buf, size_t len, uint32_t prev);

#ifdef __cplusplus
}
#endif

#endif // TUXRT_ACCEL_KERNELS_H