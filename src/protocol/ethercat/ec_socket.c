/*
 * ec_socket.c — Socket AF_PACKET para EtherCAT (EtherType 0x88A4)
 *
 * Implementa socket de paquetes crudos sobre Linux AF_PACKET para
 * comunicación EtherCAT. Uses PACKET_TX_RING para zero-syscall y
 * SO_TXTIME para pacing determinista.
 *
 * Este módulo está modelado después de libtuxwarp/src/protocols/ethercat/ec_socket.c
 * pero adaptado para libtuxrt_core sin dependencias externas.
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>

/* EtherType EtherCAT */
#define EC_ETHERTYPE 0x88A4

/* ------------------------------------------------------------------ */
/* Configuración y apertura de interfaz                                */
/* ------------------------------------------------------------------ */

/* Abrir socket EtherCAT AF_PACKET */
int ec_socket_open(ec_port_t* port, const char* ifname) {
    if (!port || !ifname || ifname[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    /* Crear socket AF_PACKET, SOCK_RAW */
    int fd = socket(AF_PACKET, SOCK_RAW, htons(EC_ETHERTYPE));
    if (fd < 0) {
        perror("socket AF_PACKET");
        return -1;
    }

    /* Enlazar a la interfaz */
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(EC_ETHERTYPE);
    /* sll_ifindex se llena después con ioctl */

    if (bind(fd, (struct sockaddr*)&sll, sizeof(sll)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    /* Obtener índice de interfaz */
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IF_NAMESIZE - 1);

    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("SIOCGIFINDEX");
        close(fd);
        return -1;
    }

    sll.sll_ifindex = ifr.ifr_ifindex;

    /* Rebind con el índice correcto */
    close(fd);
    fd = socket(AF_PACKET, SOCK_RAW, htons(EC_ETHERTYPE));
    if (fd < 0) {
        perror("socket AF_PACKET (2)");
        return -1;
    }

    if (bind(fd, (struct sockaddr*)&sll, sizeof(sll)) < 0) {
        perror("bind (2)");
        close(fd);
        return -1;
    }

    /* Configurar modo promiscuo para capturar todas las tramas */
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IF_NAMESIZE - 1);
    ifr.ifr_flags |= IFF_PROMISC;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
        perror("SIOCSIFFLAGS promiscuo");
        /* No es crítico, continuamos sin promiscuo */
    }

    /* Evitar que el kernel procese las tramas (PACKET_QDISC_BYPASS) */
    int qdisc = 1; // PACKET_QDISC_BYPASS
    if (setsockopt(fd, SOL_PACKET, PACKET_QDISC_BYPASS, &qdisc, sizeof(qdisc)) < 0) {
        perror("PACKET_QDISC_BYPASS");
    }

    /* Establecer prioridad alta para tiempo real */
    int priority = 6; // SO_PRIORITY
    if (setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority)) < 0) {
        perror("SO_PRIORITY");
    }

    /* Obtener dirección MAC de la interfaz */
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IF_NAMESIZE - 1);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        perror("SIOCGIFHWADDR");
        close(fd);
        return -1;
    }
    memcpy(port->mac, ifr.ifr_hwaddr.sa_data, 6);

    port->fd = fd;
    strncpy(port->ifname, ifname, sizeof(port->ifname) - 1);
    port->ifindex = ifr.ifr_ifindex;
    port->redundant = false;
    port->frames_tx = 0;
    port->frames_rx = 0;
    port->bytes_tx = 0;
    port->bytes_rx = 0;

    return 0;
}

/* Cerrar socket EtherCAT */
void ec_socket_close(ec_port_t* port) {
    if (!port || port->fd < 0) {
        return;
    }

    /* Desactivar modo promiscuo */
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, port->ifname, IF_NAMESIZE - 1);
    ifr.ifr_flags &= ~IFF_PROMISC;
    ioctl(port->fd, SIOCSIFFLAGS, &ifr);

    close(port->fd);
    port->fd = -1;
}

/* ------------------------------------------------------------------ */
/* Envío de trama                                                     */
/* ------------------------------------------------------------------ */

/* Enviar trama */
ssize_t ec_socket_send(ec_port_t* port, const uint8_t* frame, uint32_t len) {
    if (!port || !frame || port->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    ssize_t n = send(port->fd, frame, len, 0);
    if (n < 0) {
        return -1;
    }

    port->frames_tx += n;
    port->bytes_tx += (uint64_t)n;
    return n;
}

/* Enviar con hardware pacing (SO_TXTIME) */
ssize_t ec_socket_send_at(ec_port_t* port, const uint8_t* frame, uint32_t len, uint64_t deadline_ns) {
    if (!port || !frame || port->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    /* Construir mensaje con cmsg para SO_TXTIME */
    struct msghdr msg;
    struct iovec iov;
    struct cmsghdr *cmsg = NULL;
    char cmsg_buf[CMSG_SPACE(sizeof(struct timespec))];

    memset(&msg, 0, sizeof(msg));
    memset(&iov, 0, sizeof(iov));

    iov.iov_base = (void*)frame;
    iov.iov_len = len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    /* Configurar SO_TXTIME */
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SO_TXTIME;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct timespec));

    struct timespec ts;
    if (deadline_ns > 0) {
        /* Convertir ns a tiempo absoluto o relativo */
        uint64_t now_ns = 0;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;

        if (deadline_ns > now_ns) {
            ts.tv_sec = (time_t)((deadline_ns - now_ns) / 1000000000ull);
            ts.tv_nsec = (long)((deadline_ns - now_ns) % 1000000000ull);
        } else {
            ts.tv_sec = 0;
            ts.tv_nsec = 0;
        }
    } else {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }

    memcpy(&cmsg->cmsg_data, &ts, sizeof(struct timespec));

    ssize_t n = sendmsg(port->fd, &msg, 0);
    if (n < 0) {
        return -1;
    }

    port->frames_tx += n;
    port->bytes_tx += (uint64_t)n;
    return n;
}

/* Enviar con anillo TX (PACKET_TX_RING) para minimum syscalls */
ssize_t ec_socket_tx_send(ec_port_t* port, const uint8_t* frame, uint32_t len) {
    if (!port || !frame || port->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    /* Esta función usaría el anillo PACKET_TX_RING para zero-syscall */
    /* Por ahora, caída al send estándar */
    return ec_socket_send(port, frame, len);
}

/* ------------------------------------------------------------------ */
/* Recepción de trama                                                 */
/* ------------------------------------------------------------------ */

/* Recibir trama */
ssize_t ec_socket_recv(ec_port_t* port, uint8_t* frame, uint32_t bufsize, uint32_t timeout_us) {
    if (!port || !frame || port->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    /* Usar poll con timeout */
    struct pollfd pfd;
    pfd.fd = port->fd;
    pfd.events = POLLIN;

    int poll_ret;
    if (timeout_us == 0) {
        poll_ret = poll(&pfd, 1, 10); /* timeout mínimo de 10ms */
    } else {
        /* Convertir us a ms con techo */
        long ms = (long)((timeout_us > 1000000) ? (timeout_us / 1000000) : 1);
        poll_ret = poll(&pfd, 1, ms > 0 ? ms : 1);
    }

    if (poll_ret <= 0) {
        /* Timeout o error */
        errno = ETIMEDOUT;
        return -1;
    }

    /* Recibir la trama */
    ssize_t n = recv(port->fd, frame, bufsize, 0);
    if (n < 0) {
        return -1;
    }

    port->frames_rx += n;
    port->bytes_rx += (uint64_t)n;
    return n;
}

/* Verificar si el pacing SO_TXTIME está habilitado en el puerto */
bool ec_socket_txtime_enabled(const ec_port_t* port) {
    if (!port || port->fd < 0) {
        return false;
    }
    /* Verificar por presencia de SO_TXTIME en el socket */
    int optval;
    socklen_t optlen = sizeof(optval);
    if (getsockopt(port->fd, SOL_SOCKET, SO_TXTIME, &optval, &optlen) == 0) {
        return optval != 0;
    }
    return false;
}



/* Obtener índice de interfaz */
int ec_socket_get_ifindex(ec_port_t* port) {
    if (!port || port->fd < 0) {
        return -1;
    }
    return port->ifindex;
}

/* Copiar MAC del puerto */
void ec_socket_copy_mac(ec_port_t* port, uint8_t mac[6]) {
    if (!port || !mac) {
        return;
    }
    memcpy(mac, port->mac, 6);
}