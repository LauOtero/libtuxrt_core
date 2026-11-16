/**
 * hwdet.c — Hardware Detection Implementation
 *
 * Implemeta detección de features de hardware usando:
 * - /proc/self/auxv (AT_HWCAP, AT_HWCAP2)
 * - /proc/cpuinfo (modelo, fabricante)
 * - /proc/meminfo (memoria total)
 *
 * Design: TRT-20 - HWD /proc/self/auxv (P1)
 */

#include "hwdet.h"
#include "../../sdk/include/tuxrt_sdk.h"
#include <sys/auxv.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <errno.h>

// =============================================================================
// Feature flags state
// =============================================================================

static uint64_t cached_features = 0;
static bool hwdet_initialized = false;

// =============================================================================
// Auxv-based detection
// =============================================================================

static void detect_features_from_auxv(void) {
    unsigned long hwcap = getauxval(AT_HWCAP);
#ifdef AT_HWCAP2
    unsigned long hwcap2 = getauxval(AT_HWCAP2);
#else
    unsigned long hwcap2 = 0;
#endif

#if defined(__x86_64__) || defined(__i386__)
    // x86/x86_64 feature detection.
    // NOTA: en x86 glibc sólo expone en AT_HWCAP/AT_HWCAP2 los registros
    // CPUID.1:EDX/ECX, un subconjunto que NO incluye SSE4.2/AVX2/PCLMUL/AES
    // (residen en CPUID.1:ECX y hojas superiores). Por eso se usa
    // __builtin_cpu_supports(), que consulta CPUID completo.
    (void)hwcap;
    (void)hwcap2;
    cached_features |= TUXRT_FEAT_FPU;   // x87 FPU (baseline x86-64)
    cached_features |= TUXRT_FEAT_TSC;   // RDTSC    (baseline x86-64)
#ifdef __GNUC__
    if (__builtin_cpu_supports("cmov"))     cached_features |= TUXRT_FEAT_CMOV;
    if (__builtin_cpu_supports("mmx"))      cached_features |= TUXRT_FEAT_MMX;
    if (__builtin_cpu_supports("sse"))      cached_features |= TUXRT_FEAT_SSE;
    if (__builtin_cpu_supports("sse2"))     cached_features |= TUXRT_FEAT_SSE2;
    if (__builtin_cpu_supports("sse3"))     cached_features |= TUXRT_FEAT_SSE3;
    if (__builtin_cpu_supports("ssse3"))    cached_features |= TUXRT_FEAT_SSSE3;
    if (__builtin_cpu_supports("sse4.1"))   cached_features |= TUXRT_FEAT_SSE41;
    if (__builtin_cpu_supports("sse4.2"))   cached_features |= TUXRT_FEAT_SSE42;
    if (__builtin_cpu_supports("aes"))      cached_features |= TUXRT_FEAT_AES;
    if (__builtin_cpu_supports("pclmul"))   cached_features |= TUXRT_FEAT_PCLMUL;
    if (__builtin_cpu_supports("avx"))      cached_features |= TUXRT_FEAT_AVX;
    if (__builtin_cpu_supports("avx2"))     cached_features |= TUXRT_FEAT_AVX2;
    if (__builtin_cpu_supports("fma"))      cached_features |= TUXRT_FEAT_FMA;
    if (__builtin_cpu_supports("bmi2"))     cached_features |= TUXRT_FEAT_BMI2;
    if (__builtin_cpu_supports("avx512f"))  cached_features |= TUXRT_FEAT_AVX512F;
    if (__builtin_cpu_supports("avx512dq")) cached_features |= TUXRT_FEAT_AVX512DQ;
    if (__builtin_cpu_supports("avx512bw")) cached_features |= TUXRT_FEAT_AVX512BW;
    if (__builtin_cpu_supports("avx512vl")) cached_features |= TUXRT_FEAT_AVX512VL;
    if (__builtin_cpu_supports("avx512cd")) cached_features |= TUXRT_FEAT_AVX512CD;
    // NOTA: avx512er/avx512pf son exclusivas de Xeon Phi (Knights Landing) y
    // GCC no las acepta como parámetro de __builtin_cpu_supports; se omiten.
    if (__builtin_cpu_supports("rdrnd"))    cached_features |= TUXRT_FEAT_RDRAND;
    if (__builtin_cpu_supports("rdseed"))   cached_features |= TUXRT_FEAT_RDSEED;
#endif

#elif defined(__aarch64__) || defined(__arm__)
    // ARM feature detection via AT_HWCAP
    if (hwcap & 0x00000001UL) cached_features |= TUXRT_FEAT_NEON;       // NEON
    if (hwcap & 0x00000010UL) cached_features |= TUXRT_FEAT_CRC32;      // CRC32
    if (hwcap & 0x00000100UL) cached_features |= TUXRT_FEAT_AES;        // AES
    if (hwcap & 0x00000200UL) cached_features |= TUXRT_FEAT_PMULL;      // PMULL
    if (hwcap & 0x00000400UL) cached_features |= TUXRT_FEAT_SHA1;        // SHA1
    if (hwcap & 0x00000800UL) cached_features |= TUXRT_FEAT_SHA2;        // SHA2
#endif

    // Fallback: use GCC CPU detection if AT_HWCAP didn't work
    if (cached_features == 0) {
#ifdef __GNUC__
        // Use GCC's runtime CPU detection
        #if defined(__x86_64__) || defined(__i386__)
        #ifdef __SSE4_2__
        cached_features |= TUXRT_FEAT_SSE42;
        #endif
        #ifdef __AVX2__
        cached_features |= TUXRT_FEAT_AVX2;
        #endif
        #ifdef __AES__
        cached_features |= TUXRT_FEAT_AES;
        #endif
        #endif
        #if defined(__aarch64__) || defined(__arm__)
        #ifdef __ARM_NEON
        cached_features |= TUXRT_FEAT_NEON;
        #endif
        #endif
#endif
    }
}

// =============================================================================
// CPU Count Detection
// =============================================================================

static int cpu_count_cache = -1;
static int logical_count_cache = -1;

static void detect_cpu_counts(void) {
    // Logical count = sysconf(_SC_NPROCESSORS_ONLN)
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    logical_count_cache = (nprocs > 0) ? (int)nprocs : 1;

    // Physical count: parse /proc/cpuinfo
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (f) {
        int cores = 0;
        char line[512];
        bool in_physical = false;

        while (fgets(line, sizeof(line), f)) {
            // Count "processor" lines (logical)
            // Count "core id" lines (physical cores per socket)
            // Count "physical id" lines (sockets)
            if (strstr(line, "core id") != NULL) {
                cores++;
                in_physical = true;
            }
        }

        if (in_physical && cores > 0) {
            cpu_count_cache = cores;
        } else {
            // Fallback: logical = physical (no hyperthreading detected)
            cpu_count_cache = logical_count_cache;
        }
        fclose(f);
    } else {
        cpu_count_cache = logical_count_cache;
    }
}

// =============================================================================
// CPU Frequency Detection
// =============================================================================

static uint32_t cpu_freq_cache = 0;

static void detect_cpu_freq_mhz(void) {
    FILE* f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
    if (f) {
        unsigned int freq_khz;
        if (fscanf(f, "%u", &freq_khz) == 1) {
            cpu_freq_cache = freq_khz / 1000; // Convert to MHz
        }
        fclose(f);
    }

    if (cpu_freq_cache == 0) {
        // Fallback: parse /proc/cpuinfo
        f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                unsigned int freq_khz;
                if (sscanf(line, "cpu MHz : %u", &freq_khz) == 1 ||
                    sscanf(line, "cpu MHz : %u.0", &freq_khz) == 1) {
                    // Actually "cpu MHz : <float>"
                }
                if (strstr(line, "cpu MHz") != NULL) {
                    unsigned int mhz;
                    if (sscanf(line, "cpu MHz : %u", &mhz) == 1) {
                        cpu_freq_cache = mhz;
                    }
                }
            }
            fclose(f);
        }
    }

    if (cpu_freq_cache == 0) {
        // Final fallback: assume 2400 MHz
        cpu_freq_cache = 2400;
    }
}

// =============================================================================
// Virtualization Detection
// =============================================================================

static bool virtual_cache = false;
static bool virtual_checked = false;

static void detect_virtualization(void) {
    if (virtual_checked) return;
    virtual_checked = true;

    FILE* f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            // Check for hypervisor flag in cpu flags
            if (strstr(line, "flags") != NULL &&
                strstr(line, "hypervisor") != NULL) {
                virtual_cache = true;
                break;
            }
            // Check for common VM hypervisor strings
            if (strstr(line, " QEMU") != NULL ||
                strstr(line, "KVM") != NULL ||
                strstr(line, "VMware") != NULL ||
                strstr(line, "VirtualBox") != NULL ||
                strstr(line, "Hyper-V") != NULL) {
                virtual_cache = true;
                break;
            }
        }
        fclose(f);
    }

    // Also check /sys/hypervisor/type
    if (!virtual_cache) {
        FILE* hf = fopen("/sys/hypervisor/type", "r");
        if (hf) {
            char buf[64];
            if (fgets(buf, sizeof(buf), hf)) {
                if (strstr(buf, "xen") || strstr(buf, "kvm") ||
                    strstr(buf, "vmware") || strstr(buf, "microsoft")) {
                    virtual_cache = true;
                }
            }
            fclose(hf);
        }
    }
}

// =============================================================================
// Public API
// =============================================================================

int tuxrt_hwdet_init(void) {
    if (hwdet_initialized) {
        return 0;
    }

    // Detect hardware features from auxv
    detect_features_from_auxv();

    // Detect CPU counts
    detect_cpu_counts();

    // Detect CPU frequency
    detect_cpu_freq_mhz();

    // Detect virtualization
    detect_virtualization();

    hwdet_initialized = true;
    return 0;
}

bool tuxrt_hwdet_has_feature(tuxrt_hw_feature_t feature) {
    if (!hwdet_initialized) {
        tuxrt_hwdet_init();
    }
    return (cached_features & (uint64_t)feature) != 0;
}

int tuxrt_hwdet_get_cpu_count(void) {
    if (!hwdet_initialized) {
        tuxrt_hwdet_init();
    }
    return cpu_count_cache;
}

int tuxrt_hwdet_get_logical_count(void) {
    if (!hwdet_initialized) {
        tuxrt_hwdet_init();
    }
    return logical_count_cache;
}

uint32_t tuxrt_hwdet_get_cpu_freq_mhz(void) {
    if (!hwdet_initialized) {
        tuxrt_hwdet_init();
    }
    return cpu_freq_cache;
}

bool tuxrt_hwdet_is_virtual(void) {
    if (!hwdet_initialized) {
        tuxrt_hwdet_init();
    }
    return virtual_cache;
}

int tuxrt_hwdet_get_manufacturer(char* out_buf, size_t buf_len) {
    if (!out_buf || buf_len == 0) return TUXRT_ERR_INVAL;

    if (!hwdet_initialized) {
        tuxrt_hwdet_init();
    }

    // x86: Read from CPUID directly (vendor string)
    // ARM: Read manufacturer from /proc/cpuinfo
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (!f) {
        strncpy(out_buf, "Unknown", buf_len);
        out_buf[buf_len - 1] = '\0';
        return 0;
    }

    int found = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
#if defined(__x86_64__) || defined(__i386__)
        if (strstr(line, "vendor_id") != NULL) {
            char vendor[128];
            if (sscanf(line, "vendor_id : %127s", vendor) == 1) {
                strncpy(out_buf, vendor, buf_len);
                out_buf[buf_len - 1] = '\0';
                found = 1;
                break;
            }
        }
#elif defined(__aarch64__) || defined(__arm__)
        if (strstr(line, "model name") != NULL ||
            strstr(line, "Hardware") != NULL ||
            strstr(line, "Processor") != NULL) {
            char model[256];
            if (sscanf(line, "%*[^:]:%255[^\n]", model) == 1) {
                strncpy(out_buf, model, buf_len);
                out_buf[buf_len - 1] = '\0';
                found = 1;
                break;
            }
        }
#endif
    }
    fclose(f);

    if (!found) {
        strncpy(out_buf, "Unknown", buf_len);
        out_buf[buf_len - 1] = '\0';
    }

    return 0;
}

int tuxrt_hwdet_get_model(char* out_buf, size_t buf_len) {
    if (!out_buf || buf_len == 0) return TUXRT_ERR_INVAL;

    if (!hwdet_initialized) {
        tuxrt_hwdet_init();
    }

    FILE* f = fopen("/proc/cpuinfo", "r");
    if (!f) {
        strncpy(out_buf, "Unknown", buf_len);
        out_buf[buf_len - 1] = '\0';
        return 0;
    }

    int found = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
#if defined(__x86_64__) || defined(__i386__)
        if (strstr(line, "model name") != NULL) {
            char* colon = strchr(line, ':');
            if (colon) {
                colon++;
                while (*colon == ' ') colon++;
                size_t len = strlen(colon);
                if (colon[len - 1] == '\n') len--;
                size_t copy_len = (len < buf_len - 1) ? len : buf_len - 1;
                memcpy(out_buf, colon, copy_len);
                out_buf[copy_len] = '\0';
                found = 1;
                break;
            }
        }
#elif defined(__aarch64__) || defined(__arm__)
        if (strstr(line, "model name") != NULL) {
            char* colon = strchr(line, ':');
            if (colon) {
                colon++;
                while (*colon == ' ') colon++;
                size_t len = strlen(colon);
                if (colon[len - 1] == '\n') len--;
                size_t copy_len = (len < buf_len - 1) ? len : buf_len - 1;
                memcpy(out_buf, colon, copy_len);
                out_buf[copy_len] = '\0';
                found = 1;
                break;
            }
        }
#endif
    }
    fclose(f);

    if (!found) {
        strncpy(out_buf, "Unknown", buf_len);
        out_buf[buf_len - 1] = '\0';
    }

    return 0;
}