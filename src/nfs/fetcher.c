/* libdjlink NFS fetcher: the high-level "read a player's USB" layer.
 *
 * Mounts a slot (SD = /B/, USB = /C/), walks paths with LOOKUP, reads whole
 * files with a READ loop, and assembles one track's metadata + analysis from
 * export.pdb plus its .DAT/.EXT/.2EX ANLZ files.
 *
 * Every call here blocks, so it must run on the metadata worker thread (or an
 * application thread), never on the I/O thread. One djl_nfs handle owns one
 * socket and is not safe to use from two threads at once.
 */
#include "nfs_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* NFS v2 caps a READ at 8192 bytes. Large reads mean fewer round trips but
 * more IP fragments; back off if a player does not like the big ones. */
#define READ_CHUNK_MAX  8192u
#define READ_CHUNK_MIN  1024u
#define MAX_FILE_BYTES  (64u * 1024u * 1024u)

struct djl_nfs {
    djl_rpc  rpc;
    uint8_t  ip[4];
    djl_slot slot;
    char     mount_path[8];      /* "/B/" or "/C/" */
    uint16_t mount_port, nfs_port;
    djl_fh   root;
    bool     mounted;
    bool     dot_pioneer;        /* HFS+ media hides it as ".PIONEER" */
    uint32_t chunk;

    djl_blob  pdb_raw;           /* cached export.pdb bytes */
    djl_pdb  *pdb;               /* parsed view over pdb_raw */
};

bool djl_nfs_supported(void) { return true; }

static const char *slot_mount_path(djl_slot slot)
{
    switch (slot) {
    case DJL_SLOT_SD:  return "/B/";
    case DJL_SLOT_USB: return "/C/";
    default:           return NULL;
    }
}

/* ---------------- open / close ---------------- */

djl_err djl_nfs_open_addr(const uint8_t ip[4], djl_slot slot, djl_nfs **out)
{
    if (!ip || !out) return DJL_ERR_INVAL;
    *out = NULL;
    const char *mpath = slot_mount_path(slot);
    if (!mpath) return DJL_ERR_INVAL;

    struct djl_nfs *n = calloc(1, sizeof *n);
    if (!n) return DJL_ERR_NOMEM;
    memcpy(n->ip, ip, 4);
    n->slot  = slot;
    n->chunk = READ_CHUNK_MAX;
    snprintf(n->mount_path, sizeof n->mount_path, "%s", mpath);

    djl_err e = djl_rpc_open(&n->rpc, ip);
    if (e != DJL_OK) { free(n); return e; }

    /* Ports are not fixed: discover both through the portmapper. On our rig
     * mountd answered on 48353 and nfs on 2049, but neither is guaranteed. */
    e = djl_portmap_getport(&n->rpc, DJL_PROG_MOUNT, 1, &n->mount_port);
    if (e != DJL_OK) goto fail;
    e = djl_portmap_getport(&n->rpc, DJL_PROG_NFS, 2, &n->nfs_port);
    if (e != DJL_OK) goto fail;

    e = djl_mount_mnt(&n->rpc, n->mount_port, n->mount_path, &n->root);
    if (e != DJL_OK) goto fail;
    n->mounted = true;

    *out = n;
    return DJL_OK;

fail:
    djl_rpc_close(&n->rpc);
    free(n);
    return e;
}

djl_err djl_nfs_open(djl_context *ctx, uint8_t player, djl_slot slot, djl_nfs **out)
{
    if (!ctx || !out) return DJL_ERR_INVAL;
    djl_device_info info;
    djl_err e = djl_device_by_number(ctx, player, &info);
    if (e != DJL_OK) return DJL_ERR_NOT_FOUND;
    return djl_nfs_open_addr(info.ip, slot, out);
}

void djl_nfs_close(djl_nfs *n)
{
    if (!n) return;
    if (n->mounted) djl_mount_umnt(&n->rpc, n->mount_port, n->mount_path);
    djl_pdb_close(n->pdb);
    djl_blob_free(&n->pdb_raw);
    djl_rpc_close(&n->rpc);
    free(n);
}

/* ---------------- path resolution ---------------- */

/* Look up one element, retrying "PIONEER" as ".PIONEER" on HFS+ media. */
static djl_err lookup_element(struct djl_nfs *n, const djl_fh *dir,
                              const char *name, djl_nfs_stat *st)
{
    bool pioneer = (strcmp(name, "PIONEER") == 0);
    if (pioneer && n->dot_pioneer)
        return djl_nfs2_lookup(&n->rpc, n->nfs_port, dir, ".PIONEER", st);

    djl_err e = djl_nfs2_lookup(&n->rpc, n->nfs_port, dir, name, st);
    if (e == DJL_ERR_NOT_FOUND && pioneer) {
        e = djl_nfs2_lookup(&n->rpc, n->nfs_port, dir, ".PIONEER", st);
        if (e == DJL_OK) n->dot_pioneer = true;   /* remember for this slot */
    }
    return e;
}

/* Walk a slash-separated path from the mount root. */
static djl_err resolve_path(struct djl_nfs *n, const char *path, djl_nfs_stat *out)
{
    if (!path) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    out->fh   = n->root;
    out->type = 2;

    char elem[256];
    const char *p = path;
    djl_fh cur = n->root;

    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        size_t k = 0;
        while (*p && *p != '/' && *p != '\\') {
            if (k + 1 >= sizeof elem) return DJL_ERR_INVAL;
            elem[k++] = *p++;
        }
        elem[k] = '\0';
        if (k == 0 || strcmp(elem, ".") == 0) continue;

        djl_err e = lookup_element(n, &cur, elem, out);
        if (e != DJL_OK) return e;
        cur = out->fh;
    }
    return DJL_OK;
}

/* ---------------- file read ---------------- */

djl_err djl_nfs_read_file(djl_nfs *n, const char *path, djl_blob *out)
{
    if (!n || !path || !out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);

    djl_nfs_stat st;
    djl_err e = resolve_path(n, path, &st);
    if (e != DJL_OK) return e;
    if (st.type != 1) return DJL_ERR_NOT_FOUND;      /* not a regular file */
    if (st.size > MAX_FILE_BYTES) return DJL_ERR_NOMEM;

    uint8_t *buf = malloc(st.size ? st.size : 1);
    if (!buf) return DJL_ERR_NOMEM;

    uint32_t off = 0;
    while (off < st.size) {
        uint32_t want = st.size - off;
        if (want > n->chunk) want = n->chunk;
        size_t got = 0;
        e = djl_nfs2_read(&n->rpc, n->nfs_port, &st.fh, off, want,
                          buf + off, st.size - off, &got);
        if (e != DJL_OK) {
            /* Some players are unhappy with 8 KB reads (IP fragmentation);
             * shrink once and retry from the same offset before giving up. */
            if (n->chunk > READ_CHUNK_MIN) {
                n->chunk /= 2;
                continue;
            }
            free(buf);
            return e;
        }
        if (got == 0) break;                          /* short read = EOF */
        off += (uint32_t)got;
    }

    out->data   = buf;
    out->length = off;
    return DJL_OK;
}

djl_err djl_nfs_list_dir(djl_nfs *n, const char *path, djl_nfs_dirent *out,
                         size_t max, size_t *count)
{
    if (!n || !out || !count) return DJL_ERR_INVAL;
    *count = 0;

    djl_nfs_stat st;
    djl_err e = resolve_path(n, path ? path : "", &st);
    if (e != DJL_OK) return e;
    if (st.type != 2) return DJL_ERR_INVAL;           /* not a directory */
    return djl_nfs2_readdir(&n->rpc, n->nfs_port, &st.fh, out, max, count);
}

/* ---------------- export.pdb ---------------- */

djl_err djl_nfs_pdb(djl_nfs *n, const djl_pdb **out)
{
    if (!n || !out) return DJL_ERR_INVAL;
    *out = NULL;
    if (n->pdb) { *out = n->pdb; return DJL_OK; }

    djl_blob raw;
    djl_err e = djl_nfs_read_file(n, "PIONEER/rekordbox/export.pdb", &raw);
    if (e != DJL_OK) return e;

    djl_pdb *p = NULL;
    e = djl_pdb_open(raw.data, raw.length, &p);
    if (e != DJL_OK) { djl_blob_free(&raw); return e; }

    n->pdb_raw = raw;        /* the reader borrows these bytes; keep them alive */
    n->pdb     = p;
    *out       = p;
    return DJL_OK;
}

/* ---------------- one track, end to end ---------------- */

/* Swap the extension of an ANLZ path, e.g. ANLZ0000.DAT -> ANLZ0000.EXT. */
static void swap_ext(const char *in, const char *ext, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s", in);
    char *dot = strrchr(out, '.');
    const char *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash)) {
        size_t room = outsz - (size_t)(dot - out) - 1;
        snprintf(dot + 1, room, "%s", ext);
    }
}

djl_err djl_nfs_fetch_track(djl_nfs *n, uint32_t track_id, djl_nfs_track *out)
{
    if (!n || !out || track_id == 0) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);

    const djl_pdb *p = NULL;
    djl_err e = djl_nfs_pdb(n, &p);
    if (e != DJL_OK) return e;

    char dat[512];
    e = djl_pdb_track(p, track_id, &out->meta, dat, sizeof dat);
    if (e != DJL_OK) return e;
    out->has_meta = out->meta.found;
    out->meta.slot = n->slot;
    out->meta.type = DJL_TRACK_REKORDBOX;
    snprintf(out->anlz_path, sizeof out->anlz_path, "%s", dat);
    if (dat[0] == '\0') return DJL_OK;               /* metadata only */

    /* .DAT holds PQTZ/PCOB/PPTH/PWAV, .EXT adds PCO2/PSSI/PWV4/PWV5,
     * .2EX adds the CDJ-3000 3-band waveforms. Parse in that order so the
     * richer tags win. Only .DAT is required. */
    static const char *exts[3] = { "DAT", "EXT", "2EX" };
    bool any = false;
    for (int i = 0; i < 3; i++) {
        char path[512];
        swap_ext(dat, exts[i], path, sizeof path);
        djl_blob f;
        if (djl_nfs_read_file(n, path, &f) != DJL_OK) continue;
        if (djl_anlz_parse(f.data, f.length, &out->anlz) == DJL_OK) any = true;
        djl_blob_free(&f);
    }
    return (any || out->has_meta) ? DJL_OK : DJL_ERR_UNAVAILABLE;
}

void djl_nfs_track_free(djl_nfs_track *t)
{
    if (!t) return;
    djl_anlz_free(&t->anlz);
    memset(t, 0, sizeof *t);
}

djl_err djl_nfs_read_artwork(djl_nfs *n, uint32_t artwork_id, djl_blob *out)
{
    if (!n || !out || artwork_id == 0) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);

    const djl_pdb *p = NULL;
    djl_err e = djl_nfs_pdb(n, &p);
    if (e != DJL_OK) return e;

    char path[512];
    e = djl_pdb_artwork_path(p, artwork_id, path, sizeof path);
    if (e != DJL_OK) return e;
    return djl_nfs_read_file(n, path, out);
}
