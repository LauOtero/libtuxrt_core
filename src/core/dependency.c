/**
 * dependency.c — Dependencias opcionales resueltas en tiempo de ejecución
 *
 * libtuxrt_core NO enlaza contra ChronosRT en tiempo de compilación y no debe
 * fallar ni romperse si `libchronosrt.so` no está instalada. La detección se
 * hace sobre el CONTRATO UNIVERSAL DE PROVEEDOR (ver tuxrt_provider.h y
 * src/provider.c): cada dependencia es un proveedor externo que exporta el
 * símbolo `tuxrt_provider_descriptor`. El estado se expone de forma
 * estructurada para que la CLI/TUI/FFI informe al sistema.
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include "../../sdk/include/tuxrt_sdk.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>

// Dependencias opcionales conocidas del núcleo. Cada entrada describe un
// proveedor externo (contrato TuxRT Provider ABI). `feature` indica la
// capacidad del núcleo que la necesita, para un mensaje accionable.
typedef struct {
    const char* name;      // identidad legible ("ChronosRT")
    const char* soname;    // librería a cargar ("libchronosrt.so")
    const char* required;  // versión mínima ("0.1.0")
    const char* feature;   // capacidad que la requiere
} tuxrt_known_dep_t;

static const tuxrt_known_dep_t tuxrt_known_deps[] = {
    { "ChronosRT", "libchronosrt.so", "0.1.0", "el transporte EtherCAT" },
};

#define TUXRT_KNOWN_DEP_COUNT \
    (sizeof(tuxrt_known_deps) / sizeof(tuxrt_known_deps[0]))

// Comparación de nombres insensible a mayúsculas/minúsculas.
static bool tuxrt_dep_name_matches(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static const tuxrt_known_dep_t* tuxrt_known_dep_find(const char* name) {
    for (size_t i = 0; i < TUXRT_KNOWN_DEP_COUNT; ++i) {
        if (tuxrt_dep_name_matches(tuxrt_known_deps[i].name, name)) {
            return &tuxrt_known_deps[i];
        }
    }
    return NULL;
}

tuxrt_err_t tuxrt_dependency_query(const char* name, tuxrt_dependency_t* out) {
    if (!name || !out) return TUXRT_ERR_INVAL;

    memset(out, 0, sizeof(*out));

    const tuxrt_known_dep_t* dep = tuxrt_known_dep_find(name);
    if (!dep) return TUXRT_ERR_NOENT;

    strncpy(out->name, dep->name, sizeof(out->name) - 1);
    strncpy(out->soname, dep->soname, sizeof(out->soname) - 1);
    strncpy(out->required_version, dep->required,
            sizeof(out->required_version) - 1);

    tuxrt_provider_info_t info;
    out->available = (tuxrt_provider_probe(dep->soname, &info) == TUXRT_OK);

    if (out->available) {
        snprintf(out->installed_version, sizeof(out->installed_version), "%s",
                 info.version);
        snprintf(out->hint, sizeof(out->hint), "%s disponible (%s)",
                 dep->name,
                 out->installed_version[0] ? out->installed_version
                                           : "version desconocida");
        return TUXRT_OK;
    }

    snprintf(out->hint, sizeof(out->hint),
             "Falta %s >= %s: instale %s (requerida por %s)",
             dep->name, dep->required, dep->soname, dep->feature);
    return TUXRT_ERR_DEPENDENCY_MISSING;
}

tuxrt_err_t tuxrt_dependency_report(char* buf, size_t len) {
    if (!buf || len == 0) return TUXRT_ERR_INVAL;
    buf[0] = '\0';

    size_t used = 0;
    for (size_t i = 0; i < TUXRT_KNOWN_DEP_COUNT; ++i) {
        tuxrt_dependency_t dep;
        tuxrt_err_t rc = tuxrt_dependency_query(tuxrt_known_deps[i].name, &dep);
        int n;
        if (rc == TUXRT_OK) {
            n = snprintf(buf + used, len - used, "%s%s: OK (%s)",
                         used ? "\n" : "", dep.name,
                         dep.installed_version[0] ? dep.installed_version
                                                  : "version desconocida");
        } else {
            n = snprintf(buf + used, len - used,
                         "%s%s: NO INSTALADA (requerida >= %s). %s",
                         used ? "\n" : "", dep.name, dep.required_version,
                         dep.hint);
        }
        if (n < 0) break;
        used += (size_t)n;
        if (used >= len) break;
    }
    return TUXRT_OK;
}
