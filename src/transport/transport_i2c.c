/**
 * transport_i2c.c — I2C Transport Adapter (Linux i2c-dev backend)
 * 
 * Implements tuxrt_transport_vtable_t for I2C communication
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include "../../sdk/include/tuxrt_sdk.h"

typedef struct {
    int fd;
    uint16_t addr;
    pthread_mutex_t mutex;
    bool configured;
} i2c_transport_impl_t;

static tuxrt_err_t i2c_open(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return TUXRT_ERR_INVAL;
    
    i2c_transport_impl_t* impl = calloc(1, sizeof(i2c_transport_impl_t));
    if (!impl) return TUXRT_ERR_NOMEM;
    
    pthread_mutex_init(&impl->mutex, NULL);
    
    // I2C device path format: /dev/i2c-X@0xYY
    char bus_path[64] = {0};
    uint16_t addr = 0;
    if (adapter->config.device_path) {
        char* at = strchr(adapter->config.device_path, '@');
        if (at) {
            strncpy(bus_path, adapter->config.device_path, at - adapter->config.device_path);
            sscanf(at + 1, "%hx", &addr);
        } else {
            strncpy(bus_path, adapter->config.device_path, sizeof(bus_path) - 1);
        }
    }
    
    impl->fd = open(bus_path, O_RDWR);
    if (impl->fd < 0) {
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return TUXRT_ERR_NOENT;
    }
    
    if (addr) {
        if (ioctl(impl->fd, I2C_SLAVE, addr) < 0) {
            close(impl->fd);
            pthread_mutex_destroy(&impl->mutex);
            free(impl);
            return TUXRT_ERR_INVAL;
        }
        impl->addr = addr;
    }
    
    impl->configured = true;
    adapter->impl = impl;
    adapter->active = true;
    
    tuxrt_sirc_update_health_success(adapter);
    
    return TUXRT_OK;
}

static tuxrt_err_t i2c_close(tuxrt_transport_adapter_t* adapter) {
    if (!adapter || !adapter->impl) return TUXRT_ERR_INVAL;
    
    i2c_transport_impl_t* impl = (i2c_transport_impl_t*)adapter->impl;
    
    pthread_mutex_lock(&impl->mutex);
    
    if (impl->configured) {
        close(impl->fd);
        impl->fd = -1;
        impl->configured = false;
    }
    
    pthread_mutex_unlock(&impl->mutex);
    pthread_mutex_destroy(&impl->mutex);
    free(impl);
    adapter->impl = NULL;
    adapter->active = false;
    
    return TUXRT_OK;
}

static tuxrt_err_t i2c_send(tuxrt_transport_adapter_t* adapter, const uint8_t* data, size_t len, tuxrt_timestamp_ns_t deadline) {
    if (!adapter || !adapter->impl || !data) return TUXRT_ERR_INVAL;
    
    i2c_transport_impl_t* impl = (i2c_transport_impl_t*)adapter->impl;
    (void)deadline;
    
    // I2C write: first byte is register address, rest is data
    // For simplicity, we write raw bytes
    pthread_mutex_lock(&impl->mutex);
    ssize_t n = write(impl->fd, data, len);
    pthread_mutex_unlock(&impl->mutex);
    
    if (n == (ssize_t)len) {
        tuxrt_sirc_update_health_success(adapter);
    } else {
        tuxrt_sirc_update_health_failure(adapter);
    }
    
    return (n == (ssize_t)len) ? TUXRT_OK : TUXRT_ERR_IO;
}

static tuxrt_err_t i2c_recv(tuxrt_transport_adapter_t* adapter, uint8_t* buf, size_t* len, tuxrt_timestamp_ns_t deadline) {
    if (!adapter || !adapter->impl || !buf || !len) return TUXRT_ERR_INVAL;
    
    i2c_transport_impl_t* impl = (i2c_transport_impl_t*)adapter->impl;
    (void)deadline;
    
    // I2C read: write register address, then read
    // For simplicity, read raw bytes
    pthread_mutex_lock(&impl->mutex);
    ssize_t n = read(impl->fd, buf, *len);
    pthread_mutex_unlock(&impl->mutex);
    
    if (n >= 0) {
        *len = n;
    } else {
        *len = 0;
    }
    
    if (n >= 0) {
        tuxrt_sirc_update_health_success(adapter);
    } else {
        tuxrt_sirc_update_health_failure(adapter);
    }
    
    return (n >= 0) ? TUXRT_OK : TUXRT_ERR_IO;
}

static tuxrt_err_t i2c_flush(tuxrt_transport_adapter_t* adapter) {
    return TUXRT_OK;
}

static tuxrt_err_t i2c_get_stats(tuxrt_transport_adapter_t* adapter, void* stats_out) {
    if (!adapter || !stats_out) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&adapter->mutex);
    *(tuxrt_health_stats_t*)stats_out = adapter->health;
    pthread_mutex_unlock(&adapter->mutex);
    
    return TUXRT_OK;
}

static void i2c_destroy(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return;
    i2c_close(adapter);
}

const tuxrt_transport_vtable_t tuxrt_transport_i2c_vtable = {
    .abi_version = 1,
    .open = i2c_open,
    .close = i2c_close,
    .send = i2c_send,
    .recv = i2c_recv,
    .flush = i2c_flush,
    .get_stats = i2c_get_stats,
    .destroy = i2c_destroy
};