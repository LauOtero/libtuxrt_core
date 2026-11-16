/**
 * tuxrt_mutex.c — Real-time Mutex Wrapper Implementation
 *
 * Implementación de mutexes pthread con PTHREAD_PRIO_PROTECT
 * para evitar inversión de prioridad en sistemas en tiempo real.
 *
 * Design: TRT-02 - Priority Protection in Shared Mutexes
 */

#include "tuxrt_mutex.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/**
 * Initialize a tuxrt_mutex_t with PTHREAD_PRIO_PROTECT protocol.
 *
 * This function sets up a pthread mutex with the priority protection protocol
 * to prevent priority inversion when multiple threads with different priorities
 * share the same mutex. The priority ceiling is set to prevent any thread below
 * the ceiling from blocking a higher-priority thread.
 *
 * @param mux      Pointer to tuxrt_mutex_t to initialize
 * @param prio_ceiling Priority ceiling value (99 for RT, or thread max priority)
 * @return TUXRT_OK on success, TUXRT_ERR_INVAL if mux is NULL, TUXRT_ERR_IO on failure
 */
tuxrt_err_t tuxrt_mutex_init_rt(tuxrt_mutex_t* mux, int prio_ceiling) {
    if (!mux) {
        return TUXRT_ERR_INVAL;
    }

    pthread_mutexattr_t attr;
    int ret;

    // Initialize mutex attributes
    ret = pthread_mutexattr_init(&attr);
    if (ret != 0) {
        return TUXRT_ERR_IO;
    }

    // Set priority protection protocol
    ret = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_PROTECT);
    if (ret != 0) {
        pthread_mutexattr_destroy(&attr);
        return TUXRT_ERR_IO;
    }

    // Set the priority ceiling
    ret = pthread_mutexattr_setprioceiling(&attr, prio_ceiling);
    if (ret != 0) {
        pthread_mutexattr_destroy(&attr);
        return TUXRT_ERR_IO;
    }

    // Initialize the mutex with the attributes
    ret = pthread_mutex_init(&mux->mtx, &attr);
    if (ret != 0) {
        pthread_mutexattr_destroy(&attr);
        return TUXRT_ERR_IO;
    }

    // Destroy attributes (no longer needed)
    pthread_mutexattr_destroy(&attr);

    // Store the priority ceiling
    mux->prio_ceiling = prio_ceiling;

    return TUXRT_OK;
}

/**
 * Destroy a tuxrt_mutex_t previously initialized with tuxrt_mutex_init_rt().
 *
 * @param mux      Pointer to tuxrt_mutex_t to destroy
 * @return TUXRT_OK on success, TUXRT_ERR_INVAL if mux is NULL
 */
tuxrt_err_t tuxrt_mutex_destroy_rt(tuxrt_mutex_t* mux) {
    if (!mux) {
        return TUXRT_ERR_INVAL;
    }

    int ret = pthread_mutex_destroy(&mux->mtx);
    if (ret != 0) {
        return TUXRT_ERR_IO;
    }

    mux->prio_ceiling = 0;

    return TUXRT_OK;
}

/**
 * Lock a tuxrt_mutex_t.
 *
 * @param mux      Pointer to tuxrt_mutex_t to lock
 * @return TUXRT_OK on success, TUXRT_ERR_IO on failure
 */
tuxrt_err_t tuxrt_mutex_lock_rt(tuxrt_mutex_t* mux) {
    if (!mux) {
        return TUXRT_ERR_INVAL;
    }

    int ret = pthread_mutex_lock(&mux->mtx);
    if (ret != 0) {
        return TUXRT_ERR_IO;
    }

    return TUXRT_OK;
}

/**
 * Unlock a tuxrt_mutex_t.
 *
 * @param mux      Pointer to tuxrt_mutex_t to unlock
 * @return TUXRT_OK on success, TUXRT_ERR_IO on failure
 */
tuxrt_err_t tuxrt_mutex_unlock_rt(tuxrt_mutex_t* mux) {
    if (!mux) {
        return TUXRT_ERR_INVAL;
    }

    int ret = pthread_mutex_unlock(&mux->mtx);
    if (ret != 0) {
        return TUXRT_ERR_IO;
    }

    return TUXRT_OK;
}