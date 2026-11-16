/**
 * transport_can.c — CAN Bus Transport Adapter (SocketCAN backend)
 * 
 * Implements tuxrt_transport_vtable_t for CAN communication
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define TUXRT_CORE_EXPORTS
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "../../sdk/include/tuxrt_sdk.h"

typedef struct {
    int sock;
    struct sockaddr_can addr;
    pthread_mutex_t mutex;
    uint32_t bitrate;
    bool configured;
} can_transport_impl_t;

static tuxrt_err_t can_open(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return TUXRT_ERR_INVAL;
    
    can_transport_impl_t* impl = calloc(1, sizeof(can_transport_impl_t));
    if (!impl) return TUXRT_ERR_NOMEM;
    
    pthread_mutex_init(&impl->mutex, NULL);
    impl->bitrate = adapter->config.bitrate;
    
    // Create socket
    impl->sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (impl->sock < 0) {
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return TUXRT_ERR_IO;
    }
    
    // Get interface index
    struct ifreq ifr = {0};
    strncpy(ifr.ifr_name, adapter->config.device_path, IF_NAMESIZE - 1);
    if (ioctl(impl->sock, SIOCGIFINDEX, &ifr) < 0) {
        close(impl->sock);
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return TUXRT_ERR_NOENT;
    }
    
    // Bitrate is set via `ip link set` before starting, not via ioctl
    // Non-fatal if not set
    (void)impl->bitrate;
    
    // Bind
    impl->addr.can_family = AF_CAN;
    impl->addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(impl->sock, (struct sockaddr*)&impl->addr, sizeof(impl->addr)) < 0) {
        close(impl->sock);
        pthread_mutex_destroy(&impl->mutex);
        free(impl);
        return TUXRT_ERR_IO;
    }
    
    // Enable FD frames if needed
    int enable = 1;
    setsockopt(impl->sock, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable, sizeof(enable));
    
    impl->configured = true;
    adapter->impl = impl;
    adapter->active = true;
    
    tuxrt_sirc_update_health_success(adapter);
    
    return TUXRT_OK;
}

static tuxrt_err_t can_close(tuxrt_transport_adapter_t* adapter) {
    if (!adapter || !adapter->impl) return TUXRT_ERR_INVAL;
    
    can_transport_impl_t* impl = (can_transport_impl_t*)adapter->impl;
    
    pthread_mutex_lock(&impl->mutex);
    
    if (impl->configured) {
        close(impl->sock);
        impl->sock = -1;
        impl->configured = false;
    }
    
    pthread_mutex_unlock(&impl->mutex);
    pthread_mutex_destroy(&impl->mutex);
    free(impl);
    adapter->impl = NULL;
    adapter->active = false;
    
    return TUXRT_OK;
}

static tuxrt_err_t can_send(tuxrt_transport_adapter_t* adapter, const uint8_t* data, size_t len, tuxrt_timestamp_ns_t deadline) {
    if (!adapter || !adapter->impl || !data) return TUXRT_ERR_INVAL;
    
    can_transport_impl_t* impl = (can_transport_impl_t*)adapter->impl;
    (void)deadline;
    
    // CAN frames are max 8 bytes (classic) or 64 bytes (FD)
    // We expect data to be a can_frame or canfd_frame
    if (len != sizeof(struct can_frame) && len != sizeof(struct canfd_frame)) {
        return TUXRT_ERR_INVAL;
    }
    
    pthread_mutex_lock(&impl->mutex);
    
    ssize_t n = write(impl->sock, data, len);
    
    pthread_mutex_unlock(&impl->mutex);
    
    if (n == (ssize_t)len) {
        tuxrt_sirc_update_health_success(adapter);
    } else {
        tuxrt_sirc_update_health_failure(adapter);
    }
    
    return (n == (ssize_t)len) ? TUXRT_OK : TUXRT_ERR_IO;
}

static tuxrt_err_t can_recv(tuxrt_transport_adapter_t* adapter, uint8_t* buf, size_t* len, tuxrt_timestamp_ns_t deadline) {
    if (!adapter || !adapter->impl || !buf || !len) return TUXRT_ERR_INVAL;
    
    can_transport_impl_t* impl = (can_transport_impl_t*)adapter->impl;
    
    // Use poll with deadline
    struct pollfd pfd = { .fd = impl->sock, .events = POLLIN };
    uint64_t timeout_ms = (deadline > tuxrt_now_ns()) ? (deadline - tuxrt_now_ns()) / 1000000 : 100;
    
    pthread_mutex_lock(&impl->mutex);
    
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) {
        pthread_mutex_unlock(&impl->mutex);
        *len = 0;
        tuxrt_sirc_update_health_failure(adapter);
        return (ret == 0) ? TUXRT_ERR_TIMEDOUT : TUXRT_ERR_IO;
    }
    
    ssize_t n = read(impl->sock, buf, *len);
    
    pthread_mutex_unlock(&impl->mutex);
    
    if (n < 0) {
        *len = 0;
        return TUXRT_ERR_IO;
    }
    
    *len = n;
    
    if (n > 0) {
        tuxrt_sirc_update_health_success(adapter);
    } else {
        tuxrt_sirc_update_health_failure(adapter);
    }
    
    return TUXRT_OK;
}

static tuxrt_err_t can_flush(tuxrt_transport_adapter_t* adapter) {
    (void)adapter;
    // CAN doesn't have explicit flush
    return TUXRT_OK;
}

static tuxrt_err_t can_get_stats(tuxrt_transport_adapter_t* adapter, void* stats_out) {
    if (!adapter || !stats_out) return TUXRT_ERR_INVAL;
    
    pthread_mutex_lock(&adapter->mutex);
    *(tuxrt_health_stats_t*)stats_out = adapter->health;
    pthread_mutex_unlock(&adapter->mutex);
    
    return TUXRT_OK;
}

static void can_destroy(tuxrt_transport_adapter_t* adapter) {
    if (!adapter) return;
    can_close(adapter);
}

const tuxrt_transport_vtable_t tuxrt_transport_can_vtable = {
    .abi_version = 1,
    .open = can_open,
    .close = can_close,
    .send = can_send,
    .recv = can_recv,
    .flush = can_flush,
    .get_stats = can_get_stats,
    .destroy = can_destroy
};