/**
 * tuxrt_mutex.h — Real-time Mutex Wrapper
 *
 * Wrapper para mutexes pthread con soporte PTHREAD_PRIO_PROTECT
 * para evitar inversión de prioridad en hilos en tiempo real.
 *
 * Design: TRT-02 - Priority Protection in Shared Mutexes
 */

#ifndef TUXRT_MUTEX_H
#define TUXRT_MUTEX_H

#include "../../sdk/include/tuxrt_sdk.h"

#include <stdint.h>
#include <pthread.h>

// =============================================================================
// Mutex Type and Priority Ceiling
// =============================================================================

#define TUXRT_MUTEX_PRIO_CEILING  99  // Max priority for real-time systems

typedef struct tuxrt_mutex_t {
    pthread_mutex_t mtx;          // Underlying pthread mutex
    int             prio_ceiling; // Priority ceiling for PTHREAD_PRIO_PROTECT
} tuxrt_mutex_t;

// =============================================================================
// Mutex Initialization
// =============================================================================

/**
 * Initialize a tuxrt_mutex_t with PTHREAD_PRIO_PROTECT protocol.
 *
 * This is the real-time safe alternative to pthread_mutex_init.
 * Uses PTHREAD_PRIO_PROTECT to prevent priority inversion when
 * multiple threads with different priorities share the same mutex.
 *
 * @param mux      Pointer to tuxrt_mutex_t to initialize
 * @param prio_ceiling Priority ceiling value (99 for RT, or thread max priority)
 * @return TUXRT_OK on success, TUXRT_ERR_IO on failure
 */
tuxrt_err_t tuxrt_mutex_init_rt(tuxrt_mutex_t* mux, int prio_ceiling);

/**
 * Destroy a tuxrt_mutex_t previously initialized with tuxrt_mutex_init_rt().
 *
 * @param mux      Pointer to tuxrt_mutex_t to destroy
 * @return TUXRT_OK on success, TUXRT_ERR_INVAL if mux is NULL
 */
tuxrt_err_t tuxrt_mutex_destroy_rt(tuxrt_mutex_t* mux);

// =============================================================================
// Mutex Lock/Unlock (RT-safe wrappers)
// =============================================================================

/**
 * Lock a tuxrt_mutex_t.
 *
 * @param mux      Pointer to tuxrt_mutex_t to lock
 * @return TUXRT_OK on success, TUXRT_ERR_IO on failure
 */
tuxrt_err_t tuxrt_mutex_lock_rt(tuxrt_mutex_t* mux);

/**
 * Unlock a tuxrt_mutex_t.
 *
 * @param mux      Pointer to tuxrt_mutex_t to unlock
 * @return TUXRT_OK on success, TUXRT_ERR_IO on failure
 */
tuxrt_err_t tuxrt_mutex_unlock_rt(tuxrt_mutex_t* mux);

#ifdef __cplusplus
extern "C" {
#endif

#endif // TUXRT_MUTEX_H