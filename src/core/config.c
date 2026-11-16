/**
 * config.c — Configuration Store (CORE)
 *
 * Implementación del API de configuración declarado en tuxrt_sdk.h:
 * store de pares clave/valor cargable desde archivo INI extendido,
 * con getters tipados (int/double/bool/string/vec3) y setters.
 *
 * Requerido por kares.c (tuxrt_config_get_double) y por el resto de
 * módulos CORE que reciben un tuxrt_config_t*.
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include "../../sdk/include/tuxrt_sdk.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define CFG_MAX_ENTRIES 512

typedef struct {
    char* key;
    char* value;
} cfg_entry_t;

struct tuxrt_config_t {
    cfg_entry_t entries[CFG_MAX_ENTRIES];
    size_t count;
    bool read_only;   // true si fue cargado desde archivo
};

// -----------------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------------

static char* cfg_strdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static cfg_entry_t* cfg_find(const struct tuxrt_config_t* cfg, const char* key) {
    for (size_t i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0) return (cfg_entry_t*)&cfg->entries[i];
    }
    return NULL;
}

static tuxrt_err_t cfg_set_raw(struct tuxrt_config_t* cfg, const char* key, const char* value) {
    cfg_entry_t* e = cfg_find(cfg, key);
    if (e) {
        char* nv = cfg_strdup(value);
        if (!nv) return TUXRT_ERR_NOMEM;
        free(e->value);
        e->value = nv;
        return TUXRT_OK;
    }
    if (cfg->count >= CFG_MAX_ENTRIES) return TUXRT_ERR_NOMEM;
    char* nk = cfg_strdup(key);
    char* nv = cfg_strdup(value);
    if (!nk || !nv) {
        free(nk);
        free(nv);
        return TUXRT_ERR_NOMEM;
    }
    cfg->entries[cfg->count].key = nk;
    cfg->entries[cfg->count].value = nv;
    cfg->count++;
    return TUXRT_OK;
}

static char* cfg_trim(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char* end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

static bool cfg_parse_bool(const char* v, bool* out) {
    if (!v) return false;
    if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
        strcmp(v, "yes") == 0 || strcmp(v, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 ||
        strcmp(v, "no") == 0 || strcmp(v, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

tuxrt_err_t tuxrt_config_create(tuxrt_config_t** out_config) {
    if (!out_config) return TUXRT_ERR_INVAL;
    struct tuxrt_config_t* cfg = calloc(1, sizeof(struct tuxrt_config_t));
    if (!cfg) return TUXRT_ERR_NOMEM;
    cfg->read_only = false;
    *out_config = cfg;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_config_load(const char* path, tuxrt_config_t** out_config) {
    if (!path || !out_config) return TUXRT_ERR_INVAL;

    FILE* f = fopen(path, "r");
    if (!f) return TUXRT_ERR_NOENT;

    struct tuxrt_config_t* cfg = calloc(1, sizeof(struct tuxrt_config_t));
    if (!cfg) {
        fclose(f);
        return TUXRT_ERR_NOMEM;
    }

    char line[512];
    char section[128] = "";
    while (fgets(line, sizeof(line), f)) {
        char* s = cfg_trim(line);
        if (*s == '\0' || *s == '#' || *s == ';') continue;

        if (*s == '[') {
            char* end = strchr(s, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", cfg_trim(s + 1));
            }
            continue;
        }

        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = cfg_trim(s);
        char* value = cfg_trim(eq + 1);

        char full_key[256];
        if (section[0] != '\0') {
            snprintf(full_key, sizeof(full_key), "%s.%s", section, key);
        } else {
            snprintf(full_key, sizeof(full_key), "%s", key);
        }

        if (cfg_set_raw(cfg, full_key, value) != TUXRT_OK) {
            fclose(f);
            tuxrt_config_destroy(cfg);
            return TUXRT_ERR_NOMEM;
        }
    }
    fclose(f);

    cfg->read_only = true;
    *out_config = cfg;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_config_clone(const tuxrt_config_t* src, tuxrt_config_t** out_config) {
    if (!src || !out_config) return TUXRT_ERR_INVAL;
    struct tuxrt_config_t* cfg = calloc(1, sizeof(struct tuxrt_config_t));
    if (!cfg) return TUXRT_ERR_NOMEM;
    for (size_t i = 0; i < src->count; i++) {
        if (cfg_set_raw(cfg, src->entries[i].key, src->entries[i].value) != TUXRT_OK) {
            tuxrt_config_destroy(cfg);
            return TUXRT_ERR_NOMEM;
        }
    }
    cfg->read_only = src->read_only;
    *out_config = cfg;
    return TUXRT_OK;
}

void tuxrt_config_destroy(tuxrt_config_t* config) {
    if (!config) return;
    for (size_t i = 0; i < config->count; i++) {
        free(config->entries[i].key);
        free(config->entries[i].value);
    }
    free(config);
}

// -----------------------------------------------------------------------------
// Getters
// -----------------------------------------------------------------------------

tuxrt_err_t tuxrt_config_get_int(const tuxrt_config_t* cfg, const char* key, int64_t* out) {
    if (!cfg || !key || !out) return TUXRT_ERR_INVAL;
    cfg_entry_t* e = cfg_find(cfg, key);
    if (!e) return TUXRT_ERR_NOENT;
    char* end = NULL;
    long long v = strtoll(e->value, &end, 0);
    if (end == e->value) return TUXRT_ERR_INVAL;
    *out = (int64_t)v;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_config_get_double(const tuxrt_config_t* cfg, const char* key, double* out) {
    if (!cfg || !key || !out) return TUXRT_ERR_INVAL;
    cfg_entry_t* e = cfg_find(cfg, key);
    if (!e) return TUXRT_ERR_NOENT;
    char* end = NULL;
    double v = strtod(e->value, &end);
    if (end == e->value) return TUXRT_ERR_INVAL;
    *out = v;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_config_get_bool(const tuxrt_config_t* cfg, const char* key, bool* out) {
    if (!cfg || !key || !out) return TUXRT_ERR_INVAL;
    cfg_entry_t* e = cfg_find(cfg, key);
    if (!e) return TUXRT_ERR_NOENT;
    if (!cfg_parse_bool(e->value, out)) return TUXRT_ERR_INVAL;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_config_get_string(const tuxrt_config_t* cfg, const char* key, const char** out) {
    if (!cfg || !key || !out) return TUXRT_ERR_INVAL;
    cfg_entry_t* e = cfg_find(cfg, key);
    if (!e) return TUXRT_ERR_NOENT;
    *out = e->value;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_config_get_vec3(const tuxrt_config_t* cfg, const char* key, tuxrt_vec3_t* out) {
    if (!cfg || !key || !out) return TUXRT_ERR_INVAL;
    cfg_entry_t* e = cfg_find(cfg, key);
    if (!e) return TUXRT_ERR_NOENT;
    double x = 0.0, y = 0.0, z = 0.0;
    if (sscanf(e->value, "%lf,%lf,%lf", &x, &y, &z) < 3) return TUXRT_ERR_INVAL;
    out->x = x;
    out->y = y;
    out->z = z;
    return TUXRT_OK;
}

// -----------------------------------------------------------------------------
// Setters (rechazados en configs cargadas desde archivo)
// -----------------------------------------------------------------------------

static tuxrt_err_t cfg_ready_set(struct tuxrt_config_t* cfg) {
    if (!cfg) return TUXRT_ERR_INVAL;
    if (cfg->read_only) return TUXRT_ERR_CONFIG_INVALID;
    return TUXRT_OK;
}

tuxrt_err_t tuxrt_config_set_int(tuxrt_config_t* cfg, const char* key, int64_t val) {
    tuxrt_err_t rc = cfg_ready_set(cfg);
    if (rc != TUXRT_OK) return rc;
    if (!key) return TUXRT_ERR_INVAL;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)val);
    return cfg_set_raw(cfg, key, buf);
}

tuxrt_err_t tuxrt_config_set_double(tuxrt_config_t* cfg, const char* key, double val) {
    tuxrt_err_t rc = cfg_ready_set(cfg);
    if (rc != TUXRT_OK) return rc;
    if (!key) return TUXRT_ERR_INVAL;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", val);
    return cfg_set_raw(cfg, key, buf);
}

tuxrt_err_t tuxrt_config_set_bool(tuxrt_config_t* cfg, const char* key, bool val) {
    tuxrt_err_t rc = cfg_ready_set(cfg);
    if (rc != TUXRT_OK) return rc;
    if (!key) return TUXRT_ERR_INVAL;
    return cfg_set_raw(cfg, key, val ? "true" : "false");
}

tuxrt_err_t tuxrt_config_set_string(tuxrt_config_t* cfg, const char* key, const char* val) {
    tuxrt_err_t rc = cfg_ready_set(cfg);
    if (rc != TUXRT_OK) return rc;
    if (!key || !val) return TUXRT_ERR_INVAL;
    return cfg_set_raw(cfg, key, val);
}
