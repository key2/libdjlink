/* libdjlink: portmap v2 (RFC 1057), MOUNT v1 and NFS v2 (RFC 1094) clients.
 *
 * THE critical Pioneer quirk: the MOUNT DirPath and every NFS LOOKUP filename
 * must be encoded as UTF-16LE, not ASCII. Sending ASCII gets EACCES (13) from
 * a CDJ-3000X, which is what made us wrongly conclude that the firmware had
 * locked NFS down. Verified live 2026-08-27: MNT "/C/" in UTF-16LE returns a
 * valid file handle from firmware 1.31. See HANDOVER.md section 5.
 *
 * File handles are the standard 32-byte NFS v2 FHSIZE and fattr is 68 bytes
 * with the file size at +20.
 */
#include "nfs_internal.h"

#include <string.h>

/* Procedure numbers. */
#define PMAP_GETPORT   3
#define MOUNT_MNT      1
#define MOUNT_UMNT     3
#define NFS_GETATTR    1
#define NFS_LOOKUP     4
#define NFS_READ       6
#define NFS_READDIR   16

#define IPPROTO_UDP_XDR 17

/* Longest path element we will encode as UTF-16LE. */
#define NAME_MAX_U16 512

/* ---------------- portmap ---------------- */

djl_err djl_portmap_getport(djl_rpc *r, uint32_t prog, uint32_t vers, uint16_t *out)
{
    if (!r || !out) return DJL_ERR_INVAL;
    *out = 0;

    uint8_t args[16];
    djl_xdr_w w;
    djl_xdr_w_init(&w, args, sizeof args);
    djl_xdr_put_u32(&w, prog);
    djl_xdr_put_u32(&w, vers);
    djl_xdr_put_u32(&w, IPPROTO_UDP_XDR);
    djl_xdr_put_u32(&w, 0);            /* port: 0 when querying */
    if (w.err) return DJL_ERR_NOMEM;

    const uint8_t *rep = NULL;
    size_t rlen = 0;
    djl_err e = djl_rpc_call(r, 111, DJL_PROG_PORTMAP, 2, PMAP_GETPORT,
                             args, w.len, &rep, &rlen);
    if (e != DJL_OK) return e;

    djl_xdr_r rd;
    djl_xdr_r_init(&rd, rep, rlen);
    uint32_t port = djl_xdr_get_u32(&rd);
    if (rd.err) return DJL_ERR_SHORT;
    if (port == 0 || port > 0xffff) return DJL_ERR_UNAVAILABLE;
    *out = (uint16_t)port;
    return DJL_OK;
}

/* ---------------- mount v1 ---------------- */

/* Encode a string as an XDR opaque holding its UTF-16LE bytes. */
static djl_err put_u16_name(djl_xdr_w *w, const char *s)
{
    uint8_t name[NAME_MAX_U16];
    size_t n = djl_utf16le_encode(s, name, sizeof name);
    if (n == 0 && s[0] != '\0') return DJL_ERR_INVAL;
    djl_xdr_put_opaque(w, name, n);
    return w->err ? DJL_ERR_NOMEM : DJL_OK;
}

djl_err djl_mount_mnt(djl_rpc *r, uint16_t port, const char *path, djl_fh *out)
{
    if (!r || !path || !out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);

    uint8_t args[NAME_MAX_U16 + 8];
    djl_xdr_w w;
    djl_xdr_w_init(&w, args, sizeof args);
    djl_err e = put_u16_name(&w, path);
    if (e != DJL_OK) return e;

    const uint8_t *rep = NULL;
    size_t rlen = 0;
    e = djl_rpc_call(r, port, DJL_PROG_MOUNT, 1, MOUNT_MNT,
                     args, w.len, &rep, &rlen);
    if (e != DJL_OK) return e;

    djl_xdr_r rd;
    djl_xdr_r_init(&rd, rep, rlen);
    uint32_t status = djl_xdr_get_u32(&rd);
    if (rd.err) return DJL_ERR_SHORT;
    /* status is an errno: 13 = EACCES, which is what ASCII paths produce. */
    if (status != 0) return (status == 13) ? DJL_ERR_STATE : DJL_ERR_UNAVAILABLE;
    if (!djl_xdr_get_fixed(&rd, out->h, DJL_FHSIZE)) return DJL_ERR_SHORT;
    return DJL_OK;
}

djl_err djl_mount_umnt(djl_rpc *r, uint16_t port, const char *path)
{
    if (!r || !path) return DJL_ERR_INVAL;
    uint8_t args[NAME_MAX_U16 + 8];
    djl_xdr_w w;
    djl_xdr_w_init(&w, args, sizeof args);
    djl_err e = put_u16_name(&w, path);
    if (e != DJL_OK) return e;

    const uint8_t *rep = NULL;
    size_t rlen = 0;
    /* UMNT returns void; a timeout here is not worth reporting. */
    return djl_rpc_call(r, port, DJL_PROG_MOUNT, 1, MOUNT_UMNT,
                        args, w.len, &rep, &rlen);
}

/* ---------------- nfs v2 ---------------- */

/* Map an NFS v2 status (an errno) onto our error space. */
static djl_err nfs_status(uint32_t st)
{
    switch (st) {
    case 0:  return DJL_OK;
    case 2:  return DJL_ERR_NOT_FOUND;    /* NFSERR_NOENT */
    case 13: return DJL_ERR_STATE;        /* NFSERR_ACCES */
    default: return DJL_ERR_UNAVAILABLE;
    }
}

djl_err djl_nfs2_lookup(djl_rpc *r, uint16_t port, const djl_fh *dir,
                        const char *name, djl_nfs_stat *out)
{
    if (!r || !dir || !name || !out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);

    uint8_t args[DJL_FHSIZE + NAME_MAX_U16 + 8];
    djl_xdr_w w;
    djl_xdr_w_init(&w, args, sizeof args);
    djl_xdr_put_fixed(&w, dir->h, DJL_FHSIZE);
    djl_err e = put_u16_name(&w, name);
    if (e != DJL_OK) return e;

    const uint8_t *rep = NULL;
    size_t rlen = 0;
    e = djl_rpc_call(r, port, DJL_PROG_NFS, 2, NFS_LOOKUP,
                     args, w.len, &rep, &rlen);
    if (e != DJL_OK) return e;

    djl_xdr_r rd;
    djl_xdr_r_init(&rd, rep, rlen);
    uint32_t status = djl_xdr_get_u32(&rd);
    if (rd.err) return DJL_ERR_SHORT;
    if (status != 0) return nfs_status(status);

    if (!djl_xdr_get_fixed(&rd, out->fh.h, DJL_FHSIZE)) return DJL_ERR_SHORT;
    /* fattr: type at +0, size at +20. */
    out->type = djl_xdr_get_u32(&rd);
    if (!djl_xdr_skip(&rd, 16)) return DJL_ERR_SHORT;   /* mode, nlink, uid, gid */
    out->size = djl_xdr_get_u32(&rd);
    if (rd.err) return DJL_ERR_SHORT;
    return DJL_OK;
}

djl_err djl_nfs2_read(djl_rpc *r, uint16_t port, const djl_fh *fh,
                      uint32_t offset, uint32_t count,
                      uint8_t *out, size_t out_cap, size_t *got)
{
    if (!r || !fh || !out || !got) return DJL_ERR_INVAL;
    *got = 0;

    uint8_t args[DJL_FHSIZE + 12];
    djl_xdr_w w;
    djl_xdr_w_init(&w, args, sizeof args);
    djl_xdr_put_fixed(&w, fh->h, DJL_FHSIZE);
    djl_xdr_put_u32(&w, offset);
    djl_xdr_put_u32(&w, count);
    djl_xdr_put_u32(&w, 0);            /* totalcount, unused since NFS v2 */
    if (w.err) return DJL_ERR_NOMEM;

    const uint8_t *rep = NULL;
    size_t rlen = 0;
    djl_err e = djl_rpc_call(r, port, DJL_PROG_NFS, 2, NFS_READ,
                             args, w.len, &rep, &rlen);
    if (e != DJL_OK) return e;

    djl_xdr_r rd;
    djl_xdr_r_init(&rd, rep, rlen);
    uint32_t status = djl_xdr_get_u32(&rd);
    if (rd.err) return DJL_ERR_SHORT;
    if (status != 0) return nfs_status(status);
    if (!djl_xdr_skip(&rd, DJL_FATTR_SZ)) return DJL_ERR_SHORT;

    uint32_t dlen = 0;
    const uint8_t *data = djl_xdr_get_opaque(&rd, &dlen);
    if (!data) return DJL_ERR_SHORT;
    if (dlen > out_cap) return DJL_ERR_NOMEM;
    if (dlen) memcpy(out, data, dlen);
    *got = dlen;
    return DJL_OK;
}

/* Decode a directory-entry name. Pioneer sends these UTF-16LE, like every other
 * NFS string.
 *
 * The tolerance for plain bytes exists only for non-Pioneer servers; deciding
 * on src[1] alone was wrong, because any name whose first character is U+0100
 * or above (Cyrillic, Greek, CJK -- all common in music libraries) has a
 * non-zero second byte and was then copied out as raw bytes, producing invalid
 * UTF-8 that no longer round-trips through LOOKUP. Require the NUL in every
 * odd byte instead, which only genuine UTF-16LE ASCII satisfies. */
static bool looks_utf16le(const uint8_t *src, size_t n)
{
    if (n < 2 || (n % 2) != 0) return false;
    size_t high_zero = 0, pairs = n / 2;
    for (size_t i = 0; i < pairs; i++)
        if (src[i * 2 + 1] == 0) high_zero++;
    /* All-ASCII UTF-16LE gives every high byte zero. A name with non-Latin
     * characters gives some non-zero high bytes, but a byte-string name would
     * have to be pathological to put NUL in most odd positions, so a majority
     * is a safe discriminator. */
    return high_zero * 2 >= pairs;
}

static void decode_entry_name(const uint8_t *src, size_t n, char *out, size_t outsz)
{
    if (outsz == 0) return;
    bool u16 = looks_utf16le(src, n);
    size_t o = 0;
    if (u16) {
        for (size_t i = 0; i + 1 < n; i += 2) {
            uint32_t cp = (uint32_t)src[i] | ((uint32_t)src[i + 1] << 8);
            if (cp == 0) break;
            if (cp < 0x80) { if (o + 1 >= outsz) break; out[o++] = (char)cp; }
            else if (cp < 0x800) {
                if (o + 2 >= outsz) break;
                out[o++] = (char)(0xc0 | (cp >> 6));
                out[o++] = (char)(0x80 | (cp & 0x3f));
            } else {
                if (o + 3 >= outsz) break;
                out[o++] = (char)(0xe0 | (cp >> 12));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                out[o++] = (char)(0x80 | (cp & 0x3f));
            }
        }
    } else {
        for (size_t i = 0; i < n && o + 1 < outsz; i++) {
            if (src[i] == 0) break;
            out[o++] = (char)src[i];
        }
    }
    out[o] = '\0';
}

djl_err djl_nfs2_readdir(djl_rpc *r, uint16_t port, const djl_fh *dir,
                         djl_nfs_dirent *out, size_t max, size_t *count)
{
    if (!r || !dir || !out || !count) return DJL_ERR_INVAL;
    *count = 0;

    uint8_t cookie[4] = {0, 0, 0, 0};
    size_t filled = 0;

    /* READDIR is a cookie-driven loop; keep going until the server says eof. */
    for (int page = 0; page < 64 && filled < max; page++) {
        uint8_t args[DJL_FHSIZE + 8];
        djl_xdr_w w;
        djl_xdr_w_init(&w, args, sizeof args);
        djl_xdr_put_fixed(&w, dir->h, DJL_FHSIZE);
        djl_xdr_put_fixed(&w, cookie, 4);      /* opaque cookie, echoed back */
        djl_xdr_put_u32(&w, 4096);             /* count: reply byte budget */
        if (w.err) return DJL_ERR_NOMEM;

        const uint8_t *rep = NULL;
        size_t rlen = 0;
        djl_err e = djl_rpc_call(r, port, DJL_PROG_NFS, 2, NFS_READDIR,
                                 args, w.len, &rep, &rlen);
        if (e != DJL_OK) return e;

        djl_xdr_r rd;
        djl_xdr_r_init(&rd, rep, rlen);
        uint32_t status = djl_xdr_get_u32(&rd);
        if (rd.err) return DJL_ERR_SHORT;
        if (status != 0) return nfs_status(status);

        bool got_any = false;
        for (;;) {
            uint32_t follows = djl_xdr_get_u32(&rd);
            if (rd.err) return DJL_ERR_SHORT;
            if (follows == 0) break;

            uint32_t fileid = djl_xdr_get_u32(&rd);
            uint32_t nlen = 0;
            const uint8_t *name = djl_xdr_get_opaque(&rd, &nlen);
            if (!name) return DJL_ERR_SHORT;
            uint8_t next_cookie[4];
            if (!djl_xdr_get_fixed(&rd, next_cookie, 4)) return DJL_ERR_SHORT;

            memcpy(cookie, next_cookie, 4);
            got_any = true;
            if (filled < max) {
                memset(&out[filled], 0, sizeof out[filled]);
                out[filled].fileid = fileid;
                decode_entry_name(name, nlen, out[filled].name,
                                  sizeof out[filled].name);
                filled++;
            }
        }

        uint32_t eof = djl_xdr_get_u32(&rd);
        if (rd.err) break;               /* truncated reply: stop cleanly */
        if (eof || !got_any) break;
    }

    *count = filled;
    return DJL_OK;
}
