#include "accel_runtime.h"
#include "hwdet.h"
#include <string.h>
#include <stdint.h>

static tuxrt_accel_ops_t ops = {0};
static int initialized = 0;

/* ============================ Escalar (fallback) ========================== */

static int find_command_scalar(const char* data, size_t len, char cmd) {
    const char* p = memchr(data, cmd, len);
    return p ? (int)(p - data) : -1;
}

static size_t skip_whitespace_scalar(const char* data, size_t len) {
    size_t i = 0;
    while (i < len && (data[i] == ' ' || data[i] == '\t' || data[i] == '\n' || data[i] == '\r')) i++;
    return i;
}

/* ================================ x86-64 AVX2 =============================
 *
 * Las rutas SIMD se compilan con `__attribute__((target("avx2")))` y se
 * seleccionan SOLO tras comprobar `tuxrt_hwdet_has_feature(TUXRT_FEAT_AVX2)`,
 * de modo que el binario no emite instrucciones AVX2 fuera de esos puntos y
 * nunca provoca SIGILL en CPUs sin AVX2.
 * ========================================================================= */

#if defined(__x86_64__)
#  include <immintrin.h>

__attribute__((target("avx2")))
static int find_command_avx2(const char* data, size_t len, char cmd) {
    const __m256i needle = _mm256_set1_epi8(cmd);
    size_t i = 0;
    for (; i + 32 <= len; i += 32) {
        __m256i chunk = _mm256_loadu_si256((const __m256i*)(const void*)(data + i));
        unsigned mask = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, needle));
        if (mask != 0u) return (int)(i + (size_t)__builtin_ctz(mask));
    }
    for (; i < len; i++) {
        if (data[i] == cmd) return (int)i;
    }
    return -1;
}

__attribute__((target("avx2")))
static size_t skip_whitespace_avx2(const char* data, size_t len) {
    const __m256i vsp  = _mm256_set1_epi8(' ');
    const __m256i vtab = _mm256_set1_epi8('\t');
    const __m256i vnl  = _mm256_set1_epi8('\n');
    const __m256i vcr  = _mm256_set1_epi8('\r');
    size_t i = 0;
    for (; i + 32 <= len; i += 32) {
        __m256i chunk = _mm256_loadu_si256((const __m256i*)(const void*)(data + i));
        __m256i m = _mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(chunk, vsp), _mm256_cmpeq_epi8(chunk, vtab)),
            _mm256_or_si256(_mm256_cmpeq_epi8(chunk, vnl), _mm256_cmpeq_epi8(chunk, vcr)));
        unsigned nonws = ~(unsigned)_mm256_movemask_epi8(m);
        if (nonws != 0u) return i + (size_t)__builtin_ctz(nonws);
    }
    while (i < len && (data[i] == ' ' || data[i] == '\t' || data[i] == '\n' || data[i] == '\r')) i++;
    return i;
}
#endif /* __x86_64__ */

/* ================================= Dispatch =============================== */

void tuxrt_accel_runtime_init(void) {
    if (initialized) return;
    tuxrt_hwdet_init();

    ops.find_command = find_command_scalar;
    ops.skip_whitespace = skip_whitespace_scalar;

#if defined(__x86_64__)
    if (tuxrt_hwdet_has_feature(TUXRT_FEAT_AVX2)) {
        ops.find_command = find_command_avx2;
        ops.skip_whitespace = skip_whitespace_avx2;
    }
#endif

    initialized = 1;
}

const tuxrt_accel_ops_t* tuxrt_accel_runtime_get(void) {
    tuxrt_accel_runtime_init();
    return &ops;
}
