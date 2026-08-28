/* libdjlink DeviceSQL (export.pdb) reader.
 *
 * Pure and bounds-checked: it borrows the caller's buffer and performs no I/O,
 * so it works equally on a file read over NFS and on a local rekordbox export
 * archive. Layout per crate-digger's rekordbox_pdb.ksy.
 *
 * Structure: the file is a series of fixed-size pages. Page 0 is a header that
 * gives the page size and lists tables; each table is a linked list of pages.
 * A page carries a heap of rows plus a row index built *backwards* from the end
 * of the page, in groups of 16, with a presence bitmask per group. Rows link to
 * variable-length strings by offset within the page.
 *
 * Everything is little-endian here, unlike the ANLZ files and the network
 * protocol, which are big-endian.
 */
#include "djl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PDB_HEAP_POS   40      /* row heap starts here within a page */
#define PDB_GROUP_SIZE 0x24    /* bytes per 16-row index group */
#define PDB_MAX_PAGES  100000  /* loop guard on a corrupt page chain */

/* Table / page types we care about. */
enum {
    PT_TRACKS = 0, PT_GENRES = 1, PT_ARTISTS = 2, PT_ALBUMS = 3,
    PT_LABELS = 4, PT_KEYS = 5, PT_COLORS = 6, PT_ARTWORK = 13
};

/* Where the id lives inside each row type, and where its name starts. */
#define TRACK_OFS_ID      0x48
#define TRACK_OFS_STRINGS 0x5e
#define TRACK_NUM_STRINGS 21

typedef struct {
    uint32_t id;
    uint32_t page;      /* byte offset of the page within the file */
    uint16_t row;       /* byte offset of the row within the page */
} rowref;

typedef struct { rowref *v; size_t n, cap; } rowvec;

struct djl_pdb {
    const uint8_t *d;
    size_t         len;
    uint32_t       page_size;
    rowvec tracks, artists, albums, genres, labels, keys, colors, artwork;
};

/* ---------------- little-endian accessors ---------------- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool vec_push(rowvec *v, uint32_t id, uint32_t page, uint16_t row)
{
    if (v->n == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 64;
        rowref *nv = realloc(v->v, ncap * sizeof *nv);
        if (!nv) return false;
        v->v = nv; v->cap = ncap;
    }
    v->v[v->n].id = id; v->v[v->n].page = page; v->v[v->n].row = row;
    v->n++;
    return true;
}

static const rowref *vec_find(const rowvec *v, uint32_t id)
{
    for (size_t i = 0; i < v->n; i++)
        if (v->v[i].id == id) return &v->v[i];
    return NULL;
}

/* ---------------- device_sql_string ---------------- */

/* Decode the string at page-relative offset ofs into UTF-8. Returns false if
 * the string is malformed or runs past the page. */
static bool read_dsql(const uint8_t *page, uint32_t page_size, uint32_t ofs,
                      char *out, size_t outsz)
{
    if (outsz == 0) return false;
    out[0] = '\0';
    if (ofs >= page_size) return false;

    uint8_t kind = page[ofs];
    if (kind == 0x40 || kind == 0x90) {
        /* Long form: u1 kind, u2 length (including this 4-byte header), u1 pad. */
        if (ofs + 4 > page_size) return false;
        uint16_t len = rd16(page + ofs + 1);
        if (len < 4) return false;
        uint32_t body = ofs + 4, blen = (uint32_t)len - 4;
        if (body + blen > page_size) return false;
        size_t o = 0;
        if (kind == 0x40) {                       /* ASCII */
            for (uint32_t i = 0; i < blen && o + 1 < outsz; i++) {
                if (page[body + i] == 0) break;
                out[o++] = (char)page[body + i];
            }
        } else {                                  /* UTF-16LE */
            for (uint32_t i = 0; i + 1 < blen; i += 2) {
                uint32_t cp = (uint32_t)page[body + i] | ((uint32_t)page[body + i + 1] << 8);
                if (cp == 0) break;
                if (cp < 0x80)       { if (o + 1 >= outsz) break; out[o++] = (char)cp; }
                else if (cp < 0x800) { if (o + 2 >= outsz) break;
                                       out[o++] = (char)(0xc0 | (cp >> 6));
                                       out[o++] = (char)(0x80 | (cp & 0x3f)); }
                else                 { if (o + 3 >= outsz) break;
                                       out[o++] = (char)(0xe0 | (cp >> 12));
                                       out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                                       out[o++] = (char)(0x80 | (cp & 0x3f)); }
            }
        }
        out[o] = '\0';
        return true;
    }

    /* Short ASCII: the kind byte itself carries the length, incremented,
     * doubled and incremented again -- so the payload is (kind>>1)-1 bytes. */
    uint32_t total = (uint32_t)(kind >> 1);
    if (total < 1) return false;
    uint32_t blen = total - 1;
    if (ofs + 1 + blen > page_size) return false;
    size_t o = 0;
    for (uint32_t i = 0; i < blen && o + 1 < outsz; i++) {
        if (page[ofs + 1 + i] == 0) break;
        out[o++] = (char)page[ofs + 1 + i];
    }
    out[o] = '\0';
    return true;
}

/* Read a name that a row stores as a near (u1) or far (u2) offset, chosen by
 * bit 0x04 of the row subtype. Used by artist/album/tag rows. */
static bool read_named_row(const uint8_t *page, uint32_t page_size, uint32_t row,
                           uint32_t near_at, uint32_t far_at, char *out, size_t outsz)
{
    if (row + near_at >= page_size) return false;
    uint16_t subtype = rd16(page + row);
    uint32_t ofs;
    if (subtype & 0x04) {
        if (row + far_at + 2 > page_size) return false;
        ofs = rd16(page + row + far_at);
    } else {
        ofs = page[row + near_at];
    }
    return read_dsql(page, page_size, row + ofs, out, outsz);
}

/* ---------------- table / page walking ---------------- */

/* Visit every present row of one table type, recording (id, page, row).
 *
 * The page chain is attacker-controlled, so it is walked with a visited bitmap:
 * rejecting only a self-link lets a two-page cycle be traversed PDB_MAX_PAGES
 * times, re-indexing the same rows on every pass. A 12 KB file with 32 real
 * rows produced 1.6 million rowrefs that way, and a large page size pushes it
 * into the gigabytes. */
static void index_table(struct djl_pdb *p, uint32_t first_page, uint32_t last_page,
                        uint32_t want_type, rowvec *into, uint32_t id_ofs, int id_size)
{
    uint32_t page_count = (uint32_t)(p->len / p->page_size);
    if (page_count == 0) return;
    uint8_t *seen = calloc((page_count + 7u) / 8u, 1);
    if (!seen) return;

    uint32_t idx = first_page;
    for (int guard = 0; guard < PDB_MAX_PAGES; guard++) {
        if (idx >= page_count) break;
        if (seen[idx >> 3] & (uint8_t)(1u << (idx & 7u))) break;   /* cycle */
        seen[idx >> 3] |= (uint8_t)(1u << (idx & 7u));
        uint64_t off = (uint64_t)idx * p->page_size;
        if (off + p->page_size > p->len) goto done;
        const uint8_t *page = p->d + off;

        uint32_t type = rd32(page + 8);
        uint32_t next = rd32(page + 12);
        uint8_t  flags = page[27];
        bool is_data = (flags & 0x40) == 0;

        if (is_data && type == want_type) {
            /* 13-bit row-offset count and 11-bit live-row count share 3 bytes. */
            uint32_t packed = (uint32_t)page[24] | ((uint32_t)page[25] << 8) |
                              ((uint32_t)page[26] << 16);
            uint32_t nofs = packed & 0x1fff;
            uint32_t groups = nofs ? ((nofs - 1) / 16 + 1) : 0;

            for (uint32_t g = 0; g < groups; g++) {
                /* Groups build backwards from the end of the page. */
                uint32_t base = p->page_size - g * PDB_GROUP_SIZE;
                if (base < 6 || base > p->page_size) break;
                uint16_t present = rd16(page + base - 4);
                uint32_t in_group = nofs - g * 16;
                if (in_group > 16) in_group = 16;

                for (uint32_t i = 0; i < in_group; i++) {
                    if (!((present >> i) & 1)) continue;   /* deleted row */
                    /* Bound the slot BEFORE computing it. base can legitimately
                     * be as small as 6, so base - 6 - i*2 underflows, and a
                     * check on the result would wrap too (0xfffffffe + 2 == 0)
                     * and never fire -- that was a ~4 GB out-of-bounds read
                     * reachable from a malformed export.pdb. */
                    uint32_t need = 6u + i * 2u + 2u;      /* end of slot i */
                    if (need > base) break;                /* index off the page */
                    uint32_t ptr = base - 6 - i * 2;
                    uint32_t row = PDB_HEAP_POS + rd16(page + ptr);
                    if (row + id_ofs + (uint32_t)id_size > p->page_size) continue;
                    uint32_t id = (id_size == 2) ? rd16(page + row + id_ofs)
                                                 : rd32(page + row + id_ofs);
                    if (!vec_push(into, id, (uint32_t)off, (uint16_t)row)) goto done;
                }
            }
        }

        if (idx == last_page) goto done;
        if (next == idx || next == 0) goto done;             /* malformed chain */
        uint64_t noff = (uint64_t)next * p->page_size;
        if (noff + p->page_size > p->len) goto done;
        idx = next;
    }

done:
    free(seen);
}

djl_err djl_pdb_open(const uint8_t *data, size_t len, djl_pdb **out)
{
    if (!data || !out) return DJL_ERR_INVAL;
    *out = NULL;
    if (len < 28) return DJL_ERR_SHORT;

    uint32_t page_size = rd32(data + 4);
    uint32_t ntables   = rd32(data + 8);
    if (page_size < 512 || page_size > 65536 || (page_size & (page_size - 1)) != 0)
        return DJL_ERR_INVAL;
    if (ntables == 0 || ntables > 64) return DJL_ERR_INVAL;
    if (28 + (uint64_t)ntables * 16 > len) return DJL_ERR_SHORT;

    struct djl_pdb *p = calloc(1, sizeof *p);
    if (!p) return DJL_ERR_NOMEM;
    p->d = data; p->len = len; p->page_size = page_size;

    for (uint32_t t = 0; t < ntables; t++) {
        const uint8_t *e = data + 28 + t * 16;
        uint32_t type  = rd32(e);
        uint32_t first = rd32(e + 8);
        uint32_t last  = rd32(e + 12);
        switch (type) {
        case PT_TRACKS:  index_table(p, first, last, type, &p->tracks,  TRACK_OFS_ID, 4); break;
        case PT_ARTISTS: index_table(p, first, last, type, &p->artists, 0x04, 4); break;
        case PT_ALBUMS:  index_table(p, first, last, type, &p->albums,  0x0c, 4); break;
        case PT_GENRES:  index_table(p, first, last, type, &p->genres,  0x00, 4); break;
        case PT_LABELS:  index_table(p, first, last, type, &p->labels,  0x00, 4); break;
        case PT_KEYS:    index_table(p, first, last, type, &p->keys,    0x00, 4); break;
        case PT_COLORS:  index_table(p, first, last, type, &p->colors,  0x05, 2); break;
        case PT_ARTWORK: index_table(p, first, last, type, &p->artwork, 0x00, 4); break;
        default: break;
        }
    }

    *out = p;
    return DJL_OK;
}

void djl_pdb_close(djl_pdb *p)
{
    if (!p) return;
    free(p->tracks.v);  free(p->artists.v); free(p->albums.v); free(p->genres.v);
    free(p->labels.v);  free(p->keys.v);    free(p->colors.v); free(p->artwork.v);
    free(p);
}

size_t djl_pdb_track_count(const djl_pdb *p) { return p ? p->tracks.n : 0; }

djl_err djl_pdb_track_id_at(const djl_pdb *p, size_t index, uint32_t *out_id)
{
    if (!p || !out_id) return DJL_ERR_INVAL;
    if (index >= p->tracks.n) return DJL_ERR_NOT_FOUND;
    *out_id = p->tracks.v[index].id;
    return DJL_OK;
}

/* ---------------- name resolution ---------------- */

/* Resolve a row in a table whose name follows the id inline. */
static void name_inline(const struct djl_pdb *p, const rowvec *v, uint32_t id,
                        uint32_t name_at, char *out, size_t outsz)
{
    if (outsz) out[0] = '\0';
    if (id == 0) return;
    const rowref *r = vec_find(v, id);
    if (!r) return;
    const uint8_t *page = p->d + r->page;
    read_dsql(page, p->page_size, r->row + name_at, out, outsz);
}

/* Resolve an artist-style row (near/far name offset). */
static void name_near_far(const struct djl_pdb *p, const rowvec *v, uint32_t id,
                          uint32_t near_at, uint32_t far_at, char *out, size_t outsz)
{
    if (outsz) out[0] = '\0';
    if (id == 0) return;
    const rowref *r = vec_find(v, id);
    if (!r) return;
    const uint8_t *page = p->d + r->page;
    read_named_row(page, p->page_size, r->row, near_at, far_at, out, outsz);
}

djl_err djl_pdb_track(const djl_pdb *p, uint32_t track_id, djl_track_info *out,
                      char *anlz_path, size_t anlz_path_sz)
{
    if (!p || !out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    if (anlz_path && anlz_path_sz) anlz_path[0] = '\0';

    const rowref *r = vec_find(&p->tracks, track_id);
    if (!r) return DJL_ERR_NOT_FOUND;

    const uint8_t *page = p->d + r->page;
    uint32_t row = r->row;
    uint32_t ps  = p->page_size;
    if (row + TRACK_OFS_STRINGS + TRACK_NUM_STRINGS * 2 > ps) return DJL_ERR_SHORT;

    out->rekordbox_id = track_id;
    out->type         = DJL_TRACK_REKORDBOX;
    out->artwork_id   = rd32(page + row + 0x1c);
    out->label_id     = rd32(page + row + 0x28);
    out->bitrate      = rd32(page + row + 0x30);
    out->tempo_x100   = rd32(page + row + 0x38);
    out->genre_id     = rd32(page + row + 0x3c);
    out->album_id     = rd32(page + row + 0x40);
    out->artist_id    = rd32(page + row + 0x44);
    out->year         = rd16(page + row + 0x50);
    out->duration_s   = rd16(page + row + 0x54);
    out->color_id     = page[row + 0x58];
    out->rating       = page[row + 0x59];

    uint32_t key_id            = rd32(page + row + 0x20);
    uint32_t original_artist_id = rd32(page + row + 0x24);
    uint32_t remixer_id        = rd32(page + row + 0x2c);

    /* The 21 string offsets are relative to the start of the row. */
    const uint8_t *ofs = page + row + TRACK_OFS_STRINGS;
    uint32_t s_date_added  = rd16(ofs + 10 * 2);
    uint32_t s_analyze     = rd16(ofs + 14 * 2);
    uint32_t s_comment     = rd16(ofs + 16 * 2);
    uint32_t s_title       = rd16(ofs + 17 * 2);

    read_dsql(page, ps, row + s_title,      out->title,      sizeof out->title);
    read_dsql(page, ps, row + s_comment,    out->comment,    sizeof out->comment);
    read_dsql(page, ps, row + s_date_added, out->date_added, sizeof out->date_added);

    if (anlz_path && anlz_path_sz)
        read_dsql(page, ps, row + s_analyze, anlz_path, anlz_path_sz);

    /* Cross-table names. Artists and albums use the near/far offset scheme;
     * genres, labels, keys and colors store the name inline after the id. */
    name_near_far(p, &p->artists, out->artist_id, 0x09, 0x0a,
                  out->artist, sizeof out->artist);
    name_near_far(p, &p->albums,  out->album_id,  0x15, 0x16,
                  out->album, sizeof out->album);
    name_near_far(p, &p->artists, original_artist_id, 0x09, 0x0a,
                  out->original_artist, sizeof out->original_artist);
    name_near_far(p, &p->artists, remixer_id, 0x09, 0x0a,
                  out->remixer, sizeof out->remixer);
    name_inline(p, &p->genres, out->genre_id, 0x04, out->genre, sizeof out->genre);
    name_inline(p, &p->labels, out->label_id, 0x04, out->label, sizeof out->label);
    name_inline(p, &p->keys,   key_id,        0x08, out->key,   sizeof out->key);
    name_inline(p, &p->colors, out->color_id, 0x08, out->color_name, sizeof out->color_name);

    out->found = true;
    return DJL_OK;
}

djl_err djl_pdb_artwork_path(const djl_pdb *p, uint32_t artwork_id,
                             char *out, size_t outsz)
{
    if (!p || !out || outsz == 0) return DJL_ERR_INVAL;
    out[0] = '\0';
    const rowref *r = vec_find(&p->artwork, artwork_id);
    if (!r) return DJL_ERR_NOT_FOUND;
    const uint8_t *page = p->d + r->page;
    /* artwork_row: u4 id, then the path string inline. */
    if (!read_dsql(page, p->page_size, r->row + 4, out, outsz)) return DJL_ERR_SHORT;
    return out[0] ? DJL_OK : DJL_ERR_NOT_FOUND;
}
