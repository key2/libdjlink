/* libdjlink NFS client internals: XDR, ONC-RPC v2, portmap, mount v1, NFS v2.
 *
 * The encode/decode halves are pure and unit-tested; only djl_rpc_call touches
 * a socket, and it does so through the OSAL so the Windows port stays in
 * osal.c. See ARCHITECTURE.md section 8.3 and HANDOVER.md section 5.1.
 */
#ifndef DJL_NFS_INTERNAL_H
#define DJL_NFS_INTERNAL_H

#include "djl_internal.h"

/* ---------------- XDR (pure, bounds-checked) ---------------- */

typedef struct { uint8_t       *p; size_t cap, len;  bool err; } djl_xdr_w;
typedef struct { const uint8_t *p; size_t len, pos;  bool err; } djl_xdr_r;

void     djl_xdr_w_init(djl_xdr_w *w, uint8_t *buf, size_t cap);
void     djl_xdr_put_u32(djl_xdr_w *w, uint32_t v);
void     djl_xdr_put_fixed(djl_xdr_w *w, const uint8_t *d, size_t n);   /* data + pad */
void     djl_xdr_put_opaque(djl_xdr_w *w, const uint8_t *d, size_t n);  /* len + data + pad */

void     djl_xdr_r_init(djl_xdr_r *r, const uint8_t *buf, size_t len);
uint32_t djl_xdr_get_u32(djl_xdr_r *r);
bool     djl_xdr_get_fixed(djl_xdr_r *r, uint8_t *out, size_t n);
bool     djl_xdr_skip(djl_xdr_r *r, size_t n);
/* Returns a pointer into the reader's buffer, or NULL. Advances past padding. */
const uint8_t *djl_xdr_get_opaque(djl_xdr_r *r, uint32_t *out_len);

/* UTF-8 -> UTF-16LE. Returns bytes written, 0 if it does not fit.
 * Pioneer's NFS requires every MOUNT path and LOOKUP filename in this form. */
size_t   djl_utf16le_encode(const char *utf8, uint8_t *out, size_t cap);

/* ---------------- ONC-RPC v2 over UDP ---------------- */

#define DJL_RPC_BUF      65536
#define DJL_RPC_ATTEMPTS 4
#define DJL_RPC_TIMEO_MS 400

/* Program numbers. */
#define DJL_PROG_PORTMAP 100000u
#define DJL_PROG_NFS     100003u
#define DJL_PROG_MOUNT   100005u

typedef struct {
    djl_sock sock;         /* ephemeral source port; no privileged bind needed */
    uint8_t  ip[4];
    uint32_t xid;
    unsigned timeout_ms;   /* first attempt, doubled on each retry */
    unsigned attempts;
    uint8_t *rbuf;         /* per-handle receive buffer, DJL_RPC_BUF bytes */
} djl_rpc;

/* Pure framing. */
void    djl_rpc_build_call(djl_xdr_w *w, uint32_t xid, uint32_t prog,
                           uint32_t vers, uint32_t proc);
/* Validate a reply and locate the procedure result. */
djl_err djl_rpc_reply_body(const uint8_t *buf, size_t len, uint32_t xid, size_t *body_off);

/* Socket-backed request/response with retransmission. On success *body points
 * into the handle's own buffer and stays valid until the next call on it. */
djl_err djl_rpc_open(djl_rpc *r, const uint8_t ip[4]);
void    djl_rpc_close(djl_rpc *r);
djl_err djl_rpc_call(djl_rpc *r, uint16_t port, uint32_t prog, uint32_t vers,
                     uint32_t proc, const uint8_t *args, size_t arglen,
                     const uint8_t **body, size_t *body_len);

/* ---------------- portmap v2 / mount v1 / nfs v2 ---------------- */

#define DJL_FHSIZE   32     /* NFS v2 file handle */
#define DJL_FATTR_SZ 68     /* NFS v2 fattr */

typedef struct { uint8_t h[DJL_FHSIZE]; } djl_fh;

djl_err djl_portmap_getport(djl_rpc *r, uint32_t prog, uint32_t vers, uint16_t *out);

djl_err djl_mount_mnt(djl_rpc *r, uint16_t port, const char *path, djl_fh *out);
djl_err djl_mount_umnt(djl_rpc *r, uint16_t port, const char *path);

typedef struct {
    djl_fh   fh;
    uint32_t type;    /* 1 = regular file, 2 = directory */
    uint32_t size;
} djl_nfs_stat;

djl_err djl_nfs2_lookup(djl_rpc *r, uint16_t port, const djl_fh *dir,
                        const char *name, djl_nfs_stat *out);
/* One READ round trip; *got may be short at end of file. */
djl_err djl_nfs2_read(djl_rpc *r, uint16_t port, const djl_fh *fh,
                      uint32_t offset, uint32_t count,
                      uint8_t *out, size_t out_cap, size_t *got);
djl_err djl_nfs2_readdir(djl_rpc *r, uint16_t port, const djl_fh *dir,
                         djl_nfs_dirent *out, size_t max, size_t *count);

#endif /* DJL_NFS_INTERNAL_H */
