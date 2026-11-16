/**
 * autocfg.c — AUTOCFG Auto-configuration System
 * 
 * Auto-configuration with validation (CFGVAL), results (CFGRES) and tracing (CFGTRC)
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include "../../sdk/include/tuxrt_sdk.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

#define TUXRT_AUTOCFG_MAX_KEYS 256
#define TUXRT_AUTOCFG_MAX_TRACES 1024
#define TUXRT_AUTOCFG_KEY_LEN 64
#define TUXRT_AUTOCFG_VAL_LEN 512

typedef struct {
    char key[TUXRT_AUTOCFG_KEY_LEN];
    char value[TUXRT_AUTOCFG_VAL_LEN];
    bool set;
} tuxrt_cfg_entry_t;

typedef struct {
    tuxrt_cfgval_rule_t* rules;
    size_t rule_count;
    size_t rule_capacity;
} tuxrt_cfgval_t;

struct tuxrt_autocfg_t {
    tuxrt_cfg_entry_t entries[TUXRT_AUTOCFG_MAX_KEYS];
    size_t entry_count;
    
    tuxrt_cfgval_t validator;
    
    tuxrt_cfgtrc_entry_t traces[TUXRT_AUTOCFG_MAX_TRACES];
    size_t trace_count;
    bool trace_enabled;
    
    pthread_mutex_t mutex;
};

// Helper: get timestamp
static tuxrt_timestamp_ns_t tuxrt_get_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (tuxrt_timestamp_ns_t)(ts.tv_sec * 1000000000ULL + ts.tv_nsec);
}

// Helper: add trace entry
static void tuxrt_autocfg_trace(tuxrt_autocfg_t* cfg, const char* key, const char* source, 
                                 const char* old_val, const char* new_val, 
                                 tuxrt_cfgval_result_t validation, const char* message) {
    if (!cfg->trace_enabled) return;
    
    pthread_mutex_lock(&cfg->mutex);
    
    if (cfg->trace_count < TUXRT_AUTOCFG_MAX_TRACES) {
        tuxrt_cfgtrc_entry_t* entry = &cfg->traces[cfg->trace_count++];
        entry->timestamp = tuxrt_get_timestamp_ns();
        entry->key = key;
        entry->source = source;
        entry->old_value = old_val;
        entry->new_value = new_val;
        entry->validation = validation;
        entry->message = message;
    }
    
    pthread_mutex_unlock(&cfg->mutex);
}

// Create auto-config manager
tuxrt_err_t tuxrt_autocfg_create(tuxrt_autocfg_t** out_cfg) {
    if (!out_cfg) return TUXRT_ERR_INVAL;
    
    tuxrt_autocfg_t* cfg = (tuxrt_autocfg_t*)calloc(1, sizeof(tuxrt_autocfg_t));
    if (!cfg) return TUXRT_ERR_NOMEM;
    
    if (pthread_mutex_init(&cfg->mutex, NULL) != 0) {
        free(cfg);
        return TUXRT_ERR_IO;
    }
    
    cfg->validator.rule_capacity = 16;
    cfg->validator.rules = (tuxrt_cfgval_rule_t*)calloc(cfg->validator.rule_capacity, sizeof(tuxrt_cfgval_rule_t));
    if (!cfg->validator.rules) {
        pthread_mutex_destroy(&cfg->mutex);
        free(cfg);
        return TUXRT_ERR_NOMEM;
    }
    
    cfg->trace_enabled = false;
    *out_cfg = cfg;
    return TUXRT_OK;
}

// Destroy auto-config manager
tuxrt_err_t tuxrt_autocfg_destroy(tuxrt_autocfg_t* cfg) {
    if (!cfg) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&cfg->mutex);
    free(cfg->validator.rules);
    pthread_mutex_unlock(&cfg->mutex);
    pthread_mutex_destroy(&cfg->mutex);
    free(cfg);
    
    return TUXRT_OK;
}

// Add validation rule
tuxrt_err_t tuxrt_cfgval_add_rule(tuxrt_autocfg_t* cfg, const tuxrt_cfgval_rule_t* rule) {
    if (!cfg || !rule || !rule->key) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&cfg->mutex);
    
    if (cfg->validator.rule_count >= cfg->validator.rule_capacity) {
        size_t new_cap = cfg->validator.rule_capacity * 2;
        tuxrt_cfgval_rule_t* new_rules = (tuxrt_cfgval_rule_t*)realloc(
            cfg->validator.rules, new_cap * sizeof(tuxrt_cfgval_rule_t)
        );
        if (!new_rules) {
            pthread_mutex_unlock(&cfg->mutex);
            return TUXRT_ERR_NOMEM;
        }
        cfg->validator.rules = new_rules;
        cfg->validator.rule_capacity = new_cap;
    }
    
    cfg->validator.rules[cfg->validator.rule_count++] = *rule;
    pthread_mutex_unlock(&cfg->mutex);
    
    return TUXRT_OK;
}

// Validate a single key-value pair
tuxrt_err_t tuxrt_cfgval_validate_key(tuxrt_autocfg_t* cfg, const char* key, const char* value, tuxrt_cfgval_result_t* out_result) {
    if (!cfg || !key || !out_result) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&cfg->mutex);
    
    // Find rule for this key
    tuxrt_cfgval_rule_t* rule = NULL;
    for (size_t i = 0; i < cfg->validator.rule_count; i++) {
        if (strcmp(cfg->validator.rules[i].key, key) == 0) {
            rule = &cfg->validator.rules[i];
            break;
        }
    }
    
    // If no rule found, validation passes
    if (!rule) {
        *out_result = TUXRT_CFGVAL_OK;
        pthread_mutex_unlock(&cfg->mutex);
        return TUXRT_OK;
    }
    
    tuxrt_cfgval_result_t result = TUXRT_CFGVAL_OK;
    
    // Check required
    if (rule->required && (!value || strlen(value) == 0)) {
        result = TUXRT_CFGVAL_FATAL;
    }
    // Check min/max if numeric
    else if (rule->min_value && value) {
        long val = strtol(value, NULL, 10);
        long min = strtol(rule->min_value, NULL, 10);
        if (val < min) result = TUXRT_CFGVAL_ERROR;
    }
    else if (rule->max_value && value) {
        long val = strtol(value, NULL, 10);
        long max = strtol(rule->max_value, NULL, 10);
        if (val > max) result = TUXRT_CFGVAL_ERROR;
    }
    // Check allowed values
    else if (rule->allowed_values && value) {
        char* allowed_copy = strdup(rule->allowed_values);
        char* token = strtok(allowed_copy, ",");
        bool found = false;
        while (token) {
            if (strcmp(token, value) == 0) {
                found = true;
                break;
            }
            token = strtok(NULL, ",");
        }
        free(allowed_copy);
        
        if (!found) result = TUXRT_CFGVAL_ERROR;
    }
    
    *out_result = result;
    pthread_mutex_unlock(&cfg->mutex);
    return TUXRT_OK;
}

// Validate all configuration
tuxrt_err_t tuxrt_cfgval_validate_all(tuxrt_autocfg_t* cfg, tuxrt_cfgval_result_t* out_result) {
    if (!cfg || !out_result) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&cfg->mutex);
    
    tuxrt_cfgval_result_t worst_result = TUXRT_CFGVAL_OK;
    
    for (size_t i = 0; i < cfg->entry_count; i++) {
        tuxrt_cfg_entry_t* entry = &cfg->entries[i];
        if (!entry->set) continue;
        
        tuxrt_cfgval_result_t result;
        tuxrt_err_t err = tuxrt_cfgval_validate_key(cfg, entry->key, entry->value, &result);
        if (err == TUXRT_OK && result > worst_result) {
            worst_result = result;
        }
    }
    
    *out_result = worst_result;
    pthread_mutex_unlock(&cfg->mutex);
    return TUXRT_OK;
}

// Load default values
tuxrt_err_t tuxrt_autocfg_load_defaults(tuxrt_autocfg_t* cfg) {
    if (!cfg) return TUXRT_ERR_INVAL;
    
    // Default configuration values
    const char* defaults[][2] = {
        {"tuxrt.core.tick_rate", "1000"},
        {"tuxrt.core.heartbeat_interval", "5000"},
        {"tuxrt.core.max_joints", "32"},
        {"tuxrt.sirc.safe_mode", "N1"},
        {"tuxrt.sirc.circuit_breaker_threshold", "5"},
        {NULL, NULL}
    };
    
    for (int i = 0; defaults[i][0] != NULL; i++) {
        tuxrt_autocfg_set(cfg, defaults[i][0], defaults[i][1]);
        tuxrt_autocfg_trace(cfg, defaults[i][0], "default", NULL, defaults[i][1], TUXRT_CFGVAL_OK, "default value loaded");
    }
    
    return TUXRT_OK;
}

// Load from file (INI-style simple format)
tuxrt_err_t tuxrt_autocfg_load_file(tuxrt_autocfg_t* cfg, const char* path) {
    if (!cfg || !path) return TUXRT_ERR_INVAL;
    
    FILE* f = fopen(path, "r");
    if (!f) return TUXRT_ERR_NOENT;
    
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        // Skip comments and empty lines
        char* trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        if (*trimmed == '#' || *trimmed == ';' || *trimmed == '\n' || *trimmed == '\0') continue;
        
        // Parse key=value
        char* eq = strchr(trimmed, '=');
        if (!eq) continue;
        
        *eq = '\0';
        char* key = trimmed;
        char* value = eq + 1;
        
        // Trim whitespace
        char* end = key + strlen(key) - 1;
        while (end > key && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) *end-- = '\0';
        
        end = value + strlen(value) - 1;
        while (end > value && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) *end-- = '\0';
        
        char old_val[TUXRT_AUTOCFG_VAL_LEN] = {0};
        const char* current = tuxrt_autocfg_get(cfg, key);
        if (current) strncpy(old_val, current, sizeof(old_val) - 1);
        
        tuxrt_autocfg_set(cfg, key, value);
        tuxrt_autocfg_trace(cfg, key, "file", old_val[0] ? old_val : NULL, value, TUXRT_CFGVAL_OK, "loaded from file");
    }
    
    fclose(f);
    return TUXRT_OK;
}

// Load from environment variables
tuxrt_err_t tuxrt_autocfg_load_env(tuxrt_autocfg_t* cfg) {
    if (!cfg) return TUXRT_ERR_INVAL;
    
    // Load all TUXRT_* environment variables
    extern char** environ;
    for (char** env = environ; *env != NULL; env++) {
        char* eq = strchr(*env, '=');
        if (!eq) continue;
        
        *eq = '\0';
        const char* key = *env;
        const char* value = eq + 1;
        
        if (strncmp(key, "TUXRT_", 6) == 0) {
            char tuxrt_key[TUXRT_AUTOCFG_KEY_LEN];
            snprintf(tuxrt_key, sizeof(tuxrt_key), "tuxrt.%s", key + 6);
            
            char old_val[TUXRT_AUTOCFG_VAL_LEN] = {0};
            const char* current = tuxrt_autocfg_get(cfg, tuxrt_key);
            if (current) strncpy(old_val, current, sizeof(old_val) - 1);
            
            tuxrt_autocfg_set(cfg, tuxrt_key, value);
            tuxrt_autocfg_trace(cfg, tuxrt_key, "env", old_val[0] ? old_val : NULL, value, TUXRT_CFGVAL_OK, "loaded from env");
        }
    }
    
    return TUXRT_OK;
}

// Load from CLI arguments
tuxrt_err_t tuxrt_autocfg_load_cli(tuxrt_autocfg_t* cfg, int argc, char** argv) {
    if (!cfg || argc < 2 || !argv) return TUXRT_OK;
    
    for (int i = 1; i < argc; i++) {
        char* arg = argv[i];
        if (strncmp(arg, "--", 2) == 0) {
            char* eq = strchr(arg + 2, '=');
            if (eq) {
                *eq = '\0';
                char* key = arg + 2;
                char* value = eq + 1;
                
                char tuxrt_key[TUXRT_AUTOCFG_KEY_LEN];
                snprintf(tuxrt_key, sizeof(tuxrt_key), "tuxrt.%s", key);
                
                char old_val[TUXRT_AUTOCFG_VAL_LEN] = {0};
                const char* current = tuxrt_autocfg_get(cfg, tuxrt_key);
                if (current) strncpy(old_val, current, sizeof(old_val) - 1);
                
                tuxrt_autocfg_set(cfg, tuxrt_key, value);
                tuxrt_autocfg_trace(cfg, tuxrt_key, "cli", old_val[0] ? old_val : NULL, value, TUXRT_CFGVAL_OK, "loaded from CLI");
            }
        }
    }
    
    return TUXRT_OK;
}

// Enable/disable tracing
tuxrt_err_t tuxrt_cfgtrc_enable(tuxrt_autocfg_t* cfg, bool enable) {
    if (!cfg) return TUXRT_ERR_INVAL;
    cfg->trace_enabled = enable;
    return TUXRT_OK;
}

// Get trace entries
tuxrt_err_t tuxrt_cfgtrc_get_entries(tuxrt_autocfg_t* cfg, tuxrt_cfgtrc_entry_t** out_entries, size_t* out_count) {
    if (!cfg || !out_entries || !out_count) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&cfg->mutex);
    *out_entries = cfg->traces;
    *out_count = cfg->trace_count;
    pthread_mutex_unlock(&cfg->mutex);
    
    return TUXRT_OK;
}

// Clear traces
tuxrt_err_t tuxrt_cfgtrc_clear(tuxrt_autocfg_t* cfg) {
    if (!cfg) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&cfg->mutex);
    cfg->trace_count = 0;
    memset(cfg->traces, 0, sizeof(cfg->traces));
    pthread_mutex_unlock(&cfg->mutex);
    
    return TUXRT_OK;
}

// Get config value
const char* tuxrt_autocfg_get(const tuxrt_autocfg_t* cfg, const char* key) {
    if (!cfg || !key) return NULL;
    
    pthread_mutex_lock((pthread_mutex_t*)&cfg->mutex);
    
    for (size_t i = 0; i < cfg->entry_count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0 && cfg->entries[i].set) {
            const char* val = cfg->entries[i].value;
            pthread_mutex_unlock((pthread_mutex_t*)&cfg->mutex);
            return val;
        }
    }
    
    pthread_mutex_unlock((pthread_mutex_t*)&cfg->mutex);
    return NULL;
}

// Set config value
tuxrt_err_t tuxrt_autocfg_set(tuxrt_autocfg_t* cfg, const char* key, const char* value) {
    if (!cfg || !key || !value) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&cfg->mutex);
    
    // Find existing entry
    size_t idx = cfg->entry_count;
    for (size_t i = 0; i < cfg->entry_count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0) {
            idx = i;
            break;
        }
    }
    
    // Create new entry if needed
    if (idx >= cfg->entry_count) {
        if (cfg->entry_count >= TUXRT_AUTOCFG_MAX_KEYS) {
            pthread_mutex_unlock(&cfg->mutex);
            return TUXRT_ERR_NOMEM;
        }
        idx = cfg->entry_count++;
    }
    
    // Store old value for tracing
    char old_val[TUXRT_AUTOCFG_VAL_LEN] = {0};
    if (cfg->entries[idx].set) {
        strncpy(old_val, cfg->entries[idx].value, sizeof(old_val) - 1);
    }
    
    // Update entry
    strncpy(cfg->entries[idx].key, key, TUXRT_AUTOCFG_KEY_LEN - 1);
    strncpy(cfg->entries[idx].value, value, TUXRT_AUTOCFG_VAL_LEN - 1);
    cfg->entries[idx].set = true;
    
    pthread_mutex_unlock(&cfg->mutex);
    return TUXRT_OK;
}

// Config result functions
tuxrt_err_t tuxrt_cfgres_create(tuxrt_cfgres_t** out_res) {
    if (!out_res) return TUXRT_ERR_INVAL;
    *out_res = (tuxrt_cfgres_t*)calloc(1, sizeof(tuxrt_cfgres_t));
    return *out_res ? TUXRT_OK : TUXRT_ERR_NOMEM;
}

tuxrt_err_t tuxrt_cfgres_destroy(tuxrt_cfgres_t* res) {
    if (!res) return TUXRT_ERR_INVAL;
    free(res->data);
    free(res);
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_cfgres_set_data(tuxrt_cfgres_t* res, const void* data, size_t size) {
    if (!res || !data) return TUXRT_ERR_INVAL;
    res->data = (char*)malloc(size);
    if (!res->data) return TUXRT_ERR_NOMEM;
    memcpy(res->data, data, size);
    res->size = size;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_cfgres_set_result(tuxrt_cfgres_t* res, tuxrt_cfgval_result_t result, const char* message) {
    if (!res) return TUXRT_ERR_INVAL;
    res->result = result;
    res->error_message = message;
    return TUXRT_OK;
}
