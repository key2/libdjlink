/* libdjlink OneLibrary reader: rekordbox Device Library Plus (exportLibrary.db).
 *
 * exportLibrary.db is a SQLCipher-4 database written by rekordbox for the
 * newer all-in-one players (OPUS-QUAD, OMNIS-DUO, XDJ-AZ) that do not write the
 * DeviceSQL export.pdb. We decrypt it in memory (sqlcipher.c) and read it with
 * libsqlite3 via sqlite3_deserialize, so there is no temp file and no
 * hand-rolled b-tree walker. The schema mirrors rekordbox's master.db; we join
 * `content` against the artist/album/genre/key/label/color tables to fill the
 * same djl_track_info the dbserver and PDB paths produce. See ARCHITECTURE.md
 * section 1.13.
 */
#include "djl_internal.h"
#include "onelibrary/onelibrary_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct djl_onelibrary {
    sqlite3 *db;
    uint8_t *image;      /* decrypted SQLite image, owned by sqlite while open */
    uint32_t *ids;       /* content_id list, ascending */
    size_t    n_ids;
};

bool djl_onelibrary_supported(void) { return true; }

djl_err djl_onelibrary_decrypt(const uint8_t *enc, size_t len, djl_blob *out)
{
    return djl_onelibrary_sqlcipher_decrypt(enc, len, out);
}

/* Copy a TEXT column into a fixed buffer, tolerating NULL. */
static void copy_text(sqlite3_stmt *st, int col, char *dst, size_t cap)
{
    if (!cap) return;
    dst[0] = '\0';
    if (sqlite3_column_type(st, col) == SQLITE_NULL) return;
    const unsigned char *s = sqlite3_column_text(st, col);
    if (s) snprintf(dst, cap, "%s", (const char *)s);
}

static uint32_t col_u32(sqlite3_stmt *st, int col)
{
    return (sqlite3_column_type(st, col) == SQLITE_NULL)
           ? 0u : (uint32_t)sqlite3_column_int64(st, col);
}

/* Load the ascending content_id list so track_id_at is a stable index. */
static djl_err load_ids(djl_onelibrary *o)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(o->db, "SELECT content_id FROM content ORDER BY content_id",
                           -1, &st, NULL) != SQLITE_OK)
        return DJL_ERR_STATE;

    size_t cap = 64, n = 0;
    uint32_t *v = malloc(cap * sizeof *v);
    if (!v) { sqlite3_finalize(st); return DJL_ERR_NOMEM; }

    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap *= 2;
            uint32_t *nv = realloc(v, cap * sizeof *v);
            if (!nv) { free(v); sqlite3_finalize(st); return DJL_ERR_NOMEM; }
            v = nv;
        }
        v[n++] = (uint32_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    o->ids = v;
    o->n_ids = n;
    return DJL_OK;
}

djl_err djl_onelibrary_open(const uint8_t *enc, size_t len, djl_onelibrary **out)
{
    if (!enc || !out) return DJL_ERR_INVAL;
    *out = NULL;

    djl_blob plain;
    djl_err e = djl_onelibrary_sqlcipher_decrypt(enc, len, &plain);
    if (e != DJL_OK) return e;

    struct djl_onelibrary *o = calloc(1, sizeof *o);
    if (!o) { djl_blob_free(&plain); return DJL_ERR_NOMEM; }
    o->image = plain.data;              /* ownership moves to the handle */

    if (sqlite3_open(":memory:", &o->db) != SQLITE_OK) {
        djl_onelibrary_close(o);
        return DJL_ERR_STATE;
    }
    /* Hand sqlite the decrypted image directly; READONLY so it never writes,
     * and sqlite keeps our buffer for the connection's lifetime. */
    if (sqlite3_deserialize(o->db, "main", o->image, plain.length, plain.length,
                            SQLITE_DESERIALIZE_READONLY) != SQLITE_OK) {
        djl_onelibrary_close(o);
        return DJL_ERR_STATE;
    }

    e = load_ids(o);
    if (e != DJL_OK) { djl_onelibrary_close(o); return e; }

    *out = o;
    return DJL_OK;
}

void djl_onelibrary_close(djl_onelibrary *o)
{
    if (!o) return;
    if (o->db) sqlite3_close(o->db);
    free(o->image);                     /* freed after sqlite_close releases it */
    free(o->ids);
    free(o);
}

size_t djl_onelibrary_track_count(const djl_onelibrary *o)
{
    return o ? o->n_ids : 0;
}

djl_err djl_onelibrary_track_id_at(const djl_onelibrary *o, size_t index, uint32_t *out_id)
{
    if (!o || !out_id) return DJL_ERR_INVAL;
    if (index >= o->n_ids) return DJL_ERR_NOT_FOUND;
    *out_id = o->ids[index];
    return DJL_OK;
}

/* One row per track, names resolved by joining the lookup tables. Column order
 * here defines the indices used below. */
static const char *TRACK_SQL =
    "SELECT c.content_id, c.title, c.bpmx100, c.length, c.releaseYear,"
    "       c.rating, c.color_id, c.image_id, c.bitrate, c.djComment,"
    "       c.dateAdded, c.analysisDataFilePath,"
    "       c.artist_id_artist, c.album_id, c.genre_id, c.label_id,"
    "       a.name  AS artist_name,"
    "       al.name AS album_name,"
    "       g.name  AS genre_name,"
    "       l.name  AS label_name,"
    "       k.name  AS key_name,"
    "       co.name AS color_name,"
    "       ar.name AS remixer_name,"
    "       ao.name AS orig_name "
    "FROM content c "
    "LEFT JOIN artist a  ON a.artist_id  = c.artist_id_artist "
    "LEFT JOIN album  al ON al.album_id  = c.album_id "
    "LEFT JOIN genre  g  ON g.genre_id   = c.genre_id "
    "LEFT JOIN label  l  ON l.label_id   = c.label_id "
    "LEFT JOIN key    k  ON k.key_id     = c.key_id "
    "LEFT JOIN color  co ON co.color_id  = c.color_id "
    "LEFT JOIN artist ar ON ar.artist_id = c.artist_id_remixer "
    "LEFT JOIN artist ao ON ao.artist_id = c.artist_id_originalArtist "
    "WHERE c.content_id = ?";

djl_err djl_onelibrary_track(const djl_onelibrary *o, uint32_t content_id,
                             djl_track_info *out, char *anlz_path, size_t anlz_path_sz)
{
    if (!o || !out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    if (anlz_path && anlz_path_sz) anlz_path[0] = '\0';

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(o->db, TRACK_SQL, -1, &st, NULL) != SQLITE_OK)
        return DJL_ERR_STATE;
    sqlite3_bind_int64(st, 1, content_id);

    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DJL_ERR_NOT_FOUND; }

    out->rekordbox_id = content_id;
    out->type         = DJL_TRACK_REKORDBOX;
    copy_text(st, 1, out->title, sizeof out->title);
    out->tempo_x100   = col_u32(st, 2);
    out->duration_s   = col_u32(st, 3);
    out->year         = col_u32(st, 4);
    out->rating       = (uint8_t)col_u32(st, 5);
    out->color_id     = (uint8_t)col_u32(st, 6);
    out->artwork_id   = col_u32(st, 7);
    out->bitrate      = col_u32(st, 8);
    copy_text(st, 9,  out->comment,    sizeof out->comment);
    copy_text(st, 10, out->date_added, sizeof out->date_added);
    if (anlz_path && anlz_path_sz) copy_text(st, 11, anlz_path, anlz_path_sz);

    out->artist_id = col_u32(st, 12);
    out->album_id  = col_u32(st, 13);
    out->genre_id  = col_u32(st, 14);
    out->label_id  = col_u32(st, 15);

    copy_text(st, 16, out->artist,          sizeof out->artist);
    copy_text(st, 17, out->album,           sizeof out->album);
    copy_text(st, 18, out->genre,           sizeof out->genre);
    copy_text(st, 19, out->label,           sizeof out->label);
    copy_text(st, 20, out->key,             sizeof out->key);
    copy_text(st, 21, out->color_name,      sizeof out->color_name);
    copy_text(st, 22, out->remixer,         sizeof out->remixer);
    copy_text(st, 23, out->original_artist, sizeof out->original_artist);

    out->found = true;
    sqlite3_finalize(st);
    return DJL_OK;
}

djl_err djl_onelibrary_artwork_path(const djl_onelibrary *o, uint32_t artwork_id,
                                    char *out, size_t outsz)
{
    if (!o || !out || outsz == 0) return DJL_ERR_INVAL;
    out[0] = '\0';
    if (artwork_id == 0) return DJL_ERR_NOT_FOUND;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(o->db, "SELECT path FROM image WHERE image_id = ?",
                           -1, &st, NULL) != SQLITE_OK)
        return DJL_ERR_STATE;
    sqlite3_bind_int64(st, 1, artwork_id);

    djl_err e = DJL_ERR_NOT_FOUND;
    if (sqlite3_step(st) == SQLITE_ROW) {
        copy_text(st, 0, out, outsz);
        if (out[0]) e = DJL_OK;
    }
    sqlite3_finalize(st);
    return e;
}
