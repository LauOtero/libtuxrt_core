/**
 * build_system.c — Build System In-Situ
 * 
 * Build ≤45min, rollback ≤60s, symlink-based autoload
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include "../../sdk/include/tuxrt_sdk.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <limits.h>
#include <time.h>
#include <stdio.h>
#include <stdarg.h>

#define TUXRT_BUILD_MAX_PLUGINS 64
#define TUXRT_BUILD_DIR "build"
#define TUXRT_BUILD_PLUGIN_DIR "plugins"
#define TUXRT_BUILD_CURRENT_LINK "current"
#define TUXRT_BUILD_BACKUP_LINK "backup"

typedef struct {
    char name[128];
    char path[PATH_MAX];
    time_t build_time;
    bool active;
} tuxrt_build_plugin_t;

typedef struct {
    tuxrt_build_plugin_t plugins[TUXRT_BUILD_MAX_PLUGINS];
    size_t plugin_count;
    char build_root[PATH_MAX];
    pthread_mutex_t mutex;
    bool initialized;
} tuxrt_build_system_t;

// Create build system manager
tuxrt_err_t tuxrt_build_system_create(tuxrt_build_system_t** out_mgr) {
    if (!out_mgr) return TUXRT_ERR_INVAL;
    
    tuxrt_build_system_t* mgr = (tuxrt_build_system_t*)calloc(1, sizeof(tuxrt_build_system_t));
    if (!mgr) return TUXRT_ERR_NOMEM;
    
    if (pthread_mutex_init(&mgr->mutex, NULL) != 0) {
        free(mgr);
        return TUXRT_ERR_IO;
    }
    
    const char* build_root_env = getenv("TUXRT_BUILD_ROOT");
    if (build_root_env && build_root_env[0]) {
        snprintf(mgr->build_root, PATH_MAX, "%s/%s", build_root_env, TUXRT_BUILD_DIR);
    } else {
        snprintf(mgr->build_root, PATH_MAX, "./%s", TUXRT_BUILD_DIR);
    }
    
    mgr->initialized = true;
    *out_mgr = mgr;
    return TUXRT_OK;
}

// Destroy manager
tuxrt_err_t tuxrt_build_system_destroy(tuxrt_build_system_t* mgr) {
    if (!mgr) return TUXRT_ERR_INVAL;
    pthread_mutex_destroy(&mgr->mutex);
    free(mgr);
    return TUXRT_OK;
}

// Create symlink for plugin autoload
static tuxrt_err_t tuxrt_build_system_create_symlink(const char* target, const char* linkpath) {
    unlink(linkpath);
    if (symlink(target, linkpath) != 0) {
        return TUXRT_ERR_IO;
    }
    return TUXRT_OK;
}

// Rollback to previous build
tuxrt_err_t tuxrt_build_system_rollback(tuxrt_build_system_t* mgr) {
    if (!mgr || !mgr->initialized) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&mgr->mutex);

    char* backup_path = NULL;
    char* current_path = NULL;
    if (asprintf(&backup_path, "%s/%s", mgr->build_root, TUXRT_BUILD_BACKUP_LINK) < 0 ||
        asprintf(&current_path, "%s/%s", mgr->build_root, TUXRT_BUILD_CURRENT_LINK) < 0) {
        free(backup_path);
        free(current_path);
        pthread_mutex_unlock(&mgr->mutex);
        return TUXRT_ERR_NOMEM;
    }

    // Atomic switch with symlink
    char* tmp_link = NULL;
    if (asprintf(&tmp_link, "%s/.tmp_link", mgr->build_root) < 0) {
        free(backup_path);
        free(current_path);
        pthread_mutex_unlock(&mgr->mutex);
        return TUXRT_ERR_NOMEM;
    }

    tuxrt_err_t err = TUXRT_OK;
    if (symlink(backup_path, tmp_link) == 0) {
        rename(tmp_link, current_path);
        unlink(tmp_link);
    } else {
        err = TUXRT_ERR_IO;
    }

    free(backup_path);
    free(current_path);
    free(tmp_link);

    pthread_mutex_unlock(&mgr->mutex);
    return err;
}

// Scan plugins directory for autoload
tuxrt_err_t tuxrt_build_system_scan_plugins(tuxrt_build_system_t* mgr) {
    if (!mgr || !mgr->initialized) return TUXRT_ERR_INVAL;

    pthread_mutex_lock(&mgr->mutex);

    char* plugin_dir = NULL;
    if (asprintf(&plugin_dir, "%s/%s", mgr->build_root, TUXRT_BUILD_PLUGIN_DIR) < 0) {
        pthread_mutex_unlock(&mgr->mutex);
        return TUXRT_ERR_NOMEM;
    }

    DIR* dir = opendir(plugin_dir);
    if (!dir) {
        free(plugin_dir);
        pthread_mutex_unlock(&mgr->mutex);
        return TUXRT_ERR_IO;
    }

    mgr->plugin_count = 0;
    struct dirent* entry;

    while ((entry = readdir(dir)) != NULL && mgr->plugin_count < TUXRT_BUILD_MAX_PLUGINS) {
        if (entry->d_name[0] == '.') continue;

        char* full_path = NULL;
        if (asprintf(&full_path, "%s/%s", plugin_dir, entry->d_name) < 0) continue;

        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
            tuxrt_build_plugin_t* p = &mgr->plugins[mgr->plugin_count++];
            size_t name_len = strlen(entry->d_name);
            if (name_len >= sizeof(p->name)) name_len = sizeof(p->name) - 1;
            memcpy(p->name, entry->d_name, name_len);
            p->name[name_len] = '\0';
            size_t path_len = strlen(full_path);
            if (path_len >= sizeof(p->path)) path_len = sizeof(p->path) - 1;
            memcpy(p->path, full_path, path_len);
            p->path[path_len] = '\0';
            p->build_time = st.st_mtime;
            p->active = true;
        }
        free(full_path);
    }

    free(plugin_dir);
    closedir(dir);
    pthread_mutex_unlock(&mgr->mutex);

    return TUXRT_OK;
}

// Build plugin with time limit
tuxrt_err_t tuxrt_build_system_build_plugin(tuxrt_build_system_t* mgr, const char* plugin_name, 
                                       uint32_t timeout_seconds) {
    if (!mgr || !plugin_name) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&mgr->mutex);
    
    // Simulate build with timeout check
    // Real build would invoke cmake/make here
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    // Build simulation (replace with actual build commands)
    char build_cmd[PATH_MAX * 2];
    snprintf(build_cmd, sizeof(build_cmd), 
             "cd %s && cmake --build . --target %s", 
             mgr->build_root, plugin_name);
    
    // Check timeout
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t elapsed = (now.tv_sec - start.tv_sec);
    
    if (elapsed > timeout_seconds) {
        pthread_mutex_unlock(&mgr->mutex);
        return TUXRT_ERR_TIMEDOUT;
    }
    
    pthread_mutex_unlock(&mgr->mutex);
    return TUXRT_OK;
}

// Get build stats
tuxrt_err_t tuxrt_build_system_get_stats(tuxrt_build_system_t* mgr, uint32_t* plugin_count, 
                                    uint64_t* last_build_time) {
    if (!mgr || !plugin_count) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&mgr->mutex);
    *plugin_count = mgr->plugin_count;
    *last_build_time = 0;
    
    time_t latest = 0;
    for (size_t i = 0; i < mgr->plugin_count; i++) {
        if (mgr->plugins[i].build_time > latest) {
            latest = mgr->plugins[i].build_time;
        }
    }
    
    *last_build_time = (uint64_t)latest;
    pthread_mutex_unlock(&mgr->mutex);
    
    return TUXRT_OK;
}

// =============================================================================
// Metadatos de versión del SDK
// -----------------------------------------------------------------------------
// `tuxrt_sdk_version()` está declarado en tuxrt_sdk.h y consumido por la FFI
// Mojo (sdk/src/tuxrt_ffi.mojo) y por consumidores externos (p. ej. ChronosRT),
// pero no tenía implementación. Se materializa aquí con datos provistos por
// CMake en tiempo de compilación (commit git, versión de Mojo) y la fecha de
// compilación del propio objeto.
// =============================================================================
#ifndef TUXRT_GIT_COMMIT
#define TUXRT_GIT_COMMIT "unknown"
#endif
#ifndef TUXRT_BUILD_DATE
#define TUXRT_BUILD_DATE __DATE__ " " __TIME__
#endif
#ifndef TUXRT_MOJO_VERSION
#define TUXRT_MOJO_VERSION "unknown"
#endif

const tuxrt_sdk_version_t* tuxrt_sdk_version(void) {
    static const tuxrt_sdk_version_t version = {
        TUXRT_SDK_VERSION_MAJOR,
        TUXRT_SDK_VERSION_MINOR,
        TUXRT_SDK_VERSION_PATCH,
        TUXRT_SDK_VERSION_STRING,
        TUXRT_GIT_COMMIT,
        TUXRT_BUILD_DATE,
        TUXRT_MOJO_VERSION
    };
    return &version;
}
