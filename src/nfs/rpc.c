/* libdjlink: XDR codec and ONC-RPC v2 (RFC 1057) client over UDP.
 *
 * Hand-rolled on purpose: the whole surface we need is portmap GETPORT,
 * MOUNT MNT/UMNT and NFS LOOKUP/READ/READDIR, which is far less code than
 * binding libtirpc would be, and it stays thread-clean and portable.
 *
 * Authentication is AUTH_NULL. Pioneer's players accept that; what they will
 * not accept is ASCII path strings (see nfs2.c).
 */
#include "nfs_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- XDR writer ---------------- */

void djl_xdr_w_init(djl_xdr_w *w, uint8_t *buf, size_t cap)
{
    w->p = buf; w->cap = cap; w->len = 0; w->err = false;
}

static uint8_t *w_take(djl_xdr_w *w, size_t n)
{
    if (w->err || n > w->cap - w->len) { w->err = true; return NULL; }
    uint8_t *at = w->p + w->len;
    w->len += n;
    return at;
}

void djl_xdr_put_u32(djl_xdr_w *w, uint32_t v)
{
    uint8_t *at = w_take(w, 4);
    if (!at) return;
    at[0] = (uint8_t)(v >> 24); at[1] = (uint8_t)(v >> 16);
    at[2] = (uint8_t)(v >> 8);  at[3] = (uint8_t)v;
}

/* XDR pads every item up to a 4-byte boundary. */
static size_t pad4(size_t n) { return (4u - (n & 3u)) & 3u; }

void djl_xdr_put_fixed(djl_xdr_w *w, const uint8_t *d, size_t n)
{
    size_t pad = pad4(n);
    uint8_t *at = w_take(w, n + pad);
    if (!at) return;
    if (n) memcpy(at, d, n);
    if (pad) memset(at + n, 0, pad);
}

void djl_xdr_put_opaque(djl_xdr_w *w, const uint8_t *d, size_t n)
{
    if (n > 0xffffffffu) { w->err = true; return; }
    djl_xdr_put_u32(w, (uint32_t)n);
    djl_xdr_put_fixed(w, d, n);
}

/* ---------------- XDR reader ---------------- */

void djl_xdr_r_init(djl_xdr_r *r, const uint8_t *buf, size_t len)
{
    r->p = buf; r->len = len; r->pos = 0; r->err = false;
}

uint32_t djl_xdr_get_u32(djl_xdr_r *r)
{
    if (r->err || r->len - r->pos < 4) { r->err = true; return 0; }
    const uint8_t *at = r->p + r->pos;
    r->pos += 4;
    return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) |
           ((uint32_t)at[2] << 8)  | at[3];
}

bool djl_xdr_get_fixed(djl_xdr_r *r, uint8_t *out, size_t n)
{
    size_t pad = pad4(n);
    if (r->err || r->len - r->pos < n + pad) { r->err = true; return false; }
    if (n && out) memcpy(out, r->p + r->pos, n);
    r->pos += n + pad;
    return true;
}

bool djl_xdr_skip(djl_xdr_r *r, size_t n)
{
    if (r->err || r->len - r->pos < n) { r->err = true; return false; }
    r->pos += n;
    return true;
}

const uint8_t *djl_xdr_get_opaque(djl_xdr_r *r, uint32_t *out_len)
{
    if (out_len) *out_len = 0;
    uint32_t n = djl_xdr_get_u32(r);
    if (r->err) return NULL;
    size_t pad = pad4(n);
    if ((size_t)n > r->len - r->pos) { r->err = true; return NULL; }
    const uint8_t *at = r->p + r->pos;
    /* Trailing padding may be absent on the last item of a datagram. */
    r->pos += (pad <= r->len - r->pos - n) ? (size_t)n + pad : (size_t)n;
    if (out_len) *out_len = n;
    return at;
}

/* ---------------- UTF-8 -> UTF-16LE ---------------- */

size_t djl_utf16le_encode(const char *utf8, uint8_t *out, size_t cap)
{
    if (!utf8 || !out) return 0;
    size_t o = 0;
    for (const unsigned char *s = (const unsigned char *)utf8; *s; ) {
        uint32_t cp;
        if (*s < 0x80) {
            cp = *s++;
        } else if ((*s & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) {
            cp = (uint32_t)(*s & 0x1f) << 6 | (uint32_t)(s[1] & 0x3f);
            s += 2;
        } else if ((*s & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
            cp = (uint32_t)(*s & 0x0f) << 12 | (uint32_t)(s[1] & 0x3f) << 6 |
                 (uint32_t)(s[2] & 0x3f);
            s += 3;
        } else if ((*s & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 &&
                   (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
            cp = (uint32_t)(*s & 0x07) << 18 | (uint32_t)(s[1] & 0x3f) << 12 |
                 (uint32_t)(s[2] & 0x3f) << 6 | (uint32_t)(s[3] & 0x3f);
            s += 4;
        } else {
            cp = 0xfffd; s++;              /* invalid byte, substitute */
        }

        if (cp >= 0x10000) {                /* surrogate pair */
            if (o + 4 > cap) return 0;
            uint32_t v = cp - 0x10000;
            uint16_t hi = (uint16_t)(0xd800 | (v >> 10));
            uint16_t lo = (uint16_t)(0xdc00 | (v & 0x3ff));
            out[o++] = (uint8_t)hi; out[o++] = (uint8_t)(hi >> 8);
            out[o++] = (uint8_t)lo; out[o++] = (uint8_t)(lo >> 8);
        } else {
            if (o + 2 > cap) return 0;
            out[o++] = (uint8_t)cp; out[o++] = (uint8_t)(cp >> 8);
        }
    }
    return o;
}

/* ---------------- RPC framing ---------------- */

void djl_rpc_build_call(djl_xdr_w *w, uint32_t xid, uint32_t prog,
                        uint32_t vers, uint32_t proc)
{
    djl_xdr_put_u32(w, xid);
    djl_xdr_put_u32(w, 0);      /* msg_type = CALL */
    djl_xdr_put_u32(w, 2);      /* rpcvers */
    djl_xdr_put_u32(w, prog);
    djl_xdr_put_u32(w, vers);
    djl_xdr_put_u32(w, proc);
    djl_xdr_put_u32(w, 0);      /* cred: AUTH_NULL */
    djl_xdr_put_u32(w, 0);      /* cred length */
    djl_xdr_put_u32(w, 0);      /* verf: AUTH_NULL */
    djl_xdr_put_u32(w, 0);      /* verf length */
}

djl_err djl_rpc_reply_body(const uint8_t *buf, size_t len, uint32_t xid, size_t *body_off)
{
    djl_xdr_r r;
    djl_xdr_r_init(&r, buf, len);

    uint32_t got_xid = djl_xdr_get_u32(&r);
    uint32_t mtype   = djl_xdr_get_u32(&r);
    if (r.err) return DJL_ERR_SHORT;
    if (mtype != 1) return DJL_ERR_IO;              /* not a REPLY */
    if (got_xid != xid) return DJL_ERR_NOT_FOUND;   /* stale datagram */

    uint32_t reply_stat = djl_xdr_get_u32(&r);
    if (r.err) return DJL_ERR_SHORT;
    if (reply_stat != 0) return DJL_ERR_IO;         /* MSG_DENIED */

    /* Accepted reply: verifier, then accept_stat. */
    (void)djl_xdr_get_u32(&r);                      /* verf flavor */
    uint32_t verf_len = djl_xdr_get_u32(&r);
    if (r.err || !djl_xdr_skip(&r, verf_len + pad4(verf_len))) return DJL_ERR_SHORT;

    uint32_t accept_stat = djl_xdr_get_u32(&r);
    if (r.err) return DJL_ERR_SHORT;
    if (accept_stat != 0) return DJL_ERR_UNAVAILABLE;  /* PROG_MISMATCH etc. */

    if (body_off) *body_off = r.pos;
    return DJL_OK;
}

/* ---------------- RPC transport ---------------- */

djl_err djl_rpc_open(djl_rpc *r, const uint8_t ip[4])
{
    if (!r || !ip) return DJL_ERR_INVAL;
    memset(r, 0, sizeof *r);
    memcpy(r->ip, ip, 4);
    r->xid        = (uint32_t)(djl_now_ms() & 0xffffffffu) | 1u;
    r->timeout_ms = DJL_RPC_TIMEO_MS;
    r->attempts   = DJL_RPC_ATTEMPTS;
    r->rbuf       = malloc(DJL_RPC_BUF);
    if (!r->rbuf) return DJL_ERR_NOMEM;
    djl_err e = djl_udp_open(&r->sock);
    if (e != DJL_OK) { free(r->rbuf); r->rbuf = NULL; }
    return e;
}

void djl_rpc_close(djl_rpc *r)
{
    if (!r) return;
    djl_sock_close(&r->sock);
    free(r->rbuf);
    r->rbuf = NULL;
}

djl_err djl_rpc_call(djl_rpc *r, uint16_t port, uint32_t prog, uint32_t vers,
                     uint32_t proc, const uint8_t *args, size_t arglen,
                     const uint8_t **body_out, size_t *body_len)
{
    if (!r || !r->rbuf || !body_out || !body_len) return DJL_ERR_INVAL;
    if (port == 0) return DJL_ERR_INVAL;
    *body_out = NULL;
    *body_len = 0;

    uint8_t msg[2048];
    djl_xdr_w w;
    djl_xdr_w_init(&w, msg, sizeof msg);
    uint32_t xid = r->xid++;
    djl_rpc_build_call(&w, xid, prog, vers, proc);
    if (arglen) djl_xdr_put_fixed(&w, args, arglen);
    if (w.err) return DJL_ERR_NOMEM;

    unsigned timeout = r->timeout_ms;
    djl_err last = DJL_ERR_TIMEOUT;

    for (unsigned attempt = 0; attempt < r->attempts; attempt++) {
        djl_err e = djl_sock_send(&r->sock, r->ip, port, msg, w.len);
        if (e != DJL_OK) return e;

        /* Absorb late duplicates from earlier attempts: keep reading until the
         * xid matches or the window closes. */
        uint64_t deadline = djl_now_ms() + timeout;
        for (;;) {
            uint64_t now = djl_now_ms();
            if (now >= deadline) { last = DJL_ERR_TIMEOUT; break; }
            int n = djl_udp_recv_wait(&r->sock, r->rbuf, DJL_RPC_BUF,
                                      (unsigned)(deadline - now));
            if (n <= 0) { last = DJL_ERR_TIMEOUT; break; }

            size_t body = 0;
            e = djl_rpc_reply_body(r->rbuf, (size_t)n, xid, &body);
            if (e == DJL_ERR_NOT_FOUND) continue;      /* stale xid, keep waiting */
            if (e != DJL_OK) return e;

            *body_out = r->rbuf + body;
            *body_len = (size_t)n - body;
            return DJL_OK;
        }
        timeout *= 2;
    }
    return last;
}
