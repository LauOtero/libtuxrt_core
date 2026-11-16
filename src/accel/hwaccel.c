/*
 * hwaccel.c — HAL de aceleración por hardware (CRC-32C y FEC RS).
 *
 * Implementación con despacho en tiempo de ejecución:
 *   - CRC-32C: instrucción ARMv8.1 (crc32c*) o SSE4.2 (crc32), con fallback
 *     software slice-by-8 (imprescindible en Cortex-A7, p. ej. BPI M2 Zero).
 *   - FEC RS: multiplicación en GF(2^8) por tabla de nibble + shuffle
 *     (NEON en AArch64, AVX2 en x86-64) con fallback escalar.
 *
 * Todo el estado de inicialización (tablas GF, tablas CRC, matrices del código)
 * se construye una sola vez y queda en memoria estática: el camino caliente no
 * reserva memoria.
 */
#include "tuxrt_hwaccel.h"
#include "ultracrc.h"

#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#if defined(__linux__)
  #include <unistd.h>
  #include <sys/socket.h>
  #include <sys/ioctl.h>
  #include <net/if.h>
  #include <linux/ethtool.h>
  #include <linux/sockios.h>
  #include <linux/net_tstamp.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
  #include <immintrin.h>
  #define CRT_X86 1
#else
  #define CRT_X86 0
#endif

#if defined(__aarch64__)
  #include <arm_neon.h>
  #include <sys/auxv.h>
  #ifndef HWCAP_CRC32
    #define HWCAP_CRC32 (1UL << 7)
  #endif
  #ifndef HWCAP_AES
    #define HWCAP_AES (1UL << 3)
  #endif
  #define CRT_ARM64 1
#else
  #define CRT_ARM64 0
#endif

/* ================================================================== */
/* GF(2^8) — polinomio primitivo x^8+x^4+x^3+x^2+1 (0x11D)             */
/* ================================================================== */

static uint8_t       s_gf_exp[512];
static uint8_t       s_gf_log[256];
static pthread_once_t s_gf_once = PTHREAD_ONCE_INIT;

static void gf_init(void) {
    uint16_t x = 1;
    for (int i = 0; i < 255; i++) {
        s_gf_exp[i] = (uint8_t)x;
        s_gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100u) {
            x ^= 0x11Du;
        }
    }
    /* Duplica la tabla de exponenciales para evitar el módulo 255. */
    for (int i = 255; i < 512; i++) {
        s_gf_exp[i] = s_gf_exp[i - 255];
    }
    s_gf_log[0] = 0;   /* no se usa: la multiplicación trata el 0 aparte */
}

static inline void gf_ensure(void) {
    (void)pthread_once(&s_gf_once, gf_init);
}

static inline uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0u || b == 0u) {
        return 0u;
    }
    return s_gf_exp[(unsigned)s_gf_log[a] + (unsigned)s_gf_log[b]];
}

static inline uint8_t gf_inv(uint8_t a) {
    return (a == 0u) ? 0u : s_gf_exp[255u - (unsigned)s_gf_log[a]];
}

uint8_t tuxrt_gf8_mul(uint8_t a, uint8_t b) {
    gf_ensure();
    return gf_mul(a, b);
}

uint8_t tuxrt_gf8_inv(uint8_t a) {
    gf_ensure();
    return gf_inv(a);
}

/* ================================================================== */
/* Capacidades de la plataforma                                       */
/* ================================================================== */

static tuxrt_hw_caps_t  s_caps;
static pthread_once_t       s_caps_once = PTHREAD_ONCE_INIT;

static void caps_init(void) {
    /* CRC-32C: se consulta el backend ya seleccionado por `ultracrc`, */
    /* de modo que exista una única implementación acelerada en el core. */
    s_caps.crc32c_backend = ultracrc_backend_name_get();
    s_caps.crc32c_hw      = (strcmp(s_caps.crc32c_backend, "sw") != 0);
    s_caps.fec_simd       = false;
    s_caps.fec_backend    = "scalar";
    s_caps.armv8_crypto   = false;
    s_caps.phc            = false;
    s_caps.launch_time    = false;

#if CRT_X86
    if (__builtin_cpu_supports("avx2")) {
        s_caps.fec_simd    = true;
        s_caps.fec_backend = "avx2";
    }
#elif CRT_ARM64
    unsigned long hwcap = getauxval(AT_HWCAP);
    /* NEON es obligatorio en AArch64: el FEC siempre se vectoriza. */
    s_caps.fec_simd    = true;
    s_caps.fec_backend = "neon";
    if ((hwcap & (unsigned long)HWCAP_AES) != 0UL) {
        s_caps.armv8_crypto = true;
    }
#endif
}

const tuxrt_hw_caps_t* tuxrt_hw_caps(void) {
    (void)pthread_once(&s_caps_once, caps_init);
    return &s_caps;
}

uint32_t tuxrt_crc32c_raw(uint32_t seed, const void* data, size_t len) {
    /* Delegado en `ultracrc`: una sola implementación con dispatch HW. */
    return ultracrc_crc32c_raw(data, len, seed);
}

uint32_t tuxrt_crc32c(const void* data, size_t len) {
    return ~tuxrt_crc32c_raw(0xFFFFFFFFu, data, len);
}

/* ================================================================== */
/* Multiplicación vectorial en GF(2^8) por una constante              */
/* ================================================================== */

static inline void xor_bytes(uint8_t* dst, const uint8_t* src, size_t len) {
    for (size_t i = 0; i < len; i++) {
        dst[i] ^= src[i];
    }
}

/* dst ^= src * coef, con coef constante para todo el bloque. */
static void xor_mul_scalar(uint8_t* dst, const uint8_t* src, size_t len,
                           const uint8_t tlo[16], const uint8_t thi[16]) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b = src[i];
        dst[i] ^= (uint8_t)(tlo[b & 0x0Fu] ^ thi[b >> 4]);
    }
}

#if CRT_X86
__attribute__((target("avx2")))
static void xor_mul_avx2(uint8_t* dst, const uint8_t* src, size_t len,
                         const uint8_t tlo[16], const uint8_t thi[16]) {
    __m128i tlo_v;
    __m128i thi_v;
    memcpy(&tlo_v, tlo, sizeof(tlo_v));
    memcpy(&thi_v, thi, sizeof(thi_v));

    const __m256i vlo  = _mm256_broadcastsi128_si256(tlo_v);
    const __m256i vhi  = _mm256_broadcastsi128_si256(thi_v);
    const __m256i mask = _mm256_set1_epi8(0x0F);

    size_t i = 0;
    for (; i + 32u <= len; i += 32u) {
        /* memcpy evita aliasing/alineación estricta (cargas no alineadas). */
        __m256i s;
        __m256i d;
        memcpy(&s, src + i, sizeof(s));
        memcpy(&d, dst + i, sizeof(d));

        __m256i lo = _mm256_and_si256(s, mask);
        __m256i hi = _mm256_and_si256(_mm256_srli_epi16(s, 4), mask);
        __m256i p  = _mm256_xor_si256(_mm256_shuffle_epi8(vlo, lo),
                                      _mm256_shuffle_epi8(vhi, hi));
        __m256i r  = _mm256_xor_si256(d, p);
        memcpy(dst + i, &r, sizeof(r));
    }
    for (; i < len; i++) {
        uint8_t b = src[i];
        dst[i] ^= (uint8_t)(tlo[b & 0x0Fu] ^ thi[b >> 4]);
    }
}
#endif

#if CRT_ARM64
static void xor_mul_neon(uint8_t* dst, const uint8_t* src, size_t len,
                         const uint8_t tlo[16], const uint8_t thi[16]) {
    const uint8x16_t vlo  = vld1q_u8(tlo);
    const uint8x16_t vhi  = vld1q_u8(thi);
    const uint8x16_t mask = vdupq_n_u8(0x0F);

    size_t i = 0;
    for (; i + 16u <= len; i += 16u) {
        uint8x16_t s  = vld1q_u8(src + i);
        uint8x16_t lo = vandq_u8(s, mask);
        uint8x16_t hi = vandq_u8(vshrq_n_u8(s, 4), mask);
        uint8x16_t p  = veorq_u8(vqtbl1q_u8(vlo, lo), vqtbl1q_u8(vhi, hi));
        vst1q_u8(dst + i, veorq_u8(vld1q_u8(dst + i), p));
    }
    for (; i < len; i++) {
        uint8_t b = src[i];
        dst[i] ^= (uint8_t)(tlo[b & 0x0Fu] ^ thi[b >> 4]);
    }
}
#endif

static void xor_mul_const(uint8_t* dst, const uint8_t* src, size_t len, uint8_t coef) {
    if (coef == 0u || len == 0u) {
        return;
    }
    if (coef == 1u) {
        xor_bytes(dst, src, len);
        return;
    }

    uint8_t tlo[16];
    uint8_t thi[16];
    for (unsigned i = 0; i < 16u; i++) {
        tlo[i] = gf_mul((uint8_t)i, coef);
        thi[i] = gf_mul((uint8_t)(i << 4), coef);
    }

    const tuxrt_hw_caps_t* caps = tuxrt_hw_caps();
#if CRT_X86
    if (caps->fec_simd) {
        xor_mul_avx2(dst, src, len, tlo, thi);
        return;
    }
#elif CRT_ARM64
    if (caps->fec_simd) {
        xor_mul_neon(dst, src, len, tlo, thi);
        return;
    }
#else
    (void)caps;
#endif
    xor_mul_scalar(dst, src, len, tlo, thi);
}

/* ================================================================== */
/* Matriz generadora del código RS sistemático (cacheada)             */
/* ================================================================== */

#define RS_CACHE_SLOTS 4u

typedef struct {
    bool    valid;
    uint8_t k;
    uint8_t n;
    uint8_t gen[TUXRT_RS_MAX_N][TUXRT_RS_MAX_N];  /* k x n */
} rs_entry_t;

static rs_entry_t      s_rs_cache[RS_CACHE_SLOTS];
static unsigned        s_rs_next = 0u;
static pthread_mutex_t s_rs_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Invierte una matriz n x n sobre GF(2^8) por Gauss-Jordan. */
static bool gf_matrix_inverse(const uint8_t* src, uint8_t* dst, unsigned n) {
    uint8_t aug[TUXRT_RS_MAX_N][2u * TUXRT_RS_MAX_N];

    for (unsigned r = 0; r < n; r++) {
        for (unsigned c = 0; c < n; c++) {
            aug[r][c] = src[r * n + c];
            aug[r][n + c] = (r == c) ? 1u : 0u;
        }
    }

    for (unsigned col = 0; col < n; col++) {
        unsigned piv = col;
        while (piv < n && aug[piv][col] == 0u) {
            piv++;
        }
        if (piv == n) {
            return false;   /* matriz singular */
        }
        if (piv != col) {
            for (unsigned c = 0; c < 2u * n; c++) {
                uint8_t tmp = aug[col][c];
                aug[col][c] = aug[piv][c];
                aug[piv][c] = tmp;
            }
        }
        uint8_t inv = gf_inv(aug[col][col]);
        for (unsigned c = 0; c < 2u * n; c++) {
            aug[col][c] = gf_mul(aug[col][c], inv);
        }
        for (unsigned r = 0; r < n; r++) {
            if (r == col) {
                continue;
            }
            uint8_t f = aug[r][col];
            if (f == 0u) {
                continue;
            }
            for (unsigned c = 0; c < 2u * n; c++) {
                aug[r][c] ^= gf_mul(aug[col][c], f);
            }
        }
    }

    for (unsigned r = 0; r < n; r++) {
        for (unsigned c = 0; c < n; c++) {
            dst[r * n + c] = aug[r][n + c];
        }
    }
    return true;
}

/* Construye G = Vtop^-1 · V  (k x n), sistemática: G[:, 0..k-1] = I. */
static bool rs_build_generator(uint8_t k, uint8_t n,
                               uint8_t gen[TUXRT_RS_MAX_N][TUXRT_RS_MAX_N]) {
    uint8_t v[TUXRT_RS_MAX_N][TUXRT_RS_MAX_N];
    uint8_t vtop[TUXRT_RS_MAX_N * TUXRT_RS_MAX_N];
    uint8_t vinv[TUXRT_RS_MAX_N * TUXRT_RS_MAX_N];

    for (unsigned r = 0; r < k; r++) {
        for (unsigned c = 0; c < n; c++) {
            v[r][c] = s_gf_exp[(unsigned)((r * c) % 255u)];
        }
    }
    for (unsigned r = 0; r < k; r++) {
        for (unsigned c = 0; c < k; c++) {
            vtop[r * k + c] = v[r][c];
        }
    }
    if (!gf_matrix_inverse(vtop, vinv, k)) {
        return false;
    }
    for (unsigned r = 0; r < k; r++) {
        for (unsigned c = 0; c < n; c++) {
            uint8_t acc = 0u;
            for (unsigned l = 0; l < k; l++) {
                acc ^= gf_mul(vinv[r * k + l], v[l][c]);
            }
            gen[r][c] = acc;
        }
    }
    return true;
}

/*
 * Copia bajo mutex la matriz generadora k x n a `out` (k*n bytes).
 * La copia evita que otra hebra invalide la entrada de caché mientras se usa:
 * la matriz se consulta EN EL CAMINO CALIENTE y no puede quedar compartida.
 */
static bool rs_copy_generator(uint8_t k, uint8_t n, uint8_t* out) {
    /* Las tablas de GF(2^8) son imprescindibles para construir la matriz. */
    gf_ensure();

    (void)pthread_mutex_lock(&s_rs_mutex);

    rs_entry_t* found = NULL;
    for (unsigned i = 0; i < RS_CACHE_SLOTS; i++) {
        if (s_rs_cache[i].valid && s_rs_cache[i].k == k && s_rs_cache[i].n == n) {
            found = &s_rs_cache[i];
            break;
        }
    }

    if (found == NULL) {
        rs_entry_t* e = &s_rs_cache[s_rs_next];
        s_rs_next = (s_rs_next + 1u) % RS_CACHE_SLOTS;
        e->valid = false;
        if (!rs_build_generator(k, n, e->gen)) {
            (void)pthread_mutex_unlock(&s_rs_mutex);
            return false;
        }
        e->k = k;
        e->n = n;
        e->valid = true;
        found = e;
    }

    for (unsigned r = 0; r < k; r++) {
        memcpy(out + (size_t)r * n, found->gen[r], n);
    }

    (void)pthread_mutex_unlock(&s_rs_mutex);
    return true;
}

tuxrt_err_t tuxrt_rs_encode(uint8_t k, uint8_t n,
                                       const uint8_t* const* data,
                                       uint8_t* const* parity,
                                       uint32_t len) {
    if (k == 0u || n <= k || n > TUXRT_RS_MAX_N || data == NULL || parity == NULL) {
        return TUXRT_ERR_INVAL;
    }
    if (len == 0u) {
        return TUXRT_OK;
    }

    uint8_t gen[TUXRT_RS_MAX_N * TUXRT_RS_MAX_N];
    if (!rs_copy_generator(k, n, gen)) {
        return TUXRT_ERR_NOT_INITIALIZED;
    }

    for (unsigned j = 0; j < (unsigned)(n - k); j++) {
        if (parity[j] == NULL) {
            return TUXRT_ERR_INVAL;
        }
        memset(parity[j], 0, len);
        for (unsigned r = 0; r < k; r++) {
            if (data[r] == NULL) {
                return TUXRT_ERR_INVAL;
            }
            xor_mul_const(parity[j], data[r], len, gen[r * n + k + j]);
        }
    }
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_rs_decode(uint8_t k, uint8_t n,
                                       uint8_t* const* blocks,
                                       const bool* present,
                                       uint32_t len) {
    if (k == 0u || n <= k || n > TUXRT_RS_MAX_N ||
        blocks == NULL || present == NULL) {
        return TUXRT_ERR_INVAL;
    }

    uint8_t gen[TUXRT_RS_MAX_N * TUXRT_RS_MAX_N];
    if (!rs_copy_generator(k, n, gen)) {
        return TUXRT_ERR_NOT_INITIALIZED;
    }

    /* Columnas presentes. */
    uint8_t idx[TUXRT_RS_MAX_N];
    unsigned have = 0u;
    for (unsigned c = 0; c < n; c++) {
        if (!present[c]) {
            continue;
        }
        if (blocks[c] == NULL) {
            return TUXRT_ERR_INVAL;
        }
        idx[have++] = (uint8_t)c;
    }
    if (have < k) {
        return TUXRT_ERR_ECC_UNCORRECTABLE;   /* borrados irrecuperables */
    }
    if (have == n) {
        return TUXRT_OK;         /* nada que reconstruir */
    }

    /* A[t][r] = G[r][idx[t]];  m = A^-1 · y */
    uint8_t a[TUXRT_RS_MAX_N * TUXRT_RS_MAX_N];
    uint8_t ainv[TUXRT_RS_MAX_N * TUXRT_RS_MAX_N];
    for (unsigned t = 0; t < k; t++) {
        for (unsigned r = 0; r < k; r++) {
            a[t * k + r] = gen[r * n + idx[t]];
        }
    }
    if (!gf_matrix_inverse(a, ainv, k)) {
        return TUXRT_ERR_ECC_UNCORRECTABLE;   /* matriz singular */
    }

    if (len == 0u) {
        return TUXRT_OK;
    }

    uint8_t* msg = (uint8_t*)malloc((size_t)k * (size_t)len);
    if (msg == NULL) {
        return TUXRT_ERR_NOMEM;
    }

    for (unsigned r = 0; r < k; r++) {
        uint8_t* mr = msg + (size_t)r * len;
        memset(mr, 0, len);
        for (unsigned t = 0; t < k; t++) {
            uint8_t coef = ainv[r * k + t];
            if (coef != 0u) {
                xor_mul_const(mr, blocks[idx[t]], len, coef);
            }
        }
    }

    for (unsigned c = 0; c < n; c++) {
        if (present[c]) {
            continue;
        }
        if (blocks[c] == NULL) {
            free(msg);
            return TUXRT_ERR_INVAL;
        }
        memset(blocks[c], 0, len);
        for (unsigned r = 0; r < k; r++) {
            uint8_t coef = gen[r * n + c];
            if (coef != 0u) {
                xor_mul_const(blocks[c], msg + (size_t)r * len, len, coef);
            }
        }
    }

    free(msg);
    return TUXRT_OK;
}

/* ================================================================== */
/* Capacidades ligadas a una interfaz de red (PHC / Launch Time)      */
/* ================================================================== */

tuxrt_err_t tuxrt_hw_caps_for_interface(const char* ifname,
                                                   tuxrt_hw_caps_t* out) {
    if (ifname == NULL || ifname[0] == '\0' || out == NULL) {
        return TUXRT_ERR_INVAL;
    }

    *out = *tuxrt_hw_caps();
    out->phc = false;
    out->launch_time = false;

#if defined(__linux__)
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return TUXRT_ERR_IO;
    }

    struct ethtool_ts_info info;
    memset(&info, 0, sizeof(info));
    info.cmd = ETHTOOL_GET_TS_INFO;

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_data = (void*)&info;

    tuxrt_err_t st = TUXRT_ERR_NOSYS;
    if (ioctl(fd, SIOCETHTOOL, &ifr) == 0) {
        uint32_t tx_hw = (info.so_timestamping & (uint32_t)SOF_TIMESTAMPING_TX_HARDWARE);
        out->phc = (tx_hw != 0u);
        /* El pacing por hardware (ETF/SO_TXTIME) se apoya en el reloj PHC. */
        out->launch_time = out->phc;
        st = TUXRT_OK;
    }
    close(fd);
    return st;
#else
    return TUXRT_ERR_NOSYS;
#endif
}
