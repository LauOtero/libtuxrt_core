/**
 * scheduler.c — Real-time Scheduler (CORE)
 * 
 * Implementación de scheduler RT con timerfd + SCHED_FIFO/RR/DEADLINE
 * Cumple C-ABI tuxrt_sdk.h
 */

#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include "../../sdk/include/tuxrt_sdk.h"
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

// =============================================================================
// Internal Structures
// =============================================================================

struct tuxrt_scheduler_t {
    int epoll_fd;                    // epoll para timers
    int timerfd;                     // timerfd principal (CLOCK_MONOTONIC)
    pthread_t rt_thread;             // Hilo RT principal
    atomic_bool running;             // Estado
    tuxrt_thread_attr_t main_attr;   // Atributos hilo principal
    
    // Periodic timers
    struct {
        uint64_t id;
        int timerfd;
        tuxrt_task_fn_t fn;
        void* arg;
        struct itimerspec its;
    } *timers;
    size_t timer_capacity;
    size_t timer_count;
    pthread_mutex_t timer_mutex;
    
    // Stats
    tuxrt_scheduler_stats_t stats;
    tuxrt_timestamp_ns_t last_tick_ns;
};

// Task wrapper para pthread
struct task_wrapper {
    tuxrt_task_fn_t fn;
    void* arg;
};

// =============================================================================
// Helpers
// =============================================================================

static tuxrt_timestamp_ns_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (tuxrt_timestamp_ns_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int policy_to_sched(tuxrt_sched_policy_t policy) {
    switch (policy) {
        case TUXRT_SCHED_POLICY_FIFO: return SCHED_FIFO;
        case TUXRT_SCHED_POLICY_RR: return SCHED_RR;
        case TUXRT_SCHED_POLICY_DEADLINE:
#ifdef SCHED_DEADLINE
            return SCHED_DEADLINE;
#else
            return SCHED_FIFO; // fallback
#endif
        default: return SCHED_FIFO;
    }
}

static void* rt_thread_main(void* arg) {
    struct tuxrt_scheduler_t* sched = (struct tuxrt_scheduler_t*)arg;
    struct epoll_event events[16];
    
    while (atomic_load(&sched->running)) {
        // Use 1ms timeout for real-time granularity (was 100ms causing up to 100ms jitter)
        int n = epoll_wait(sched->epoll_fd, events, 16, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        
        tuxrt_timestamp_ns_t tick = now_ns();
        
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == sched->timerfd) {
                // Main tick - read timerfd to clear
                uint64_t expirations;
                read(sched->timerfd, &expirations, sizeof(expirations));
                
                // Update stats
                if (sched->last_tick_ns > 0) {
                    uint64_t latency = tick - sched->last_tick_ns;
                    sched->stats.ticks_total++;
                    if (latency > sched->stats.max_latency_ns) {
                        sched->stats.max_latency_ns = latency;
                    }
                    // Running average
                    sched->stats.avg_latency_ns = 
                        (sched->stats.avg_latency_ns * (sched->stats.ticks_total - 1) + latency) 
                        / sched->stats.ticks_total;
                }
                sched->last_tick_ns = tick;
            } else {
                // Periodic timer callback
                for (size_t j = 0; j < sched->timer_count; j++) {
                    if (sched->timers[j].timerfd == events[i].data.fd) {
                        uint64_t exp;
                        read(events[i].data.fd, &exp, sizeof(exp));
                        if (sched->timers[j].fn) {
                            sched->timers[j].fn(sched->timers[j].arg);
                        }
                        break;
                    }
                }
            }
        }
    }
    return NULL;
}

static int create_timerfd(uint32_t hz, struct itimerspec* its) {
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) return -1;
    
    its->it_interval.tv_sec = 0;
    its->it_interval.tv_nsec = 1000000000LL / hz;
    its->it_value = its->it_interval;
    
    if (timerfd_settime(fd, 0, its, NULL) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// =============================================================================
// API Implementation
// =============================================================================

tuxrt_err_t tuxrt_scheduler_create(const tuxrt_config_t* cfg, tuxrt_scheduler_t** out_sched) {
    (void)cfg; // TODO: leer config (tick_hz, cpu_affinity, etc.)
    if (!out_sched) return TUXRT_ERR_INVAL;
    
    tuxrt_scheduler_t* sched = calloc(1, sizeof(tuxrt_scheduler_t));
    if (!sched) return TUXRT_ERR_NOMEM;
    
    sched->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (sched->epoll_fd < 0) {
        free(sched);
        return TUXRT_ERR_IO;
    }
    
    // Main timerfd @ 1kHz default
    struct itimerspec its;
    sched->timerfd = create_timerfd(1000, &its);
    if (sched->timerfd < 0) {
        close(sched->epoll_fd);
        free(sched);
        return TUXRT_ERR_IO;
    }
    
    // Add to epoll
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = sched->timerfd};
    if (epoll_ctl(sched->epoll_fd, EPOLL_CTL_ADD, sched->timerfd, &ev) < 0) {
        close(sched->timerfd);
        close(sched->epoll_fd);
        free(sched);
        return TUXRT_ERR_IO;
    }
    
    sched->timer_capacity = 32;
    sched->timers = calloc(sched->timer_capacity, sizeof(*sched->timers));
    if (!sched->timers) {
        close(sched->timerfd);
        close(sched->epoll_fd);
        free(sched);
        return TUXRT_ERR_NOMEM;
    }
    pthread_mutex_init(&sched->timer_mutex, NULL);
    atomic_init(&sched->running, false);
    memset(&sched->stats, 0, sizeof(sched->stats));
    
    *out_sched = sched;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_scheduler_spawn(tuxrt_scheduler_t* sched,
                                   tuxrt_task_fn_t fn,
                                   void* arg,
                                   const tuxrt_thread_attr_t* attr,
                                   uint64_t* out_thread_id) {
    if (!sched || !fn || atomic_load(&sched->running)) return TUXRT_ERR_INVAL;
    
    struct task_wrapper* wrapper = malloc(sizeof(struct task_wrapper));
    if (!wrapper) return TUXRT_ERR_NOMEM;
    wrapper->fn = fn;
    wrapper->arg = arg;
    
    // Configurar atributos
    pthread_attr_t pthread_attr;
    pthread_attr_init(&pthread_attr);
    pthread_attr_setinheritsched(&pthread_attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&pthread_attr, policy_to_sched(attr ? attr->policy : TUXRT_SCHED_POLICY_FIFO));
    
    struct sched_param param = {.sched_priority = attr ? attr->priority : 50};
    pthread_attr_setschedparam(&pthread_attr, &param);
    
    if (attr && attr->cpu_affinity >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(attr->cpu_affinity, &cpuset);
        pthread_attr_setaffinity_np(&pthread_attr, sizeof(cpu_set_t), &cpuset);
    }
    
    // For SCHED_DEADLINE we'd need sched_attr (not implemented in stub)
    
    // SCHED_DEADLINE requires special handling (sched_attr)
    if (attr && attr->policy == TUXRT_SCHED_POLICY_DEADLINE) {
        // TODO: usar pthread_attr_set_schedattr (Linux 4.8+) o syscall directo
    }
    
    int ret = pthread_create(&sched->rt_thread, &pthread_attr, rt_thread_main, sched);
    pthread_attr_destroy(&pthread_attr);
    
    if (ret != 0) {
        free(wrapper);
        return TUXRT_ERR_IO;
    }
    
    atomic_store(&sched->running, true);
    
    if (out_thread_id) *out_thread_id = (uint64_t)sched->rt_thread;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_scheduler_periodic(tuxrt_scheduler_t* sched,
                                      tuxrt_task_fn_t fn,
                                      void* arg,
                                      uint32_t hz,
                                      const tuxrt_thread_attr_t* attr,
                                      uint64_t* out_timer_id) {
    if (!sched || !fn || hz == 0) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&sched->timer_mutex);
    
    if (sched->timer_count >= sched->timer_capacity) {
        size_t new_cap = sched->timer_capacity * 2;
        void* new_timers = realloc(sched->timers, new_cap * sizeof(*sched->timers));
        if (!new_timers) {
            pthread_mutex_unlock(&sched->timer_mutex);
            return TUXRT_ERR_NOMEM;
        }
        sched->timers = new_timers;
        sched->timer_capacity = new_cap;
    }
    
    size_t idx = sched->timer_count++;
    struct itimerspec its;
    int tfd = create_timerfd(hz, &its);
    if (tfd < 0) {
        sched->timer_count--;
        pthread_mutex_unlock(&sched->timer_mutex);
        return TUXRT_ERR_IO;
    }
    
    sched->timers[idx].id = idx + 1;
    sched->timers[idx].timerfd = tfd;
    sched->timers[idx].fn = fn;
    sched->timers[idx].arg = arg;
    sched->timers[idx].its = its;
    
    // Add to epoll
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = tfd};
    if (epoll_ctl(sched->epoll_fd, EPOLL_CTL_ADD, tfd, &ev) < 0) {
        close(tfd);
        sched->timer_count--;
        pthread_mutex_unlock(&sched->timer_mutex);
        return TUXRT_ERR_IO;
    }
    
    if (out_timer_id) *out_timer_id = sched->timers[idx].id;
    pthread_mutex_unlock(&sched->timer_mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_scheduler_cancel(tuxrt_scheduler_t* sched, uint64_t timer_id) {
    if (!sched) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&sched->timer_mutex);
    for (size_t i = 0; i < sched->timer_count; i++) {
        if (sched->timers[i].id == timer_id) {
            epoll_ctl(sched->epoll_fd, EPOLL_CTL_DEL, sched->timers[i].timerfd, NULL);
            close(sched->timers[i].timerfd);
            
            // Compact array
            memmove(&sched->timers[i], &sched->timers[i + 1],
                    (sched->timer_count - i - 1) * sizeof(*sched->timers));
            sched->timer_count--;
            pthread_mutex_unlock(&sched->timer_mutex);
            return TUXRT_OK;
        }
    }
    pthread_mutex_unlock(&sched->timer_mutex);
    return TUXRT_ERR_NOENT;
}

tuxrt_timestamp_ns_t tuxrt_scheduler_now(const tuxrt_scheduler_t* sched) {
    (void)sched;
    return now_ns();
}

tuxrt_err_t tuxrt_scheduler_sleep_until(tuxrt_scheduler_t* sched, tuxrt_timestamp_ns_t deadline) {
    (void)sched;
    struct timespec ts;
    ts.tv_sec = deadline / 1000000000LL;
    ts.tv_nsec = deadline % 1000000000LL;
    return clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == 0 ? TUXRT_OK : TUXRT_ERR_TIMEDOUT;
}

tuxrt_err_t tuxrt_scheduler_stats(const tuxrt_scheduler_t* sched, tuxrt_scheduler_stats_t* out) {
    if (!sched || !out) return TUXRT_ERR_INVAL;
    *out = sched->stats;
    return TUXRT_OK;
}

void tuxrt_scheduler_destroy(tuxrt_scheduler_t* sched) {
    if (!sched) return;
    
    atomic_store(&sched->running, false);
    if (sched->rt_thread) pthread_join(sched->rt_thread, NULL);
    
    for (size_t i = 0; i < sched->timer_count; i++) {
        epoll_ctl(sched->epoll_fd, EPOLL_CTL_DEL, sched->timers[i].timerfd, NULL);
        close(sched->timers[i].timerfd);
    }
    free(sched->timers);
    
    if (sched->timerfd >= 0) close(sched->timerfd);
    if (sched->epoll_fd >= 0) close(sched->epoll_fd);
    pthread_mutex_destroy(&sched->timer_mutex);
    free(sched);
}