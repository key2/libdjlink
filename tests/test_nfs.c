/* libdjlink NFS / PDB / ANLZ codec tests.
 *
 * The XDR and RPC halves are exercised as pure functions; the PDB and ANLZ
 * readers run against byte fixtures built here so the layout assumptions are
 * pinned without needing a USB stick. Golden values for the ANLZ section
 * framing come from a real rekordbox export read off CDJ-3000X player 1
 * (PIONEER/USBANLZ/P012/0000973C/ANLZ0001.DAT, 2026-08-27).
 */
#include "djlink.h"
#include "nfs/nfs_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int  djl_test_checks;
extern int  djl_test_failures;

#define CHECK(cond, ...) do {                              \
    djl_test_checks++;                                     \
    if (!(cond)) {                                         \
        djl_test_failures++;                               \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
        printf(__VA_ARGS__);                               \
        printf("\n");                                      \
    }                                                      \
} while (0)

#define CHECK_EQ_U(a, b) do {                                          \
    djl_test_checks++;                                                 \
    unsigned long long _a = (unsigned long long)(a);                   \
    unsigned long long _b = (unsigned long long)(b);                   \
    if (_a != _b) {                                                    \
        djl_test_failures++;                                           \
        printf("FAIL %s:%d: %s = %llu, expected %llu\n",               \
               __FILE__, __LINE__, #a, _a, _b);                        \
    }                                                                  \
} while (0)

#define CHECK_STR(a, b) do {                                           \
    djl_test_checks++;                                                 \
    if (strcmp((a), (b)) != 0) {                                       \
        djl_test_failures++;                                           \
        printf("FAIL %s:%d: %s = \"%s\", expected \"%s\"\n",           \
               __FILE__, __LINE__, #a, (a), (b));                      \
    }                                                                  \
} while (0)

/* ---------------- XDR ---------------- */

static void test_xdr_roundtrip(void)
{
    uint8_t buf[128];
    djl_xdr_w w;
    djl_xdr_w_init(&w, buf, sizeof buf);

    djl_xdr_put_u32(&w, 0x01020304u);
    const uint8_t three[3] = { 0xaa, 0xbb, 0xcc };
    djl_xdr_put_opaque(&w, three, 3);      /* 4 len + 3 data + 1 pad = 8 */
    const uint8_t four[4] = { 1, 2, 3, 4 };
    djl_xdr_put_fixed(&w, four, 4);        /* no padding needed */
    CHECK(!w.err, "writer must not overflow");
    CHECK_EQ_U(w.len, 4 + 8 + 4);

    /* Integers are big-endian on the wire regardless of host order. */
    CHECK_EQ_U(buf[0], 0x01); CHECK_EQ_U(buf[3], 0x04);
    CHECK_EQ_U(buf[7], 3);                 /* opaque length */
    CHECK_EQ_U(buf[11], 0x00);             /* pad byte is zeroed */

    djl_xdr_r r;
    djl_xdr_r_init(&r, buf, w.len);
    CHECK_EQ_U(djl_xdr_get_u32(&r), 0x01020304u);
    uint32_t n = 0;
    const uint8_t *p = djl_xdr_get_opaque(&r, &n);
    CHECK(p != NULL, "opaque must decode");
    CHECK_EQ_U(n, 3);
    CHECK(p && p[0] == 0xaa && p[2] == 0xcc, "opaque payload must round-trip");
    uint8_t got[4] = {0};
    CHECK(djl_xdr_get_fixed(&r, got, 4), "fixed must decode");
    CHECK(memcmp(got, four, 4) == 0, "fixed payload must round-trip");
    CHECK(!r.err, "reader must not error on exact consumption");
}

static void test_xdr_bounds(void)
{
    uint8_t small[4];
    djl_xdr_w w;
    djl_xdr_w_init(&w, small, sizeof small);
    djl_xdr_put_u32(&w, 1);
    CHECK(!w.err, "first word fits");
    djl_xdr_put_u32(&w, 2);
    CHECK(w.err, "second word must set the overflow flag");

    /* A truncated reader must fail rather than read past the end. */
    const uint8_t two[2] = { 0, 1 };
    djl_xdr_r r;
    djl_xdr_r_init(&r, two, sizeof two);
    (void)djl_xdr_get_u32(&r);
    CHECK(r.err, "short read must set the error flag");

    /* A bogus opaque length must be rejected, not trusted. */
    const uint8_t liar[8] = { 0xff, 0xff, 0xff, 0xff, 1, 2, 3, 4 };
    djl_xdr_r_init(&r, liar, sizeof liar);
    uint32_t n = 0;
    CHECK(djl_xdr_get_opaque(&r, &n) == NULL, "oversized opaque must be refused");
}

static void test_utf16le(void)
{
    uint8_t out[32];
    /* The single most important encoding in this subsystem: Pioneer's NFS
     * wants UTF-16LE mount paths and file names, not ASCII. */
    size_t n = djl_utf16le_encode("/C/", out, sizeof out);
    CHECK_EQ_U(n, 6);
    CHECK(out[0] == '/' && out[1] == 0 && out[2] == 'C' && out[3] == 0 &&
          out[4] == '/' && out[5] == 0, "\"/C/\" must encode as UTF-16LE");

    n = djl_utf16le_encode("PIONEER", out, sizeof out);
    CHECK_EQ_U(n, 14);
    CHECK(out[0] == 'P' && out[1] == 0 && out[12] == 'R' && out[13] == 0,
          "ASCII names must widen to UTF-16LE");

    /* Non-ASCII and astral planes. */
    n = djl_utf16le_encode("\xc3\xa9", out, sizeof out);       /* U+00E9 */
    CHECK_EQ_U(n, 2);
    CHECK(out[0] == 0xe9 && out[1] == 0x00, "U+00E9 encodes little-endian");
    n = djl_utf16le_encode("\xf0\x9f\x8e\xa7", out, sizeof out); /* U+1F3A7 */
    CHECK_EQ_U(n, 4);
    CHECK(out[0] == 0x3c && out[1] == 0xd8, "astral chars use a surrogate pair");

    /* Must refuse to overflow rather than truncate silently. */
    uint8_t tiny[2];
    CHECK_EQ_U(djl_utf16le_encode("ab", tiny, sizeof tiny), 0);
    CHECK_EQ_U(djl_utf16le_encode("", out, sizeof out), 0);
}

/* ---------------- RPC framing ---------------- */

static void test_rpc_call_header(void)
{
    uint8_t buf[64];
    djl_xdr_w w;
    djl_xdr_w_init(&w, buf, sizeof buf);
    djl_rpc_build_call(&w, 0xdeadbeefu, DJL_PROG_MOUNT, 1, 1);
    CHECK(!w.err, "call header must fit");
    CHECK_EQ_U(w.len, 40);                       /* 10 words, AUTH_NULL */

    CHECK_EQ_U(buf[0], 0xde);                    /* xid */
    CHECK_EQ_U((buf[4]<<24)|(buf[5]<<16)|(buf[6]<<8)|buf[7], 0);  /* CALL */
    CHECK_EQ_U(buf[11], 2);                      /* rpcvers = 2 */
    CHECK_EQ_U(((uint32_t)buf[12]<<24)|((uint32_t)buf[13]<<16)|
               ((uint32_t)buf[14]<<8)|buf[15], DJL_PROG_MOUNT);
    CHECK_EQ_U(buf[19], 1);                      /* vers */
    CHECK_EQ_U(buf[23], 1);                      /* proc = MNT */
    for (int i = 24; i < 40; i++)
        CHECK_EQ_U(buf[i], 0);                   /* AUTH_NULL cred + verf */
}

/* Build a synthetic accepted reply carrying `payload`. */
static size_t make_reply(uint8_t *buf, uint32_t xid, uint32_t reply_stat,
                         uint32_t accept_stat, const uint8_t *payload, size_t plen)
{
    djl_xdr_w w;
    djl_xdr_w_init(&w, buf, 256);
    djl_xdr_put_u32(&w, xid);
    djl_xdr_put_u32(&w, 1);              /* REPLY */
    djl_xdr_put_u32(&w, reply_stat);
    djl_xdr_put_u32(&w, 0);              /* verf flavor */
    djl_xdr_put_u32(&w, 0);              /* verf length */
    djl_xdr_put_u32(&w, accept_stat);
    if (plen) djl_xdr_put_fixed(&w, payload, plen);
    return w.len;
}

static void test_rpc_reply_parse(void)
{
    uint8_t buf[256];
    const uint8_t body[4] = { 0, 0, 0x08, 0x01 };   /* e.g. a port number */
    size_t len = make_reply(buf, 0x1234u, 0, 0, body, sizeof body);

    size_t off = 0;
    CHECK_EQ_U(djl_rpc_reply_body(buf, len, 0x1234u, &off), DJL_OK);
    CHECK_EQ_U(off, 24);
    CHECK(memcmp(buf + off, body, 4) == 0, "result body must follow accept_stat");

    /* A reply for a different transaction must be reported as stale, not
     * accepted: that is what lets the retransmit loop skip duplicates. */
    CHECK_EQ_U(djl_rpc_reply_body(buf, len, 0x9999u, &off), DJL_ERR_NOT_FOUND);

    /* Denied and program-error replies must not be mistaken for success. */
    len = make_reply(buf, 7, 1 /* MSG_DENIED */, 0, NULL, 0);
    CHECK_EQ_U(djl_rpc_reply_body(buf, len, 7, &off), DJL_ERR_IO);
    len = make_reply(buf, 7, 0, 2 /* PROG_MISMATCH */, NULL, 0);
    CHECK_EQ_U(djl_rpc_reply_body(buf, len, 7, &off), DJL_ERR_UNAVAILABLE);

    /* Truncation anywhere must be caught. */
    len = make_reply(buf, 7, 0, 0, body, sizeof body);
    for (size_t cut = 0; cut < len; cut++)
        CHECK(djl_rpc_reply_body(buf, cut, 7, &off) != DJL_OK ||
              cut >= 24, "truncated reply must not parse as success");

    /* A CALL arriving where a REPLY belongs must be rejected. */
    djl_xdr_w w;
    djl_xdr_w_init(&w, buf, sizeof buf);
    djl_rpc_build_call(&w, 7, DJL_PROG_NFS, 2, 4);
    CHECK_EQ_U(djl_rpc_reply_body(buf, w.len, 7, &off), DJL_ERR_IO);
}

/* ---------------- ANLZ fixtures ---------------- */

static void be32_at(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void be16_at(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

/* Append one tagged section. Body is copied; len_tag counts the 12-byte
 * prologue, which is exactly how real files do it (verified against a live
 * export: PPTH len_header=0x10 len_tag=0x92, PQTZ len_header=0x18). */
static size_t put_section(uint8_t *buf, size_t at, const char *fourcc,
                          uint32_t len_header, const uint8_t *body, size_t blen)
{
    memcpy(buf + at, fourcc, 4);
    be32_at(buf + at + 4, len_header);
    be32_at(buf + at + 8, (uint32_t)(blen + 12));
    if (blen) memcpy(buf + at + 12, body, blen);
    return at + 12 + blen;
}

static void test_anlz_beat_grid(void)
{
    uint8_t file[512];
    memset(file, 0, sizeof file);
    memcpy(file, "PMAI", 4);
    be32_at(file + 4, 0x1c);

    /* PQTZ: two unknown words, beat count, then 8-byte beats. */
    uint8_t body[12 + 3 * 8];
    memset(body, 0, sizeof body);
    be32_at(body + 4, 0x00080000u);
    be32_at(body + 8, 3);
    struct { uint16_t bib, bpm; uint32_t ms; } beats[3] = {
        { 1, 12800, 236 }, { 2, 12800, 704 }, { 3, 12800, 1172 }
    };
    for (int i = 0; i < 3; i++) {
        be16_at(body + 12 + i * 8,     beats[i].bib);
        be16_at(body + 12 + i * 8 + 2, beats[i].bpm);
        be32_at(body + 12 + i * 8 + 4, beats[i].ms);
    }
    size_t end = put_section(file, 0x1c, "PQTZ", 0x18, body, sizeof body);
    be32_at(file + 8, (uint32_t)end);

    djl_anlz a;
    memset(&a, 0, sizeof a);
    CHECK_EQ_U(djl_anlz_parse(file, end, &a), DJL_OK);
    CHECK(a.has_grid, "PQTZ must yield a beat grid");
    CHECK_EQ_U(a.grid.count, 3);
    if (a.grid.count == 3) {
        CHECK_EQ_U(a.grid.entries[0].beat_within_bar, 1);
        CHECK_EQ_U(a.grid.entries[0].tempo_x100, 12800);
        CHECK_EQ_U(a.grid.entries[0].time_ms, 236);
        CHECK_EQ_U(a.grid.entries[2].time_ms, 1172);
    }
    djl_anlz_free(&a);

    /* A count that overruns the section must be rejected, not trusted. */
    be32_at(file + 0x1c + 12 + 8, 9999);
    memset(&a, 0, sizeof a);
    CHECK_EQ_U(djl_anlz_parse(file, end, &a), DJL_OK);
    CHECK(!a.has_grid, "an impossible beat count must be refused");
    djl_anlz_free(&a);
}

static void test_anlz_cues(void)
{
    uint8_t file[1024];
    memset(file, 0, sizeof file);
    memcpy(file, "PMAI", 4);
    be32_at(file + 4, 0x1c);

    /* PCOB with one PCPT loop entry. */
    uint8_t pcob[12 + 0x38];
    memset(pcob, 0, sizeof pcob);
    be16_at(pcob + 6, 1);                       /* len_cues */
    uint8_t *e = pcob + 12;
    memcpy(e, "PCPT", 4);
    be32_at(e + 4, 0x1c);
    be32_at(e + 8, 0x38);                       /* entry length */
    be32_at(e + 0x0c, 0);                       /* memory point */
    e[0x1c] = 2;                                /* loop */
    be32_at(e + 0x20, 5000);
    be32_at(e + 0x24, 9000);
    size_t end = put_section(file, 0x1c, "PCOB", 0x18, pcob, sizeof pcob);

    djl_anlz a;
    memset(&a, 0, sizeof a);
    CHECK_EQ_U(djl_anlz_parse(file, end, &a), DJL_OK);
    CHECK(a.has_cues, "PCOB must yield cues");
    CHECK_EQ_U(a.cues.count, 1);
    CHECK(!a.cues.extended, "PCOB is the basic form");
    if (a.cues.count == 1) {
        CHECK(a.cues.entries[0].is_loop, "entry kind 2 is a loop");
        CHECK_EQ_U(a.cues.entries[0].start_ms, 5000);
        CHECK_EQ_U(a.cues.entries[0].end_ms, 9000);
    }

    /* Now an extended PCO2 hot cue with a comment and an embedded color: it
     * must replace whatever PCOB contributed, the way rekordbox intends. */
    uint8_t pco2[8 + 0x40];
    memset(pco2, 0, sizeof pco2);
    be32_at(pco2, 1);                           /* list type: hot cues */
    be16_at(pco2 + 4, 1);
    uint8_t *x = pco2 + 8;
    memcpy(x, "PCP2", 4);
    be32_at(x + 4, 0x1c);
    be32_at(x + 8, 0x40);
    be32_at(x + 0x0c, 1);                       /* hot cue A */
    x[0x10] = 1;                                /* plain cue */
    be32_at(x + 0x14, 236);
    be32_at(x + 0x28, 4);                       /* comment: 2 UTF-16BE chars */
    be16_at(x + 0x2c, 'O'); be16_at(x + 0x2e, 'K');
    x[0x30] = 0x0a;                             /* color id */
    x[0x31] = 0xff; x[0x32] = 0x00; x[0x33] = 0x17;
    size_t end2 = put_section(file, end, "PCO2", 0x18, pco2, sizeof pco2);
    be32_at(file + 8, (uint32_t)end2);

    CHECK_EQ_U(djl_anlz_parse(file, end2, &a), DJL_OK);
    CHECK(a.cues.extended, "PCO2 must supersede PCOB");
    CHECK_EQ_U(a.cues.count, 1);
    if (a.cues.count == 1) {
        const djl_cue_entry *c = &a.cues.entries[0];
        CHECK_EQ_U(c->hot_cue, 1);
        CHECK_EQ_U(c->start_ms, 236);
        CHECK_STR(c->comment, "OK");
        CHECK(c->has_color, "embedded RGB must be reported");
        CHECK_EQ_U(c->r, 0xff); CHECK_EQ_U(c->g, 0x00); CHECK_EQ_U(c->b, 0x17);
    }
    djl_anlz_free(&a);
}

static void test_anlz_waveforms(void)
{
    uint8_t file[4096];
    memset(file, 0, sizeof file);
    memcpy(file, "PMAI", 4);
    be32_at(file + 4, 0x1c);

    /* PWV5: RGB detail, 2 bytes per entry, with the extra unknown word. */
    uint8_t pwv5[12 + 8 * 2];
    memset(pwv5, 0, sizeof pwv5);
    be32_at(pwv5, 2);          /* len_entry_bytes */
    be32_at(pwv5 + 4, 8);      /* len_entries */
    for (int i = 0; i < 8; i++) be16_at(pwv5 + 12 + i * 2, (uint16_t)(0x1234 + i));
    size_t end = put_section(file, 0x1c, "PWV5", 0x18, pwv5, sizeof pwv5);

    /* PWV7: 3-band detail, 3 bytes per entry, no unknown word. */
    uint8_t pwv7[8 + 8 * 3];
    memset(pwv7, 0, sizeof pwv7);
    be32_at(pwv7, 3);
    be32_at(pwv7 + 4, 8);
    size_t end2 = put_section(file, end, "PWV7", 0x14, pwv7, sizeof pwv7);
    be32_at(file + 8, (uint32_t)end2);

    djl_anlz a;
    memset(&a, 0, sizeof a);
    CHECK_EQ_U(djl_anlz_parse(file, end2, &a), DJL_OK);
    CHECK(a.has_detail, "detail waveform must be present");
    /* 3-band outranks RGB for display... */
    CHECK_EQ_U(a.detail.style, DJL_WAVE_THREE_BAND);
    CHECK_EQ_U(djl_waveform_segment_count(&a.detail), 8);
    /* ...but the RGB detail must survive separately, because the track
     * signature is defined over it. */
    CHECK(a.has_rgb_detail, "PWV5 must be retained for signatures");
    CHECK_EQ_U(a.rgb_detail.length, 16);
    CHECK_EQ_U(a.rgb_detail.style, DJL_WAVE_RGB);
    CHECK(a.rgb_detail.detail, "rgb_detail is a detail waveform");
    djl_anlz_free(&a);
}

static void test_anlz_rejects_garbage(void)
{
    djl_anlz a;
    memset(&a, 0, sizeof a);
    const uint8_t nope[16] = { 'N','O','P','E' };
    CHECK_EQ_U(djl_anlz_parse(nope, sizeof nope, &a), DJL_ERR_UNKNOWN);
    CHECK_EQ_U(djl_anlz_parse(NULL, 0, &a), DJL_ERR_INVAL);
    const uint8_t stub[4] = { 'P','M','A','I' };
    CHECK_EQ_U(djl_anlz_parse(stub, sizeof stub, &a), DJL_ERR_SHORT);

    /* A section claiming a length that runs off the end must stop the walk
     * without reading past the buffer. */
    uint8_t file[64];
    memset(file, 0, sizeof file);
    memcpy(file, "PMAI", 4);
    be32_at(file + 4, 0x1c);
    memcpy(file + 0x1c, "PQTZ", 4);
    be32_at(file + 0x1c + 8, 0xfffffff0u);
    CHECK_EQ_U(djl_anlz_parse(file, sizeof file, &a), DJL_OK);
    CHECK(!a.has_grid, "a lying len_tag must not produce data");
    djl_anlz_free(&a);
}

/* ---------------- PDB fixture ---------------- */

static void le16_at(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32_at(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Write a short-form DeviceSQL ASCII string, returning its byte length. The
 * kind byte is the length incremented, doubled and incremented again. */
static size_t put_dsql(uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    p[0] = (uint8_t)(((n + 1) << 1) | 1);
    memcpy(p + 1, s, n);
    return n + 1;
}

#define PAGE_SZ 4096

/* Build a one-page tracks table plus a one-page artists table. */
static size_t build_pdb(uint8_t *buf, size_t cap)
{
    memset(buf, 0, cap);
    /* File header: zero, page size, table count, then table descriptors. */
    le32_at(buf + 4, PAGE_SZ);
    le32_at(buf + 8, 2);
    /* tracks: type 0, first page 1, last page 1 */
    le32_at(buf + 28 + 0,  0);
    le32_at(buf + 28 + 8,  1);
    le32_at(buf + 28 + 12, 1);
    /* artists: type 2, first page 2, last page 2 */
    le32_at(buf + 44 + 0,  2);
    le32_at(buf + 44 + 8,  2);
    le32_at(buf + 44 + 12, 2);

    /* ---- page 1: one track row ---- */
    uint8_t *pg = buf + PAGE_SZ;
    le32_at(pg + 4, 1);            /* page_index */
    le32_at(pg + 8, 0);            /* type = tracks */
    le32_at(pg + 12, 1);           /* next_page = self, ends the chain */
    pg[24] = 1;                    /* num_row_offsets = 1 */
    pg[27] = 0x24;                 /* data page (bit 0x40 clear) */

    uint8_t *row = pg + 40;        /* heap starts at 40 */
    le32_at(row + 0x1c, 77);       /* artwork_id */
    le32_at(row + 0x30, 320);      /* bitrate */
    le32_at(row + 0x38, 12800);    /* tempo x100 */
    le32_at(row + 0x44, 5);        /* artist_id */
    le32_at(row + 0x48, 33);       /* track id */
    le16_at(row + 0x50, 2014);     /* year */
    le16_at(row + 0x54, 225);      /* duration */
    row[0x58] = 0;                 /* color id */
    row[0x59] = 4;                 /* rating */

    /* Strings live after the 21-entry offset array, which starts at 0x5e. */
    uint16_t strings_at = 0x5e + 21 * 2;
    uint8_t *sp = row + strings_at;
    uint16_t ofs_title   = strings_at;
    size_t   used        = put_dsql(sp, "Outside");
    uint16_t ofs_analyze = (uint16_t)(strings_at + used);
    used += put_dsql(row + ofs_analyze, "/PIONEER/USBANLZ/P012/0000973C/ANLZ0001.DAT");
    uint16_t ofs_added   = (uint16_t)(strings_at + used);
    put_dsql(row + ofs_added, "2026-08-26");

    le16_at(row + 0x5e + 10 * 2, ofs_added);
    le16_at(row + 0x5e + 14 * 2, ofs_analyze);
    le16_at(row + 0x5e + 17 * 2, ofs_title);

    /* Row index, built backwards from the end of the page. */
    le16_at(pg + PAGE_SZ - 4, 0x0001);   /* row_present_flags: row 0 present */
    le16_at(pg + PAGE_SZ - 6, 0);        /* ofs_row 0: heap-relative */

    /* ---- page 2: one artist row ---- */
    uint8_t *pg2 = buf + 2 * PAGE_SZ;
    le32_at(pg2 + 4, 2);
    le32_at(pg2 + 8, 2);           /* type = artists */
    le32_at(pg2 + 12, 2);
    pg2[24] = 1;
    pg2[27] = 0x24;

    uint8_t *arow = pg2 + 40;
    le16_at(arow + 0, 0x60);       /* subtype: near name offset */
    le32_at(arow + 4, 5);          /* artist id */
    arow[8] = 0x03;
    arow[9] = 0x0a;                /* ofs_name_near */
    put_dsql(arow + 0x0a, "Calvin Harris");
    le16_at(pg2 + PAGE_SZ - 4, 0x0001);
    le16_at(pg2 + PAGE_SZ - 6, 0);

    return 3 * PAGE_SZ;
}

static void test_pdb_reader(void)
{
    uint8_t *buf = malloc(3 * PAGE_SZ);
    CHECK(buf != NULL, "fixture allocation");
    if (!buf) return;
    size_t len = build_pdb(buf, 3 * PAGE_SZ);

    djl_pdb *p = NULL;
    CHECK_EQ_U(djl_pdb_open(buf, len, &p), DJL_OK);
    CHECK(p != NULL, "reader must open");
    if (p) {
        CHECK_EQ_U(djl_pdb_track_count(p), 1);
        uint32_t id = 0;
        CHECK_EQ_U(djl_pdb_track_id_at(p, 0, &id), DJL_OK);
        CHECK_EQ_U(id, 33);
        CHECK_EQ_U(djl_pdb_track_id_at(p, 1, &id), DJL_ERR_NOT_FOUND);

        djl_track_info ti;
        char anlz[256];
        CHECK_EQ_U(djl_pdb_track(p, 33, &ti, anlz, sizeof anlz), DJL_OK);
        CHECK(ti.found, "track must be found");
        CHECK_STR(ti.title, "Outside");
        CHECK_STR(ti.artist, "Calvin Harris");   /* resolved across tables */
        CHECK_STR(ti.date_added, "2026-08-26");
        CHECK_STR(anlz, "/PIONEER/USBANLZ/P012/0000973C/ANLZ0001.DAT");
        CHECK_EQ_U(ti.tempo_x100, 12800);
        CHECK_EQ_U(ti.duration_s, 225);
        CHECK_EQ_U(ti.year, 2014);
        CHECK_EQ_U(ti.bitrate, 320);
        CHECK_EQ_U(ti.rating, 4);
        CHECK_EQ_U(ti.artwork_id, 77);
        CHECK_EQ_U(ti.artist_id, 5);
        CHECK_EQ_U(ti.rekordbox_id, 33);

        CHECK_EQ_U(djl_pdb_track(p, 999, &ti, NULL, 0), DJL_ERR_NOT_FOUND);
        djl_pdb_close(p);
    }

    /* Deleted rows must be skipped: clear the presence bit and re-read. */
    le16_at(buf + PAGE_SZ + PAGE_SZ - 4, 0x0000);
    p = NULL;
    CHECK_EQ_U(djl_pdb_open(buf, len, &p), DJL_OK);
    if (p) {
        CHECK_EQ_U(djl_pdb_track_count(p), 0);
        djl_pdb_close(p);
    }

    /* Malformed headers must be refused rather than trusted. */
    p = NULL;
    CHECK_EQ_U(djl_pdb_open(buf, 8, &p), DJL_ERR_SHORT);
    le32_at(buf + 4, 7);                       /* page size not a power of two */
    CHECK_EQ_U(djl_pdb_open(buf, len, &p), DJL_ERR_INVAL);
    le32_at(buf + 4, PAGE_SZ);
    le32_at(buf + 8, 100000);                  /* absurd table count */
    CHECK_EQ_U(djl_pdb_open(buf, len, &p), DJL_ERR_INVAL);
    CHECK_EQ_U(djl_pdb_open(NULL, len, &p), DJL_ERR_INVAL);

    free(buf);
}

/* ---------------- fuzz ---------------- */

static uint32_t rng_state = 0x13572468u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Both readers must be total: no crash, no out-of-bounds read, for any input.
 * Run under ASan/UBSan this is where layout mistakes surface. */
static void test_fuzz_parsers(void)
{
    uint8_t buf[1024];
    for (int iter = 0; iter < 30000; iter++) {
        size_t len = 1 + (rng() % sizeof buf);
        for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)rng();

        /* Half the time make it look like a plausible container so the walkers
         * get past their magic checks and into the field parsing. */
        if ((iter & 1) && len >= 32) {
            memcpy(buf, "PMAI", 4);
            be32_at(buf + 4, 0x1c);
            djl_anlz a;
            memset(&a, 0, sizeof a);
            if (djl_anlz_parse(buf, len, &a) == DJL_OK) djl_anlz_free(&a);
            else djl_anlz_free(&a);
        } else {
            djl_anlz a;
            memset(&a, 0, sizeof a);
            if (djl_anlz_parse(buf, len, &a) == DJL_OK) djl_anlz_free(&a);
            else djl_anlz_free(&a);
        }

        if (len >= 64) {
            le32_at(buf + 4, 1024);          /* plausible page size */
            le32_at(buf + 8, 1 + (rng() % 4));
            djl_pdb *p = NULL;
            if (djl_pdb_open(buf, len, &p) == DJL_OK && p) {
                size_t n = djl_pdb_track_count(p);
                for (size_t i = 0; i < n && i < 8; i++) {
                    uint32_t id = 0;
                    if (djl_pdb_track_id_at(p, i, &id) == DJL_OK) {
                        djl_track_info ti;
                        char path[128];
                        (void)djl_pdb_track(p, id, &ti, path, sizeof path);
                    }
                }
                djl_pdb_close(p);
            }
        }

        /* XDR readers over arbitrary bytes. */
        djl_xdr_r r;
        djl_xdr_r_init(&r, buf, len);
        for (int k = 0; k < 6; k++) {
            uint32_t n = 0;
            (void)djl_xdr_get_u32(&r);
            (void)djl_xdr_get_opaque(&r, &n);
        }
        size_t off = 0;
        (void)djl_rpc_reply_body(buf, len, 0x1234u, &off);
    }
    djl_test_checks++;
    printf("  fuzz: 30000 NFS/PDB/ANLZ iterations completed without crashing\n");
}

void djl_test_nfs(void);
void djl_test_nfs(void)
{
    test_xdr_roundtrip();
    test_xdr_bounds();
    test_utf16le();
    test_rpc_call_header();
    test_rpc_reply_parse();
    test_anlz_beat_grid();
    test_anlz_cues();
    test_anlz_waveforms();
    test_anlz_rejects_garbage();
    test_pdb_reader();
    test_fuzz_parsers();
}
