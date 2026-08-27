/* libdjlink dbserver client: metadata, waveforms, and menu browsing over TCP.
 *
 * Wire format transcribed byte-for-byte from beat-link's dbserver package
 * (Field / NumberField / StringField / BinaryField / Message / Client).
 *
 * Field type tags:  0x0f u8, 0x10 u16, 0x11 u32, 0x14 blob, 0x26 UTF-16BE string.
 * Argument tags:    0x06 number, 0x02 string, 0x03 blob.
 * A message is: number(0x872349ae), number(txid,4), number(type,2),
 *               number(argcount,1), blob(12 arg-type tags), then the arguments.
 * A blob argument is omitted from the body when the number field right before
 * it is 0.
 */
#include "djl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_QUERY_PORT   12523
#define DB_MAGIC        0x872349aeu
#define DB_TIMEOUT_MS   10000
#define DB_MENU_BATCH   64

/* Menu identifiers (2nd byte of R:M:S:T). */
#define MENU_MAIN 1
#define MENU_DATA 8

/* ANLZ tag / file constants (byte-reversed FourCC). */
#define TAG_PWV4 0x34565750u  /* RGB preview  (EXT) */
#define TAG_PWV5 0x35565750u  /* RGB detail   (EXT) */
#define TAG_PWV6 0x36565750u  /* 3band preview(2EX) */
#define TAG_PWV7 0x37565750u  /* 3band detail (2EX) */
#define EXT_EXT  0x00545845u  /* "EXT" */
#define EXT_2EX  0x00584532u  /* "2EX" */

struct djl_db {
    djl_tcp  sock;
    uint8_t  target;
    uint8_t  posing;
    uint8_t  their_number;
    uint16_t port;
    uint32_t txid;
    int      dead;     /* set once a read desyncs the stream; fail fast after */
};

/* -------------------- growable output buffer -------------------- */

typedef struct { uint8_t *p; size_t len, cap; int err; } obuf;

static void ob_need(obuf *b, size_t extra)
{
    if (b->err) return;
    if (b->len + extra <= b->cap) return;
    size_t ncap = b->cap ? b->cap * 2 : 128;
    while (ncap < b->len + extra) ncap *= 2;
    uint8_t *np = realloc(b->p, ncap);
    if (!np) { b->err = 1; return; }
    b->p = np; b->cap = ncap;
}
static void ob_u8(obuf *b, uint8_t v)  { ob_need(b,1); if(!b->err) b->p[b->len++]=v; }
static void ob_be(obuf *b, uint64_t v, int n){ ob_need(b,(size_t)n); if(b->err)return;
    for(int i=n-1;i>=0;i--) b->p[b->len++]=(uint8_t)((v>>(8*i))&0xff); }
static void ob_bytes(obuf *b, const uint8_t *d, size_t n){ ob_need(b,n); if(!b->err){memcpy(b->p+b->len,d,n); b->len+=n;} }

/* field writers */
static void fld_num(obuf *b, uint32_t v)          { ob_u8(b,0x11); ob_be(b,v,4); }
static void fld_blob(obuf *b, const uint8_t*d, uint32_t n){ ob_u8(b,0x14); ob_be(b,n,4); ob_bytes(b,d,n); }
static void fld_str(obuf *b, const char *s)
{
    size_t n = strlen(s);
    ob_u8(b, 0x26);
    ob_be(b, (uint32_t)(n + 1), 4);          /* code units incl. trailing NUL */
    for (size_t i = 0; i < n; i++) { ob_u8(b, 0x00); ob_u8(b, (uint8_t)s[i]); }
    ob_u8(b, 0x00); ob_u8(b, 0x00);          /* NUL */
}

/* -------------------- message arguments -------------------- */

enum { A_NUM, A_STR, A_BLOB };
typedef struct { int kind; uint32_t num; const char *str; const uint8_t *blob; uint32_t blen; } db_arg;

static uint8_t arg_tag(const db_arg *a)
{
    switch (a->kind) { case A_STR: return 0x02; case A_BLOB: return 0x03; default: return 0x06; }
}

/* -------------------- TCP helpers -------------------- */
/* All socket work goes through the OSAL so this file stays portable. */

static djl_err read_exact(djl_tcp *t, uint8_t *buf, size_t n)
{
    return djl_tcp_recv_exact(t, buf, n);
}

static djl_err write_all(djl_tcp *t, const uint8_t *buf, size_t n)
{
    return djl_tcp_send_all(t, buf, n);
}

/* -------------------- parsed fields / messages -------------------- */

typedef struct {
    uint8_t  tag;
    uint64_t num;
    char    *str;     /* UTF-8, for 0x26 */
    uint8_t *blob;    /* raw, for 0x14 */
    uint32_t blen;
} db_field;

typedef struct {
    uint32_t txid;
    uint16_t type;
    uint8_t  argcount;
    db_field args[12];
} db_message;

static void field_free(db_field *f){ free(f->str); free(f->blob); f->str=NULL; f->blob=NULL; }
static void msg_free(db_message *m){ for (int i=0;i<12;i++) field_free(&m->args[i]); }

static void utf16be_to_utf8(const uint8_t *src, size_t units, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; i < units; i++) {
        uint32_t cp = ((uint32_t)src[i*2] << 8) | src[i*2+1];
        if (cp == 0) break;
        if (cp < 0x80) { if (o+1>=outsz) break; out[o++]=(char)cp; }
        else if (cp < 0x800) { if (o+2>=outsz) break; out[o++]=(char)(0xc0|(cp>>6)); out[o++]=(char)(0x80|(cp&0x3f)); }
        else { if (o+3>=outsz) break; out[o++]=(char)(0xe0|(cp>>12)); out[o++]=(char)(0x80|((cp>>6)&0x3f)); out[o++]=(char)(0x80|(cp&0x3f)); }
    }
    out[o] = 0;
}

/* Read one field. For strings, decodes to a freshly malloc'd UTF-8 string. */
static djl_err read_field(djl_tcp *t, db_field *f)
{
    memset(f, 0, sizeof *f);
    uint8_t tag;
    djl_err e = read_exact(t, &tag, 1);
    if (e != DJL_OK) return e;
    f->tag = tag;
    uint8_t tmp[4];
    switch (tag) {
    case 0x0f: if ((e=read_exact(t,tmp,1))) return e; f->num = tmp[0]; return DJL_OK;
    case 0x10: if ((e=read_exact(t,tmp,2))) return e; f->num = ((uint32_t)tmp[0]<<8)|tmp[1]; return DJL_OK;
    case 0x11: if ((e=read_exact(t,tmp,4))) return e;
        f->num = ((uint64_t)tmp[0]<<24)|((uint32_t)tmp[1]<<16)|((uint32_t)tmp[2]<<8)|tmp[3]; return DJL_OK;
    case 0x14: {
        if ((e=read_exact(t,tmp,4))) return e;
        uint32_t n = ((uint32_t)tmp[0]<<24)|((uint32_t)tmp[1]<<16)|((uint32_t)tmp[2]<<8)|tmp[3];
        if (n > 8u*1024*1024) return DJL_ERR_IO;   /* sanity cap */
        f->blob = n ? malloc(n) : NULL;
        if (n && !f->blob) return DJL_ERR_NOMEM;
        if (n && (e=read_exact(t,f->blob,n))) { free(f->blob); f->blob=NULL; return e; }
        f->blen = n;
        return DJL_OK;
    }
    case 0x26: {
        if ((e=read_exact(t,tmp,4))) return e;
        uint32_t units = ((uint32_t)tmp[0]<<24)|((uint32_t)tmp[1]<<16)|((uint32_t)tmp[2]<<8)|tmp[3];
        if (units > 1024*1024) return DJL_ERR_IO;
        uint8_t *raw = units ? malloc((size_t)units*2) : NULL;
        if (units && !raw) return DJL_ERR_NOMEM;
        if (units && (e=read_exact(t,raw,(size_t)units*2))) { free(raw); return e; }
        f->str = malloc((size_t)units*3 + 1);
        if (!f->str) { free(raw); return DJL_ERR_NOMEM; }
        utf16be_to_utf8(raw, units, f->str, (size_t)units*3 + 1);
        f->blen = units;
        free(raw);
        return DJL_OK;
    }
    default:
        return DJL_ERR_IO;   /* unknown tag */
    }
}

static djl_err read_message(djl_tcp *t, db_message *m)
{
    memset(m, 0, sizeof *m);
    db_field f;
    djl_err e;

    if ((e = read_field(t, &f))) return e;
    if (f.tag != 0x11 || f.num != DB_MAGIC) { field_free(&f); return DJL_ERR_IO; }
    field_free(&f);

    if ((e = read_field(t, &f))) return e;
    if (f.tag != 0x11) { field_free(&f); return DJL_ERR_IO; }
    m->txid = (uint32_t)f.num; field_free(&f);

    if ((e = read_field(t, &f))) return e;
    if (f.tag != 0x10) { field_free(&f); return DJL_ERR_IO; }
    m->type = (uint16_t)f.num; field_free(&f);

    if ((e = read_field(t, &f))) return e;
    if (f.tag != 0x0f) { field_free(&f); return DJL_ERR_IO; }
    m->argcount = (uint8_t)f.num; field_free(&f);
    if (m->argcount > 12) return DJL_ERR_IO;

    if ((e = read_field(t, &f))) return e;
    if (f.tag != 0x14 || f.blen < m->argcount) { field_free(&f); return DJL_ERR_IO; }
    uint8_t tags[12] = {0};
    if (m->argcount && f.blob) memcpy(tags, f.blob, m->argcount);
    field_free(&f);

    uint8_t last_tag = 0; uint64_t last_num = 0;
    for (int i = 0; i < m->argcount; i++) {
        if (tags[i] == 0x03 && last_tag == 0x11 && last_num == 0) {
            m->args[i].tag = 0x14;   /* omitted empty blob */
            m->args[i].blen = 0;
        } else {
            if ((e = read_field(t, &m->args[i]))) { msg_free(m); return e; }
        }
        last_tag = m->args[i].tag;
        last_num = m->args[i].num;
    }
    return DJL_OK;
}

/* -------------------- send / request -------------------- */

static uint32_t rmst(const struct djl_db *d, uint8_t menu, djl_slot slot, djl_track_type type)
{
    return ((uint32_t)d->posing << 24) | ((uint32_t)menu << 16) |
           (((uint32_t)slot & 0xff) << 8) | ((uint32_t)type & 0xff);
}

static djl_err send_message(struct djl_db *d, uint32_t txid, uint16_t type,
                            const db_arg *args, int nargs)
{
    obuf b = {0};
    uint8_t tags[12] = {0};
    for (int i = 0; i < nargs; i++) tags[i] = arg_tag(&args[i]);

    fld_num(&b, DB_MAGIC);
    fld_num(&b, txid);
    ob_u8(&b, 0x10); ob_be(&b, type, 2);        /* type is 2-byte number */
    ob_u8(&b, 0x0f); ob_be(&b, (uint32_t)nargs, 1);
    fld_blob(&b, tags, 12);
    for (int i = 0; i < nargs; i++) {
        switch (args[i].kind) {
        case A_NUM:  fld_num(&b, args[i].num); break;
        case A_STR:  fld_str(&b, args[i].str ? args[i].str : ""); break;
        case A_BLOB: fld_blob(&b, args[i].blob, args[i].blen); break;
        }
    }
    if (b.err) { free(b.p); return DJL_ERR_NOMEM; }
    djl_err e = write_all(&d->sock, b.p, b.len);
    free(b.p);
    return e;
}

/* Send a request and read exactly one response message, checking the txid. */
static djl_err request(struct djl_db *d, uint16_t type, const db_arg *args, int nargs,
                       db_message *resp)
{
    if (d->dead) return DJL_ERR_STATE;
    uint32_t txid = ++d->txid;
    djl_err e = send_message(d, txid, type, args, nargs);
    if (e != DJL_OK) { d->dead = 1; return e; }
    e = read_message(&d->sock, resp);
    if (e != DJL_OK) { d->dead = 1; return e; }
    if (resp->txid != txid) { msg_free(resp); d->dead = 1; return DJL_ERR_IO; }
    return DJL_OK;
}

/* -------------------- open / close -------------------- */

static djl_err port_lookup(const uint8_t ip[4], uint16_t *out_port)
{
    djl_tcp sock;
    djl_err e = djl_tcp_connect(&sock, ip, DB_QUERY_PORT, DB_TIMEOUT_MS);
    if (e != DJL_OK) return e;
    /* 00 00 00 0f 52 65 6d 6f 74 65 44 42 53 65 72 76 65 72 00 */
    static const uint8_t q[] = { 0x00,0x00,0x00,0x0f,
        'R','e','m','o','t','e','D','B','S','e','r','v','e','r', 0x00 };
    e = write_all(&sock, q, sizeof q);
    if (e == DJL_OK) {
        uint8_t r[2];
        e = read_exact(&sock, r, 2);
        if (e == DJL_OK) *out_port = (uint16_t)((r[0] << 8) | r[1]);
    }
    djl_tcp_close(&sock);
    return e;
}

djl_err djl_db_open(djl_context *ctx, uint8_t player, djl_db **out)
{
    if (!ctx || !out) return DJL_ERR_INVAL;
    *out = NULL;

    djl_device_info info;
    if (djl_device_by_number(ctx, player, &info) != DJL_OK) return DJL_ERR_NOT_FOUND;
    int posing = djl_own_number(ctx);
    if (posing < 1 || posing > 6) return DJL_ERR_STATE;

    uint16_t port = 0;
    djl_err e = port_lookup(info.ip, &port);
    if (e != DJL_OK) return e;
    if (port == 0 || port == DB_QUERY_PORT) return DJL_ERR_UNAVAILABLE;

    djl_tcp sock;
    e = djl_tcp_connect(&sock, info.ip, port, DB_TIMEOUT_MS);
    if (e != DJL_OK) return e;

    struct djl_db *d = calloc(1, sizeof *d);
    if (!d) { djl_tcp_close(&sock); return DJL_ERR_NOMEM; }
    d->sock = sock; d->target = player; d->posing = (uint8_t)posing; d->port = port;

    /* Greeting: a 4-byte number field with value 1, echoed back. */
    obuf g = {0}; fld_num(&g, 1);
    e = write_all(&d->sock, g.p, g.len); free(g.p);
    if (e == DJL_OK) {
        db_field f;
        e = read_field(&d->sock, &f);
        if (e == DJL_OK && (f.tag != 0x11 || f.num != 1)) e = DJL_ERR_IO;
        field_free(&f);
    }
    if (e != DJL_OK) { djl_tcp_close(&d->sock); free(d); return e; }

    /* Setup: type 0x0000, txid 0xfffffffe, one arg = our device number. */
    db_arg a = { A_NUM, (uint32_t)posing, NULL, NULL, 0 };
    obuf sb = {0};
    uint8_t tags[12] = { 0x06 };
    fld_num(&sb, DB_MAGIC); fld_num(&sb, 0xfffffffeu);
    ob_u8(&sb, 0x10); ob_be(&sb, 0x0000, 2);
    ob_u8(&sb, 0x0f); ob_be(&sb, 1, 1);
    fld_blob(&sb, tags, 12);
    fld_num(&sb, a.num);
    e = sb.err ? DJL_ERR_NOMEM : write_all(&d->sock, sb.p, sb.len);
    free(sb.p);
    if (e == DJL_OK) {
        db_message m;
        e = read_message(&d->sock, &m);
        if (e == DJL_OK) {
            if (m.type != 0x4000 || m.argcount < 2) e = DJL_ERR_IO;
            else d->their_number = (uint8_t)m.args[1].num;
            msg_free(&m);
        }
    }
    if (e != DJL_OK) { djl_tcp_close(&d->sock); free(d); return e; }

    *out = d;
    return DJL_OK;
}

void djl_db_close(djl_db *d)
{
    if (!d) return;
    if (d->sock.fd >= 0) {
        /* Teardown: type 0x0100, txid 0xfffffffe, no arguments. */
        obuf b = {0};
        uint8_t tags[12] = {0};
        fld_num(&b, DB_MAGIC); fld_num(&b, 0xfffffffeu);
        ob_u8(&b, 0x10); ob_be(&b, 0x0100, 2);
        ob_u8(&b, 0x0f); ob_be(&b, 0, 1);
        fld_blob(&b, tags, 12);
        if (!b.err) write_all(&d->sock, b.p, b.len);
        free(b.p);
        djl_tcp_close(&d->sock);
    }
    free(d);
}

int      djl_db_target(const djl_db *d)       { return d ? d->target : -1; }
int      djl_db_their_number(const djl_db *d) { return d ? d->their_number : -1; }
uint16_t djl_db_port(const djl_db *d)         { return d ? d->port : 0; }

/* -------------------- menu rendering -------------------- */

typedef void (*item_cb)(const db_message *item, void *ud);

/* Given a MENU_AVAILABLE just received, page through RENDER_MENU_REQ and invoke
 * cb for every MENU_ITEM (0x4101). */
static djl_err render_menu(struct djl_db *d, uint32_t rmst_val, uint32_t total,
                           uint32_t max_items, item_cb cb, void *ud)
{
    uint32_t fetched = 0;
    uint32_t want = (max_items && max_items < total) ? max_items : total;
    while (fetched < want) {
        uint32_t batch = want - fetched;
        if (batch > DB_MENU_BATCH) batch = DB_MENU_BATCH;

        db_arg args[6] = {
            { A_NUM, rmst_val, 0,0,0 }, { A_NUM, fetched, 0,0,0 },
            { A_NUM, batch, 0,0,0 },    { A_NUM, 0, 0,0,0 },
            { A_NUM, total, 0,0,0 },    { A_NUM, 0, 0,0,0 },
        };
        if (d->dead) return DJL_ERR_STATE;
        uint32_t txid = ++d->txid;
        djl_err e = send_message(d, txid, 0x3000, args, 6);
        if (e != DJL_OK) { d->dead = 1; return e; }

        db_message m;
        e = read_message(&d->sock, &m);           /* MENU_HEADER 0x4001 */
        if (e != DJL_OK) { d->dead = 1; return e; }
        if (m.type != 0x4001) { msg_free(&m); d->dead = 1; return DJL_ERR_IO; }
        msg_free(&m);

        for (uint32_t i = 0; i < batch; i++) {
            e = read_message(&d->sock, &m);        /* MENU_ITEM 0x4101 */
            if (e != DJL_OK) { d->dead = 1; return e; }
            if (m.type == 0x4101 && cb) cb(&m, ud);
            msg_free(&m);
        }

        e = read_message(&d->sock, &m);           /* MENU_FOOTER 0x4201 */
        if (e != DJL_OK) { d->dead = 1; return e; }
        msg_free(&m);

        fetched += batch;
    }
    return DJL_OK;
}

/* -------------------- metadata -------------------- */

static void copy_str(char *dst, size_t cap, const db_field *f)
{
    if (f->tag == 0x26 && f->str) { snprintf(dst, cap, "%s", f->str); }
}

static void meta_item_cb(const db_message *m, void *ud)
{
    djl_track_info *t = ud;
    if (m->argcount < 7) return;
    uint16_t itype = (uint16_t)(m->args[6].num & 0xffff);
    switch (itype) {
    case 0x0004: /* title */
        copy_str(t->title, sizeof t->title, &m->args[3]);
        t->artwork_id = (uint32_t)m->args[8].num;
        t->artist_id  = (uint32_t)m->args[0].num;
        break;
    case 0x0007: /* artist */
        copy_str(t->artist, sizeof t->artist, &m->args[3]);
        t->artist_id = (uint32_t)m->args[1].num; break;
    case 0x0028: /* original artist */
        copy_str(t->original_artist, sizeof t->original_artist, &m->args[3]); break;
    case 0x0029: /* remixer */
        copy_str(t->remixer, sizeof t->remixer, &m->args[3]); break;
    case 0x0002: /* album */
        copy_str(t->album, sizeof t->album, &m->args[3]);
        t->album_id = (uint32_t)m->args[1].num; break;
    case 0x0006: /* genre */
        copy_str(t->genre, sizeof t->genre, &m->args[3]);
        t->genre_id = (uint32_t)m->args[1].num; break;
    case 0x000e: /* label */
        copy_str(t->label, sizeof t->label, &m->args[3]);
        t->label_id = (uint32_t)m->args[1].num; break;
    case 0x000b: t->duration_s = (uint32_t)m->args[1].num; break;   /* duration */
    case 0x000d: t->tempo_x100 = (uint32_t)m->args[1].num; break;   /* tempo */
    case 0x0010: t->bitrate    = (uint32_t)m->args[1].num; break;   /* bit rate */
    case 0x0011: t->year       = (uint32_t)m->args[1].num; break;   /* year */
    case 0x0023: copy_str(t->comment, sizeof t->comment, &m->args[3]); break;
    case 0x000f: copy_str(t->key, sizeof t->key, &m->args[3]); break;
    case 0x000a: t->rating = (uint8_t)m->args[1].num; break;        /* rating */
    case 0x002e: copy_str(t->date_added, sizeof t->date_added, &m->args[3]); break;
    default:
        if (itype >= 0x0013 && itype <= 0x001b) {                   /* color */
            t->color_id = (uint8_t)m->args[1].num;
            copy_str(t->color_name, sizeof t->color_name, &m->args[3]);
        }
        break;
    }
}

djl_err djl_db_track_metadata(djl_db *d, djl_slot slot, djl_track_type type,
                              uint32_t rekordbox_id, djl_track_info *out)
{
    if (!d || !out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    out->rekordbox_id = rekordbox_id; out->slot = slot; out->type = type;

    /* rekordbox tracks use 0x2002 by rekordbox id; unanalyzed and audio-CD
     * tracks use 0x2202 by track number. Both answer MENU_AVAILABLE then
     * render the same menu-item format. */
    uint16_t req = (type == DJL_TRACK_REKORDBOX) ? 0x2002 : 0x2202;

    db_arg a[2] = {
        { A_NUM, rmst(d, MENU_MAIN, slot, type), 0,0,0 },
        { A_NUM, rekordbox_id, 0,0,0 },
    };
    db_message m;
    djl_err e = request(d, req, a, 2, &m);
    if (e != DJL_OK) return e;
    if (m.type != 0x4000 || m.argcount < 2) { msg_free(&m); return DJL_ERR_UNAVAILABLE; }
    uint32_t count = (uint32_t)m.args[1].num;
    msg_free(&m);
    if (count == 0 || count == 0xffffffffu) return DJL_ERR_UNAVAILABLE;

    e = render_menu(d, rmst(d, MENU_MAIN, slot, type), count, 64, meta_item_cb, out);
    if (e != DJL_OK) return e;
    out->found = true;
    return DJL_OK;
}

/* -------------------- browsing (folder / track list) -------------------- */

typedef struct { djl_menu_row *rows; size_t max, n; } row_sink;

static void row_cb(const db_message *m, void *ud)
{
    row_sink *s = ud;
    if (s->n >= s->max || m->argcount < 7) return;
    djl_menu_row *r = &s->rows[s->n++];
    memset(r, 0, sizeof *r);
    r->parent_id = (uint32_t)m->args[0].num;
    r->id        = (uint32_t)m->args[1].num;
    r->item_type = (uint16_t)(m->args[6].num & 0xffff);
    copy_str(r->label1, sizeof r->label1, &m->args[3]);
    copy_str(r->label2, sizeof r->label2, &m->args[5]);
}

static djl_err menu_collect(djl_db *d, uint16_t req_type, const db_arg *setup, int nsetup,
                            uint32_t render_rmst, djl_menu_row *out, size_t max, size_t *count)
{
    if (count) *count = 0;
    db_message m;
    djl_err e = request(d, req_type, setup, nsetup, &m);
    if (e != DJL_OK) return e;
    if (m.type != 0x4000 || m.argcount < 2) { msg_free(&m); return DJL_ERR_UNAVAILABLE; }
    uint32_t total = (uint32_t)m.args[1].num;
    msg_free(&m);
    if (total == 0 || total == 0xffffffffu) return DJL_OK;

    row_sink sink = { out, max, 0 };
    e = render_menu(d, render_rmst, total, (uint32_t)max, row_cb, &sink);
    if (count) *count = sink.n;
    return e;
}

djl_err djl_db_folder_menu(djl_db *d, djl_slot slot, djl_track_type type,
                           uint32_t folder_id, djl_menu_row *out, size_t max, size_t *count)
{
    if (!d || !out) return DJL_ERR_INVAL;
    uint32_t r = rmst(d, MENU_MAIN, slot, type);
    db_arg a[4] = {
        { A_NUM, r, 0,0,0 }, { A_NUM, 0, 0,0,0 },
        { A_NUM, folder_id, 0,0,0 }, { A_NUM, 0, 0,0,0 },
    };
    return menu_collect(d, 0x2006, a, 4, r, out, max, count);
}

djl_err djl_db_track_list(djl_db *d, djl_slot slot, djl_track_type type,
                          djl_menu_row *out, size_t max, size_t *count)
{
    if (!d || !out) return DJL_ERR_INVAL;
    uint32_t r = rmst(d, MENU_MAIN, slot, type);
    db_arg a[2] = { { A_NUM, r, 0,0,0 }, { A_NUM, 0, 0,0,0 } };
    return menu_collect(d, 0x1004, a, 2, r, out, max, count);
}

djl_err djl_db_root_menu(djl_db *d, djl_slot slot, djl_track_type type,
                         djl_menu_row *out, size_t max, size_t *count)
{
    if (!d || !out) return DJL_ERR_INVAL;
    uint32_t r = rmst(d, MENU_MAIN, slot, type);
    db_arg a[3] = { { A_NUM, r, 0,0,0 }, { A_NUM, 0, 0,0,0 },
                    { A_NUM, 0xffffffu, 0,0,0 } };
    return menu_collect(d, 0x1000, a, 3, r, out, max, count);
}

/* -------------------- waveforms -------------------- */

/* Fetch a binary response (0x4402/0x4a02/0x4f02) and hand back arg[3] blob. */
static djl_err fetch_blob(struct djl_db *d, uint16_t req_type, const db_arg *args, int nargs,
                          uint16_t expect_type, uint8_t **data, uint32_t *len)
{
    db_message m;
    djl_err e = request(d, req_type, args, nargs, &m);
    if (e != DJL_OK) return e;
    if (m.type == 0x4003 /* UNAVAILABLE */ || m.type != expect_type || m.argcount < 4 ||
        m.args[3].tag != 0x14 || m.args[3].blen == 0) {
        msg_free(&m);
        return DJL_ERR_UNAVAILABLE;
    }
    *data = m.args[3].blob; *len = m.args[3].blen;
    m.args[3].blob = NULL;   /* transfer ownership */
    msg_free(&m);
    return DJL_OK;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

/* Fetch an ANLZ tag (0x2c04) and return the *tag body*, i.e. the bytes after
 * the dbserver 4-byte length prefix and the 12-byte TaggedSection header
 * (fourcc, len_header, len_tag). This matches beat-link's getTagViaDbServer,
 * which does position(4) then parses a TaggedSection whose body starts at +12.
 * Caller frees *body. */
static djl_err anlz_body(struct djl_db *d, djl_slot slot, djl_track_type type, uint32_t id,
                         uint32_t tag, uint32_t ext, uint8_t **body, uint32_t *body_len)
{
    uint8_t *raw = NULL; uint32_t rlen = 0;
    db_arg a[4] = {
        { A_NUM, rmst(d, MENU_MAIN, slot, type), 0,0,0 },
        { A_NUM, id, 0,0,0 }, { A_NUM, tag, 0,0,0 }, { A_NUM, ext, 0,0,0 },
    };
    djl_err e = fetch_blob(d, 0x2c04, a, 4, 0x4f02, &raw, &rlen);
    if (e != DJL_OK) return e;
    if (rlen < 16) { free(raw); return DJL_ERR_UNAVAILABLE; }
    /* raw = [len(4)][fourcc(4)][len_header(4)][len_tag(4)][body...] */
    uint32_t len_tag = be32(raw + 12);
    uint32_t blen = (len_tag >= 12) ? (len_tag - 12) : 0;
    if (blen == 0 || blen > rlen - 16) blen = rlen - 16;   /* clamp to available */
    uint8_t *b = malloc(blen ? blen : 1);
    if (!b) { free(raw); return DJL_ERR_NOMEM; }
    memcpy(b, raw + 16, blen);
    free(raw);
    *body = b; *body_len = blen;
    return DJL_OK;
}

/* Strip the waveform sub-header (len_entry_bytes u4, len_entries u4, and an
 * optional unknown u4 present on PWV4/5/7 but not PWV6) by auto-detecting its
 * size from len_entry_bytes*len_entries, leaving pure entry bytes in *out. */
static djl_err normalize_wave(uint8_t *body, uint32_t body_len, djl_waveform_blob *out,
                              djl_waveform_style style, bool detail)
{
    if (body_len < 8) { free(body); return DJL_ERR_UNAVAILABLE; }
    uint32_t esz = be32(body);
    uint32_t cnt = be32(body + 4);
    uint64_t entries_bytes = (uint64_t)esz * cnt;
    uint32_t hdr;
    if (esz && cnt && (uint64_t)body_len >= 8 + entries_bytes && body_len - 8 == entries_bytes)
        hdr = 8;                                  /* PWV6: no unknown field */
    else if (esz && cnt && (uint64_t)body_len >= 12 + entries_bytes)
        hdr = 12;                                 /* PWV4/PWV5/PWV7 */
    else { free(body); return DJL_ERR_UNAVAILABLE; }

    uint32_t elen = (uint32_t)entries_bytes;
    uint8_t *ent = malloc(elen ? elen : 1);
    if (!ent) { free(body); return DJL_ERR_NOMEM; }
    memcpy(ent, body + hdr, elen);
    free(body);
    out->style = style; out->detail = detail; out->data = ent; out->length = elen;
    return DJL_OK;
}

djl_err djl_db_waveform(djl_db *d, djl_slot slot, djl_track_type type, uint32_t id,
                        djl_waveform_style want, bool detail, djl_waveform_blob *out)
{
    if (!d || !out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    uint8_t *data = NULL, *body = NULL; uint32_t len = 0, blen = 0;
    djl_err e;

    /* RGB/3-band live in ANLZ .EXT/.2EX files that only exist for
     * rekordbox-analyzed tracks; requesting them for unanalyzed media leaves
     * the player silent and desyncs the connection, so gate on track type. */
    if (type == DJL_TRACK_REKORDBOX && want == DJL_WAVE_RGB) {
        e = anlz_body(d, slot, type, id, detail ? TAG_PWV5 : TAG_PWV4, EXT_EXT, &body, &blen);
        if (e == DJL_OK) return normalize_wave(body, blen, out, DJL_WAVE_RGB, detail);
    } else if (type == DJL_TRACK_REKORDBOX && want == DJL_WAVE_THREE_BAND) {
        e = anlz_body(d, slot, type, id, detail ? TAG_PWV7 : TAG_PWV6, EXT_2EX, &body, &blen);
        if (e == DJL_OK) return normalize_wave(body, blen, out, DJL_WAVE_THREE_BAND, detail);
    }

    /* Blue via dedicated messages (already pure entry bytes, no tag header). */
    if (detail) {
        db_arg a[3] = { { A_NUM, rmst(d, MENU_MAIN, slot, type),0,0,0 },
                        { A_NUM, id,0,0,0 }, { A_NUM, 0,0,0,0 } };
        e = fetch_blob(d, 0x2904, a, 3, 0x4a02, &data, &len);
    } else {
        db_arg a[4] = { { A_NUM, rmst(d, MENU_DATA, slot, type),0,0,0 },
                        { A_NUM, 1,0,0,0 }, { A_NUM, id,0,0,0 }, { A_NUM, 0,0,0,0 } };
        e = fetch_blob(d, 0x2004, a, 4, 0x4402, &data, &len);
    }
    if (e != DJL_OK) return e;
    out->style = DJL_WAVE_BLUE; out->detail = detail; out->length = len; out->data = data;
    return DJL_OK;
}

void djl_waveform_free(djl_waveform_blob *wf)
{
    if (wf) { free(wf->data); wf->data = NULL; wf->length = 0; }
}

/* -------------------- beat grid -------------------- */

djl_err djl_db_beat_grid(djl_db *d, djl_slot slot, djl_track_type type,
                         uint32_t id, djl_beat_grid *out)
{
    if (!d || !out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    db_arg a[2] = { { A_NUM, rmst(d, MENU_DATA, slot, type),0,0,0 }, { A_NUM, id,0,0,0 } };
    db_message m;
    djl_err e = request(d, 0x2204, a, 2, &m);
    if (e != DJL_OK) return e;
    if (m.type == 0x4003 || m.argcount < 4 || m.args[3].tag != 0x14 || m.args[3].blen < 20) {
        msg_free(&m); return DJL_ERR_UNAVAILABLE;
    }
    const uint8_t *g = m.args[3].blob;
    uint32_t glen = m.args[3].blen;
    uint32_t count = (glen - 20) / 16;
    if (count) {
        out->entries = calloc(count, sizeof *out->entries);
        if (!out->entries) { msg_free(&m); return DJL_ERR_NOMEM; }
        for (uint32_t i = 0; i < count; i++) {
            uint32_t base = 20 + i * 16;   /* little-endian, unusually for this protocol */
            out->entries[i].beat_within_bar = (uint16_t)(g[base] | (g[base+1]<<8));
            out->entries[i].tempo_x100      = (uint16_t)(g[base+2] | (g[base+3]<<8));
            out->entries[i].time_ms         = (uint32_t)g[base+4] |
                                              ((uint32_t)g[base+5] << 8) |
                                              ((uint32_t)g[base+6] << 16) |
                                              ((uint32_t)g[base+7] << 24);
        }
    }
    out->count = count;
    msg_free(&m);
    return DJL_OK;
}

void djl_beat_grid_free(djl_beat_grid *g)
{
    if (g) { free(g->entries); g->entries = NULL; g->count = 0; }
}

/* -------------------- album art -------------------- */

djl_err djl_db_album_art(djl_db *d, djl_slot slot, djl_track_type type,
                         uint32_t artwork_id, djl_blob *out)
{
    if (!d || !out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    db_arg a[2] = { { A_NUM, rmst(d, MENU_DATA, slot, type),0,0,0 },
                    { A_NUM, artwork_id,0,0,0 } };
    uint8_t *data = NULL; uint32_t len = 0;
    djl_err e = fetch_blob(d, 0x2003, a, 2, 0x4002, &data, &len);
    if (e != DJL_OK) return e;
    out->data = data; out->length = len;
    return DJL_OK;
}

void djl_blob_free(djl_blob *b)
{
    if (b) { free(b->data); b->data = NULL; b->length = 0; }
}

/* -------------------- cue list -------------------- */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1]<<8)); }

static void utf16le_to_utf8(const uint8_t *src, size_t bytes, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        uint32_t cp = src[i] | (src[i+1] << 8);
        if (cp == 0) break;
        if (cp < 0x80) { if (o+1>=outsz) break; out[o++]=(char)cp; }
        else if (cp < 0x800) { if (o+2>=outsz) break; out[o++]=(char)(0xc0|(cp>>6)); out[o++]=(char)(0x80|(cp&0x3f)); }
        else { if (o+3>=outsz) break; out[o++]=(char)(0xe0|(cp>>12)); out[o++]=(char)(0x80|((cp>>6)&0x3f)); out[o++]=(char)(0x80|(cp&0x3f)); }
    }
    out[o] = 0;
}

/* rekordbox hot-cue color code (0x01..0x3e) to RGB, ported from beat-link's
 * CueList.findRekordboxColor. */
bool djl_rekordbox_color(uint8_t code, uint8_t *r, uint8_t *g, uint8_t *b)
{
    static const uint8_t tbl[0x3f][3] = {
        {0,0,0},
        {0x30,0x5a,0xff},{0x50,0x73,0xff},{0x50,0x8c,0xff},{0x50,0xa0,0xff},
        {0x50,0xb4,0xff},{0x50,0xb0,0xf2},{0x50,0xae,0xe8},{0x45,0xac,0xdb},
        {0x00,0xe0,0xff},{0x19,0xda,0xf0},{0x32,0xd2,0xe6},{0x21,0xb4,0xb9},
        {0x20,0xaa,0xa0},{0x1f,0xa3,0x92},{0x19,0xa0,0x8c},{0x14,0xa5,0x84},
        {0x14,0xaa,0x7d},{0x10,0xb1,0x76},{0x30,0xd2,0x6e},{0x37,0xde,0x5a},
        {0x3c,0xeb,0x50},{0x28,0xe2,0x14},{0x7d,0xc1,0x3d},{0x8c,0xc8,0x32},
        {0x9b,0xd7,0x23},{0xa5,0xe1,0x16},{0xa5,0xdc,0x0a},{0xaa,0xd2,0x08},
        {0xb4,0xc8,0x05},{0xb4,0xbe,0x04},{0xba,0xb4,0x04},{0xc3,0xaf,0x04},
        {0xe1,0xaa,0x00},{0xff,0xa0,0x00},{0xff,0x96,0x00},{0xff,0x8c,0x00},
        {0xff,0x75,0x00},{0xe0,0x64,0x1b},{0xe0,0x46,0x1e},{0xe0,0x30,0x1e},
        {0xe0,0x28,0x23},{0xe6,0x28,0x28},{0xff,0x37,0x6f},{0xff,0x2d,0x6f},
        {0xff,0x12,0x7b},{0xf5,0x1e,0x8c},{0xeb,0x2d,0xa0},{0xe6,0x37,0xb4},
        {0xde,0x44,0xcf},{0xde,0x44,0x8d},{0xe6,0x30,0xb4},{0xe6,0x19,0xdc},
        {0xe6,0x00,0xff},{0xdc,0x00,0xff},{0xcc,0x00,0xff},{0xb4,0x32,0xff},
        {0xb9,0x3c,0xff},{0xc5,0x42,0xff},{0xaa,0x5a,0xff},{0xaa,0x72,0xff},
        {0x82,0x72,0xff},{0x64,0x73,0xff},
    };
    if (code == 0 || code > 0x3e) return false;
    if (r) *r = tbl[code][0];
    if (g) *g = tbl[code][1];
    if (b) *b = tbl[code][2];
    return true;
}

static djl_err parse_nexus_cues(const uint8_t *b, uint32_t len, djl_cue_list *out)
{
    uint32_t n = len / 36;
    djl_cue_entry *tmp = n ? calloc(n, sizeof *tmp) : NULL;
    if (n && !tmp) return DJL_ERR_NOMEM;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = i * 36;
        int cue_flag = b[off + 1], hot = b[off + 2];
        if (cue_flag == 0 && hot == 0) continue;
        djl_cue_entry *e = &tmp[k++];
        e->hot_cue = (uint8_t)hot;
        uint32_t pos = le32(b + off + 12);
        e->start_ms = (uint32_t)djl_halfframe_to_ms(pos);
        if (b[off] != 0) { e->is_loop = true; e->end_ms = (uint32_t)djl_halfframe_to_ms(le32(b + off + 16)); }
    }
    out->entries = tmp; out->count = k; out->extended = false;
    return DJL_OK;
}

static djl_err parse_nxs2_cues(const uint8_t *b, uint32_t len, uint32_t declared, djl_cue_list *out)
{
    djl_cue_entry *tmp = declared ? calloc(declared, sizeof *tmp) : NULL;
    if (declared && !tmp) return DJL_ERR_NOMEM;
    uint32_t k = 0, off = 0;
    for (uint32_t i = 0; i < declared && off + 8 <= len; i++) {
        uint32_t esz = le32(b + off);
        if (esz < 12 || off + esz > len) break;
        int hot = b[off + 4], cue_flag = b[off + 6];
        if (cue_flag != 0 || hot != 0) {
            djl_cue_entry *e = &tmp[k++];
            e->hot_cue = (uint8_t)hot;
            e->start_ms = le32(b + off + 12);          /* nxs2 stores milliseconds */
            if (cue_flag == 2) { e->is_loop = true; e->end_ms = le32(b + off + 16); }
            uint32_t comment_size = 0;
            if (esz > 0x49) comment_size = le16(b + off + 0x48);
            if (comment_size > 2 && off + 0x4a + comment_size <= len + 2)
                utf16le_to_utf8(b + off + 0x4a, comment_size - 2, e->comment, sizeof e->comment);
            if (hot == 0) {
                e->color_id = b[off + 0x22]; e->has_color = e->color_id != 0;
            } else {
                uint32_t cbase = off + comment_size + 0x4e;
                if (cbase + 3 < len) {
                    e->color_id = b[cbase];
                    e->r = b[cbase+1]; e->g = b[cbase+2]; e->b = b[cbase+3];
                    /* If no embedded RGB, fall back to the rekordbox color LUT. */
                    if (!(e->r || e->g || e->b) && e->color_id)
                        djl_rekordbox_color(e->color_id, &e->r, &e->g, &e->b);
                    e->has_color = (e->r || e->g || e->b || e->color_id);
                }
            }
        }
        off += esz;
    }
    out->entries = tmp; out->count = k; out->extended = true;
    return DJL_OK;
}

djl_err djl_db_cue_list(djl_db *d, djl_slot slot, djl_track_type type,
                        uint32_t id, bool extended, djl_cue_list *out)
{
    if (!d || !out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    db_message m;
    djl_err e;
    if (extended) {
        db_arg a[3] = { { A_NUM, rmst(d, MENU_DATA, slot, type),0,0,0 },
                        { A_NUM, id,0,0,0 }, { A_NUM, 0,0,0,0 } };
        e = request(d, 0x2b04, a, 3, &m);
    } else {
        db_arg a[2] = { { A_NUM, rmst(d, MENU_DATA, slot, type),0,0,0 },
                        { A_NUM, id,0,0,0 } };
        e = request(d, 0x2104, a, 2, &m);
    }
    if (e != DJL_OK) return e;
    if (m.type == 0x4003 || m.argcount < 4 || m.args[3].tag != 0x14) {
        msg_free(&m); return DJL_ERR_UNAVAILABLE;
    }
    if (extended) {
        uint32_t declared = (m.argcount >= 5) ? (uint32_t)m.args[4].num : 0;
        e = parse_nxs2_cues(m.args[3].blob, m.args[3].blen, declared, out);
    } else {
        e = parse_nexus_cues(m.args[3].blob, m.args[3].blen, out);
    }
    msg_free(&m);
    return e;
}

void djl_cue_list_free(djl_cue_list *c)
{
    if (c) { free(c->entries); c->entries = NULL; c->count = 0; }
}

/* -------------------- waveform accessors --------------------
 * After normalize_wave/blue fetch, wf->data holds pure entry bytes with a
 * per-style entry size:
 *   BLUE  preview 2B (height 0-31, whiteness), detail 1B (bits 7-5 color, 4-0 height)
 *   RGB   preview 6B, detail 2B (16-bit big-endian: rrr ggg bbb hhhhh 0)
 *   3BAND preview/detail 3B: mid, high, low heights (colors amber/white/blue)
 * RGB preview byte semantics are best-effort pending verification on rekordbox
 * media; the raw entries remain available in wf->data. */
static uint32_t wf_entry_size(const djl_waveform_blob *wf)
{
    switch (wf->style) {
    case DJL_WAVE_BLUE:       return wf->detail ? 1 : 2;
    case DJL_WAVE_RGB:        return wf->detail ? 2 : 6;
    case DJL_WAVE_THREE_BAND: return 3;
    }
    return 1;
}

int djl_waveform_segment_count(const djl_waveform_blob *wf)
{
    if (!wf || !wf->data) return 0;
    uint32_t es = wf_entry_size(wf);
    return es ? (int)(wf->length / es) : 0;
}

int djl_waveform_max_height(const djl_waveform_blob *wf)
{
    int n = djl_waveform_segment_count(wf), mx = 1;
    for (int i = 0; i < n; i++) { int h = djl_waveform_height(wf, i); if (h > mx) mx = h; }
    return mx;
}

int djl_waveform_height(const djl_waveform_blob *wf, int seg)
{
    if (!wf || !wf->data || seg < 0) return 0;
    uint32_t es = wf_entry_size(wf);
    uint32_t base = (uint32_t)seg * es;
    if (base + es > wf->length) return 0;
    const uint8_t *p = wf->data + base;
    /* Height ranges differ by style: blue is 0-31 (5 bits), RGB and 3-band are
     * raw unsigned bytes. Callers should normalise (see djl_waveform_max_height). */
    switch (wf->style) {
    case DJL_WAVE_BLUE:
        return p[0] & 0x1f;
    case DJL_WAVE_RGB:
        if (wf->detail) { uint16_t v = (uint16_t)((p[0]<<8)|p[1]); return (v >> 2) & 0x1f; }
        else { int r=p[3],g=p[4],b=p[5]; int m=r>g?r:g; return m>b?m:b; }  /* back envelope */
    case DJL_WAVE_THREE_BAND: {
        int mid=p[0],hi=p[1],lo=p[2]; int m=mid>hi?mid:hi; return m>lo?m:lo;
    }
    }
    return 0;
}

void djl_waveform_rgb(const djl_waveform_blob *wf, int seg, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t rr=0,gg=0,bb=0;
    if (wf && wf->data && seg >= 0) {
        uint32_t es = wf_entry_size(wf);
        uint32_t base = (uint32_t)seg * es;
        if (base + es <= wf->length) {
            const uint8_t *p = wf->data + base;
            switch (wf->style) {
            case DJL_WAVE_RGB:
                if (wf->detail) { uint16_t v=(uint16_t)((p[0]<<8)|p[1]);
                    rr=(uint8_t)(((v>>13)&7)*36); gg=(uint8_t)(((v>>10)&7)*36); bb=(uint8_t)(((v>>7)&7)*36); }
                else {  /* red/green/blue at p[3..5], scaled by the back height (beat-link) */
                    int back = p[3] > p[4] ? p[3] : p[4]; if (p[5] > back) back = p[5];
                    if (back > 0) { rr=(uint8_t)(p[3]*255/back); gg=(uint8_t)(p[4]*255/back); bb=(uint8_t)(p[5]*255/back); }
                }
                break;
            case DJL_WAVE_THREE_BAND:   /* mid=amber, high=white, low=blue */
                rr=(uint8_t)((p[0] + p[1]) > 31 ? 255 : (p[0]+p[1])*8);
                gg=(uint8_t)(((p[0]*2/3) + p[1]) > 31 ? 200 : ((p[0]*2/3)+p[1])*8);
                bb=(uint8_t)((p[2] + p[1]) > 31 ? 255 : (p[2]+p[1])*8);
                break;
            case DJL_WAVE_BLUE: {
                int h = p[0] & 0x1f; bb = (uint8_t)(h * 8);
                break;
            }
            }
        }
    }
    if (r) *r = rr;
    if (g) *g = gg;
    if (b) *b = bb;
}

/* -------------------- song structure (PSSI) -------------------- */

static const char *PHRASE_HIGH[] = {"","Intro","Up","Down","","Chorus","Outro"};
static const char *PHRASE_MID[]  = {"","Intro","Verse 1","Verse 2","Verse 3","Verse 4",
                                    "Verse 5","Verse 6","Bridge","Chorus","Outro"};
static const char *PHRASE_LOW[]  = {"","Intro","Verse 1","Verse 1","Verse 1","Verse 2",
                                    "Verse 2","Verse 2","Bridge","Chorus","Outro"};

const char *djl_phrase_label(djl_track_mood mood, uint16_t kind)
{
    switch (mood) {
    case DJL_MOOD_HIGH: return (kind < 7)  ? PHRASE_HIGH[kind] : "";
    case DJL_MOOD_MID:  return (kind < 11) ? PHRASE_MID[kind]  : "";
    case DJL_MOOD_LOW:  return (kind < 11) ? PHRASE_LOW[kind]  : "";
    default:            return "";
    }
}

/* XOR deobfuscation mask base (crate-digger rekordbox_anlz.ksy song_structure).
 * Each byte is (base[i%19] + len_entries) & 0xff. */
static const uint8_t PSSI_MASK[19] = {
    0xCB,0xE1,0xEE,0xFA,0xE5,0xEE,0xAD,0xEE,0xE9,0xD2,
    0xE9,0xEB,0xE1,0xE9,0xF3,0xE8,0xE9,0xF4,0xE1
};

djl_err djl_parse_song_structure(const uint8_t *tag_body, size_t len, djl_song_structure *out)
{
    if (!out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    if (!tag_body || len < 8) return DJL_ERR_UNAVAILABLE;

    /* tag_body = len_entry_bytes(u4), len_entries(u2), then the maybe-masked rest. */
    uint16_t cnt = (uint16_t)((tag_body[4] << 8) | tag_body[5]);
    djl_sha1(tag_body, len, out->sha1);      /* fingerprint over the raw body */

    uint32_t mlen = (uint32_t)len - 6;
    uint8_t *mreg = malloc(mlen ? mlen : 1);  /* work on a copy, keep input const */
    if (!mreg) return DJL_ERR_NOMEM;
    memcpy(mreg, tag_body + 6, mlen);

    /* raw_mood is the first u2 of the masked region; >20 means it is masked. */
    if (mlen >= 2) {
        uint16_t raw_mood = (uint16_t)((mreg[0] << 8) | mreg[1]);
        if (raw_mood > 20)
            for (uint32_t i = 0; i < mlen; i++)
                mreg[i] ^= (uint8_t)(PSSI_MASK[i % 19] + cnt);
    }

    /* song_structure_body: mood(u2)@0, +6, end_beat(u2)@8, +2, raw_bank(u1)@12,
     * +1, entries@14, each 24 bytes: index(u2), beat(u2), kind(u2), ... */
    if (mlen >= 14) {
        out->mood     = (djl_track_mood)((mreg[0] << 8) | mreg[1]);
        out->end_beat = (uint16_t)((mreg[8] << 8) | mreg[9]);
        out->bank     = mreg[12];
    }
    if (out->mood < DJL_MOOD_HIGH || out->mood > DJL_MOOD_LOW) out->mood = DJL_MOOD_UNKNOWN;

    out->raw_len = mlen;
    out->raw = mreg;                          /* transfer the deobfuscated copy */

    uint32_t avail = (mlen >= 14) ? (mlen - 14) / 24 : 0;
    if (cnt > avail) cnt = (uint16_t)avail;
    if (cnt) {
        out->phrases = calloc(cnt, sizeof *out->phrases);
        if (!out->phrases) return DJL_ERR_NOMEM;   /* out->raw freed by caller via free */
        for (uint32_t i = 0; i < cnt; i++) {
            const uint8_t *pe = mreg + 14 + i * 24;
            djl_phrase *ph = &out->phrases[i];
            ph->index = (uint16_t)((pe[0] << 8) | pe[1]);
            ph->beat  = (uint16_t)((pe[2] << 8) | pe[3]);
            ph->kind  = (uint16_t)((pe[4] << 8) | pe[5]);
            snprintf(ph->label, sizeof ph->label, "%s", djl_phrase_label(out->mood, ph->kind));
        }
        out->count = cnt;
    }
    return DJL_OK;
}

djl_err djl_db_song_structure(djl_db *d, djl_slot slot, djl_track_type type,
                              uint32_t id, djl_song_structure *out)
{
    if (!d || !out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    if (type != DJL_TRACK_REKORDBOX) return DJL_ERR_UNAVAILABLE;

    uint8_t *body = NULL; uint32_t blen = 0;
    /* PSSI = 0x50535349, in the EXT file. */
    djl_err e = anlz_body(d, slot, type, id, 0x50535349u, EXT_EXT, &body, &blen);
    if (e != DJL_OK) return e;
    e = djl_parse_song_structure(body, blen, out);
    free(body);
    return e;
}

void djl_song_structure_free(djl_song_structure *s)
{
    if (s) { free(s->phrases); free(s->raw); s->phrases = NULL; s->raw = NULL; s->count = 0; }
}
