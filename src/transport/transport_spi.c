/**
 * transport_spi.c — SPI Transport Adapter (Linux spidev backend)
 * 
 * Implements tuxrt_transport_vtable_t for SPI communication
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "../../sdk/include/tuxrt_sdk.h"

typedef struct {
    int fd;
    uint8_t mode;
    uint8_t bits;
    uint32_t speed;
    pthread_mutex_t mutex;
    bool configured;
} spi_transport_impl_t;

static tuxrt_err_t spi_open(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return TUXRT_ERR_INVAL;
    
    spi_transport_impl_t* impl = calloc(1, sizeof(spi_transport_impl_t));
    if (!impl) return TUXRT_ERR_NOMEM;
    
    pthread_mutex_init(&impl->mutex, NULL);
    
    impl->fd = open(adapter->config.device_path, O_RDWR);
    if (impl->fd < 0) {
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return TUXRT_ERR_NOENT;
    }
    
    // Configure SPI
    impl->mode = SPI_MODE_0;
    impl->bits = 8;
    impl->speed = adapter->config.bitrate ? adapter->config.bitrate : 1000000;
    
    if (ioctl(impl->fd, SPI_IOC_WR_MODE, &impl->mode) < 0 ||
        ioctl(impl->fd, SPI_IOC_WR_BITS_PER_WORD, &impl->bits) < 0 ||
        ioctl(impl->fd, SPI_IOC_WR_MAX_SPEED_HZ, &impl->speed) < 0) {
        close(impl->fd);
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return TUXRT_ERR_IO;
    }
    
    impl->configured = true;
    adapter->impl = impl;
    adapter->active = true;
    
    tuxrt_sirc_update_health_success(adapter);
    
    return TUXRT_OK;
}

static tuxrt_err_t spi_close(tuxrt_transport_adapter_t* adapter) {
    if (!adapter || !adapter->impl) return TUXRT_ERR_INVAL;
    
    spi_transport_impl_t* impl = (spi_transport_impl_t*)adapter->impl;
    
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

static tuxrt_err_t spi_send(tuxrt_transport_adapter_t* adapter, const uint8_t* data, size_t len, tuxrt_timestamp_ns_t deadline) {
    if (!adapter || !adapter->impl || !data) return TUXRT_ERR_INVAL;
    
    spi_transport_impl_t* impl = (spi_transport_impl_t*)adapter->impl;
    (void)deadline;
    
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)data,
        .rx_buf = 0,
        .len = len,
        .speed_hz = impl->speed,
        .bits_per_word = impl->bits,
    };
    
    pthread_mutex_lock(&impl->mutex);
    int ret = ioctl(impl->fd, SPI_IOC_MESSAGE(1), &tr);
    pthread_mutex_unlock(&impl->mutex);
    
    if (ret >= 0) {
        tuxrt_sirc_update_health_success(adapter);
    } else {
        tuxrt_sirc_update_health_failure(adapter);
    }
    
    return (ret >= 0) ? TUXRT_OK : TUXRT_ERR_IO;
}

static tuxrt_err_t spi_recv(tuxrt_transport_adapter_t* adapter, uint8_t* buf, size_t* len, tuxrt_timestamp_ns_t deadline) {
    if (!adapter || !adapter->impl || !buf || !len) return TUXRT_ERR_INVAL;
    
    spi_transport_impl_t* impl = (spi_transport_impl_t*)adapter->impl;
    (void)deadline;
    
    struct spi_ioc_transfer tr = {
        .tx_buf = 0,
        .rx_buf = (unsigned long)buf,
        .len = *len,
        .speed_hz = impl->speed,
        .bits_per_word = impl->bits,
    };
    
    pthread_mutex_lock(&impl->mutex);
    int ret = ioctl(impl->fd, SPI_IOC_MESSAGE(1), &tr);
    pthread_mutex_unlock(&impl->mutex);
    
    if (ret >= 0) {
        *len = ret;
    } else {
        *len = 0;
    }
    
    if (ret >= 0) {
        tuxrt_sirc_update_health_success(adapter);
    } else {
        tuxrt_sirc_update_health_failure(adapter);
    }
    
    return (ret >= 0) ? TUXRT_OK : TUXRT_ERR_IO;
}

static tuxrt_err_t spi_flush(tuxrt_transport_adapter_t* adapter) {
    return TUXRT_OK;
}

static tuxrt_err_t spi_get_stats(tuxrt_transport_adapter_t* adapter, void* stats_out) {
    if (!adapter || !stats_out) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&adapter->mutex);
    *(tuxrt_health_stats_t*)stats_out = adapter->health;
    pthread_mutex_unlock(&adapter->mutex);
    
    return TUXRT_OK;
}

static void spi_destroy(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return;
    spi_close(adapter);
}

const tuxrt_transport_vtable_t tuxrt_transport_spi_vtable = {
    .abi_version = 1,
    .open = spi_open,
    .close = spi_close,
    .send = spi_send,
    .recv = spi_recv,
    .flush = spi_flush,
    .get_stats = spi_get_stats,
    .destroy = spi_destroy
};