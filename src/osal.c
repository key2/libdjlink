/* libdjlink OS abstraction: sockets, interface enumeration, monotonic clock.
 *
 * Every syscall in the library lives here. The rest of the tree is portable
 * C11, so bringing up a new platform means editing this file and nothing else.
 *
 * Supported: Linux, macOS/BSD (AF_LINK for the MAC address), Windows via
 * Winsock2 + IP Helper. The Windows build needs a pthreads implementation
 * (MinGW's winpthreads); only the socket and interface layers differ, and they
 * are all confined below.
 */
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0601        /* Windows 7: inet_pton, GetAdaptersAddresses */
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  include <windows.h>
#else
#  if defined(__linux__)
#    define _GNU_SOURCE
#  endif
#endif

#include "djl_internal.h"

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(_WIN32)
/* Winsock uses SOCKET handles and its own error codes; map them onto the
 * POSIX-shaped internals the rest of the file uses. */
#  define DJL_SOCK(fd)        ((SOCKET)(fd))
#  define djl_closesocket(fd) closesocket(DJL_SOCK(fd))
#  define DJL_EINTR        WSAEINTR
#  define DJL_EWOULDBLOCK  WSAEWOULDBLOCK
#  define DJL_EINPROGRESS  WSAEWOULDBLOCK   /* non-blocking connect on Winsock */
static int djl_sockerr(void) { return WSAGetLastError(); }
typedef int djl_socklen;
typedef int djl_iolen;              /* Winsock buffer lengths are int */
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/ioctl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <net/if.h>
#  include <ifaddrs.h>
#  define DJL_SOCK(fd)        ((int)(fd))
#  define djl_closesocket(fd) close(DJL_SOCK(fd))
#  define DJL_EINTR        EINTR
#  define DJL_EWOULDBLOCK  EWOULDBLOCK
#  define DJL_EINPROGRESS  EINPROGRESS
static int djl_sockerr(void) { return errno; }
typedef socklen_t djl_socklen;
typedef size_t    djl_iolen;        /* POSIX buffer lengths are size_t */
#  if defined(__linux__)
/* netpacket/packet.h provides struct sockaddr_ll; do not also include
 * linux/if_packet.h, the two redefine each other. */
#    include <netpacket/packet.h>
#  else
#    include <net/if_dl.h>            /* macOS / BSD: struct sockaddr_dl */
#  endif
#endif

/* MSG_NOSIGNAL does not exist on Windows or macOS; SIGPIPE is suppressed
 * per-socket with SO_NOSIGPIPE there, or simply not raised. */
#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif

/* ---------------- one-time Winsock startup ---------------- */

#if defined(_WIN32)
static void djl_net_init(void)
{
    static volatile LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) == 0) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        /* Deliberately never WSACleanup: the library may be used for the
         * lifetime of the process and refcounting it here would be racy. */
    }
}
#else
static void djl_net_init(void) { }
#endif

/* ---------------- clock ---------------- */

uint64_t djl_now_ms(void)
{
#if defined(_WIN32)
    /* QueryPerformanceCounter is monotonic and unaffected by clock changes. */
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000) / freq.QuadPart);
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)(tv.tv_usec / 1000);
#endif
}

void djl_sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* ---------------- interface enumeration ---------------- */

static int prefix_from_mask(const uint8_t m[4])
{
    int bits = 0;
    for (int i = 0; i < 4; i++)
        for (int b = 7; b >= 0; b--)
            if (m[i] & (1 << b)) bits++;
    return bits;
}

static void fill_derived(djl_iface *out)
{
    out->prefix_len = prefix_from_mask(out->netmask);
    for (int i = 0; i < 4; i++)
        out->broadcast[i] = (uint8_t)(out->ip[i] | (uint8_t)~out->netmask[i]);
}

#if defined(_WIN32)

/* Match on the adapter's name, friendly name, or description, since Windows
 * has no short stable interface names of the eth0 kind. */
djl_err djl_iface_lookup(const char *name, djl_iface *out)
{
    if (!name || !out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", name);
    djl_net_init();

    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES *list = NULL;
    ULONG rv;
    for (int tries = 0; tries < 3; tries++) {
        list = malloc(size);
        if (!list) return DJL_ERR_NOMEM;
        rv = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
                                  GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  NULL, list, &size);
        if (rv != ERROR_BUFFER_OVERFLOW) break;
        free(list);
        list = NULL;
    }
    if (!list || rv != NO_ERROR) { free(list); return DJL_ERR_IO; }

    djl_err result = DJL_ERR_NO_IFACE;
    for (IP_ADAPTER_ADDRESSES *a = list; a; a = a->Next) {
        char friendly[256] = {0}, desc[256] = {0};
        if (a->FriendlyName)
            WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1,
                                friendly, (int)sizeof friendly - 1, NULL, NULL);
        if (a->Description)
            WideCharToMultiByte(CP_UTF8, 0, a->Description, -1,
                                desc, (int)sizeof desc - 1, NULL, NULL);
        if (strcmp(a->AdapterName, name) != 0 &&
            strcmp(friendly, name) != 0 && strcmp(desc, name) != 0)
            continue;

        if (a->PhysicalAddressLength == 6)
            memcpy(out->mac, a->PhysicalAddress, 6);

        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            struct sockaddr_in *sin = (struct sockaddr_in *)u->Address.lpSockaddr;
            memcpy(out->ip, &sin->sin_addr, 4);

            ULONG bits = u->OnLinkPrefixLength;
            if (bits > 32) bits = 32;
            uint32_t mask = bits ? (0xffffffffu << (32 - bits)) : 0;
            out->netmask[0] = (uint8_t)(mask >> 24); out->netmask[1] = (uint8_t)(mask >> 16);
            out->netmask[2] = (uint8_t)(mask >> 8);  out->netmask[3] = (uint8_t)mask;
            fill_derived(out);
            result = DJL_OK;
            break;
        }
        break;
    }
    free(list);
    return result;
}

#else /* POSIX */

djl_err djl_iface_lookup(const char *name, djl_iface *out)
{
    if (!name || !out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", name);

    struct ifaddrs *ifa_list = NULL;
    if (getifaddrs(&ifa_list) != 0) return DJL_ERR_IO;

    bool have_ip = false, have_mac = false;

    for (struct ifaddrs *ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        /* Some configurations hand back entries with no address at all. */
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
            fill_derived(out);
            have_ip = true;
        }
#if defined(__linux__)
        else if (ifa->ifa_addr->sa_family == AF_PACKET && !have_mac) {
            struct sockaddr_ll *ll = (struct sockaddr_ll *)ifa->ifa_addr;
            if (ll->sll_halen == 6) {
                memcpy(out->mac, ll->sll_addr, 6);
                have_mac = true;
            }
        }
#elif defined(AF_LINK)
        else if (ifa->ifa_addr->sa_family == AF_LINK && !have_mac) {
            /* macOS and the BSDs report the hardware address this way. */
            struct sockaddr_dl *dl = (struct sockaddr_dl *)ifa->ifa_addr;
            if (dl->sdl_alen == 6) {
                memcpy(out->mac, LLADDR(dl), 6);
                have_mac = true;
            }
        }
#endif
    }
    freeifaddrs(ifa_list);

#if defined(__linux__)
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
#endif

    if (!have_ip)  return DJL_ERR_NO_IFACE;
    if (!have_mac) return DJL_ERR_NO_IFACE;
    return DJL_OK;
}

#endif /* platform */

/* ---------------- socket helpers ---------------- */

static void set_nonblocking(djl_fd fd, bool on)
{
#if defined(_WIN32)
    u_long v = on ? 1 : 0;
    /* FIONBIO is defined via _IOW, which yields a value too large for the
     * signed long that ioctlsocket declares; the cast is Winsock's problem,
     * not ours. */
    ioctlsocket(DJL_SOCK(fd), (long)FIONBIO, &v);
#else
    int fl = fcntl(DJL_SOCK(fd), F_GETFL, 0);
    if (fl < 0) return;
    fcntl(DJL_SOCK(fd), F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

/* Wait for readability or writability. Returns >0 ready, 0 timeout, <0 error. */
static int wait_ready(djl_fd fd, bool for_write, unsigned timeout_ms)
{
#if defined(_WIN32)
    fd_set set;
    FD_ZERO(&set);
    FD_SET(DJL_SOCK(fd), &set);
    struct timeval tv;
    tv.tv_sec  = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
    return select(0, for_write ? NULL : &set, for_write ? &set : NULL, NULL, &tv);
#else
    struct pollfd pfd;
    pfd.fd      = DJL_SOCK(fd);
    pfd.events  = for_write ? POLLOUT : POLLIN;
    pfd.revents = 0;
    int r = poll(&pfd, 1, (int)timeout_ms);
    if (r < 0 && djl_sockerr() == DJL_EINTR) return 0;
    return r;
#endif
}

djl_err djl_sock_open(djl_sock *s, uint16_t port, const char *ifname)
{
    if (!s) return DJL_ERR_INVAL;
    djl_net_init();
    s->fd = DJL_BAD_FD;
    s->port = port;

    djl_fd fd = (djl_fd)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return DJL_ERR_IO;

    int one = 1;
    setsockopt(DJL_SOCK(fd), SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(DJL_SOCK(fd), SOL_SOCKET, SO_REUSEPORT, (const char *)&one, sizeof one);
#endif
    setsockopt(DJL_SOCK(fd), SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);

#ifdef SO_BINDTODEVICE
    /* Restrict to the DJ Link interface. This is what lets us bind
     * INADDR_ANY (required to receive subnet broadcasts on Linux) while
     * still ignoring traffic from every other interface. */
    if (ifname && *ifname) {
        if (setsockopt(DJL_SOCK(fd), SOL_SOCKET, SO_BINDTODEVICE,
                       ifname, (djl_socklen)strlen(ifname)) != 0) {
            /* Non-fatal: needs CAP_NET_RAW on some kernels. */
        }
    }
#else
    (void)ifname;   /* macOS/Windows: filter by source address instead */
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(DJL_SOCK(fd), (struct sockaddr *)&addr, sizeof addr) != 0) {
        djl_closesocket(fd);
        return DJL_ERR_IO;
    }

    set_nonblocking(fd, true);
    s->fd = fd;
    return DJL_OK;
}

void djl_sock_close(djl_sock *s)
{
    if (s && s->fd >= 0) { djl_closesocket(s->fd); s->fd = DJL_BAD_FD; }
}

int djl_sock_recv(djl_sock *s, uint8_t *buf, size_t cap, uint8_t src_ip[4])
{
    if (!s || s->fd < 0) return -1;
    struct sockaddr_in from;
    djl_socklen fl = sizeof from;
    int n = (int)recvfrom(DJL_SOCK(s->fd), (char *)buf, (djl_iolen)cap, 0,
                          (struct sockaddr *)&from, &fl);
    if (n < 0) return -1;
    if (src_ip) memcpy(src_ip, &from.sin_addr, 4);
    return n;
}

djl_err djl_sock_send(djl_sock *s, const uint8_t ip[4], uint16_t port,
                      const uint8_t *buf, size_t len)
{
    if (!s || s->fd < 0 || !ip || !buf) return DJL_ERR_INVAL;
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port   = htons(port);
    memcpy(&to.sin_addr, ip, 4);
    int n = (int)sendto(DJL_SOCK(s->fd), (const char *)buf, (djl_iolen)len, 0,
                        (struct sockaddr *)&to, sizeof to);
    if (n < 0) return DJL_ERR_IO;
    return DJL_OK;
}

int djl_sock_poll(djl_sock *const *socks, size_t n, unsigned timeout_ms, bool *ready)
{
    if (!socks || !ready || n == 0 || n > 16) return -1;
    for (size_t i = 0; i < n; i++) ready[i] = false;

#if defined(_WIN32)
    fd_set set;
    FD_ZERO(&set);
    int any = 0;
    for (size_t i = 0; i < n; i++) {
        if (socks[i] && socks[i]->fd >= 0) { FD_SET(DJL_SOCK(socks[i]->fd), &set); any = 1; }
    }
    if (!any) return -1;
    struct timeval tv;
    tv.tv_sec  = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
    int r = select(0, &set, NULL, NULL, &tv);
    if (r <= 0) return r;
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        if (socks[i] && socks[i]->fd >= 0 &&
            FD_ISSET(DJL_SOCK(socks[i]->fd), &set)) { ready[i] = true; count++; }
    }
    return count;
#else
    struct pollfd pfd[16];
    for (size_t i = 0; i < n; i++) {
        pfd[i].fd      = (socks[i] && socks[i]->fd >= 0) ? DJL_SOCK(socks[i]->fd) : -1;
        pfd[i].events  = POLLIN;
        pfd[i].revents = 0;
    }
    int r = poll(pfd, (unsigned)n, (int)timeout_ms);
    if (r < 0) return (djl_sockerr() == DJL_EINTR) ? 0 : -1;
    if (r == 0) return 0;
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        if (pfd[i].revents & POLLIN) { ready[i] = true; count++; }
    }
    return count;
#endif
}

/* ---------------- client UDP (RPC / NFS) ---------------- */

djl_err djl_udp_open(djl_sock *s)
{
    if (!s) return DJL_ERR_INVAL;
    djl_net_init();
    s->fd = DJL_BAD_FD;
    s->port = 0;

    djl_fd fd = (djl_fd)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return DJL_ERR_IO;

    /* Bind an ephemeral source port. Pioneer's NFS server, unlike some Unix
     * ones, does not insist on a reserved (<1024) source port. */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(DJL_SOCK(fd), (struct sockaddr *)&addr, sizeof addr) != 0) {
        djl_closesocket(fd);
        return DJL_ERR_IO;
    }

    struct sockaddr_in got;
    djl_socklen gl = sizeof got;
    if (getsockname(DJL_SOCK(fd), (struct sockaddr *)&got, &gl) == 0)
        s->port = ntohs(got.sin_port);

    s->fd = fd;
    return DJL_OK;
}

int djl_udp_recv_wait(djl_sock *s, uint8_t *buf, size_t cap, unsigned timeout_ms)
{
    if (!s || s->fd < 0 || !buf) return -1;
    int pr = wait_ready(s->fd, false, timeout_ms);
    if (pr == 0) return 0;                     /* timed out */
    if (pr < 0) return -1;
    int n = (int)recv(DJL_SOCK(s->fd), (char *)buf, (djl_iolen)cap, 0);
    if (n < 0) {
        int e = djl_sockerr();
        return (e == DJL_EWOULDBLOCK || e == DJL_EINTR) ? 0 : -1;
    }
    return n;
}

/* ---------------- client TCP (dbserver) ---------------- */

djl_err djl_tcp_connect(djl_tcp *t, const uint8_t ip[4], uint16_t port, int timeout_ms)
{
    if (!t || !ip) return DJL_ERR_INVAL;
    djl_net_init();
    t->fd = DJL_BAD_FD;

    djl_fd fd = (djl_fd)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return DJL_ERR_IO;

    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port   = htons(port);
    memcpy(&to.sin_addr, ip, 4);

    /* Connect with a bounded wait, then go back to blocking for the transfer. */
    set_nonblocking(fd, true);
    int r = connect(DJL_SOCK(fd), (struct sockaddr *)&to, sizeof to);
    if (r != 0) {
        int e = djl_sockerr();
        if (e != DJL_EINPROGRESS
#if !defined(_WIN32)
            && e != EINTR
#endif
           ) {
            djl_closesocket(fd);
            return DJL_ERR_IO;
        }
        if (wait_ready(fd, true, (unsigned)timeout_ms) <= 0) {
            djl_closesocket(fd);
            return DJL_ERR_TIMEOUT;
        }
        int soerr = 0;
        djl_socklen sl = sizeof soerr;
        if (getsockopt(DJL_SOCK(fd), SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl) != 0 || soerr != 0) {
            djl_closesocket(fd);
            return DJL_ERR_IO;
        }
    }
    set_nonblocking(fd, false);

#if defined(_WIN32)
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(DJL_SOCK(fd), SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    setsockopt(DJL_SOCK(fd), SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);
#else
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(DJL_SOCK(fd), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(DJL_SOCK(fd), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(DJL_SOCK(fd), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    int nodelay = 1;
    setsockopt(DJL_SOCK(fd), IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);

    t->fd = fd;
    return DJL_OK;
}

void djl_tcp_close(djl_tcp *t)
{
    if (t && t->fd >= 0) { djl_closesocket(t->fd); t->fd = DJL_BAD_FD; }
}

djl_err djl_tcp_send_all(djl_tcp *t, const uint8_t *buf, size_t len)
{
    if (!t || t->fd < 0 || !buf) return DJL_ERR_INVAL;
    size_t sent = 0;
    while (sent < len) {
        int n = (int)send(DJL_SOCK(t->fd), (const char *)buf + sent,
                          (djl_iolen)(len - sent), MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && djl_sockerr() == DJL_EINTR) continue;
            return DJL_ERR_IO;
        }
        sent += (size_t)n;
    }
    return DJL_OK;
}

djl_err djl_tcp_recv_exact(djl_tcp *t, uint8_t *buf, size_t len)
{
    if (!t || t->fd < 0 || !buf) return DJL_ERR_INVAL;
    size_t got = 0;
    while (got < len) {
        int n = (int)recv(DJL_SOCK(t->fd), (char *)buf + got, (djl_iolen)(len - got), 0);
        if (n == 0) return DJL_ERR_IO;                  /* peer closed */
        if (n < 0) {
            if (djl_sockerr() == DJL_EINTR) continue;
            return DJL_ERR_TIMEOUT;
        }
        got += (size_t)n;
    }
    return DJL_OK;
}
