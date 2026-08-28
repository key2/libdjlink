/* libdjlink ANLZ reader: the PMAI container in rekordbox .DAT/.EXT/.2EX files.
 *
 * Pure and bounds-checked, no I/O: feed it bytes from NFS, from a local
 * rekordbox export archive, or from a test fixture.
 *
 * Everything in ANLZ is BIG-endian (the PDB, confusingly, is little-endian).
 * A file is a 0x1c-byte PMAI header followed by tagged sections; each section
 * is fourcc(4), len_header(4), len_tag(4), then its body. len_tag counts the
 * whole section including the 12-byte prologue.
 *
 * Tag distribution:
 *   .DAT  PQTZ (beat grid) PCOB (cues) PPTH (path) PVBR PWAV/PWV2/PWV3 (blue)
 *   .EXT  PQT2 PCO2 (extended cues) PSSI (phrases) PWV4/PWV5 (RGB)
 *   .2EX  PWV6/PWV7 (3-band) PWVC
 *
 * Cue sections appear twice per file (memory points and hot cues), so cues
 * accumulate; the extended PCO2 form supersedes the basic PCOB form wholesale.
 */
#include "djl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FourCCs, read big-endian as they appear in the file. */
#define T_PQTZ 0x5051545Au
#define T_PCOB 0x50434F42u
#define T_PCO2 0x50434F32u
#define T_PSSI 0x50535349u
#define T_PPTH 0x50505448u
#define T_PWAV 0x50574156u
#define T_PWV3 0x50575633u
#define T_PWV4 0x50575634u
#define T_PWV5 0x50575635u
#define T_PWV6 0x50575636u
#define T_PWV7 0x50575637u
#define T_PMAI 0x504D4149u

#define ANLZ_MAX_BEATS   200000u
#define ANLZ_MAX_CUES    2000u
#define ANLZ_MAX_ENTRIES 2000000u

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | p[3];
}

/* ---------------- PQTZ: beat grid ---------------- */

static void parse_pqtz(const uint8_t *b, uint32_t len, djl_anlz *out)
{
    if (out->has_grid || len < 12) return;
    /* u4 unknown, u4 unknown (0x80000), u4 num_beats, then 8-byte entries. */
    uint32_t n = be32(b + 8);
    if (n == 0 || n > ANLZ_MAX_BEATS) return;
    if ((uint64_t)12 + (uint64_t)n * 8 > len) return;

    djl_beat_grid_entry *e = calloc(n, sizeof *e);
    if (!e) return;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *p = b + 12 + i * 8;
        e[i].beat_within_bar = be16(p);
        e[i].tempo_x100      = be16(p + 2);
        e[i].time_ms         = be32(p + 4);
    }
    out->grid.entries = e;
    out->grid.count   = n;
    out->has_grid     = true;
}

/* ---------------- cue lists ---------------- */

static bool cues_reserve(djl_cue_list *c, uint32_t extra)
{
    uint32_t want = c->count + extra;
    if (want > ANLZ_MAX_CUES) return false;
    djl_cue_entry *v = realloc(c->entries, (size_t)want * sizeof *v);
    if (!v) return false;
    memset(v + c->count, 0, (size_t)extra * sizeof *v);
    c->entries = v;
    return true;
}

/* Is this cue already in the list? PCOB occurs in both .DAT and .EXT of the
 * same track, and both files are parsed into one djl_anlz, so without this the
 * whole basic cue list is emitted twice whenever there is no PCO2 to reset it. */
static bool cue_already_present(const djl_cue_list *c, const djl_cue_entry *n)
{
    for (uint32_t i = 0; i < c->count; i++) {
        const djl_cue_entry *e = &c->entries[i];
        if (e->start_ms == n->start_ms && e->hot_cue == n->hot_cue &&
            e->is_loop == n->is_loop && e->end_ms == n->end_ms)
            return true;
    }
    return false;
}

/* PCOB: basic cue list, PCPT entries, no colors or comments. */
static void parse_pcob(const uint8_t *b, uint32_t len, djl_anlz *out)
{
    if (out->cues.extended || len < 12) return;   /* PCO2 already won */
    uint16_t declared = be16(b + 6);
    if (declared == 0 || declared > ANLZ_MAX_CUES) return;
    if (!cues_reserve(&out->cues, declared)) return;

    uint32_t pos = 12, added = 0;
    for (uint16_t i = 0; i < declared && pos + 12 <= len; i++) {
        if (be32(b + pos) != 0x50435054u) break;             /* "PCPT" */
        uint32_t esz = be32(b + pos + 8);
        if (esz < 0x28 || esz > 4096 || pos + esz > len) break;
        const uint8_t *e = b + pos;

        uint32_t hot  = be32(e + 0x0c);
        uint8_t  kind = e[0x1c];                             /* 1 = cue, 2 = loop */
        /* Deliberately NOT filtering on the u4 at 0x10 that the Kaitai spec
         * calls "status". Live CDJ-3000X exports carry status 0 on hot cues
         * that are demonstrably active (both cues of track 33, confirmed
         * against the dbserver path), and beat-link's CueList.addEntriesFromTag
         * emits every entry regardless, so treating 0 as "deleted" would drop
         * real memory points. */

        djl_cue_entry cue;
        memset(&cue, 0, sizeof cue);
        cue.hot_cue  = (hot > 0 && hot < 256) ? (uint8_t)hot : 0;
        cue.start_ms = be32(e + 0x20);
        if (kind == 2) { cue.is_loop = true; cue.end_ms = be32(e + 0x24); }
        pos += esz;

        if (cue_already_present(&out->cues, &cue)) continue;
        out->cues.entries[out->cues.count + added] = cue;
        added++;
    }
    out->cues.count += added;
    if (out->cues.count) out->has_cues = true;
}

/* PCO2: extended cue list, PCP2 entries with color and comment. */
static void parse_pco2(const uint8_t *b, uint32_t len, djl_anlz *out)
{
    if (len < 8) return;
    if (!out->cues.extended) {
        /* First extended section: discard anything PCOB contributed. */
        free(out->cues.entries);
        out->cues.entries  = NULL;
        out->cues.count    = 0;
        out->cues.extended = true;
    }
    uint16_t declared = be16(b + 4);
    if (declared == 0 || declared > ANLZ_MAX_CUES) return;
    if (!cues_reserve(&out->cues, declared)) return;

    uint32_t pos = 8, added = 0;
    for (uint16_t i = 0; i < declared && pos + 12 <= len; i++) {
        if (be32(b + pos) != 0x50435032u) break;             /* "PCP2" */
        uint32_t esz = be32(b + pos + 8);
        if (esz < 0x1d || esz > 8192 || pos + esz > len) break;
        const uint8_t *e = b + pos;

        uint32_t hot  = be32(e + 0x0c);
        uint8_t  kind = e[0x10];                             /* 1 = cue, 2 = loop */
        djl_cue_entry *c = &out->cues.entries[out->cues.count + added];
        c->hot_cue  = (hot > 0 && hot < 256) ? (uint8_t)hot : 0;
        c->start_ms = be32(e + 0x14);
        if (kind == 2) { c->is_loop = true; c->end_ms = be32(e + 0x18); }

        /* A UTF-16BE comment of len_comment bytes sits at 0x2c, and the color
         * follows it. Older exports truncate the entry before either.
         *
         * The declared length always determines where the color lives, exactly
         * as the dbserver parser does. Zeroing it on an implausible value used
         * to rebase the color read into the middle of the comment, so the same
         * cue got a different color depending on which provider served it. */
        uint32_t clen = (esz >= 0x2c) ? be32(e + 0x28) : 0;
        bool comment_ok = clen > 2 && clen < 512 && 0x2c + clen <= esz;
        if (comment_ok) {
            size_t o = 0;
            for (uint32_t k = 0; k + 1 < clen && o + 3 < sizeof c->comment; k += 2) {
                uint32_t cp = ((uint32_t)e[0x2c + k] << 8) | e[0x2c + k + 1];
                if (cp == 0) break;
                if (cp < 0x80)       c->comment[o++] = (char)cp;
                else if (cp < 0x800) { c->comment[o++] = (char)(0xc0 | (cp >> 6));
                                       c->comment[o++] = (char)(0x80 | (cp & 0x3f)); }
                else                 { c->comment[o++] = (char)(0xe0 | (cp >> 12));
                                       c->comment[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                                       c->comment[o++] = (char)(0x80 | (cp & 0x3f)); }
            }
            c->comment[o] = '\0';
        }
        /* Only read the color where the entry says it is. If the declared
         * comment length is nonsense, the color position is unknowable, so
         * report no color rather than a plausible-looking wrong one. */
        if (clen == 0 || comment_ok) {
            uint32_t coff = 0x2c + clen;
            if (coff + 4 <= esz) {
                c->color_id = e[coff];
                c->r = e[coff + 1]; c->g = e[coff + 2]; c->b = e[coff + 3];
                if (!(c->r || c->g || c->b) && c->color_id)
                    djl_rekordbox_color(c->color_id, &c->r, &c->g, &c->b);
                c->has_color = (c->r || c->g || c->b || c->color_id);
            }
        }
        added++;
        pos += esz;
    }
    out->cues.count += added;
    if (out->cues.count) out->has_cues = true;
}

/* ---------------- waveforms ---------------- */

static int wave_rank(djl_waveform_style s)
{
    switch (s) {
    case DJL_WAVE_THREE_BAND: return 3;
    case DJL_WAVE_RGB:        return 2;
    default:                  return 1;
    }
}

/* Copy pure entry bytes into a blob, replacing a lower-quality style. */
static void set_wave(djl_anlz *out, bool detail, djl_waveform_style style,
                     const uint8_t *data, uint32_t len)
{
    djl_waveform_blob *slot = detail ? &out->detail : &out->preview;
    bool *have = detail ? &out->has_detail : &out->has_preview;
    if (*have && wave_rank(slot->style) >= wave_rank(style)) return;

    uint8_t *copy = malloc(len ? len : 1);
    if (!copy) return;
    memcpy(copy, data, len);
    free(slot->data);
    slot->data   = copy;
    slot->length = len;
    slot->style  = style;
    slot->detail = detail;
    *have = true;
}

/* PWV4/5/6/7 all start with u4 len_entry_bytes, u4 len_entries, and the
 * 4-byte-headed variants add one unknown u4 before the entries. Detect which
 * from the arithmetic, exactly as the dbserver path does. */
static bool wave_entries(const uint8_t *b, uint32_t len,
                         const uint8_t **data, uint32_t *bytes_out)
{
    if (len < 8) return false;
    uint32_t esz = be32(b);
    uint32_t cnt = be32(b + 4);
    if (esz == 0 || cnt == 0 || cnt > ANLZ_MAX_ENTRIES) return false;
    uint64_t bytes = (uint64_t)esz * cnt;
    uint32_t hdr;
    if ((uint64_t)len >= 8 + bytes && (uint64_t)len - 8 == bytes) hdr = 8;
    else if ((uint64_t)len >= 12 + bytes)                          hdr = 12;
    else return false;
    *data = b + hdr;
    *bytes_out = (uint32_t)bytes;
    return true;
}

static void parse_wave_sized(const uint8_t *b, uint32_t len, djl_anlz *out,
                             bool detail, djl_waveform_style style)
{
    const uint8_t *data = NULL;
    uint32_t bytes = 0;
    if (!wave_entries(b, len, &data, &bytes)) return;
    set_wave(out, detail, style, data, bytes);
}

/* Keep PWV5 in its own slot: djl_track_signature (like beat-link's
 * SignatureFinder) hashes the RGB detail waveform, so a 3-band track must not
 * lose it just because PWV7 is the better one to draw. */
static void parse_rgb_detail(const uint8_t *b, uint32_t len, djl_anlz *out)
{
    if (out->has_rgb_detail) return;
    const uint8_t *data = NULL;
    uint32_t bytes = 0;
    if (!wave_entries(b, len, &data, &bytes)) return;
    uint8_t *copy = malloc(bytes ? bytes : 1);
    if (!copy) return;
    memcpy(copy, data, bytes);
    out->rgb_detail.data   = copy;
    out->rgb_detail.length = bytes;
    out->rgb_detail.style  = DJL_WAVE_RGB;
    out->rgb_detail.detail = true;
    out->has_rgb_detail    = true;
}

/* PWV3 (blue detail / "wave scroll") is a SIZED tag like PWV4/5/6/7 -- u4
 * len_entry_bytes, u4 len_entries, u4 unknown, then entries -- NOT a PWAV-style
 * tag. Parsing it as PWAV read len_entry_bytes (1) as the segment count and
 * produced a single garbage byte instead of the whole waveform. */
static void parse_pwv3(const uint8_t *b, uint32_t len, djl_anlz *out)
{
    const uint8_t *data = NULL;
    uint32_t bytes = 0;
    if (!wave_entries(b, len, &data, &bytes)) return;
    set_wave(out, true, DJL_WAVE_BLUE, data, bytes);
}

/* PWAV (blue preview) stores u4 len_preview, u4 unknown, then one byte per
 * segment. The dbserver blue preview is two bytes per segment (height,
 * whiteness), so expand to keep one accessor for both. */
static void parse_wave_blue(const uint8_t *b, uint32_t len, djl_anlz *out)
{
    if (len < 8) return;
    uint32_t n = be32(b);
    if (n == 0 || n > ANLZ_MAX_ENTRIES || 8 + (uint64_t)n > len) return;
    const uint8_t *src = b + 8;

    djl_waveform_blob *slot = &out->preview;
    if (out->has_preview && wave_rank(slot->style) >= wave_rank(DJL_WAVE_BLUE)) return;
    uint8_t *copy = malloc((size_t)n * 2);
    if (!copy) return;
    for (uint32_t i = 0; i < n; i++) {
        copy[i * 2]     = (uint8_t)(src[i] & 0x1f);          /* height */
        copy[i * 2 + 1] = (uint8_t)((src[i] >> 5) & 0x07);   /* whiteness */
    }
    free(slot->data);
    slot->data   = copy;
    slot->length = n * 2;
    slot->style  = DJL_WAVE_BLUE;
    slot->detail = false;
    out->has_preview = true;
}

/* ---------------- PPTH: audio file path ---------------- */

static void parse_ppth(const uint8_t *b, uint32_t len, djl_anlz *out)
{
    if (len < 4 || out->path[0]) return;
    uint32_t plen = be32(b);
    if (plen < 2 || 4 + (uint64_t)plen > len) return;
    size_t o = 0;
    for (uint32_t i = 0; i + 1 < plen && o + 3 < sizeof out->path; i += 2) {
        uint32_t cp = ((uint32_t)b[4 + i] << 8) | b[4 + i + 1];   /* UTF-16BE */
        if (cp == 0) break;
        if (cp < 0x80)       out->path[o++] = (char)cp;
        else if (cp < 0x800) { out->path[o++] = (char)(0xc0 | (cp >> 6));
                               out->path[o++] = (char)(0x80 | (cp & 0x3f)); }
        else                 { out->path[o++] = (char)(0xe0 | (cp >> 12));
                               out->path[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                               out->path[o++] = (char)(0x80 | (cp & 0x3f)); }
    }
    out->path[o] = '\0';
}

/* ---------------- container walk ---------------- */

djl_err djl_anlz_parse(const uint8_t *data, size_t len, djl_anlz *inout)
{
    if (!data || !inout) return DJL_ERR_INVAL;
    if (len < 12) return DJL_ERR_SHORT;
    if (be32(data) != T_PMAI) return DJL_ERR_UNKNOWN;

    uint32_t hdr = be32(data + 4);
    if (hdr < 12 || hdr > len) hdr = 0x1c;
    if (hdr > len) return DJL_ERR_SHORT;

    size_t pos = hdr;
    while (pos + 12 <= len) {
        uint32_t tag  = be32(data + pos);
        uint32_t tlen = be32(data + pos + 8);
        if (tlen < 12 || pos + tlen > len) break;      /* truncated or garbage */

        const uint8_t *body = data + pos + 12;
        uint32_t blen = tlen - 12;

        switch (tag) {
        case T_PQTZ: parse_pqtz(body, blen, inout); break;
        case T_PCOB: parse_pcob(body, blen, inout); break;
        case T_PCO2: parse_pco2(body, blen, inout); break;
        case T_PPTH: parse_ppth(body, blen, inout); break;
        case T_PWAV: parse_wave_blue(body, blen, inout); break;
        case T_PWV3: parse_pwv3(body, blen, inout); break;
        case T_PWV4: parse_wave_sized(body, blen, inout, false, DJL_WAVE_RGB); break;
        case T_PWV5: parse_wave_sized(body, blen, inout, true,  DJL_WAVE_RGB);
                     parse_rgb_detail(body, blen, inout); break;
        case T_PWV6: parse_wave_sized(body, blen, inout, false, DJL_WAVE_THREE_BAND); break;
        case T_PWV7: parse_wave_sized(body, blen, inout, true,  DJL_WAVE_THREE_BAND); break;
        case T_PSSI:
            if (!inout->has_ss) {
                /* The PSSI body is exactly what the dbserver 0x2c04 path hands
                 * to the parser: len_entry_bytes, len_entries, masked body. */
                djl_song_structure ss;
                if (djl_parse_song_structure(body, blen, &ss) == DJL_OK) {
                    inout->ss = ss;
                    inout->has_ss = true;
                } else {
                    djl_song_structure_free(&ss);
                }
            }
            break;
        /* Deliberately not parsed. PQT2, PWVC and PVDI all appear in real
         * rekordbox 7 exports but none is in crate-digger's published spec;
         * PQT2 in particular is NOT a PQTZ with a bigger header (its beat
         * entries sit at +24 and it carries one u16 per beat from +0x38), so
         * aliasing the two silently corrupts the grid. PQTZ in the .DAT is
         * authoritative and complete. See ARCHITECTURE.md section 1.12. */
        default: break;                       /* PVBR, PQT2, PWVC, PVDI, ... */
        }
        pos += tlen;
    }
    return DJL_OK;
}

void djl_anlz_free(djl_anlz *a)
{
    if (!a) return;
    djl_beat_grid_free(&a->grid);
    djl_cue_list_free(&a->cues);
    djl_song_structure_free(&a->ss);
    djl_waveform_free(&a->preview);
    djl_waveform_free(&a->detail);
    djl_waveform_free(&a->rgb_detail);
    memset(a, 0, sizeof *a);
}
