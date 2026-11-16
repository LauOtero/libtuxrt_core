/**
 * error_boundary.c — Implementación de Error Boundary y Signal Handlers (M3b)
 * 
 * Proveer protección contra pánicos y fallos críticos en C para aislar
 * el dominio RT (CORE) del orquestador (Mojo) mediante setjmp/sigsetjmp
 * y manejo robusto de señales de excepción (SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT).
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include "../../sdk/include/tuxrt_sdk.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <execinfo.h>

// Thread-local error context key
static pthread_key_t tls_error_ctx_key;
static pthread_once_t tls_key_once = PTHREAD_ONCE_INIT;

static void tls_key_destructor(void* ptr) {
    if (ptr) {
        free(ptr);
    }
}

static void make_tls_key(void) {
    pthread_key_create(&tls_error_ctx_key, tls_key_destructor);
}

tuxrt_error_context_t* tuxrt_error_context_get(void) {
    pthread_once(&tls_key_once, make_tls_key);
    tuxrt_error_context_t* ctx = (tuxrt_error_context_t*)pthread_getspecific(tls_error_ctx_key);
    if (!ctx) {
        ctx = (tuxrt_error_context_t*)calloc(1, sizeof(tuxrt_error_context_t));
        if (ctx) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ctx->timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
            pthread_setspecific(tls_error_ctx_key, ctx);
        }
    }
    return ctx;
}

tuxrt_err_t tuxrt_error_context_init(void) {
    tuxrt_error_context_t* ctx = tuxrt_error_context_get();
    if (!ctx) {
        return TUXRT_ERR_NOMEM;
    }
    memset(ctx, 0, sizeof(tuxrt_error_context_t));
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ctx->timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    return TUXRT_OK;
}

void tuxrt_error_context_fini(void) {
    pthread_once(&tls_key_once, make_tls_key);
    tuxrt_error_context_t* ctx = (tuxrt_error_context_t*)pthread_getspecific(tls_error_ctx_key);
    if (ctx) {
        pthread_setspecific(tls_error_ctx_key, NULL);
        free(ctx);
    }
}

// Global signal handler for critical faults
static void tuxrt_global_signal_handler(int sig, siginfo_t* info, void* ucontext) {
    (void)ucontext;
    tuxrt_error_context_t* ctx = tuxrt_error_context_get();
    if (ctx) {
        ctx->caught_signal = sig;
        ctx->error_code = TUXRT_ERR_SIGNAL;
        ctx->fault_address = info ? info->si_addr : NULL;
        
        const char* sig_name = "UNKNOWN";
        switch (sig) {
            case SIGSEGV: sig_name = "SIGSEGV (Segmentation Fault)"; break;
            case SIGBUS:  sig_name = "SIGBUS (Bus Error)"; break;
            case SIGFPE:  sig_name = "SIGFPE (Floating Point Exception)"; break;
            case SIGILL:  sig_name = "SIGILL (Illegal Instruction)"; break;
            case SIGABRT: sig_name = "SIGABRT (Abort)"; break;
        }
        
        snprintf(ctx->error_message, sizeof(ctx->error_message), 
                 "Caught fatal signal %d (%s) at address %p", sig, sig_name, ctx->fault_address);

        // Capture backtrace
        void* bt_array[32];
        int bt_size = backtrace(bt_array, 32);
        char** bt_symbols = backtrace_symbols(bt_array, bt_size);
        fprintf(stderr, "[TUXRT FATAL] %s\n", ctx->error_message);
        if (bt_symbols) {
            for (int i = 0; i < bt_size && i < 10; i++) {
                fprintf(stderr, "  at %s\n", bt_symbols[i]);
            }
            free(bt_symbols);
        }

        // If siglongjmp was set up, jump back to safety
        siglongjmp(ctx->jump_buffer, sig);
    } else {
        // Fallback if no thread context
        fprintf(stderr, "[TUXRT FATAL] Caught signal %d without TLS context. Aborting.\n", sig);
        abort();
    }
}

tuxrt_err_t tuxrt_install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = tuxrt_global_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGSEGV, &sa, NULL) < 0) return TUXRT_ERR_IO;
    if (sigaction(SIGBUS, &sa, NULL) < 0) return TUXRT_ERR_IO;
    if (sigaction(SIGFPE, &sa, NULL) < 0) return TUXRT_ERR_IO;
    if (sigaction(SIGILL, &sa, NULL) < 0) return TUXRT_ERR_IO;
    if (sigaction(SIGABRT, &sa, NULL) < 0) return TUXRT_ERR_IO;

    return TUXRT_OK;
}

void tuxrt_remove_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}
