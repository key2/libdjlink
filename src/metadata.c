/* libdjlink metadata auto-fetch manager.
 *
 * On a track load, a background worker opens a dbserver connection to the
 * player that *hosts* the media (the Dr field of the loading player's status,
 * which may be a different player when tracks are shared over Link), pulls the
 * metadata, waveform, beat grid, cues and art, caches them per playing player,
 * and emits notification events. The worker never touches the I/O thread's
 * state except through ctx->lock, and does all blocking dbserver I/O with no
 * lock held.
 */
#include "djl_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- cache helpers (caller holds ctx->lock) ---- */

static void entry_clear(struct djl_meta_entry *e)
{
    if (!e) return;
    djl_waveform_free(&e->wave);
    djl_beat_grid_free(&e->grid);
    djl_cue_list_free(&e->cues);
    djl_blob_free(&e->art);
    memset(e, 0, sizeof *e);
}

/* ---- worker ---- */

static void emit_note(djl_context *ctx, djl_event_kind kind, uint8_t player,
                      void (*fill)(djl_event *, struct djl_meta_entry *),
                      struct djl_meta_entry *e)
{
    djl_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind    = kind;
    ev.device  = player;
    ev.time_ms = djl_now_ms() - ctx->t0;
    if (fill) fill(&ev, e);
    djl_emit(ctx, &ev);
}

static void perform_fetch(djl_context *ctx, uint8_t player, uint8_t host,
                          djl_slot slot, djl_track_type type, uint32_t id)
{
    /* All blocking work happens here, with no lock held. */
    djl_db *db = NULL;
    if (djl_db_open(ctx, host, &db) != DJL_OK) {
        djl_log(ctx, DJL_LOG_WARN, "metadata: cannot open dbserver on player %u", host);
        return;
    }

    djl_track_info   meta;  bool has_meta = djl_db_track_metadata(db, slot, type, id, &meta) == DJL_OK;
    djl_waveform_blob wave; bool has_wave = djl_db_waveform(db, slot, type, id, DJL_WAVE_RGB, false, &wave) == DJL_OK;
    djl_beat_grid    grid;  bool has_grid = djl_db_beat_grid(db, slot, type, id, &grid) == DJL_OK;
    djl_cue_list     cues;  bool has_cues = djl_db_cue_list(db, slot, type, id, true, &cues) == DJL_OK;
    if (!has_cues) has_cues = djl_db_cue_list(db, slot, type, id, false, &cues) == DJL_OK;

    djl_blob art; bool has_art = false;
    if (has_meta && meta.artwork_id)
        has_art = djl_db_album_art(db, slot, type, meta.artwork_id, &art) == DJL_OK;

    /* Signature needs RGB detail + grid + names; only rekordbox tracks have the
     * .EXT detail, so only attempt it there. */
    uint8_t sig[20]; bool has_sig = false;
    if (type == DJL_TRACK_REKORDBOX && has_grid) {
        djl_waveform_blob det;
        if (djl_db_waveform(db, slot, type, id, DJL_WAVE_RGB, true, &det) == DJL_OK) {
            if (det.style == DJL_WAVE_RGB &&
                djl_track_signature(has_meta ? meta.title : "", has_meta ? meta.artist : "",
                                    has_meta ? meta.duration_s : 0, &det, &grid, sig) == DJL_OK)
                has_sig = true;
            djl_waveform_free(&det);
        }
    }

    djl_db_close(db);

    /* Publish into the cache and notify. */
    pthread_mutex_lock(&ctx->lock);
    if (player < 64 && ctx->meta_cache) {
        struct djl_meta_entry *e = &ctx->meta_cache[player];
        entry_clear(e);
        e->valid = true; e->host = host; e->slot = slot; e->type = type; e->id = id;
        if (has_meta) { e->has_meta = true; e->meta = meta; }
        if (has_wave) { e->has_wave = true; e->wave = wave; }
        if (has_grid) { e->has_grid = true; e->grid = grid; }
        if (has_cues) { e->has_cues = true; e->cues = cues; }
        if (has_art)  { e->has_art  = true; e->art  = art; }
        if (has_sig)  { e->has_sig  = true; memcpy(e->sig, sig, 20); }

        djl_log(ctx, DJL_LOG_INFO,
                "metadata p%u id=%u: %s%s%s%s%s", player, id,
                has_meta ? "meta " : "", has_wave ? "wave " : "",
                has_grid ? "grid " : "", has_cues ? "cues " : "", has_art ? "art " : "");

        djl_event ev; memset(&ev, 0, sizeof ev);
        ev.device = player; ev.time_ms = djl_now_ms() - ctx->t0;
        if (has_meta) { ev.kind = DJL_EV_TRACK_METADATA; ev.u.metadata.player = player;
                        ev.u.metadata.rekordbox_id = id; djl_emit(ctx, &ev); }
        if (has_wave) { memset(&ev,0,sizeof ev); ev.device=player; ev.time_ms=djl_now_ms()-ctx->t0;
                        ev.kind = DJL_EV_WAVEFORM; ev.u.waveform.player=player;
                        ev.u.waveform.style=(int)wave.style; ev.u.waveform.detail=false;
                        ev.u.waveform.segments=(uint32_t)djl_waveform_segment_count(&wave); djl_emit(ctx,&ev); }
        if (has_grid) { memset(&ev,0,sizeof ev); ev.device=player; ev.time_ms=djl_now_ms()-ctx->t0;
                        ev.kind = DJL_EV_BEAT_GRID; ev.u.beat_grid.player=player;
                        ev.u.beat_grid.entries=grid.count; djl_emit(ctx,&ev); }
        if (has_cues) { memset(&ev,0,sizeof ev); ev.device=player; ev.time_ms=djl_now_ms()-ctx->t0;
                        ev.kind = DJL_EV_CUE_LIST; ev.u.cue_list.player=player;
                        ev.u.cue_list.entries=cues.count; ev.u.cue_list.extended=cues.extended; djl_emit(ctx,&ev); }
        if (has_art)  { memset(&ev,0,sizeof ev); ev.device=player; ev.time_ms=djl_now_ms()-ctx->t0;
                        ev.kind = DJL_EV_ALBUM_ART; ev.u.album_art.player=player;
                        ev.u.album_art.artwork_id=has_meta?meta.artwork_id:0;
                        ev.u.album_art.length=art.length; djl_emit(ctx,&ev); }
        if (has_sig)  { memset(&ev,0,sizeof ev); ev.device=player; ev.time_ms=djl_now_ms()-ctx->t0;
                        ev.kind = DJL_EV_SIGNATURE; ev.u.signature.player=player;
                        memcpy(ev.u.signature.sha1, sig, 20); djl_emit(ctx,&ev); }
    } else {
        /* nobody to store it for; drop the freshly-fetched data */
        if (has_wave) djl_waveform_free(&wave);
        if (has_grid) djl_beat_grid_free(&grid);
        if (has_cues) djl_cue_list_free(&cues);
        if (has_art)  djl_blob_free(&art);
    }
    pthread_mutex_unlock(&ctx->lock);
    (void)emit_note;
}

static void *meta_thread_fn(void *arg)
{
    djl_context *ctx = arg;
    pthread_mutex_lock(&ctx->lock);
    while (ctx->meta_running) {
        if (ctx->meta_job_head == ctx->meta_job_tail) {
            pthread_cond_wait(&ctx->meta_cond, &ctx->lock);
            continue;
        }
        /* Dequeue one job. */
        size_t i = ctx->meta_job_head;
        uint8_t player = ctx->meta_jobs[i].player;
        uint8_t host   = ctx->meta_jobs[i].host;
        djl_slot slot  = ctx->meta_jobs[i].slot;
        djl_track_type type = ctx->meta_jobs[i].type;
        uint32_t id    = ctx->meta_jobs[i].id;
        ctx->meta_job_head = (ctx->meta_job_head + 1) % 16;
        pthread_mutex_unlock(&ctx->lock);

        perform_fetch(ctx, player, host, slot, type, id);

        pthread_mutex_lock(&ctx->lock);
    }
    pthread_mutex_unlock(&ctx->lock);
    return NULL;
}

/* ---- lifecycle ---- */

djl_err djl_meta_start(djl_context *ctx)
{
    if (ctx->meta_started) return DJL_OK;
    ctx->meta_cache = calloc(64, sizeof *ctx->meta_cache);
    if (!ctx->meta_cache) return DJL_ERR_NOMEM;
    pthread_cond_init(&ctx->meta_cond, NULL);
    ctx->meta_running = true;
    ctx->meta_job_head = ctx->meta_job_tail = 0;
    if (pthread_create(&ctx->meta_thread, NULL, meta_thread_fn, ctx) != 0) {
        ctx->meta_running = false;
        free(ctx->meta_cache); ctx->meta_cache = NULL;
        pthread_cond_destroy(&ctx->meta_cond);
        return DJL_ERR_IO;
    }
    ctx->meta_started = true;
    return DJL_OK;
}

void djl_meta_stop(djl_context *ctx)
{
    if (!ctx->meta_started) return;
    pthread_mutex_lock(&ctx->lock);
    ctx->meta_running = false;
    pthread_cond_signal(&ctx->meta_cond);
    pthread_mutex_unlock(&ctx->lock);
    pthread_join(ctx->meta_thread, NULL);
    ctx->meta_started = false;

    if (ctx->meta_cache) {
        for (int i = 0; i < 64; i++) entry_clear(&ctx->meta_cache[i]);
        free(ctx->meta_cache);
        ctx->meta_cache = NULL;
    }
    pthread_cond_destroy(&ctx->meta_cond);
}

/* Called from the I/O thread with ctx->lock held. */
void djl_meta_enqueue(djl_context *ctx, uint8_t player, uint8_t host,
                      djl_slot slot, djl_track_type type, uint32_t id)
{
    if (!ctx->meta_started || player == 0 || player >= 64) return;
    if (host == 0 || id == 0 || type == DJL_TRACK_NONE) return;

    /* Skip if we already have exactly this track cached for this player. */
    struct djl_meta_entry *e = &ctx->meta_cache[player];
    if (e->valid && e->id == id && e->slot == slot && e->type == type && e->host == host)
        return;

    /* Coalesce: if a queued job already targets this player, overwrite it. */
    for (size_t i = ctx->meta_job_head; i != ctx->meta_job_tail; i = (i + 1) % 16) {
        if (ctx->meta_jobs[i].player == player) {
            ctx->meta_jobs[i].host = host; ctx->meta_jobs[i].slot = slot;
            ctx->meta_jobs[i].type = type; ctx->meta_jobs[i].id = id;
            pthread_cond_signal(&ctx->meta_cond);
            return;
        }
    }
    size_t next = (ctx->meta_job_tail + 1) % 16;
    if (next == ctx->meta_job_head) return;   /* queue full, drop */
    ctx->meta_jobs[ctx->meta_job_tail].player = player;
    ctx->meta_jobs[ctx->meta_job_tail].host   = host;
    ctx->meta_jobs[ctx->meta_job_tail].slot   = slot;
    ctx->meta_jobs[ctx->meta_job_tail].type   = type;
    ctx->meta_jobs[ctx->meta_job_tail].id     = id;
    ctx->meta_job_tail = next;
    pthread_cond_signal(&ctx->meta_cond);
}

/* ---- getters (copy out; caller frees variable-size results) ---- */

djl_err djl_get_metadata(djl_context *ctx, uint8_t player, djl_track_info *out)
{
    if (!ctx || !out || player >= 64) return DJL_ERR_INVAL;
    djl_err r = DJL_ERR_NOT_FOUND;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->meta_cache && ctx->meta_cache[player].has_meta) {
        *out = ctx->meta_cache[player].meta; r = DJL_OK;
    }
    pthread_mutex_unlock(&ctx->lock);
    return r;
}

djl_err djl_get_waveform(djl_context *ctx, uint8_t player, djl_waveform_blob *out)
{
    if (!ctx || !out || player >= 64) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    djl_err r = DJL_ERR_NOT_FOUND;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->meta_cache && ctx->meta_cache[player].has_wave) {
        djl_waveform_blob *w = &ctx->meta_cache[player].wave;
        out->data = malloc(w->length);
        if (out->data) {
            memcpy(out->data, w->data, w->length);
            out->length = w->length; out->style = w->style; out->detail = w->detail;
            r = DJL_OK;
        } else r = DJL_ERR_NOMEM;
    }
    pthread_mutex_unlock(&ctx->lock);
    return r;
}

djl_err djl_get_beat_grid(djl_context *ctx, uint8_t player, djl_beat_grid *out)
{
    if (!ctx || !out || player >= 64) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    djl_err r = DJL_ERR_NOT_FOUND;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->meta_cache && ctx->meta_cache[player].has_grid) {
        djl_beat_grid *g = &ctx->meta_cache[player].grid;
        out->entries = malloc(g->count * sizeof *g->entries);
        if (out->entries || g->count == 0) {
            if (g->count) memcpy(out->entries, g->entries, g->count * sizeof *g->entries);
            out->count = g->count; r = DJL_OK;
        } else r = DJL_ERR_NOMEM;
    }
    pthread_mutex_unlock(&ctx->lock);
    return r;
}

djl_err djl_get_cue_list(djl_context *ctx, uint8_t player, djl_cue_list *out)
{
    if (!ctx || !out || player >= 64) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    djl_err r = DJL_ERR_NOT_FOUND;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->meta_cache && ctx->meta_cache[player].has_cues) {
        djl_cue_list *c = &ctx->meta_cache[player].cues;
        out->entries = malloc(c->count * sizeof *c->entries);
        if (out->entries || c->count == 0) {
            if (c->count) memcpy(out->entries, c->entries, c->count * sizeof *c->entries);
            out->count = c->count; out->extended = c->extended; r = DJL_OK;
        } else r = DJL_ERR_NOMEM;
    }
    pthread_mutex_unlock(&ctx->lock);
    return r;
}

djl_err djl_get_album_art(djl_context *ctx, uint8_t player, djl_blob *out)
{
    if (!ctx || !out || player >= 64) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    djl_err r = DJL_ERR_NOT_FOUND;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->meta_cache && ctx->meta_cache[player].has_art) {
        djl_blob *a = &ctx->meta_cache[player].art;
        out->data = malloc(a->length);
        if (out->data) { memcpy(out->data, a->data, a->length); out->length = a->length; r = DJL_OK; }
        else r = DJL_ERR_NOMEM;
    }
    pthread_mutex_unlock(&ctx->lock);
    return r;
}

djl_err djl_get_signature(djl_context *ctx, uint8_t player, uint8_t out_sha1[20])
{
    if (!ctx || !out_sha1 || player >= 64) return DJL_ERR_INVAL;
    djl_err r = DJL_ERR_NOT_FOUND;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->meta_cache && ctx->meta_cache[player].has_sig) {
        memcpy(out_sha1, ctx->meta_cache[player].sig, 20); r = DJL_OK;
    }
    pthread_mutex_unlock(&ctx->lock);
    return r;
}
