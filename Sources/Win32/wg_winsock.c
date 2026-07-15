#include "wg_winsock.h"
#include "wg_log.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

#define TAG "WSock"

#include "wg_blink_bridge.h"

// Guest scratch base for serialized getaddrinfo results. The engine maps a 1MB
// region here per VM load and tells us where via wg_winsock_set_gai_base()
// (it's relocated above the image for large 64-bit PEs). Defaults to the legacy
// low address for small/32-bit guests.
static uint64_t s_gai_base = 0x00B00000u;   // 64-bit: box64 relocates this above 4GB
void wg_winsock_set_gai_base(uint64_t base) { if (base) s_gai_base = base; }

// ── Windows socket constants ────────────────────────────────────────
#define WSADESCRIPTION_LEN  256
#define WSASYS_STATUS_LEN   128
#define WIN_INVALID_SOCKET  (~(uint32_t)0)
#define WIN_SOCKET_ERROR    (-1)
#define WIN_SD_RECEIVE      0
#define WIN_SD_SEND         1
#define WIN_SD_BOTH         2

// Winsock error codes
#define WSAEWOULDBLOCK    10035
#define WSAEINPROGRESS    10036
#define WSAEALREADY       10037
#define WSAENOTSOCK       10038
#define WSAECONNREFUSED   10061
#define WSAECONNRESET     10054
#define WSAECONNABORTED   10053
#define WSAETIMEDOUT      10060
#define WSAENETUNREACH    10051
#define WSAEHOSTUNREACH   10065
#define WSANOTINITIALISED 10093
#define WSAEINVAL         10022
#define WSAEMSGSIZE       10040
#define WSAENOTCONN       10057
#define WSAEAFNOSUPPORT   10047

// Windows AF/SOCK/IPPROTO match BSD values on most platforms
#define WIN_AF_INET       2
#define WIN_AF_INET6      23
#define WIN_SOCK_STREAM   1
#define WIN_SOCK_DGRAM    2

// FIONBIO for ioctlsocket
#define WIN_FIONBIO       0x8004667E
#define WIN_FIONREAD      0x4004667F

// SOL_SOCKET / options
#define WIN_SOL_SOCKET    0xFFFF
#define WIN_SO_REUSEADDR  0x0004
#define WIN_SO_KEEPALIVE  0x0008
#define WIN_SO_BROADCAST  0x0020
#define WIN_SO_LINGER     0x0080
#define WIN_SO_SNDBUF     0x1001
#define WIN_SO_RCVBUF     0x1002
#define WIN_SO_SNDTIMEO   0x1005
#define WIN_SO_RCVTIMEO   0x1006
#define WIN_SO_ERROR      0x1007
#define WIN_IPPROTO_TCP   6
#define WIN_TCP_NODELAY   0x0001

// ── Socket handle table ─────────────────────────────────────────────
#define WG_MAX_SOCKETS 128

struct WGWinsock {
    bool initialised;
    int  last_error;
    // Map Windows SOCKET values to real fds. Windows SOCKETs in 32-bit
    // are unsigned ints; we use a small table indexed by (handle - base).
    int  fds[WG_MAX_SOCKETS]; // -1 = unused
    bool connect_reported[WG_MAX_SOCKETS]; // FD_CONNECT delivered once per socket
};

#define SOCK_BASE 0x1000u

WGWinsock *wg_winsock_create(void) {
    WGWinsock *ws = calloc(1, sizeof(WGWinsock));
    if (!ws) return NULL;
    for (int i = 0; i < WG_MAX_SOCKETS; i++) ws->fds[i] = -1;
    return ws;
}

void wg_winsock_destroy(WGWinsock *ws) {
    if (!ws) return;
    for (int i = 0; i < WG_MAX_SOCKETS; i++) {
        if (ws->fds[i] >= 0) close(ws->fds[i]);
    }
    free(ws);
}

static uint32_t alloc_socket(WGWinsock *ws, int fd) {
    for (int i = 0; i < WG_MAX_SOCKETS; i++) {
        if (ws->fds[i] < 0) {
            ws->fds[i] = fd;
            return SOCK_BASE + i;
        }
    }
    close(fd);
    return WIN_INVALID_SOCKET;
}

static int lookup_fd(WGWinsock *ws, uint32_t s) {
    if (s < SOCK_BASE || s >= SOCK_BASE + WG_MAX_SOCKETS) return -1;
    return ws->fds[s - SOCK_BASE];
}

static void free_socket(WGWinsock *ws, uint32_t s) {
    if (s < SOCK_BASE || s >= SOCK_BASE + WG_MAX_SOCKETS) return;
    int idx = s - SOCK_BASE;
    if (ws->fds[idx] >= 0) { close(ws->fds[idx]); ws->fds[idx] = -1; }
}

static int errno_to_wsa(int e) {
    switch (e) {
        case EWOULDBLOCK:   return WSAEWOULDBLOCK;
        case EINPROGRESS:   return WSAEINPROGRESS;
        case EALREADY:      return WSAEALREADY;
        case ECONNREFUSED:  return WSAECONNREFUSED;
        case ECONNRESET:    return WSAECONNRESET;
        case ECONNABORTED:  return WSAECONNABORTED;
        case ETIMEDOUT:     return WSAETIMEDOUT;
        case ENETUNREACH:   return WSAENETUNREACH;
        case EHOSTUNREACH:  return WSAEHOSTUNREACH;
        case EINVAL:        return WSAEINVAL;
        case EMSGSIZE:      return WSAEMSGSIZE;
        case ENOTCONN:      return WSAENOTCONN;
        case EAFNOSUPPORT:  return WSAEAFNOSUPPORT;
        case EBADF:         return WSAENOTSOCK;
        default:            return WSAEINVAL;
    }
}

// ── Win32 sockaddr ↔ BSD sockaddr conversion ────────────────────────
// Windows sockaddr_in has a 16-bit sa_family at +0 (no length byte). BSD/iOS
// sockaddr_in has sin_len@0 + sin_family@1. port@2 and addr@4 match. So a raw
// copy turns Win32 family=2 into BSD sin_len=2, sin_family=0 (AF_UNSPEC) — which
// makes connect()/bind() fail with af=0. Convert the family field explicitly.

static void read_sockaddr(void *blink, uint32_t guest_ptr, int len,
                          struct sockaddr_storage *out, socklen_t *outlen) {
    memset(out, 0, sizeof(*out));
    if (!guest_ptr || len <= 0) { *outlen = 0; return; }
    if (len > (int)sizeof(*out)) len = (int)sizeof(*out);
    uint8_t buf[sizeof(struct sockaddr_storage)] = {0};
    wg_blink_read_mem(blink, guest_ptr, buf, len);
    uint16_t win_fam = (uint16_t)(buf[0] | (buf[1] << 8)); // Win32 16-bit family
    memcpy(out, buf, len);                                  // port@2 / addr@4 align
    if (win_fam == 2) { // AF_INET
        struct sockaddr_in *si = (struct sockaddr_in *)out;
        si->sin_len = sizeof(struct sockaddr_in);
        si->sin_family = AF_INET;
        *outlen = sizeof(struct sockaddr_in);
    } else if (win_fam == WIN_AF_INET6) { // Win AF_INET6=23 -> BSD 30
        struct sockaddr_in6 *si6 = (struct sockaddr_in6 *)out;
        si6->sin6_len = sizeof(struct sockaddr_in6);
        si6->sin6_family = AF_INET6;
        *outlen = sizeof(struct sockaddr_in6);
    } else {
        ((struct sockaddr *)out)->sa_family = (sa_family_t)win_fam;
        *outlen = (socklen_t)len;
    }
}

static void write_sockaddr(void *blink, uint32_t guest_ptr, uint32_t guest_len_ptr,
                           const struct sockaddr_storage *sa, socklen_t salen) {
    if (!guest_ptr || !guest_len_ptr) return;
    // Build a WIN32-layout sockaddr (2-byte family, no sin_len byte). A raw copy
    // of the BSD struct would leave sin_len@0, so the guest reads family=0x0210.
    uint8_t wsa[sizeof(struct sockaddr_storage)] = {0};
    uint32_t wlen = salen;
    if (sa->ss_family == AF_INET) {
        const struct sockaddr_in *si = (const struct sockaddr_in *)sa;
        wsa[0] = 2; wsa[1] = 0;                 // AF_INET (16-bit)
        memcpy(wsa + 2, &si->sin_port, 2);      // port (net order) — offset matches
        memcpy(wsa + 4, &si->sin_addr, 4);      // addr — offset matches
        wlen = 16;
    } else if (sa->ss_family == AF_INET6) {
        const struct sockaddr_in6 *si6 = (const struct sockaddr_in6 *)sa;
        wsa[0] = (uint8_t)(WIN_AF_INET6 & 0xFF);
        wsa[1] = (uint8_t)(WIN_AF_INET6 >> 8);  // family (23)
        memcpy(wsa + 2,  &si6->sin6_port, 2);
        memcpy(wsa + 4,  &si6->sin6_flowinfo, 4);
        memcpy(wsa + 8,  &si6->sin6_addr, 16);
        memcpy(wsa + 24, &si6->sin6_scope_id, 4);
        wlen = 28;
    } else {
        memcpy(wsa, sa, salen > sizeof(wsa) ? sizeof(wsa) : salen);
    }
    uint32_t buflen = 0;
    wg_blink_read_mem(blink, guest_len_ptr, &buflen, 4);
    uint32_t copy = wlen < buflen ? wlen : buflen;
    wg_blink_write_mem(blink, guest_ptr, wsa, copy);
    wg_blink_write_mem(blink, guest_len_ptr, &wlen, 4);
}

// ── Handler ─────────────────────────────────────────────────────────
bool wg_winsock_handle(WGWinsock *ws, const char *fn,
                       uint32_t *args, uint64_t *out_ret,
                       void *blink) {
    // ── WSAStartup(wVersionRequested, lpWSAData) ────────────────────
    if (strcmp(fn, "WSAStartup") == 0) {
        ws->initialised = true;
        ws->last_error = 0;
        // Fill WSAData at args[1]: wVersion, wHighVersion, then strings
        if (args[1]) {
            uint8_t data[400];
            memset(data, 0, sizeof(data));
            uint16_t ver = (uint16_t)args[0];
            memcpy(data + 0, &ver, 2);      // wVersion
            memcpy(data + 2, &ver, 2);      // wHighVersion
            const char *desc = "WineGlass WS2";
            memcpy(data + 4, desc, strlen(desc));
            wg_blink_write_mem(blink, args[1], data, 400);
        }
        WG_LOGI(TAG, "WSAStartup(0x%X) -> 0", args[0]);
        *out_ret = 0;
        return true;
    }

    if (strcmp(fn, "WSACleanup") == 0) {
        ws->initialised = false;
        *out_ret = 0;
        return true;
    }

    // ── WSAGetLastError / WSASetLastError ────────────────────────────
    if (strcmp(fn, "WSAGetLastError") == 0) {
        *out_ret = (uint32_t)ws->last_error;
        return true;
    }
    if (strcmp(fn, "WSASetLastError") == 0) {
        ws->last_error = (int)args[0];
        *out_ret = 0;
        return true;
    }

    // ── inet_ntop(af, src, dst, size) -> dst (or NULL) ───────────────
    // __stdcall/4 args. Must be handled (not auto-stubbed) or the wrong arg
    // cleanup corrupts the guest stack for the NEXT call (e.g. connect af=0).
    if (strcmp(fn, "inet_ntop") == 0 || strcmp(fn, "InetNtopA") == 0) {
        int bsd_af = (args[0] == WIN_AF_INET6) ? AF_INET6 : (int)args[0];
        int alen = (bsd_af == AF_INET6) ? 16 : 4;
        uint8_t addr[16] = {0};
        wg_blink_read_mem(blink, args[1], addr, alen);
        char tmp[64] = {0};
        if (inet_ntop(bsd_af, addr, tmp, sizeof(tmp))) {
            uint32_t len = (uint32_t)strlen(tmp) + 1;
            if (args[2] && len <= args[3]) {
                wg_blink_write_mem(blink, args[2], tmp, len);
                *out_ret = args[2]; // returns the dst pointer
            } else { ws->last_error = WSAEINVAL; *out_ret = 0; }
        } else { ws->last_error = WSAEINVAL; *out_ret = 0; }
        WG_LOGI(TAG, "inet_ntop(af=%u) -> %s", args[0], tmp[0] ? tmp : "(fail)");
        return true;
    }

    // ── inet_pton(af, src_str, dst) -> 1/0/-1 ────────────────────────
    if (strcmp(fn, "inet_pton") == 0 || strcmp(fn, "InetPtonA") == 0) {
        int bsd_af = (args[0] == WIN_AF_INET6) ? AF_INET6 : (int)args[0];
        char str[128] = {0};
        wg_blink_read_mem(blink, args[1], str, sizeof(str) - 1);
        uint8_t addr[16] = {0};
        int r2 = inet_pton(bsd_af, str, addr);
        if (r2 == 1 && args[2])
            wg_blink_write_mem(blink, args[2], addr, (bsd_af == AF_INET6) ? 16 : 4);
        *out_ret = (uint32_t)r2;
        return true;
    }

    // ── socket(af, type, protocol) ──────────────────────────────────
    if (strcmp(fn, "socket") == 0) {
        int af = (int)args[0];
        int type = (int)args[1];
        int proto = (int)args[2];
        if (af == WIN_AF_INET6) af = AF_INET6;
        int fd = socket(af, type, proto);
        if (fd < 0) {
            ws->last_error = errno_to_wsa(errno);
            WG_LOGW(TAG, "socket(%d,%d,%d) failed: %s", args[0], args[1], args[2], strerror(errno));
            *out_ret = WIN_INVALID_SOCKET;
        } else {
            uint32_t s = alloc_socket(ws, fd);
            WG_LOGI(TAG, "socket(%d,%d,%d) -> 0x%X (fd=%d)", args[0], args[1], args[2], s, fd);
            *out_ret = s;
        }
        return true;
    }

    // ── closesocket(s) ──────────────────────────────────────────────
    if (strcmp(fn, "closesocket") == 0) {
        free_socket(ws, args[0]);
        WG_LOGD(TAG, "closesocket(0x%X)", args[0]);
        *out_ret = 0;
        return true;
    }

    // ── connect(s, name, namelen) ───────────────────────────────────
    if (strcmp(fn, "connect") == 0) {
        int fd = lookup_fd(ws, args[0]);
        if (fd < 0) { ws->last_error = WSAENOTSOCK; *out_ret = WIN_SOCKET_ERROR; return true; }
        struct sockaddr_storage sa; socklen_t salen;
        read_sockaddr(blink, args[1], (int)args[2], &sa, &salen);
        char ipstr[64] = {0};
        if (sa.ss_family == AF_INET) {
            struct sockaddr_in *s4 = (struct sockaddr_in *)&sa;
            inet_ntop(AF_INET, &s4->sin_addr, ipstr, sizeof(ipstr));
            WG_LOGI(TAG, "connect(0x%X, %s:%d)", args[0], ipstr, ntohs(s4->sin_port));
        } else {
            WG_LOGI(TAG, "connect(0x%X, af=%d)", args[0], sa.ss_family);
        }
        int r = connect(fd, (struct sockaddr *)&sa, salen);
        if (r < 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EALREADY)) {
            // The socket is non-blocking (the app set FIONBIO), so connect can't
            // finish immediately. Rather than hand back WSAEWOULDBLOCK and rely
            // on the app's connect-completion poll (Steam's bootstrapper waits on
            // it but never sees it complete, then declares itself offline), wait
            // for completion here via select() — same as ConnectEx — then restore
            // blocking mode for the TLS I/O that follows, and report success.
            int saved = fcntl(fd, F_GETFL, 0);
            fd_set wfds, efds;
            FD_ZERO(&wfds); FD_SET(fd, &wfds);
            FD_ZERO(&efds); FD_SET(fd, &efds);
            struct timeval tv = {15, 0};
            int sr = select(fd + 1, NULL, &wfds, &efds, &tv);
            if (sr > 0) {
                int err = 0; socklen_t elen = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
                r = err ? -1 : 0;
                if (err) errno = err;
            } else {
                r = -1; ws->last_error = WSAETIMEDOUT;
                WG_LOGW(TAG, "connect: timed out waiting for completion");
                *out_ret = WIN_SOCKET_ERROR;
                return true;
            }
            fcntl(fd, F_SETFL, saved); // restore the mode the app set (non-blocking)
        }
        if (r < 0) {
            ws->last_error = errno_to_wsa(errno);
            WG_LOGI(TAG, "connect -> err errno=%d wsa=%u", errno, ws->last_error);
            *out_ret = WIN_SOCKET_ERROR;
        } else {
            WG_LOGI(TAG, "connect -> success (%s)", ipstr);
            *out_ret = 0;
        }
        return true;
    }

    // ── send(s, buf, len, flags) ────────────────────────────────────
    if (strcmp(fn, "send") == 0) {
        int fd = lookup_fd(ws, args[0]);
        if (fd < 0) { ws->last_error = WSAENOTSOCK; *out_ret = WIN_SOCKET_ERROR; return true; }
        uint32_t len = args[2];
        if (len > 65536) len = 65536;
        uint8_t *buf = malloc(len);
        if (!buf) { ws->last_error = WSAEINVAL; *out_ret = WIN_SOCKET_ERROR; return true; }
        wg_blink_read_mem(blink, args[1], buf, len);
        char hex[64] = {0}; uint32_t hn = len < 16 ? len : 16;
        for (uint32_t i = 0; i < hn; i++) snprintf(hex + i*3, 4, "%02x ", buf[i]);
        // DIAG: dump a TLS handshake record (0x16) to a file for offline parsing.
        if (len > 64 && buf[0] == 0x16) {
            FILE *cf = fopen("/tmp/wg_clienthello.bin", "wb");
            if (cf) { fwrite(buf, 1, len, cf); fclose(cf); }
        }
        ssize_t sent = send(fd, buf, len, 0);
        free(buf);
        if (sent < 0) {
            ws->last_error = errno_to_wsa(errno);
            WG_LOGI(TAG, "send(0x%X, %u) -> ERR errno=%d wsa=%d", args[0], len, errno, ws->last_error);
            *out_ret = WIN_SOCKET_ERROR;
        } else {
            WG_LOGI(TAG, "send(0x%X, %u) -> %zd [%s]", args[0], len, sent, hex);
            // A TLS alert (record type 0x15) means the guest's TLS engine is
            // bailing. The TLS code omits frame pointers, so an EBP walk misses
            // it — instead SCAN the raw stack for dwords that point into
            // steam.exe .text (0x401000..0x6E1000), i.e. return addresses. That
            // reveals the BoringSSL/handshake call chain that produced the alert.
            if (sent >= 1 && hex[0] == '1' && hex[1] == '5') {
                uint32_t esp = (uint32_t)wg_blink_get_reg(blink, 4);
                uint32_t stk[256] = {0};
                wg_blink_read_mem(blink, esp, stk, sizeof(stk));
                int printed = 0;
                for (int i = 0; i < 256 && printed < 40; i++) {
                    uint32_t v = stk[i];
                    if (v >= 0x401000 && v < 0x6E1000) {
                        WG_LOGW(TAG, "  TLS-alert stk[+0x%X] ret=0x%X", i*4, v);
                        printed++;
                    }
                }
            }
            *out_ret = (uint32_t)sent;
        }
        return true;
    }

    // ── recv(s, buf, len, flags) ────────────────────────────────────
    if (strcmp(fn, "recv") == 0) {
        int fd = lookup_fd(ws, args[0]);
        if (fd < 0) { ws->last_error = WSAENOTSOCK; *out_ret = WIN_SOCKET_ERROR; return true; }
        uint32_t len = args[2];
        if (len > 65536) len = 65536;
        uint8_t *buf = malloc(len);
        if (!buf) { ws->last_error = WSAEINVAL; *out_ret = WIN_SOCKET_ERROR; return true; }
        ssize_t got = recv(fd, buf, len, 0);
        if (got < 0) {
            free(buf);
            ws->last_error = errno_to_wsa(errno);
            WG_LOGI(TAG, "recv(0x%X, %u) -> ERR errno=%d wsa=%d", args[0], len, errno, ws->last_error);
            *out_ret = WIN_SOCKET_ERROR;
        } else {
            char hex[64] = {0}; uint32_t hn = (got > 0) ? ((uint32_t)got < 16 ? (uint32_t)got : 16) : 0;
            for (uint32_t i = 0; i < hn; i++) snprintf(hex + i*3, 4, "%02x ", buf[i]);
            if (got > 0) wg_blink_write_mem(blink, args[1], buf, (uint32_t)got);
            free(buf);
            WG_LOGI(TAG, "recv(0x%X, %u) -> %zd bytes [%s]", args[0], len, got, hex);
            *out_ret = (uint32_t)got;
        }
        return true;
    }

    // ── select(nfds, readfds, writefds, exceptfds, timeout) ─────────
    if (strcmp(fn, "select") == 0) {
        // Windows fd_set: { u_int fd_count; SOCKET fd_array[FD_SETSIZE(=64)]; }.
        // select(nfds, readfds, writefds, exceptfds, timeout). Windows modifies
        // each set in place to contain ONLY the ready sockets; the app then uses
        // __WSAFDIsSet to test membership. We poll the real fds and rebuild each
        // set. (Old stub returned 1 without marking which fd -> infinite re-select.)
        uint32_t set_ptrs[3] = { args[1], args[2], args[3] }; // read, write, except
        int total = 0;
        int s_rdy_dbg[3] = {0,0,0};
        for (int si = 0; si < 3; si++) {
            if (!set_ptrs[si]) continue;
            uint32_t fd_count = 0;
            wg_blink_read_mem(blink, set_ptrs[si], &fd_count, 4);
            if (fd_count > 64) fd_count = 64;
            uint32_t arr[64] = {0};
            if (fd_count) wg_blink_read_mem(blink, set_ptrs[si] + 4, arr, fd_count * 4);
            uint32_t ready[64]; uint32_t rc = 0;
            for (uint32_t i = 0; i < fd_count; i++) {
                int fd = lookup_fd(ws, arr[i]);
                if (fd < 0) continue;
                struct pollfd pfd = { .fd = fd,
                    .events = (short)(si == 0 ? POLLIN : si == 1 ? POLLOUT : POLLPRI),
                    .revents = 0 };
                poll(&pfd, 1, 0);
                // STALL DIAG: when a readfds socket polls NOT-readable, check the
                // OS receive queue. FIONREAD>0 here means the kernel HAS data our
                // poll isn't surfacing (engine bug); FIONREAD==0 means the peer
                // genuinely sent nothing (dead/idle connection). Rate-limited.
                if (si == 0 && !(pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
                    static int dn = 0;
                    if ((dn++ % 20000) == 0) {
                        int navail = -1; ioctl(fd, FIONREAD, &navail);
                        WG_LOGW(TAG, "select STALL DIAG: fd=%d not-readable revents=0x%x FIONREAD=%d",
                                fd, pfd.revents, navail);
                    }
                }
                // Windows exceptfds fires ONLY for OOB/urgent data or a failed
                // connect — NOT for a normal readable/writable or half-closed
                // socket. macOS poll() raises POLLPRI *alongside* POLLHUP when the
                // peer closes (revents=0x12), which is NOT out-of-band data; a
                // plain FIN belongs in readfds (FD_CLOSE), not exceptfds. So only
                // mark except-ready for real OOB (POLLPRI without POLLHUP) or a
                // genuine SO_ERROR. Reporting a closing socket in exceptfds made
                // Steam's download select-loop treat the connection as failed and
                // tear it down the instant the server's response (or alert) arrived.
                bool except_rdy = false;
                if (si == 2) {
                    if ((pfd.revents & POLLPRI) && !(pfd.revents & POLLHUP)) {
                        except_rdy = true; // genuine OOB
                    } else if (pfd.revents & POLLERR) {
                        int soerr = 0; socklen_t sl = sizeof(soerr);
                        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr)
                            except_rdy = true;
                    }
                }
                bool rdy = (si == 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) ||
                           (si == 1 && (pfd.revents & POLLOUT)) ||
                           (si == 2 && except_rdy);
                if (rdy) ready[rc++] = arr[i];
            }
            wg_blink_write_mem(blink, set_ptrs[si], &rc, 4);
            if (rc) wg_blink_write_mem(blink, set_ptrs[si] + 4, ready, rc * 4);
            s_rdy_dbg[si] = (int)rc;
            total += (int)rc;
        }
        // Dedup: only log when the ready-count changes — a spinning select loop
        // otherwise floods the ring buffer and scrolls off real activity.
        { static int s_last_total = -1;
          if (total != s_last_total) { WG_LOGD(TAG, "select -> %d ready (r=%d w=%d e=%d)",
                                               total, s_rdy_dbg[0], s_rdy_dbg[1], s_rdy_dbg[2]);
                                       s_last_total = total; } }
        *out_ret = (uint32_t)total;
        return true;
    }

    // ── __WSAFDIsSet(s, fd_set*) -> nonzero if s is a member ─────────
    if (strcmp(fn, "__WSAFDIsSet") == 0) {
        int found = 0;
        if (args[1]) {
            uint32_t fd_count = 0;
            wg_blink_read_mem(blink, args[1], &fd_count, 4);
            if (fd_count > 64) fd_count = 64;
            uint32_t arr[64] = {0};
            if (fd_count) wg_blink_read_mem(blink, args[1] + 4, arr, fd_count * 4);
            for (uint32_t i = 0; i < fd_count; i++)
                if (arr[i] == args[0]) { found = 1; break; }
        }
        *out_ret = (uint32_t)found;
        return true;
    }

    // ── setsockopt(s, level, optname, optval, optlen) ───────────────
    if (strcmp(fn, "setsockopt") == 0) {
        int fd = lookup_fd(ws, args[0]);
        if (fd < 0) { ws->last_error = WSAENOTSOCK; *out_ret = WIN_SOCKET_ERROR; return true; }
        int level = (int)args[1];
        int opt = (int)args[2];
        // Map Windows constants to BSD
        if (level == WIN_SOL_SOCKET) level = SOL_SOCKET;
        if (level == WIN_IPPROTO_TCP) level = IPPROTO_TCP;
        // SO_UPDATE_CONNECT_CONTEXT (0x7010) — Windows-only, required after ConnectEx.
        // We connect synchronously so no kernel state to update; just succeed.
        if (level == SOL_SOCKET && opt == 0x7010) {
            WG_LOGI(TAG, "setsockopt(SO_UPDATE_CONNECT_CONTEXT) -> 0 (no-op)");
            *out_ret = 0;
            return true;
        }
        // Map option names
        int bsd_opt = opt;
        if (level == SOL_SOCKET) {
            switch (opt) {
                case WIN_SO_REUSEADDR: bsd_opt = SO_REUSEADDR; break;
                case WIN_SO_KEEPALIVE: bsd_opt = SO_KEEPALIVE; break;
                case WIN_SO_BROADCAST: bsd_opt = SO_BROADCAST; break;
                case WIN_SO_LINGER:    bsd_opt = SO_LINGER; break;
                case WIN_SO_SNDBUF:    bsd_opt = SO_SNDBUF; break;
                case WIN_SO_RCVBUF:    bsd_opt = SO_RCVBUF; break;
                case WIN_SO_SNDTIMEO:  bsd_opt = SO_SNDTIMEO; break;
                case WIN_SO_RCVTIMEO:  bsd_opt = SO_RCVTIMEO; break;
                case WIN_SO_ERROR:     bsd_opt = SO_ERROR; break;
                default:
                    // Unknown Windows SOL_SOCKET option — silently succeed
                    WG_LOGD(TAG, "setsockopt: unknown SOL_SOCKET opt=0x%X, ignoring", opt);
                    *out_ret = 0;
                    return true;
            }
        } else if (level == IPPROTO_TCP) {
            if (opt == WIN_TCP_NODELAY) bsd_opt = TCP_NODELAY;
        }
        uint32_t optlen = args[4];
        if (optlen > 64) optlen = 64;
        uint8_t optval[64] = {0};
        if (args[3] && optlen) wg_blink_read_mem(blink, args[3], optval, optlen);
        int r = setsockopt(fd, level, bsd_opt, optval, optlen);
        if (r < 0) { ws->last_error = errno_to_wsa(errno); *out_ret = WIN_SOCKET_ERROR; }
        else *out_ret = 0;
        return true;
    }

    // ── getsockopt(s, level, optname, optval, optlen) ───────────────
    if (strcmp(fn, "getsockopt") == 0) {
        int fd = lookup_fd(ws, args[0]);
        if (fd < 0) { ws->last_error = WSAENOTSOCK; *out_ret = WIN_SOCKET_ERROR; return true; }
        int level = (int)args[1] == WIN_SOL_SOCKET ? SOL_SOCKET : (int)args[1];
        int opt = (int)args[2];
        if (level == SOL_SOCKET && opt == WIN_SO_ERROR) opt = SO_ERROR;
        uint8_t optval[64] = {0};
        socklen_t optlen = 64;
        int r = getsockopt(fd, level, opt, optval, &optlen);
        if (r < 0) { ws->last_error = errno_to_wsa(errno); *out_ret = WIN_SOCKET_ERROR; }
        else {
            if (args[3]) wg_blink_write_mem(blink, args[3], optval, optlen);
            if (args[4]) wg_blink_write_mem(blink, args[4], &optlen, 4);
            *out_ret = 0;
        }
        return true;
    }

    // ── ioctlsocket(s, cmd, argp) ───────────────────────────────────
    if (strcmp(fn, "ioctlsocket") == 0) {
        int fd = lookup_fd(ws, args[0]);
        if (fd < 0) { ws->last_error = WSAENOTSOCK; *out_ret = WIN_SOCKET_ERROR; return true; }
        uint32_t cmd = args[1];
        if (cmd == WIN_FIONBIO) {
            uint32_t val = 0;
            if (args[2]) wg_blink_read_mem(blink, args[2], &val, 4);
            int flags = fcntl(fd, F_GETFL, 0);
            if (val) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
            fcntl(fd, F_SETFL, flags);
            WG_LOGD(TAG, "ioctlsocket(0x%X, FIONBIO, %u)", args[0], val);
            *out_ret = 0;
        } else if (cmd == WIN_FIONREAD) {
            int avail = 0;
            ioctl(fd, FIONREAD, &avail);
            if (args[2]) { uint32_t v = (uint32_t)avail; wg_blink_write_mem(blink, args[2], &v, 4); }
            *out_ret = 0;
        } else {
            WG_LOGW(TAG, "ioctlsocket: unknown cmd 0x%X", cmd);
            *out_ret = 0;
        }
        return true;
    }

    // ── shutdown(s, how) ────────────────────────────────────────────
    if (strcmp(fn, "shutdown") == 0) {
        int fd = lookup_fd(ws, args[0]);
        if (fd < 0) { ws->last_error = WSAENOTSOCK; *out_ret = WIN_SOCKET_ERROR; return true; }
        shutdown(fd, (int)args[1]);
        *out_ret = 0;
        return true;
    }

    // ── bind(s, name, namelen) ──────────────────────────────────────
    if (strcmp(fn, "bind") == 0) {
        int fd = lookup_fd(ws, args[0]);
        if (fd < 0) { ws->last_error = WSAENOTSOCK; *out_ret = WIN_SOCKET_ERROR; return true; }
        struct sockaddr_storage sa; socklen_t salen;
        read_sockaddr(blink, args[1], (int)args[2], &sa, &salen);
        int r = bind(fd, (struct sockaddr *)&sa, salen);
        if (r < 0) { ws->last_error = errno_to_wsa(errno); *out_ret = WIN_SOCKET_ERROR; }
        else *out_ret = 0;
        return true;
    }

    // ── listen(s, backlog) ──────────────────────────────────────────
    if (strcmp(fn, "listen") == 0) {
        int fd = lookup_fd(ws, args[0]);
        if (fd < 0) { ws->last_error = WSAENOTSOCK; *out_ret = WIN_SOCKET_ERROR; return true; }
        int r = listen(fd, (int)args[1]);
        if (r < 0) { ws->last_error = errno_to_wsa(errno); *out_ret = WIN_SOCKET_ERROR; }
        else *out_ret = 0;
        return true;
    }

    // ── getpeername(s, name, namelen) ────────────────────────────────
    if (strcmp(fn, "getpeername") == 0) {
        int fd = lookup_fd(ws, args[0]);
        if (fd < 0) { ws->last_error = WSAENOTSOCK; *out_ret = WIN_SOCKET_ERROR; return true; }
        struct sockaddr_storage sa; socklen_t salen = sizeof(sa);
        int r = getpeername(fd, (struct sockaddr *)&sa, &salen);
        if (r < 0) { ws->last_error = errno_to_wsa(errno); *out_ret = WIN_SOCKET_ERROR; }
        else { write_sockaddr(blink, args[1], args[2], &sa, salen); *out_ret = 0; }
        return true;
    }

    // ── getsockname(s, name, namelen) ───────────────────────────────
    if (strcmp(fn, "getsockname") == 0) {
        int fd = lookup_fd(ws, args[0]);
        if (fd < 0) { ws->last_error = WSAENOTSOCK; *out_ret = WIN_SOCKET_ERROR; return true; }
        struct sockaddr_storage sa; socklen_t salen = sizeof(sa);
        int r = getsockname(fd, (struct sockaddr *)&sa, &salen);
        if (r < 0) { ws->last_error = errno_to_wsa(errno); *out_ret = WIN_SOCKET_ERROR; }
        else { write_sockaddr(blink, args[1], args[2], &sa, salen); *out_ret = 0; }
        return true;
    }

    // ── htons / htonl / ntohs / ntohl ───────────────────────────────
    if (strcmp(fn, "htons") == 0) { *out_ret = htons((uint16_t)args[0]); return true; }
    if (strcmp(fn, "htonl") == 0) { *out_ret = htonl(args[0]); return true; }
    if (strcmp(fn, "ntohs") == 0) { *out_ret = ntohs((uint16_t)args[0]); return true; }
    if (strcmp(fn, "ntohl") == 0) { *out_ret = ntohl(args[0]); return true; }

    // ── inet_addr(cp) ───────────────────────────────────────────────
    if (strcmp(fn, "inet_addr") == 0) {
        char str[64] = {0};
        if (args[0]) wg_blink_read_mem(blink, args[0], str, 63);
        struct in_addr ia;
        if (inet_aton(str, &ia)) *out_ret = ia.s_addr;
        else *out_ret = 0xFFFFFFFF; // INADDR_NONE
        return true;
    }

    // ── inet_ntoa(in_addr) ──────────────────────────────────────────
    if (strcmp(fn, "inet_ntoa") == 0) {
        // args[0] is the in_addr value passed by value (4 bytes)
        struct in_addr ia; ia.s_addr = args[0];
        const char *s = inet_ntoa(ia);
        // Write to a fixed guest address (static buffer, like real inet_ntoa)
        uint32_t buf_addr = 0xA00200;
        wg_blink_write_mem(blink, buf_addr, s, (uint32_t)strlen(s) + 1);
        *out_ret = buf_addr;
        return true;
    }

    // ── gethostname(name, namelen) ──────────────────────────────────
    if (strcmp(fn, "gethostname") == 0) {
        const char *host = "wineglass";
        if (args[0] && args[1] > 0) {
            size_t n = strlen(host);
            if (n >= args[1]) n = args[1] - 1;
            char tmp[64] = {0};
            memcpy(tmp, host, n); tmp[n] = 0;
            wg_blink_write_mem(blink, args[0], tmp, (uint32_t)n + 1);
        }
        WG_LOGI(TAG, "gethostname -> '%s'", host);
        *out_ret = 0; // success
        return true;
    }

    // ── gethostbyname(name) -> struct hostent* (in guest scratch) ────
    // Steam's netadr.cpp resolves the local host this way; an unhandled call
    // returned NULL and tripped its assert. We build a hostent in the gai
    // scratch region (the engine maps 0xB00000..0xC00000 per VM).
    if (strcmp(fn, "gethostbyname") == 0) {
        char name[256] = {0};
        if (args[0]) wg_blink_read_mem(blink, args[0], name, 255);
        bool is_local = (name[0] == 0) ||
                        strcasecmp(name, "localhost") == 0 ||
                        strcasecmp(name, "wineglass") == 0;
        uint32_t ip_net = htonl(0x7F000001); // 127.0.0.1 fallback
        if (!is_local) {
            struct addrinfo h, *res = NULL;
            memset(&h, 0, sizeof h); h.ai_family = AF_INET;
            if (getaddrinfo(name, NULL, &h, &res) == 0 && res && res->ai_addr) {
                ip_net = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
            }
            if (res) freeaddrinfo(res);
        }
        const uint32_t HB = 0x00BF0000u;       // within the gai scratch region
        uint32_t name_addr = HB + 16, aliases_addr = HB + 96;
        uint32_t addrlist_addr = HB + 104, addr_addr = HB + 120;
        const char *hn = name[0] ? name : "wineglass";
        wg_blink_write_mem(blink, name_addr, (void *)hn, (uint32_t)strlen(hn) + 1);
        uint32_t zero = 0;
        wg_blink_write_mem(blink, aliases_addr, &zero, 4);          // h_aliases = {NULL}
        uint32_t al[2] = { addr_addr, 0 };
        wg_blink_write_mem(blink, addrlist_addr, al, 8);            // h_addr_list = {addr, NULL}
        wg_blink_write_mem(blink, addr_addr, &ip_net, 4);          // in_addr (network order)
        uint8_t he[16];
        memcpy(he + 0,  &name_addr, 4);
        memcpy(he + 4,  &aliases_addr, 4);
        uint16_t fam = 2 /*AF_INET*/, len = 4;
        memcpy(he + 8,  &fam, 2);
        memcpy(he + 10, &len, 2);
        memcpy(he + 12, &addrlist_addr, 4);
        wg_blink_write_mem(blink, HB, he, 16);
        WG_LOGI(TAG, "gethostbyname('%s') -> hostent@0x%X ip=0x%08X", hn, HB, ntohl(ip_net));
        *out_ret = HB;
        return true;
    }

    // ── getaddrinfo(nodename, servname, hints, res) ─────────────────
    if (strcmp(fn, "getaddrinfo") == 0) {
        char node[256] = {0}, serv[64] = {0};
        if (args[0]) wg_blink_read_mem(blink, args[0], node, 255);
        if (args[1]) wg_blink_read_mem(blink, args[1], serv, 63);

        // Read hints if provided
        struct addrinfo hints_h, *hints_p = NULL;
        memset(&hints_h, 0, sizeof(hints_h));
        if (args[2]) {
            uint32_t h[8] = {0};
            wg_blink_read_mem(blink, args[2], h, 32);
            // Win32 addrinfo: flags, family, socktype, protocol, addrlen, canonname, addr, next
            hints_h.ai_flags = (int)h[0];
            hints_h.ai_family = (int)h[1];
            if (hints_h.ai_family == WIN_AF_INET6) hints_h.ai_family = AF_INET6;
            hints_h.ai_socktype = (int)h[2];
            hints_h.ai_protocol = (int)h[3];
            hints_p = &hints_h;
        }

        WG_LOGI(TAG, "getaddrinfo('%s', '%s')", node, serv);

        struct addrinfo *res = NULL;
        int err = getaddrinfo(node[0] ? node : NULL, serv[0] ? serv : NULL, hints_p, &res);
        if (err != 0) {
            WG_LOGW(TAG, "getaddrinfo failed: %s", gai_strerror(err));
            // Map EAI errors to Winsock errors
            ws->last_error = WSAEHOSTUNREACH;
            *out_ret = 11001; // WSAHOST_NOT_FOUND
            return true;
        }

        // Serialize addrinfo chain into the guest scratch region (1MB @
        // s_gai_base). The ENGINE maps this region per VM load (load_pe_blink),
        // so it's always valid for the current VM — a winsock-side one-shot
        // static flag would skip re-mapping after a VM recreation and the guest
        // would fault reading the result. Bump from the base each call (results
        // are short-lived / freed before the next lookup).
        uint32_t ptr = s_gai_base;
        uint32_t prev_next = 0;

        for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
            uint32_t node_base = ptr;
            uint32_t sa_ptr = node_base + 32; // sockaddr right after addrinfo
            uint32_t sa_len = ai->ai_addrlen;
            uint32_t next_ptr = ai->ai_next ? (sa_ptr + ((sa_len + 3) & ~3u)) : 0;

            // Write addrinfo struct (Win32 layout)
            uint32_t aih[8] = {0};
            aih[0] = (uint32_t)ai->ai_flags;
            aih[1] = (uint32_t)ai->ai_family;
            if (aih[1] == AF_INET6) aih[1] = WIN_AF_INET6;
            aih[2] = (uint32_t)ai->ai_socktype;
            aih[3] = (uint32_t)ai->ai_protocol;
            aih[4] = sa_len;
            aih[5] = 0; // canonname (NULL)
            aih[6] = sa_ptr;
            aih[7] = next_ptr;
            wg_blink_write_mem(blink, node_base, aih, 32);

            // Write sockaddr in WIN32 layout. BSD/iOS sockaddr_in has a leading
            // sin_len byte that Win32 lacks, so a raw copy misaligns sin_family
            // (guest reads 0x0210 instead of 2 → treats addr as invalid → 0.0.0.0).
            // Build the Win32 sockaddr explicitly. Port/addr offsets already match.
            if (ai->ai_addr && sa_len > 0) {
                uint8_t wsa[28] = {0};
                if (ai->ai_family == AF_INET && sa_len >= (uint32_t)sizeof(struct sockaddr_in)) {
                    struct sockaddr_in *si = (struct sockaddr_in *)ai->ai_addr;
                    wsa[0] = 2; wsa[1] = 0;                  // AF_INET (16-bit family)
                    memcpy(wsa + 2, &si->sin_port, 2);       // port (network order)
                    memcpy(wsa + 4, &si->sin_addr, 4);       // IPv4 address
                    sa_len = 16;
                } else if (ai->ai_family == AF_INET6 && sa_len >= (uint32_t)sizeof(struct sockaddr_in6)) {
                    struct sockaddr_in6 *si6 = (struct sockaddr_in6 *)ai->ai_addr;
                    wsa[0] = (uint8_t)(WIN_AF_INET6 & 0xFF);
                    wsa[1] = (uint8_t)(WIN_AF_INET6 >> 8);   // family (23)
                    memcpy(wsa + 2,  &si6->sin6_port, 2);    // port
                    memcpy(wsa + 4,  &si6->sin6_flowinfo, 4);
                    memcpy(wsa + 8,  &si6->sin6_addr, 16);   // IPv6 address
                    memcpy(wsa + 24, &si6->sin6_scope_id, 4);
                    sa_len = 28;
                }
                wg_blink_write_mem(blink, sa_ptr, wsa, sa_len);
                // Keep addrinfo ai_addrlen consistent with what we wrote.
                wg_blink_write_mem(blink, node_base + 16, &sa_len, 4);
                char ipdbg[64] = {0};
                if (ai->ai_family == AF_INET)
                    inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, ipdbg, sizeof(ipdbg));
                WG_LOGI(TAG, "getaddrinfo: node fam=%d -> %s", ai->ai_family, ipdbg[0] ? ipdbg : "?");
            }

            if (prev_next) {
                // Patch previous node's next pointer
                wg_blink_write_mem(blink, prev_next, &node_base, 4);
            }
            prev_next = node_base + 28; // offset of ai_next
            ptr = next_ptr ? next_ptr : (sa_ptr + ((sa_len + 3) & ~3u));
        }

        // Write pointer to first node (always the region base).
        if (args[3]) {
            uint32_t first = s_gai_base;
            wg_blink_write_mem(blink, args[3], &first, 4);
        }
        (void)ptr;
        freeaddrinfo(res);
        WG_LOGI(TAG, "getaddrinfo -> OK");
        *out_ret = 0;
        return true;
    }

    // ── freeaddrinfo(ai) ────────────────────────────────────────────
    if (strcmp(fn, "freeaddrinfo") == 0) {
        // Guest memory is bump-allocated; nothing to free
        *out_ret = 0;
        return true;
    }

    // ── WSASend / WSARecv / WSASendTo / WSARecvFrom ─────────────────
    // These use WSABUF structures. For now, stub them.
    // ── WSASend(s, lpBuffers, dwBufferCount, lpBytesOut, flags, ovl, cbrtn) ──
    if (strcmp(fn, "WSASend") == 0 || strcmp(fn, "WSASendTo") == 0) {
        int fd = lookup_fd(ws, args[0]);
        if (fd < 0) { ws->last_error = WSAENOTSOCK; *out_ret = WIN_SOCKET_ERROR; return true; }
        uint32_t buf_count = args[2];
        if (buf_count == 0 || !args[1]) { ws->last_error = WSAEINVAL; *out_ret = WIN_SOCKET_ERROR; return true; }
        if (buf_count > 16) buf_count = 16;
        // Gather WSABUF entries: each is { u32 len; u32 ptr } (8 bytes in 32-bit)
        size_t total = 0;
        for (uint32_t i = 0; i < buf_count; i++) {
            uint32_t e[2]; wg_blink_read_mem(blink, args[1] + i * 8, e, 8);
            total += e[0];
        }
        if (total == 0) { if (args[3]) { uint32_t z = 0; wg_blink_write_mem(blink, args[3], &z, 4); } *out_ret = 0; return true; }
        uint8_t *buf = malloc(total);
        size_t off = 0;
        for (uint32_t i = 0; i < buf_count; i++) {
            uint32_t e[2]; wg_blink_read_mem(blink, args[1] + i * 8, e, 8);
            if (e[0] && e[1]) { wg_blink_read_mem(blink, e[1], buf + off, e[0]); off += e[0]; }
        }
        uint8_t pv[8] = {0}; memcpy(pv, buf, total < 8 ? total : 8);
        ssize_t sent = send(fd, buf, total, 0);
        free(buf);
        if (sent < 0) {
            ws->last_error = errno_to_wsa(errno);
            WG_LOGI(TAG, "WSASend(0x%X, %zu, ovl=0x%X) failed: %s", args[0], total, args[5], strerror(errno));
            *out_ret = WIN_SOCKET_ERROR;
        } else {
            if (args[3]) { uint32_t bs = (uint32_t)sent; wg_blink_write_mem(blink, args[3], &bs, 4); }
            if (args[5]) { uint32_t bs = (uint32_t)sent; wg_blink_write_mem(blink, args[5] + 4, &bs, 4); }
            WG_LOGI(TAG, "WSASend(0x%X, %zu, ovl=0x%X) -> %zd [%02x %02x %02x %02x]",
                    args[0], total, args[5], sent, pv[0], pv[1], pv[2], pv[3]);
            *out_ret = 0;
        }
        return true;
    }
    // ── WSARecv(s, lpBuffers, dwBufferCount, lpBytesOut, flags, ovl, cbrtn) ──
    if (strcmp(fn, "WSARecv") == 0 || strcmp(fn, "WSARecvFrom") == 0) {
        int fd = lookup_fd(ws, args[0]);
        if (fd < 0) { ws->last_error = WSAENOTSOCK; *out_ret = WIN_SOCKET_ERROR; return true; }
        uint32_t buf_count = args[2];
        if (buf_count == 0 || !args[1]) { ws->last_error = WSAEINVAL; *out_ret = WIN_SOCKET_ERROR; return true; }
        if (buf_count > 16) buf_count = 16;
        // Compute total capacity across WSABUF entries
        size_t total_cap = 0;
        for (uint32_t i = 0; i < buf_count; i++) {
            uint32_t e[2]; wg_blink_read_mem(blink, args[1] + i * 8, e, 8);
            total_cap += e[0];
        }
        if (total_cap > 65536) total_cap = 65536;
        uint8_t *buf = malloc(total_cap);
        ssize_t got = recv(fd, buf, total_cap, 0);
        if (got < 0) {
            free(buf);
            ws->last_error = errno_to_wsa(errno);
            WG_LOGI(TAG, "WSARecv(0x%X, cap=%zu, ovl=0x%X) failed: %s (wsa=%d)",
                    args[0], total_cap, args[5], strerror(errno), ws->last_error);
            *out_ret = WIN_SOCKET_ERROR;
        } else {
            // Scatter received bytes into WSABUF entries
            size_t off = 0;
            for (uint32_t i = 0; i < buf_count && off < (size_t)got; i++) {
                uint32_t e[2]; wg_blink_read_mem(blink, args[1] + i * 8, e, 8);
                uint32_t copy = e[0];
                if (copy > (uint32_t)((size_t)got - off)) copy = (uint32_t)((size_t)got - off);
                if (e[1] && copy > 0) wg_blink_write_mem(blink, e[1], buf + off, copy);
                off += copy;
            }
            free(buf);
            if (args[3]) { uint32_t bg = (uint32_t)got; wg_blink_write_mem(blink, args[3], &bg, 4); }
            if (args[5]) { uint32_t bg = (uint32_t)got; wg_blink_write_mem(blink, args[5] + 4, &bg, 4); }
            WG_LOGI(TAG, "WSARecv(0x%X, cap=%zu, ovl=0x%X) -> %zd", args[0], total_cap, args[5], got);
            *out_ret = 0;
        }
        return true;
    }

    // ── WSAIoctl ────────────────────────────────────────────────────
    if (strcmp(fn, "WSAIoctl") == 0) {
        // WSAIoctl(s, code, inBuf, inLen, outBuf, outLen, bytesRet, overlapped, completionRoutine)
        // We handle SIO_GET_EXTENSION_FUNCTION_POINTER in the engine dispatch.
        // For other IOCTLs, return success with 0 bytes.
        if (args[6]) {
            uint32_t z = 0;
            wg_blink_write_mem(blink, args[6], &z, 4);
        }
        *out_ret = 0;
        return true;
    }

    // ── ConnectEx(s, name, namelen, sendBuf, sendLen, bytesSent, overlapped)
    if (strcmp(fn, "ConnectEx") == 0) {
        int fd = lookup_fd(ws, args[0]);
        if (fd < 0) { ws->last_error = WSAENOTSOCK; *out_ret = 0; return true; }
        struct sockaddr_storage sa; socklen_t salen;
        read_sockaddr(blink, args[1], (int)args[2], &sa, &salen);
        char ipstr[64] = {0};
        uint16_t port = 0;
        if (sa.ss_family == AF_INET) {
            struct sockaddr_in *s4 = (struct sockaddr_in *)&sa;
            inet_ntop(AF_INET, &s4->sin_addr, ipstr, sizeof(ipstr));
            port = ntohs(s4->sin_port);
        }
        WG_LOGI(TAG, "ConnectEx(0x%X -> %s:%u)", args[0], ipstr, port);
        // Use non-blocking connect + select() with 15s timeout, then switch
        // socket to blocking for subsequent TLS I/O (send/recv block cleanly).
        int cur_flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, cur_flags | O_NONBLOCK);
        int r = connect(fd, (struct sockaddr *)&sa, salen);
        if (r < 0 && errno == EINPROGRESS) {
            fd_set wfds, efds;
            FD_ZERO(&wfds); FD_SET(fd, &wfds);
            FD_ZERO(&efds); FD_SET(fd, &efds);
            struct timeval tv = {15, 0};
            int sr = select(fd + 1, NULL, &wfds, &efds, &tv);
            if (sr > 0) {
                int err = 0; socklen_t elen = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
                if (err != 0) {
                    ws->last_error = errno_to_wsa(err);
                    WG_LOGW(TAG, "ConnectEx failed: %s", strerror(err));
                    *out_ret = 0;
                    return true;
                }
                r = 0;
            } else {
                ws->last_error = WSAETIMEDOUT;
                WG_LOGW(TAG, "ConnectEx timed out");
                *out_ret = 0;
                return true;
            }
        }
        // Restore blocking mode for TLS I/O
        fcntl(fd, F_SETFL, cur_flags & ~O_NONBLOCK);
        if (r < 0) {
            ws->last_error = errno_to_wsa(errno);
            WG_LOGW(TAG, "ConnectEx failed: %s", strerror(errno));
            *out_ret = 0;
        } else {
            WG_LOGI(TAG, "ConnectEx connected %s:%u", ipstr, port);
            if (args[5]) { uint32_t z = 0; wg_blink_write_mem(blink, args[5], &z, 4); }
            *out_ret = 1;
        }
        return true;
    }

    // ── DisconnectEx(s, overlapped, flags, reserved) ───────────────
    if (strcmp(fn, "DisconnectEx") == 0) {
        int fd = lookup_fd(ws, args[0]);
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
        *out_ret = 1;
        return true;
    }

    // ── WSASocketW(af, type, proto, info, group, flags) ─────────────
    if (strcmp(fn, "WSASocketW") == 0 || strcmp(fn, "WSASocketA") == 0) {
        int af = (int)args[0], type = (int)args[1], proto = (int)args[2];
        WG_LOGI(TAG, "%s(af=%d, type=%d, proto=%d)", fn, af, type, proto);
        if (af == WIN_AF_INET6) af = AF_INET6;
        int fd = socket(af, type, proto);
        if (fd < 0) {
            ws->last_error = errno_to_wsa(errno);
            WG_LOGW(TAG, "%s FAILED: %s (errno=%d)", fn, strerror(errno), errno);
            *out_ret = WIN_INVALID_SOCKET;
        } else {
            *out_ret = alloc_socket(ws, fd);
            WG_LOGI(TAG, "%s -> 0x%X (fd=%d)", fn, (uint32_t)*out_ret, fd);
        }
        return true;
    }

    // ── WSAEnumNetworkEvents(s, hEvent, lpNetworkEvents) ────────────
    // WSANETWORKEVENTS { long lNetworkEvents; int iErrorCode[FD_MAX_EVENTS=10]; }
    // = 4 + 40 = 44 bytes. Bits: FD_READ=0x01, FD_WRITE=0x02, FD_CONNECT=0x10,
    // FD_CLOSE=0x20. iErrorCode index for FD_CONNECT is bit 4 -> offset 4 + 4*4.
    if (strcmp(fn, "WSAEnumNetworkEvents") == 0) {
        int fd = lookup_fd(ws, args[0]);
        uint32_t events = 0;
        int connect_err = 0;
        if (fd >= 0) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLOUT, .revents = 0 };
            poll(&pfd, 1, 0);
            uint32_t sidx = args[0] - SOCK_BASE;
            // Connection completion: socket becomes writable. Report FD_CONNECT
            // exactly once (a connected socket stays writable forever otherwise).
            if ((pfd.revents & POLLOUT) && sidx < WG_MAX_SOCKETS &&
                !ws->connect_reported[sidx]) {
                int soerr = 0; socklen_t sl = sizeof(soerr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                connect_err = soerr ? errno_to_wsa(soerr) : 0;
                events |= 0x10; // FD_CONNECT
                ws->connect_reported[sidx] = true;
            }
            if (pfd.revents & POLLIN)  events |= 0x01;             // FD_READ
            if (pfd.revents & (POLLHUP | POLLERR)) events |= 0x20; // FD_CLOSE
        }
        if (args[2]) {
            uint8_t ev[44] = {0};
            memcpy(ev, &events, 4);
            if (connect_err) memcpy(ev + 4 + 4 * 4, &connect_err, 4); // iErrorCode[FD_CONNECT_BIT]
            wg_blink_write_mem(blink, args[2], ev, 44);
        }
        WG_LOGI(TAG, "WSAEnumNetworkEvents(0x%X) -> events=0x%X err=%d", args[0], events, connect_err);
        *out_ret = 0;
        return true;
    }

    // ── WSAEventSelect(s, hEvent, lNetworkEvents) ───────────────────
    if (strcmp(fn, "WSAEventSelect") == 0) {
        // Make socket non-blocking (like WSAEventSelect does)
        int fd = lookup_fd(ws, args[0]);
        if (fd >= 0) {
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
        WG_LOGI(TAG, "WSAEventSelect(sock=0x%X, hEvent=0x%X, mask=0x%X)",
                args[0], args[1], args[2]);
        *out_ret = 0;
        return true;
    }

    return false; // not handled
}

uint32_t wg_winsock_get_last_error(WGWinsock *ws) { return ws ? (uint32_t)ws->last_error : 0; }
void     wg_winsock_set_last_error(WGWinsock *ws, uint32_t err) { if (ws) ws->last_error = (int)err; }

// Reactor backstop: returns true if ANY open socket has data ready to read (or a
// pending connect/close). Used by the cooperative scheduler to detect that the
// app's async I/O threads should be woken to recv — Windows would auto-signal an
// event/IOCP completion on readiness; we have no OS notifier, so we poll.
bool wg_winsock_any_readable(WGWinsock *ws) {
    if (!ws) return false;
    struct pollfd pfds[WG_MAX_SOCKETS];
    int n = 0;
    for (int i = 0; i < WG_MAX_SOCKETS; i++) {
        if (ws->fds[i] >= 0) {
            pfds[n].fd = ws->fds[i];
            pfds[n].events = POLLIN;
            pfds[n].revents = 0;
            n++;
        }
    }
    if (n == 0) return false;
    int r = poll(pfds, (nfds_t)n, 0);
    if (r <= 0) return false;
    for (int i = 0; i < n; i++) {
        if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) return true;
    }
    return false;
}
