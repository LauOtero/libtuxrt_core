/**
 * sirc.c — SIRC F1-F10: Safety Integrity Runtime Controller
 * 
 * Features:
 * - Safe Mode states N1-N6 (IEC 61800-5-2 / ISO 13849)
 * - Circuit Breaker with exponential backoff
 * - Transport adapter abstraction (USB/UART/CAN/EtherCAT/SPI/I2C)
 * - Failover with policy engine
 * - Health monitoring
 * - Watchdog
 * 
 * Compiles with: -std=c17 -pthread -Wall -Wextra -Werror
 */

#define TUXRT_CORE_EXPORTS
#define _POSIX_C_SOURCE 200809L
#include "../../sdk/include/tuxrt_sdk.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>
#include <stdint.h>
#define SIRC_PRIMARY_ERROR_RATE_THRESHOLD 0.5f
#define SIRC_CONSECUTIVE_FAILURES_THRESHOLD 5
#define SIRC_FAILOVER_RETRY_DELAY_NS (100LL * 1000000)

// ============================================================================
// Internal Types
// ============================================================================

// tuxrt_transport_adapter_t is defined in tuxrt_sdk.h (public for driver access)
// SIRC internal state is defined here

// SIRC internal state
struct tuxrt_sirc_t {
    // Configuration
    tuxrt_sirc_config_t cfg;
    
    // Safe mode state machine
    tuxrt_safe_mode_t safe_mode;
    tuxrt_sirc_level_t current_level;
    
    // Handler
    tuxrt_sirc_handler_t handler;
    void* handler_user_data;
    
    // Watchdog
    uint64_t last_kick_ns;
    
    // Circuit Breaker
    tuxrt_cb_state_t cb_state;
    uint32_t cb_consecutive_failures;
    uint32_t cb_consecutive_successes;
    uint64_t cb_state_change_ns;
    
    // Backoff state
    uint32_t backoff_attempt;
    uint64_t next_retry_ns;
    tuxrt_timestamp_ns_t last_failover_check_ns;

    // Transport adapters
    tuxrt_transport_adapter_t* primary_adapter;
    tuxrt_transport_adapter_t* fallback_adapter;
    tuxrt_transport_adapter_t* active_adapter;

    // Policy
    tuxrt_policy_fn_t policy_fn;
    void* policy_user_data;

    // Failover state
    bool failover_pending;
    tuxrt_timestamp_ns_t failover_retry_ns;
    
    // Health stats
    tuxrt_health_stats_t aggregate_health;
    
    // Synchronization
    pthread_mutex_t mutex;
    
    // Initialization flag
    bool initialized;
};

// ============================================================================
// Helpers
// ============================================================================

static inline tuxrt_timestamp_ns_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (tuxrt_timestamp_ns_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static inline void emit_event(tuxrt_sirc_t* s, tuxrt_sirc_level_t level, 
                               const char* msg, uint32_t fault_code) {
    if (s->handler && level != TUXRT_SIRC_LEVEL_NONE) {
        tuxrt_sirc_event_t event = {
            .level = level,
            .message = msg,
            .timestamp_ns = now_ns(),
            .fault_code = fault_code
        };
        s->handler(&event, s->handler_user_data);
    }
}

static inline void update_health_on_success(tuxrt_health_stats_t* h) {
    h->last_success_ns = now_ns();
    h->consecutive_successes++;
    h->consecutive_failures = 0;
    // Exponential moving average of error rate
    h->error_rate = h->error_rate * 0.9;
}

static inline void update_health_on_failure(tuxrt_health_stats_t* h) {
    h->last_failure_ns = now_ns();
    h->consecutive_failures++;
    h->consecutive_successes = 0;
    h->error_rate = h->error_rate * 0.9 + 0.1;
}

// Circuit Breaker state machine
static void cb_update(tuxrt_sirc_t* s) {
    tuxrt_timestamp_ns_t now = now_ns();
    
    switch (s->cb_state) {
        case TUXRT_CB_CLOSED:
            if (s->cb_consecutive_failures >= s->cfg.cb_failure_threshold) {
                s->cb_state = TUXRT_CB_OPEN;
                s->cb_state_change_ns = now;
                emit_event(s, TUXRT_SIRC_LEVEL_WARN, 
                          "Circuit breaker OPEN: too many failures", 0x5001);
            }
            break;
            
        case TUXRT_CB_OPEN:
            if (now - s->cb_state_change_ns >= s->cfg.cb_timeout_ns) {
                s->cb_state = TUXRT_CB_HALF_OPEN;
                s->cb_consecutive_successes = 0;
                emit_event(s, TUXRT_SIRC_LEVEL_WARN,
                          "Circuit breaker HALF_OPEN: testing recovery", 0x5002);
            }
            break;
            
        case TUXRT_CB_HALF_OPEN:
            if (s->cb_consecutive_successes >= s->cfg.cb_success_threshold) {
                s->cb_state = TUXRT_CB_CLOSED;
                s->cb_consecutive_failures = 0;
                emit_event(s, TUXRT_SIRC_LEVEL_NONE,
                          "Circuit breaker CLOSED: recovered", 0x5003);
            } else if (s->cb_consecutive_failures > 0) {
                s->cb_state = TUXRT_CB_OPEN;
                s->cb_state_change_ns = now;
                emit_event(s, TUXRT_SIRC_LEVEL_WARN,
                          "Circuit breaker OPEN again: recovery failed", 0x5004);
            }
            break;
    }
    
    // Update aggregate health
    s->aggregate_health.cb_state = s->cb_state;
    s->aggregate_health.consecutive_failures = s->cb_consecutive_failures;
    s->aggregate_health.consecutive_successes = s->cb_consecutive_successes;
    s->aggregate_health.error_rate = (double)s->cb_consecutive_failures / 
                                     (s->cb_consecutive_failures + s->cb_consecutive_successes + 1);
}

// Exponential backoff with jitter
TUXRT_SDK_API tuxrt_timestamp_ns_t tuxrt_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (tuxrt_timestamp_ns_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

TUXRT_SDK_API void tuxrt_sirc_update_health_success(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return;
    pthread_mutex_lock(&adapter->mutex);
    adapter->health.last_success_ns = tuxrt_now_ns();
    adapter->health.consecutive_successes++;
    adapter->health.consecutive_failures = 0;
    adapter->health.error_rate = adapter->health.error_rate * 0.9;
    adapter->health.cb_state = TUXRT_CB_CLOSED;
    pthread_mutex_unlock(&adapter->mutex);
}

TUXRT_SDK_API void tuxrt_sirc_update_health_failure(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return;
    pthread_mutex_lock(&adapter->mutex);
    adapter->health.last_failure_ns = tuxrt_now_ns();
    adapter->health.consecutive_failures++;
    adapter->health.consecutive_successes = 0;
    adapter->health.error_rate = adapter->health.error_rate * 0.9 + 0.1;
    pthread_mutex_unlock(&adapter->mutex);
}

uint64_t tuxrt_backoff_compute(const tuxrt_backoff_config_t* cfg, uint32_t attempt) {
    if (!cfg) return 0;
    
    double delay = (double)cfg->base_delay_ns * pow(cfg->multiplier, (double)attempt);
    if (delay > (double)cfg->max_delay_ns) delay = (double)cfg->max_delay_ns;
    
    // Add jitter
    if (cfg->jitter_factor > 0.0) {
        double jitter = delay * cfg->jitter_factor * ((double)rand() / RAND_MAX * 2.0 - 1.0);
        delay += jitter;
    }
    
    return (uint64_t)delay;
}

// Forward declarations for functions defined later in this file
// tuxrt_sirc_set_active_transport is declared in tuxrt_sdk.h

// Deterministic sleep using clock_nanosleep with absolute time
tuxrt_err_t tuxrt_sirc_deterministic_sleep(tuxrt_timestamp_ns_t abs_deadline_ns) {
    if (abs_deadline_ns == 0) return TUXRT_ERR_INVAL;
    
    struct timespec ts;
    uint64_t secs = abs_deadline_ns / 1000000000ULL;
    uint64_t nsecs = abs_deadline_ns % 1000000000ULL;
    ts.tv_sec = (time_t)secs;
    ts.tv_nsec = (long)nsecs;
    
    int ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    if (ret == EINTR) return TUXRT_ERR_INTERRUPTED;
    if (ret == 0) return TUXRT_OK;
    return TUXRT_ERR_IO;
}

// Evaluate whether failover to fallback adapter is needed based on primary health
static tuxrt_err_t evaluate_failover(tuxrt_sirc_t* s) {
    if (!s->cfg.enable_transport_failover) return TUXRT_ERR_NOENT;
    if (!s->primary_adapter || !s->fallback_adapter) return TUXRT_ERR_NOENT;
    
    tuxrt_timestamp_ns_t now = now_ns();
    
    // Rate-limit failover checks to every 100ms minimum
    if (now - s->last_failover_check_ns < SIRC_FAILOVER_RETRY_DELAY_NS) {
        return TUXRT_ERR_BUSY;
    }
    s->last_failover_check_ns = now;
    
    // If we're already on fallback, no need to failover
    if (s->active_adapter == s->fallback_adapter) return TUXRT_OK;
    
    // Check primary adapter health under lock
    pthread_mutex_lock(&s->primary_adapter->mutex);
    double error_rate = s->primary_adapter->health.error_rate;
    uint32_t consec_failures = s->primary_adapter->health.consecutive_failures;
    pthread_mutex_unlock(&s->primary_adapter->mutex);
    
    // Trigger failover if error rate exceeds threshold or consecutive failures hit limit
    if (error_rate > SIRC_PRIMARY_ERROR_RATE_THRESHOLD ||
        consec_failures >= SIRC_CONSECUTIVE_FAILURES_THRESHOLD) {
        
        emit_event(s, TUXRT_SIRC_LEVEL_WARN,
                   "Automatic transport failover: primary adapter unhealthy", 0x7001);
        
        // Perform failover to fallback
        tuxrt_err_t err = tuxrt_sirc_set_active_transport(s, s->fallback_adapter->config.type);
        if (err == TUXRT_OK) {
            s->failover_pending = true;
            s->failover_retry_ns = now + s->cfg.failover_timeout_ns;
            emit_event(s, TUXRT_SIRC_LEVEL_NONE,
                       "Failover to fallback adapter complete", 0x7002);
        } else {
            emit_event(s, TUXRT_SIRC_LEVEL_EMERGENCY,
                       "Failover to fallback adapter failed", 0x7003);
        }
        return err;
    }
    
    return TUXRT_OK;
}

// Check if primary adapter should be recovered and switched back
static tuxrt_err_t evaluate_recovery(tuxrt_sirc_t* s) {
    if (!s->failover_pending) return TUXRT_OK;
    if (!s->primary_adapter || !s->fallback_adapter) return TUXRT_ERR_NOENT;
    
    tuxrt_timestamp_ns_t now = now_ns();
    
    // Check timeout for waiting on fallback
    if (now >= s->failover_retry_ns) {
        // Timeout reached, attempt to return to primary
        emit_event(s, TUXRT_SIRC_LEVEL_WARN,
                   "Failover timeout: returning to primary adapter", 0x7004);
        
        tuxrt_err_t err = tuxrt_sirc_set_active_transport(s, s->primary_adapter->config.type);
        if (err == TUXRT_OK) {
            s->failover_pending = false;
            emit_event(s, TUXRT_SIRC_LEVEL_NONE,
                       "Recovered primary adapter", 0x7005);
        }
        return err;
    }
    
    // Check fallback adapter health for recovery decision
    pthread_mutex_lock(&s->fallback_adapter->mutex);
    double fallback_error_rate = s->fallback_adapter->health.error_rate;
    pthread_mutex_unlock(&s->fallback_adapter->mutex);
    
    // If fallback is also unhealthy, extend the retry timeout
    if (fallback_error_rate > SIRC_PRIMARY_ERROR_RATE_THRESHOLD) {
        s->failover_retry_ns = now + (s->cfg.failover_timeout_ns / 2);
    }
    
    return TUXRT_OK;
}

// Periodic SIRC monitoring: watchdog, failover, circuit breaker, recovery
tuxrt_err_t tuxrt_sirc_monitor(tuxrt_sirc_t* sirc) {
    if (!sirc || !sirc->initialized) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&sirc->mutex);
    
    // Update circuit breaker
    cb_update(sirc);
    
    // Check transport failover needs
    if (sirc->cfg.enable_transport_failover) {
        if (sirc->failover_pending) {
            evaluate_recovery(sirc);
        } else {
            evaluate_failover(sirc);
        }
    }
    
    pthread_mutex_unlock(&sirc->mutex);
    
    return TUXRT_OK;
}

// Safe mode transition helper to avoid format-truncation diagnostics
static void format_transition_msg(char* buf, size_t buf_len, tuxrt_safe_mode_t old, tuxrt_safe_mode_t new_mode, const char* reason) {
    size_t prefix_len = snprintf(buf, buf_len, "Safe mode transition: %d -> %d (", old, new_mode);
    if (prefix_len < buf_len && reason) {
        size_t reason_len = strlen(reason);
        size_t available = buf_len - prefix_len - 2; // for closing paren and null
        if (reason_len > available) reason_len = available;
        memcpy(buf + prefix_len, reason, reason_len);
        buf[prefix_len + reason_len] = ')';
        buf[prefix_len + reason_len + 1] = '\0';
    } else {
        snprintf(buf, buf_len, "Safe mode transition: %d -> %d (unknown)", old, new_mode);
    }
}

// Safe mode transition
static void safe_mode_transition(tuxrt_sirc_t* s, tuxrt_safe_mode_t new_mode, const char* reason) {
    if (new_mode == s->safe_mode) return;

    tuxrt_safe_mode_t old = s->safe_mode;
    s->safe_mode = new_mode;

    char msg[256];
    format_transition_msg(msg, sizeof(msg), old, new_mode, reason);
    
    tuxrt_sirc_level_t level = TUXRT_SIRC_LEVEL_NONE;
    uint32_t fault = 0;
    
    switch (new_mode) {
        case TUXRT_SAFE_MODE_N1_NORMAL:
            level = TUXRT_SIRC_LEVEL_NONE;
            break;
        case TUXRT_SAFE_MODE_N2_DEGRADED:
            level = TUXRT_SIRC_LEVEL_WARN;
            fault = 0x6001;
            break;
        case TUXRT_SAFE_MODE_N3_SAFE_STOP:
            level = TUXRT_SIRC_LEVEL_SAFE_STOP;
            fault = 0x6002;
            break;
        case TUXRT_SAFE_MODE_N4_EMERGENCY:
            level = TUXRT_SIRC_LEVEL_EMERGENCY;
            fault = 0x6003;
            break;
        case TUXRT_SAFE_MODE_N5_RECOVERY:
            level = TUXRT_SIRC_LEVEL_WARN;
            fault = 0x6004;
            break;
        case TUXRT_SAFE_MODE_N6_QUARANTINE:
            level = TUXRT_SIRC_LEVEL_EMERGENCY;
            fault = 0x6005;
            break;
    }
    
    emit_event(s, level, msg, fault);
}

// Default policy: simple threshold-based
static tuxrt_policy_decision_t default_policy(const tuxrt_sirc_event_t* event,
                                               const void* context,
                                               void* user_data) {
    (void)context;
    (void)user_data;
    
    tuxrt_policy_decision_t decision = {0};
    
    switch (event->level) {
        case TUXRT_SIRC_LEVEL_WARN:
            decision.action = TUXRT_POLICY_ACTION_RETRY;
            decision.target_safe_mode = TUXRT_SAFE_MODE_N2_DEGRADED;
            decision.retry_delay_ns = 10000000; // 10ms
            decision.reason = "Warning level: retry with backoff";
            break;
            
        case TUXRT_SIRC_LEVEL_SAFE_STOP:
            decision.action = TUXRT_POLICY_ACTION_SAFE_STOP;
            decision.target_safe_mode = TUXRT_SAFE_MODE_N3_SAFE_STOP;
            decision.reason = "Safe stop triggered";
            break;
            
        case TUXRT_SIRC_LEVEL_EMERGENCY:
            decision.action = TUXRT_POLICY_ACTION_EMERGENCY;
            decision.target_safe_mode = TUXRT_SAFE_MODE_N4_EMERGENCY;
            decision.reason = "Emergency stop triggered";
            break;
            
        default:
            decision.action = TUXRT_POLICY_ACTION_NONE;
            decision.target_safe_mode = TUXRT_SAFE_MODE_N1_NORMAL;
            decision.reason = "No action needed";
            break;
    }
    
    return decision;
}

// Transport adapter creation
static tuxrt_transport_adapter_t* create_adapter(const tuxrt_transport_adapter_config_t* cfg,
                                                  const tuxrt_transport_vtable_t* vtable) {
    if (!cfg || !vtable) return NULL;
    
    tuxrt_transport_adapter_t* adapter = calloc(1, sizeof(tuxrt_transport_adapter_t));
    if (!adapter) return NULL;
    
    adapter->vtable = vtable;
    adapter->config = *cfg;
    adapter->active = false;
    pthread_mutex_init(&adapter->mutex, NULL);
    
    // Initialize health
    adapter->health.last_success_ns = 0;
    adapter->health.last_failure_ns = 0;
    adapter->health.consecutive_failures = 0;
    adapter->health.consecutive_successes = 0;
    adapter->health.cb_state = TUXRT_CB_CLOSED;
    adapter->health.error_rate = 0.0;
    
    // Call vtable init if available (via impl allocation)
    if (vtable->open) {
        // impl will be allocated by the transport driver
        adapter->impl = NULL; // Set by driver
    }
    
    return adapter;
}

static void destroy_adapter(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return;
    
    if (adapter->vtable && adapter->vtable->destroy) {
        adapter->vtable->destroy(adapter);
    }
    pthread_mutex_destroy(&adapter->mutex);
    free(adapter);
}

// ============================================================================
// SIRC API Implementation
// ============================================================================

tuxrt_err_t tuxrt_sirc_create(const tuxrt_sirc_config_t* cfg, tuxrt_sirc_t** out_sirc) {
    if (!cfg || !out_sirc) return TUXRT_ERR_INVAL;
    
    tuxrt_sirc_t* s = calloc(1, sizeof(tuxrt_sirc_t));
    if (!s) return TUXRT_ERR_NOMEM;
    
    s->cfg = *cfg;
    s->safe_mode = TUXRT_SAFE_MODE_N1_NORMAL;
    s->current_level = TUXRT_SIRC_LEVEL_NONE;
    s->handler = cfg->handler;
    s->handler_user_data = cfg->handler_user_data;
    s->last_kick_ns = now_ns();
    s->cb_state = TUXRT_CB_CLOSED;
    s->cb_consecutive_failures = 0;
    s->cb_consecutive_successes = 0;
    s->cb_state_change_ns = 0;
    s->backoff_attempt = 0;
    s->next_retry_ns = 0;
    s->policy_fn = cfg->policy_fn ? cfg->policy_fn : default_policy;
    s->policy_user_data = cfg->policy_user_data;
    s->primary_adapter = NULL;
    s->fallback_adapter = NULL;
    s->active_adapter = NULL;
    pthread_mutex_init(&s->mutex, NULL);
    s->initialized = true;
    
    // Initialize aggregate health
    memset(&s->aggregate_health, 0, sizeof(tuxrt_health_stats_t));
    s->aggregate_health.cb_state = TUXRT_CB_CLOSED;
    s->aggregate_health.error_rate = 0.0;
    
    // Create primary transport if configured
    if (cfg->primary_transport.type != TUXRT_TRANSPORT_USB || cfg->primary_transport.device_path) {
        // Note: vtable must be registered separately via tuxrt_sirc_register_transport
    }
    
    // Create fallback transport if configured
    if (cfg->enable_transport_failover && 
        (cfg->fallback_transport.type != TUXRT_TRANSPORT_USB || cfg->fallback_transport.device_path)) {
        // Note: vtable must be registered separately
    }
    
    *out_sirc = s;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_sirc_check(tuxrt_sirc_t* sirc,
                              const tuxrt_joint_state_t* cmd,
                              const tuxrt_joint_state_t* fb,
                              const double* temperatures,
                              size_t temp_count) {
    if (!sirc || !cmd || !fb) return TUXRT_ERR_INVAL;
    if (!sirc->initialized) return TUXRT_ERR_NOT_INITIALIZED;
    
    pthread_mutex_lock(&sirc->mutex);
    
    tuxrt_sirc_level_t new_level = TUXRT_SIRC_LEVEL_NONE;
    char msg[256] = {0};
    uint32_t fault_code = 0;
    tuxrt_timestamp_ns_t now = now_ns();
    
    // Position check
    if (sirc->cfg.enable_position_check) {
        for (uint8_t i = 0; i < cmd->axis_count && i < fb->axis_count; i++) {
            double err = cmd->position[i] - fb->position[i];
            if (err < 0) err = -err;
            if (err > sirc->cfg.max_position_error) {
                new_level = TUXRT_SIRC_LEVEL_SAFE_STOP;
                fault_code = 0x1000 | i;
                snprintf(msg, sizeof(msg), "Position error axis %d: %.3f mm > %.3f mm", 
                        i, err, sirc->cfg.max_position_error);
                break;
            }
        }
    }
    
    // Velocity check
    if (new_level == TUXRT_SIRC_LEVEL_NONE && sirc->cfg.enable_position_check) {
        for (uint8_t i = 0; i < cmd->axis_count && i < fb->axis_count; i++) {
            double err = cmd->velocity[i] - fb->velocity[i];
            if (err < 0) err = -err;
            if (err > sirc->cfg.max_velocity_error) {
                new_level = TUXRT_SIRC_LEVEL_SAFE_STOP;
                fault_code = 0x1100 | i;
                snprintf(msg, sizeof(msg), "Velocity error axis %d: %.3f mm/s > %.3f mm/s", 
                        i, err, sirc->cfg.max_velocity_error);
                break;
            }
        }
    }
    
    // Thermal check
    if (new_level == TUXRT_SIRC_LEVEL_NONE && sirc->cfg.enable_thermal_check && temperatures) {
        for (size_t i = 0; i < temp_count; i++) {
            if (temperatures[i] > sirc->cfg.max_temperature) {
                new_level = TUXRT_SIRC_LEVEL_EMERGENCY;
                fault_code = 0x2000 | i;
                snprintf(msg, sizeof(msg), "Thermal runaway sensor %zu: %.1f C > %.1f C",
                        i, temperatures[i], sirc->cfg.max_temperature);
                break;
            }
            if (temperatures[i] < sirc->cfg.min_temperature) {
                if (new_level < TUXRT_SIRC_LEVEL_WARN) {
                    new_level = TUXRT_SIRC_LEVEL_WARN;
                    fault_code = 0x3000 | i;
                    snprintf(msg, sizeof(msg), "Thermal fault sensor %zu: %.1f C < %.1f C",
                            i, temperatures[i], sirc->cfg.min_temperature);
                }
            }
        }
    }
    
    // Watchdog
    if (sirc->cfg.enable_watchdog) {
        uint64_t elapsed = now - sirc->last_kick_ns;
        if (elapsed > sirc->cfg.watchdog_timeout_ns) {
            new_level = TUXRT_SIRC_LEVEL_EMERGENCY;
            fault_code = 0x4000;
            snprintf(msg, sizeof(msg), "Watchdog timeout: %llu ns > %llu ns",
                    (unsigned long long)elapsed, (unsigned long long)sirc->cfg.watchdog_timeout_ns);
        }
    }
    
    // Circuit Breaker check
    if (sirc->cb_state == TUXRT_CB_OPEN) {
        if (new_level == TUXRT_SIRC_LEVEL_NONE) {
            new_level = TUXRT_SIRC_LEVEL_WARN;
            fault_code = 0x5005;
            snprintf(msg, sizeof(msg), "Circuit breaker OPEN: blocking requests");
        }
    }
    
    // Update level and emit event
    if (new_level > sirc->current_level) {
        sirc->current_level = new_level;
        emit_event(sirc, new_level, msg[0] ? msg : "SIRC event", fault_code);
    }
    
    // Safe mode transition based on level
    tuxrt_safe_mode_t target_mode = sirc->safe_mode;
    switch (new_level) {
        case TUXRT_SIRC_LEVEL_NONE:
            if (sirc->safe_mode == TUXRT_SAFE_MODE_N2_DEGRADED ||
                sirc->safe_mode == TUXRT_SAFE_MODE_N5_RECOVERY) {
                target_mode = TUXRT_SAFE_MODE_N1_NORMAL;
            }
            break;
        case TUXRT_SIRC_LEVEL_WARN:
            if (sirc->safe_mode == TUXRT_SAFE_MODE_N1_NORMAL) {
                target_mode = TUXRT_SAFE_MODE_N2_DEGRADED;
            }
            break;
        case TUXRT_SIRC_LEVEL_SAFE_STOP:
            target_mode = TUXRT_SAFE_MODE_N3_SAFE_STOP;
            break;
        case TUXRT_SIRC_LEVEL_EMERGENCY:
            target_mode = TUXRT_SAFE_MODE_N4_EMERGENCY;
            break;
    }
    
    if (target_mode != sirc->safe_mode) {
        safe_mode_transition(sirc, target_mode, msg[0] ? msg : "Level change");
    }
    
    // Update circuit breaker on success/failure
    if (new_level == TUXRT_SIRC_LEVEL_NONE) {
        sirc->cb_consecutive_successes++;
        sirc->cb_consecutive_failures = 0;
        sirc->backoff_attempt = 0;
    } else {
        sirc->cb_consecutive_failures++;
        sirc->cb_consecutive_successes = 0;
        sirc->backoff_attempt++;
        sirc->next_retry_ns = now + tuxrt_backoff_compute(&sirc->cfg.backoff, sirc->backoff_attempt);
    }
    
    cb_update(sirc);
    update_health_on_success(&sirc->aggregate_health);
    
    pthread_mutex_unlock(&sirc->mutex);
    
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_sirc_kick_watchdog(tuxrt_sirc_t* sirc) {
    if (!sirc) return TUXRT_ERR_INVAL;
    sirc->last_kick_ns = now_ns();
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_sirc_get_safe_mode(const tuxrt_sirc_t* sirc, tuxrt_safe_mode_t* out_mode) {
    if (!sirc || !out_mode) return TUXRT_ERR_INVAL;
    pthread_mutex_lock((pthread_mutex_t*)&sirc->mutex);
    *out_mode = sirc->safe_mode;
    pthread_mutex_unlock((pthread_mutex_t*)&sirc->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_sirc_get_health(const tuxrt_sirc_t* sirc, tuxrt_health_stats_t* out_health) {
    if (!sirc || !out_health) return TUXRT_ERR_INVAL;
    pthread_mutex_lock((pthread_mutex_t*)&sirc->mutex);
    *out_health = sirc->aggregate_health;
    pthread_mutex_unlock((pthread_mutex_t*)&sirc->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_sirc_get_transport_stats(const tuxrt_sirc_t* sirc, void* stats_out) {
    if (!sirc || !stats_out) return TUXRT_ERR_INVAL;
    
    // Return active adapter health if available
    pthread_mutex_lock((pthread_mutex_t*)&sirc->mutex);
    if (sirc->active_adapter) {
        pthread_mutex_lock(&sirc->active_adapter->mutex);
        *(tuxrt_health_stats_t*)stats_out = sirc->active_adapter->health;
        pthread_mutex_unlock(&sirc->active_adapter->mutex);
    } else {
        *(tuxrt_health_stats_t*)stats_out = sirc->aggregate_health;
    }
    pthread_mutex_unlock((pthread_mutex_t*)&sirc->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_sirc_register_transport(tuxrt_sirc_t* sirc,
                                           const tuxrt_transport_adapter_config_t* cfg,
                                           const tuxrt_transport_vtable_t* vtable) {
    if (!sirc || !cfg || !vtable) return TUXRT_ERR_INVAL;
    if (!sirc->initialized) return TUXRT_ERR_NOT_INITIALIZED;
    
    pthread_mutex_lock(&sirc->mutex);
    
    tuxrt_transport_adapter_t* adapter = create_adapter(cfg, vtable);
    if (!adapter) {
        pthread_mutex_unlock(&sirc->mutex);
        return TUXRT_ERR_NOMEM;
    }
    
    if (cfg->type == sirc->cfg.primary_transport.type && 
        cfg->device_path && sirc->cfg.primary_transport.device_path &&
        strcmp(cfg->device_path, sirc->cfg.primary_transport.device_path) == 0) {
        // Primary
        destroy_adapter(sirc->primary_adapter);
        sirc->primary_adapter = adapter;
        if (!sirc->active_adapter) sirc->active_adapter = adapter;
    } else if (sirc->cfg.enable_transport_failover &&
               cfg->type == sirc->cfg.fallback_transport.type &&
               cfg->device_path && sirc->cfg.fallback_transport.device_path &&
               strcmp(cfg->device_path, sirc->cfg.fallback_transport.device_path) == 0) {
        // Fallback
        destroy_adapter(sirc->fallback_adapter);
        sirc->fallback_adapter = adapter;
    } else {
        // Additional transport
        destroy_adapter(adapter);
        pthread_mutex_unlock(&sirc->mutex);
        return TUXRT_ERR_INVAL;
    }
    
    pthread_mutex_unlock(&sirc->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_sirc_set_active_transport(tuxrt_sirc_t* sirc, tuxrt_transport_type_t type) {
    if (!sirc) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&sirc->mutex);
    
    tuxrt_transport_adapter_t* target = NULL;
    if (sirc->primary_adapter && sirc->primary_adapter->config.type == type) {
        target = sirc->primary_adapter;
    } else if (sirc->fallback_adapter && sirc->fallback_adapter->config.type == type) {
        target = sirc->fallback_adapter;
    }
    
    if (!target) {
        pthread_mutex_unlock(&sirc->mutex);
        return TUXRT_ERR_NOENT;
    }
    
    // Close current active
    if (sirc->active_adapter && sirc->active_adapter != target) {
        if (sirc->active_adapter->vtable && sirc->active_adapter->vtable->close) {
            sirc->active_adapter->vtable->close(sirc->active_adapter);
        }
        sirc->active_adapter->active = false;
    }
    
    // Open new active
    if (target->vtable && target->vtable->open) {
        tuxrt_err_t err = target->vtable->open(target);
        if (err != TUXRT_OK) {
            pthread_mutex_unlock(&sirc->mutex);
            return err;
        }
    }
    target->active = true;
    sirc->active_adapter = target;
    
    pthread_mutex_unlock(&sirc->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_sirc_trigger_failover(tuxrt_sirc_t* sirc, tuxrt_transport_type_t target) {
    if (!sirc) return TUXRT_ERR_INVAL;
    if (!sirc->cfg.enable_transport_failover) return TUXRT_ERR_NOENT;
    
    return tuxrt_sirc_set_active_transport(sirc, target);
}

tuxrt_err_t tuxrt_sirc_set_policy(tuxrt_sirc_t* sirc, tuxrt_policy_fn_t fn, void* user_data) {
    if (!sirc) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&sirc->mutex);
    sirc->policy_fn = fn ? fn : default_policy;
    sirc->policy_user_data = user_data;
    pthread_mutex_unlock(&sirc->mutex);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_sirc_set_handler(tuxrt_sirc_t* sirc, tuxrt_sirc_handler_t h, void* user_data) {
    if (!sirc) return TUXRT_ERR_INVAL;
    pthread_mutex_lock(&sirc->mutex);
    sirc->handler = h;
    sirc->handler_user_data = user_data;
    pthread_mutex_unlock(&sirc->mutex);
    return TUXRT_OK;
}

void tuxrt_sirc_destroy(tuxrt_sirc_t* sirc) {
    if (!sirc) return;

    pthread_mutex_lock(&sirc->mutex);

    destroy_adapter(sirc->primary_adapter);
    destroy_adapter(sirc->fallback_adapter);

    pthread_mutex_unlock(&sirc->mutex);
    pthread_mutex_destroy(&sirc->mutex);
    free(sirc);
}

/* ------------------------------------------------------------------ */
/* EtherCAT Transport Registration Helper                           */
/* ------------------------------------------------------------------ */
/* EtherCAT vtable is declared extern in tuxrt_sdk.h (line ~667).
 * This function wraps tuxrt_sirc_register_transport() with the vtable. */
tuxrt_err_t tuxrt_sirc_register_ethercat(tuxrt_sirc_t* sirc,
                                        const tuxrt_transport_adapter_config_t* cfg) {
    if (!sirc || !cfg) return TUXRT_ERR_INVAL;
    return tuxrt_sirc_register_transport(sirc, cfg, &tuxrt_transport_ethercat_vtable);
}

/* EtherTux Transport Registration Helper                           */
/* ------------------------------------------------------------------ */
/* EtherTux vtable is declared extern in tuxrt_sdk.h (line ~667).
 * This function wraps tuxrt_sirc_register_transport() with the vtable. */
tuxrt_err_t tuxrt_sirc_register_ethertux(tuxrt_sirc_t* sirc,
                                          const tuxrt_transport_adapter_config_t* cfg) {
    if (!sirc || !cfg) return TUXRT_ERR_INVAL;
    return tuxrt_sirc_register_transport(sirc, cfg, &tuxrt_transport_ethertux_vtable);
}