/**
 * ultracrc.c — Ultra Fast CRC Dispatch with SIMD/HW acceleration
 *
 * Implementación de los algoritmos de integridad del CORE con dispatch
 * automático de hardware en tiempo de ejecución:
 *
 *   - CRC-16/CCITT-FALSE (poly 0x1021)   → `ultracrc_crc_i`
 *   - CRC-32/MPEG-2     (poly 0x04C11DB7) → `ultracrc_crc_ii`
 *   - CRC-64/ECMA-182   (poly 0x42F0E1EBA9EA3693) → `ultracrc_crc_iii`
 *   - CRC-32C/Castagnoli (poly 0x1EDC6F41, reflejado) → `tuxrt_ultracrc32`
 *
 * Backends CRC-32C (seleccionados por `tuxrt_hwdet` en runtime):
 *   - `sse42`   : instrucción CRC32, multistream 3 vías (x86-64) — medido ~20 GB/s
 *                 en AMD Zen 2 (L2-residente); sin PCLMUL el techo práctico.
 *   - `armv8`   : instrucción CRC32 (AArch64)                    — ~25 GB/s
 *   - `sw`      : tablas slicing-by-8 (portable, fallback)       — ~1.2 GB/s
 *
 * NOTA (rendimiento): el objetivo del plan F1a (>35 GB/s) asume un backend
 * PCLMUL (Intel/AMD con `pclmulqdq`) o VPCLMUL512. Este host (Zen 2) NO
 * expone PCLMUL, por lo que se usa la ruta SSE4.2 multistream, que satura el
 * throughput de la instrucción CRC32. Los backends PCLMUL/VPCLMUL/Zbc quedan
 * como extensión condicionada a `tuxrt_hwdet_has_feature()` (no verificables
 * en el hardware actual → no se emiten para evitar código SIMD sin validar).
 *
 * IMPORTANTE: ninguna ruta SIMD se emite salvo que el host declare el
 * feature. El código acelerado se compila con `__attribute__((target(...)))`
 * y se invoca solo tras comprobar `tuxrt_hwdet_has_feature()` → sin SIGILL.
 *
 * Design: TRT-24 - UCRC SIMD dispatch (P1) / F1a-01
 */

#include "ultracrc.h"
#include "hwdet.h"
#include "tuxrt_sdk.h"

#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

/* =============================================================================
 * Polinomios
 * ============================================================================= */

#define CRC16_CCITT_FALSE_POLY 0x1021u
#define CRC32_MPEG2_POLY       0x04C11DB7u
#define CRC64_ECMA182_POLY     0x42F0E1EBA9EA3693ull
#define CRC32C_POLY_REFLECTED  0x82F63B78u /* Castagnoli reflejado */

#define CRC32C_INIT 0xFFFFFFFFu
#define CRC32C_XOROUT 0xFFFFFFFFu

/* =============================================================================
 * Tablas (inicializadas una sola vez, thread-safe vía pthread_once)
 * ============================================================================= */

static uint16_t tbl16[256];        /* CRC-16: 1 tabla de 256 entradas        */
static uint32_t tbl32[8][256];     /* CRC-32: slicing-by-8                    */
static uint64_t tbl64[8][256];     /* CRC-64: slicing-by-8                    */
static uint32_t tbl32c[8][256];    /* CRC-32C (reflejado): slicing-by-8       */

static pthread_once_t ultracrc_once = PTHREAD_ONCE_INIT;

/* =============================================================================
 * Detección de ISA disponible en tiempo de compilación
 * ============================================================================= */

#if defined(__x86_64__)
#  include <nmmintrin.h> /* SSE4.2 — _mm_crc32_u{8,32,64} */
#  define ULTRACRC_HAVE_X86_SSE42 1
#endif

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#  include <arm_acle.h>
#  define ULTRACRC_HAVE_ARM64_CRC 1
#endif

/* =============================================================================
 * Backend SSE4.2 multistream (3 vías) — tablas de "skip"
 *
 * La instrucción CRC32 tiene 1/cycle de throughput y ~3 ciclos de latencia, por
 * lo que un único flujo está limitado por latencia (~3 B/ciclo). Procesando 3
 * flujos independientes en paralelo se satura el pipeline (8 B/ciclo).
 *
 * El combine entre flujos usa el álgebra lineal del CRC: para M = M0||M1,
 *   estado(M0||M1) = advance(estado(M0), |M1|) ^ estado_desde_cero(M1)
 * donde `advance(s, n)` avanza el estado por n bytes cero. `advance` se
 * descompone en nibbles sobre una tabla [8][16] (misma técnica que la industria:
 * google/crc32c, Intel "Fast CRC Computation").
 * ============================================================================= */

#if defined(ULTRACRC_HAVE_X86_SSE42)
#  define CRC32C_NGROUPS 3u
#  define CRC32C_BLOCK0  ((16u * 1024u / CRC32C_NGROUPS / 64u) * 64u) /* 5440 */
#  define CRC32C_BLOCK1  ((4u * 1024u / CRC32C_NGROUPS / 8u) * 8u)    /* 1360 */
#  define CRC32C_BLOCK2  ((1024u / CRC32C_NGROUPS / 8u) * 8u)         /*  336 */
/* Bloque pequeño: cubre las tramas Ethernet (64–1500 B), que con los bloques
 * anteriores (≥336 B ⇒ 1008 B por grupo) caían siempre al bucle escalar. */
#  define CRC32C_BLOCK3  64u

static uint32_t tbl32c_skip0[8][16];
static uint32_t tbl32c_skip1[8][16];
static uint32_t tbl32c_skip2[8][16];
static uint32_t tbl32c_skip3[8][16];
#endif

/* =============================================================================
 * Generación de tablas
 * ============================================================================= */

#if defined(ULTRACRC_HAVE_X86_SSE42)
/* Avanza el estado CRC-32C (crudo, sin init/final xor) por `n` bytes cero. */
static uint32_t crc32c_advance_bytes(uint32_t crc, size_t n)
{
    while (n-- > 0) {
        crc = (crc >> 8) ^ tbl32c[0][crc & 0xFFu];
    }
    return crc;
}

/* Construye una tabla de skip [8 nibbles][16 valores] que avanza por `n` bytes. */
static void crc32c_build_skip_table(uint32_t tab[8][16], size_t n)
{
    uint32_t bitval[32];
    for (int j = 0; j < 32; j++) {
        bitval[j] = crc32c_advance_bytes(1u << j, n);
    }
    for (int i = 0; i < 8; i++) {
        for (int v = 0; v < 16; v++) {
            uint32_t acc = 0;
            for (int b = 0; b < 4; b++) {
                if (v & (1 << b)) acc ^= bitval[4 * i + b];
            }
            tab[i][v] = acc;
        }
    }
}

/* Aplica a `crc` el avance representado por una tabla de skip. */
static inline uint32_t crc32c_skip(uint32_t crc, uint32_t (*tab)[16])
{
    return tab[0][crc & 0xFu]           ^ tab[1][(crc >> 4) & 0xFu]
         ^ tab[2][(crc >> 8) & 0xFu]    ^ tab[3][(crc >> 12) & 0xFu]
         ^ tab[4][(crc >> 16) & 0xFu]   ^ tab[5][(crc >> 20) & 0xFu]
         ^ tab[6][(crc >> 24) & 0xFu]   ^ tab[7][(crc >> 28) & 0xFu];
}
#endif

static void ultracrc_build_tables(void)
{
    /* --- CRC-16/CCITT-FALSE (no reflejado) --- */
    for (uint32_t i = 0; i < 256u; i++) {
        uint16_t c = (uint16_t)(i << 8);
        for (int b = 0; b < 8; b++) {
            c = (uint16_t)((c & 0x8000u) ? ((uint16_t)(c << 1) ^ CRC16_CCITT_FALSE_POLY)
                                         : (uint16_t)(c << 1));
        }
        tbl16[i] = c;
    }

    /* --- CRC-32/MPEG-2 (no reflejado): tabla base + slicing-by-8 --- */
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i << 24;
        for (int b = 0; b < 8; b++) {
            c = (c & 0x80000000u) ? ((c << 1) ^ CRC32_MPEG2_POLY) : (c << 1);
        }
        tbl32[0][i] = c;
    }
    for (int k = 1; k < 8; k++) {
        for (uint32_t i = 0; i < 256u; i++) {
            uint32_t prev = tbl32[k - 1][i];
            tbl32[k][i] = (prev << 8) ^ tbl32[0][(prev >> 24) & 0xFFu];
        }
    }

    /* --- CRC-64/ECMA-182 (no reflejado): tabla base + slicing-by-8 --- */
    for (uint32_t i = 0; i < 256u; i++) {
        uint64_t c = (uint64_t)i << 56;
        for (int b = 0; b < 8; b++) {
            c = (c & 0x8000000000000000ull) ? ((c << 1) ^ CRC64_ECMA182_POLY) : (c << 1);
        }
        tbl64[0][i] = c;
    }
    for (int k = 1; k < 8; k++) {
        for (uint32_t i = 0; i < 256u; i++) {
            uint64_t prev = tbl64[k - 1][i];
            tbl64[k][i] = (prev << 8) ^ tbl64[0][(prev >> 56) & 0xFFu];
        }
    }

    /* --- CRC-32C/Castagnoli (reflejado): tabla base + slicing-by-8 --- */
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t c = i;
        for (int b = 0; b < 8; b++) {
            c = (c & 1u) ? ((c >> 1) ^ CRC32C_POLY_REFLECTED) : (c >> 1);
        }
        tbl32c[0][i] = c;
    }
    for (int k = 1; k < 8; k++) {
        for (uint32_t i = 0; i < 256u; i++) {
            uint32_t prev = tbl32c[k - 1][i];
            tbl32c[k][i] = (prev >> 8) ^ tbl32c[0][prev & 0xFFu];
        }
    }

#if defined(ULTRACRC_HAVE_X86_SSE42)
    /* --- Tablas de skip para el backend multistream (dependen de tbl32c[0]) --- */
    crc32c_build_skip_table(tbl32c_skip0, CRC32C_BLOCK0);
    crc32c_build_skip_table(tbl32c_skip1, CRC32C_BLOCK1);
    crc32c_build_skip_table(tbl32c_skip2, CRC32C_BLOCK2);
    crc32c_build_skip_table(tbl32c_skip3, CRC32C_BLOCK3);
#endif
}

/* =============================================================================
 * Backends CRC-32C
 * ============================================================================= */

/* --- Escalar (slicing-by-8, reflejado) — siempre disponible --- */
static uint32_t crc32c_sw(uint32_t crc, const uint8_t* data, size_t len)
{
    while (len >= 8) {
        uint32_t w;
        memcpy(&w, data, sizeof(w)); /* carga little-endian (reflejado) */
        crc ^= w;
        crc = tbl32c[7][crc & 0xFFu] ^ tbl32c[6][(crc >> 8) & 0xFFu]
            ^ tbl32c[5][(crc >> 16) & 0xFFu] ^ tbl32c[4][(crc >> 24) & 0xFFu]
            ^ tbl32c[3][data[4]] ^ tbl32c[2][data[5]]
            ^ tbl32c[1][data[6]] ^ tbl32c[0][data[7]];
        data += 8;
        len -= 8;
    }
    while (len > 0) {
        crc = (crc >> 8) ^ tbl32c[0][(crc ^ *data) & 0xFFu];
        data++;
        len--;
    }
    return crc;
}

/* --- x86-64 SSE4.2: instrucción CRC32 (Castagnoli), multistream 3 vías --- */
#if defined(ULTRACRC_HAVE_X86_SSE42)
static __attribute__((target("sse4.2")))
uint32_t crc32c_sse42(uint32_t crc, const uint8_t* data, size_t len)
{
    const uint8_t* p = data;
    const uint8_t* e = data + len;
    uint32_t l = crc;

    /* Tres flujos independientes por iteración para saturar el pipeline de
     * CRC32 (throughput 1/cycle, latencia 3 ciclos). Se recorre de mayor a
     * menor bloque para minimizar el resto que cae en la ruta escalar. */
    static const size_t      blks[4] = { CRC32C_BLOCK0, CRC32C_BLOCK1,
                                         CRC32C_BLOCK2, CRC32C_BLOCK3 };
    static uint32_t (*const  tabs[4])[16] = { tbl32c_skip0, tbl32c_skip1,
                                              tbl32c_skip2, tbl32c_skip3 };

    for (int tier = 0; tier < 4; tier++) {
        const size_t blk = blks[tier];
        uint32_t (*tab)[16] = tabs[tier];

        while ((size_t)(e - p) >= (size_t)CRC32C_NGROUPS * blk) {
            uint32_t l1 = 0, l2 = 0;
            const uint8_t* p1 = p + blk;
            const uint8_t* p2 = p + 2 * blk;
            size_t i = 0;

            for (; i + 64 <= blk; i += 64) {
                for (int k = 0; k < 8; k++) {
                    uint64_t v0, v1, v2;
                    memcpy(&v0, p + i + 8 * k, sizeof(v0));
                    memcpy(&v1, p1 + i + 8 * k, sizeof(v1));
                    memcpy(&v2, p2 + i + 8 * k, sizeof(v2));
                    l  = (uint32_t)_mm_crc32_u64(l, v0);
                    l1 = (uint32_t)_mm_crc32_u64(l1, v1);
                    l2 = (uint32_t)_mm_crc32_u64(l2, v2);
                }
            }
            for (; i < blk; i += 8) {
                uint64_t v0, v1, v2;
                memcpy(&v0, p + i, sizeof(v0));
                memcpy(&v1, p1 + i, sizeof(v1));
                memcpy(&v2, p2 + i, sizeof(v2));
                l  = (uint32_t)_mm_crc32_u64(l, v0);
                l1 = (uint32_t)_mm_crc32_u64(l1, v1);
                l2 = (uint32_t)_mm_crc32_u64(l2, v2);
            }

            l = crc32c_skip(l, tab) ^ l1;
            l = crc32c_skip(l, tab) ^ l2;
            p += (size_t)CRC32C_NGROUPS * blk; /* los 3 flujos cubren 3*blk */
        }
    }

    /* Cola desenrollada: el resto (< 192 B tras el último tier) es habitual en
     * tramas de control pequeñas, donde el bucle simple pagaba su overhead. */
    while ((size_t)(e - p) >= 32) {
        uint64_t v0, v1, v2, v3;
        memcpy(&v0, p,      sizeof(v0));
        memcpy(&v1, p + 8,  sizeof(v1));
        memcpy(&v2, p + 16, sizeof(v2));
        memcpy(&v3, p + 24, sizeof(v3));
        l = (uint32_t)_mm_crc32_u64(l, v0);
        l = (uint32_t)_mm_crc32_u64(l, v1);
        l = (uint32_t)_mm_crc32_u64(l, v2);
        l = (uint32_t)_mm_crc32_u64(l, v3);
        p += 32;
    }
    while ((size_t)(e - p) >= 8) {
        uint64_t v;
        memcpy(&v, p, sizeof(v));
        l = (uint32_t)_mm_crc32_u64(l, v);
        p += 8;
    }
    if ((size_t)(e - p) >= 4) {
        uint32_t v;
        memcpy(&v, p, sizeof(v));
        l = _mm_crc32_u32(l, v);
        p += 4;
    }
    while (p != e) {
        l = _mm_crc32_u8(l, *p);
        p++;
    }
    return l;
}
#endif

/* --- AArch64 ARMv8-CRC: instrucción CRC32 (Castagnoli) --- */
#if defined(ULTRACRC_HAVE_ARM64_CRC)
static uint32_t crc32c_armv8(uint32_t crc, const uint8_t* data, size_t len)
{
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, data, sizeof(v));
        crc = __crc32cd(crc, v);
        data += 8;
        len -= 8;
    }
    if (len >= 4) {
        uint32_t v;
        memcpy(&v, data, sizeof(v));
        crc = __crc32cw(crc, v);
        data += 4;
        len -= 4;
    }
    while (len > 0) {
        crc = __crc32cb(crc, *data);
        data++;
        len--;
    }
    return crc;
}
#endif

/* =============================================================================
 * Dispatch CRC-32C
 * ============================================================================= */

typedef uint32_t (*crc32c_impl_t)(uint32_t crc, const uint8_t* data, size_t len);

/* El puntero de despacho se publica con una escritura atómica y se lee en el
 * camino caliente con una carga relajada: evita el coste de `pthread_once` en
 * cada trama (≈3–5 ns, significativo para tramas de 64 B). */
static _Atomic(crc32c_impl_t) crc32c_impl = NULL;
static const char* crc32c_backend_name = "sw";

static void ultracrc_global_init(void)
{
    ultracrc_build_tables();

    atomic_store_explicit(&crc32c_impl, crc32c_sw, memory_order_relaxed);
    crc32c_backend_name = "sw";

    if (tuxrt_hwdet_init() != TUXRT_OK) {
        return; /* sin detección de HW: fallback escalar */
    }

#if defined(ULTRACRC_HAVE_X86_SSE42)
    if (tuxrt_hwdet_has_feature(TUXRT_FEAT_SSE42)) {
        atomic_store_explicit(&crc32c_impl, crc32c_sse42, memory_order_relaxed);
        crc32c_backend_name = "sse42";
        return;
    }
#endif
#if defined(ULTRACRC_HAVE_ARM64_CRC)
    if (tuxrt_hwdet_has_feature(TUXRT_FEAT_ARMV8_CRC)) {
        atomic_store_explicit(&crc32c_impl, crc32c_armv8, memory_order_relaxed);
        crc32c_backend_name = "armv8";
        return;
    }
#endif
}

static void ultracrc_ensure(void)
{
    (void)pthread_once(&ultracrc_once, ultracrc_global_init);
}

/* Despacho sin `pthread_once` en el camino caliente. */
static inline crc32c_impl_t crc32c_dispatch(void)
{
    crc32c_impl_t fn = atomic_load_explicit(&crc32c_impl, memory_order_relaxed);
    if (__builtin_expect(fn == NULL, 0)) {
        ultracrc_ensure();
        fn = atomic_load_explicit(&crc32c_impl, memory_order_relaxed);
    }
    return fn;
}

/* =============================================================================
 * API interna (protocolo): CRC-16/32/64
 * ============================================================================= */

uint16_t ultracrc_crc_i(const uint8_t* buf, size_t len, uint16_t prev)
{
    ultracrc_ensure();
    uint16_t crc = prev;
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)((crc << 8) ^ tbl16[((crc >> 8) ^ buf[i]) & 0xFFu]);
    }
    return crc;
}

uint32_t ultracrc_crc_ii(const uint8_t* buf, size_t len, uint32_t prev)
{
    ultracrc_ensure();
    uint32_t crc = prev;
    while (len >= 8) {
        uint32_t w = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16)
                   | ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
        crc ^= w;
        crc = tbl32[7][(crc >> 24) & 0xFFu] ^ tbl32[6][(crc >> 16) & 0xFFu]
            ^ tbl32[5][(crc >> 8) & 0xFFu] ^ tbl32[4][crc & 0xFFu]
            ^ tbl32[3][buf[4]] ^ tbl32[2][buf[5]]
            ^ tbl32[1][buf[6]] ^ tbl32[0][buf[7]];
        buf += 8;
        len -= 8;
    }
    while (len > 0) {
        crc = (crc << 8) ^ tbl32[0][((crc >> 24) ^ *buf) & 0xFFu];
        buf++;
        len--;
    }
    return crc;
}

uint64_t ultracrc_crc_iii(const uint8_t* buf, size_t len, uint64_t prev)
{
    ultracrc_ensure();
    uint64_t crc = prev;
    while (len >= 8) {
        uint64_t w = ((uint64_t)buf[0] << 56) | ((uint64_t)buf[1] << 48)
                   | ((uint64_t)buf[2] << 40) | ((uint64_t)buf[3] << 32)
                   | ((uint64_t)buf[4] << 24) | ((uint64_t)buf[5] << 16)
                   | ((uint64_t)buf[6] << 8) | (uint64_t)buf[7];
        uint64_t c = crc ^ w;
        crc = tbl64[7][(c >> 56) & 0xFFu] ^ tbl64[6][(c >> 48) & 0xFFu]
            ^ tbl64[5][(c >> 40) & 0xFFu] ^ tbl64[4][(c >> 32) & 0xFFu]
            ^ tbl64[3][(c >> 24) & 0xFFu] ^ tbl64[2][(c >> 16) & 0xFFu]
            ^ tbl64[1][(c >> 8) & 0xFFu] ^ tbl64[0][c & 0xFFu];
        buf += 8;
        len -= 8;
    }
    while (len > 0) {
        crc = (crc << 8) ^ tbl64[0][((crc >> 56) ^ *buf) & 0xFFu];
        buf++;
        len--;
    }
    return crc;
}

uint64_t ultracrc_compute(tuxrc_crc_algo_e algo, const uint8_t* buf, size_t len, uint64_t prev)
{
    switch (algo) {
    case CRC_I:
        return (uint64_t)ultracrc_crc_i(buf, len, (uint16_t)prev);
    case CRC_II:
        return (uint64_t)ultracrc_crc_ii(buf, len, (uint32_t)prev);
    case CRC_III:
        return (uint64_t)ultracrc_crc_iii(buf, len, (uint64_t)prev);
    default:
        return 0u;
    }
}

const char* ultracrc_backend_name_get(void)
{
    ultracrc_ensure();
    return crc32c_backend_name;
}

/* =============================================================================
 * API pública (ABI C): CRC-32C acelerado
 * ============================================================================= */

uint32_t tuxrt_ultracrc32(const void* data, size_t len)
{
    uint32_t crc = CRC32C_INIT;
    if (data != NULL && len > 0) {
        crc = crc32c_dispatch()(crc, (const uint8_t*)data, len);
    }
    return crc ^ CRC32C_XOROUT;
}

// =============================================================================
// CRC-32C incremental (primitiva compartida con la HAL de aceleración)
// =============================================================================

uint32_t ultracrc_crc32c_raw(const void* buf, size_t len, uint32_t prev)
{
    if (buf == NULL || len == 0) {
        return prev;
    }
    return crc32c_dispatch()(prev, (const uint8_t*)buf, len);
}
