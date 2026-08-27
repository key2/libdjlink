/* libdjlink OSAL: POSIX sockets, interface enumeration, monotonic clock. */
#define _GNU_SOURCE
#include "djl_internal.h"

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>

#ifdef __linux__
/* netpacket/packet.h provides struct sockaddr_ll; do not also include
 * linux/if_packet.h, the two redefine each other. */
#include <netpacket/packet.h>
#endif

uint64_t djl_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

void djl_sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int prefix_from_mask(const uint8_t m[4])
{
    int bits = 0;
    for (int i = 0; i < 4; i++)
        for (int b = 7; b >= 0; b--)
            if (m[i] & (1 << b)) bits++;
    return bits;
}

djl_err djl_iface_lookup(const char *name, djl_iface *out)
{
    if (!name || !out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", name);

    struct ifaddrs *ifa_list = NULL;
    if (getifaddrs(&ifa_list) != 0) return DJL_ERR_IO;

    bool have_ip = false, have_mac = false;

    for (struct ifaddrs *ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        /* Windows-style null guard; also seen on some Linux configs. */
        if (!ifa->ifa_name || !ifa->ifa_addr) continue;
        if (strcmp(ifa->ifa_name, name) != 0) continue;

        if (ifa->ifa_addr->sa_family == AF_INET && !have_ip) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            memcpy(out->ip, &sin->sin_addr.s_addr, 4);
            if (ifa->ifa_netmask) {
                struct sockaddr_in *nm = (struct sockaddr_in *)ifa->ifa_netmask;
                memcpy(out->netmask, &nm->sin_addr.s_addr, 4);
            } else {
                memset(out->netmask, 0xff, 4);
            }
            out->prefix_len = prefix_from_mask(out->netmask);
            for (int i = 0; i < 4; i++)
                out->broadcast[i] = (uint8_t)(out->ip[i] | (uint8_t)~out->netmask[i]);
            have_ip = true;
        }
#ifdef __linux__
        else if (ifa->ifa_addr->sa_family == AF_PACKET && !have_mac) {
            struct sockaddr_ll *ll = (struct sockaddr_ll *)ifa->ifa_addr;
            if (ll->sll_halen == 6) {
                memcpy(out->mac, ll->sll_addr, 6);
                have_mac = true;
            }
        }
#endif
    }
    freeifaddrs(ifa_list);

    if (!have_mac) {
        /* Fall back to SIOCGIFHWADDR. */
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof ifr);
            snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
            if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
                memcpy(out->mac, ifr.ifr_hwaddr.sa_data, 6);
                have_mac = true;
            }
            close(fd);
        }
    }

    if (!have_ip)  return DJL_ERR_NO_IFACE;
    if (!have_mac) return DJL_ERR_NO_IFACE;
    return DJL_OK;
}

djl_err djl_sock_open(djl_sock *s, uint16_t port, const char *ifname)
{
    if (!s) return DJL_ERR_INVAL;
    s->fd = -1;
    s->port = port;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return DJL_ERR_IO;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);

#ifdef SO_BINDTODEVICE
    /* Restrict to the DJ Link interface. This is what lets us bind
     * INADDR_ANY (required to receive subnet broadcasts on Linux) while
     * still ignoring traffic from every other interface. */
    if (ifname && *ifname) {
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
                       ifname, (socklen_t)strlen(ifname)) != 0) {
            /* Non-fatal: needs CAP_NET_RAW on some kernels. */
        }
    }
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return DJL_ERR_IO;
    }

    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    s->fd = fd;
    return DJL_OK;
}

void djl_sock_close(djl_sock *s)
{
    if (s && s->fd >= 0) { close(s->fd); s->fd = -1; }
}

int djl_sock_recv(djl_sock *s, uint8_t *buf, size_t cap, uint8_t src_ip[4])
{
    if (!s || s->fd < 0) return -1;
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t n = recvfrom(s->fd, buf, cap, 0, (struct sockaddr *)&from, &fl);
    if (n < 0) return -1;
    if (src_ip) memcpy(src_ip, &from.sin_addr.s_addr, 4);
    return (int)n;
}

djl_err djl_sock_send(djl_sock *s, const uint8_t ip[4], uint16_t port,
                      const uint8_t *buf, size_t len)
{
    if (!s || s->fd < 0 || !ip || !buf) return DJL_ERR_INVAL;
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port   = htons(port);
    memcpy(&to.sin_addr.s_addr, ip, 4);
    ssize_t n = sendto(s->fd, buf, len, 0, (struct sockaddr *)&to, sizeof to);
    if (n < 0) return DJL_ERR_IO;
    return DJL_OK;
}

/* ---------------- client UDP (RPC / NFS) ---------------- */

djl_err djl_udp_open(djl_sock *s)
{
    if (!s) return DJL_ERR_INVAL;
    s->fd = -1;
    s->port = 0;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return DJL_ERR_IO;

    /* Bind an ephemeral source port. Pioneer's NFS server, unlike some Unix
     * ones, does not insist on a reserved (<1024) source port. */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return DJL_ERR_IO;
    }

    struct sockaddr_in got;
    socklen_t gl = sizeof got;
    if (getsockname(fd, (struct sockaddr *)&got, &gl) == 0)
        s->port = ntohs(got.sin_port);

    s->fd = fd;
    return DJL_OK;
}

int djl_udp_recv_wait(djl_sock *s, uint8_t *buf, size_t cap, unsigned timeout_ms)
{
    if (!s || s->fd < 0 || !buf) return -1;
    struct pollfd pfd;
    pfd.fd = s->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, (int)timeout_ms);
    if (pr == 0) return 0;                     /* timed out */
    if (pr < 0) return (errno == EINTR) ? 0 : -1;
    ssize_t n = recv(s->fd, buf, cap, 0);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    return (int)n;
}
