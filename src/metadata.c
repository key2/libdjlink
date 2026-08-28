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
    djl_song_structure_free(&e->ss);
    memset(e, 0, sizeof *e);
}

/* The position tracker borrows the cached grid rather than copying it, so every
 * change to the cache entry has to be reflected here while the lock is held. */
void djl_pos_attach_grid(djl_context *ctx, uint8_t player)
{
    if (!ctx || player == 0 || player >= 64) return;
    djl_pos_state *ps = &ctx->positions[player];
    const djl_beat_grid *g = NULL;
    if (ctx->meta_cache && ctx->meta_cache[player].valid &&
        ctx->meta_cache[player].has_grid && ctx->meta_cache[player].grid.count)
        g = &ctx->meta_cache[player].grid;
    if (ps->grid != g) {
        ps->grid = g;
        /* A new track invalidates any position we extrapolated for the old one;
         * the next status packet re-anchors us against the new grid. */
        ps->grid_beat_known = false;
    }
}

void djl_pos_detach_grid(djl_context *ctx, uint8_t player)
{
    if (!ctx || player == 0 || player >= 64) return;
    djl_pos_state *ps = &ctx->positions[player];
    ps->grid            = NULL;
    ps->grid_beat_known = false;
    /* Any grid-derived length belonged to the old track. */
    if (!ps->from_precise) ps->track_length_ms = -1;
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

/* One fetch attempt's results, before they are published into the cache. */
typedef struct {
    bool has_meta; djl_track_info     meta;
    bool has_wave; djl_waveform_blob  wave;
    bool has_grid; djl_beat_grid      grid;
    bool has_cues; djl_cue_list       cues;
    bool has_art;  djl_blob           art;
    bool has_ss;   djl_song_structure ss;
    bool has_sig;  uint8_t            sig[20];
    /* "We asked and got an answer", as distinct from "the answer was non-empty".
     * A track with no saved cues is complete, not incomplete: without this the
     * early-out could never fire for such a track and every load paid for a
     * second provider it did not need. */
    bool cues_resolved;
} fetch_result;

static void result_free(fetch_result *f)
{
    if (f->has_wave) djl_waveform_free(&f->wave);
    if (f->has_grid) djl_beat_grid_free(&f->grid);
    if (f->has_cues) djl_cue_list_free(&f->cues);
    if (f->has_art)  djl_blob_free(&f->art);
    if (f->has_ss)   djl_song_structure_free(&f->ss);
    memset(f, 0, sizeof *f);
}

/* Enough to be worth caching and to stop trying further providers. */
static bool result_usable(const fetch_result *f)
{
    return f->has_meta || f->has_grid || f->has_cues || f->has_wave || f->has_ss;
}

static void sign_if_possible(fetch_result *f, const djl_waveform_blob *rgb_detail)
{
    if (f->has_sig || !f->has_grid || !rgb_detail || !rgb_detail->data) return;
    if (rgb_detail->style != DJL_WAVE_RGB || !rgb_detail->detail) return;
    if (djl_track_signature(f->has_meta ? f->meta.title : "",
                            f->has_meta ? f->meta.artist : "",
                            f->has_meta ? f->meta.duration_s : 0,
                            rgb_detail, &f->grid, f->sig) == DJL_OK)
        f->has_sig = true;
}

/* ---- provider: NFS (export.pdb + ANLZ off the player's own media) ---- */

/* Mount cache. Only the metadata worker thread touches ctx->nfs_cache, so no
 * lock is needed here; djl_nfs is not thread-safe and must not be shared. */

static uint32_t media_gen_of(djl_context *ctx, uint8_t host)
{
    pthread_mutex_lock(&ctx->lock);
    uint32_t g = (host < 64) ? ctx->media_gen[host] : 0;
    pthread_mutex_unlock(&ctx->lock);
    return g;
}

static void nfs_cache_drop(djl_context *ctx, size_t i)
{
    if (ctx->nfs_cache[i].h) djl_nfs_close(ctx->nfs_cache[i].h);
    memset(&ctx->nfs_cache[i], 0, sizeof ctx->nfs_cache[i]);
}

/* Close every cached mount; called when the worker stops. */
void djl_nfs_cache_clear(djl_context *ctx)
{
    for (size_t i = 0; i < sizeof ctx->nfs_cache / sizeof ctx->nfs_cache[0]; i++)
        nfs_cache_drop(ctx, i);
}

/* An open, still-valid mount for this player and slot, opening one if needed.
 * Re-using it is what keeps a track load from re-downloading export.pdb. */
static djl_nfs *nfs_cache_get(djl_context *ctx, uint8_t host, djl_slot slot)
{
    const size_t n_slots = sizeof ctx->nfs_cache / sizeof ctx->nfs_cache[0];
    uint32_t gen = media_gen_of(ctx, host);

    size_t victim = 0;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < n_slots; i++) {
        if (ctx->nfs_cache[i].h && ctx->nfs_cache[i].host == host &&
            ctx->nfs_cache[i].slot == (uint8_t)slot) {
            if (ctx->nfs_cache[i].gen != gen) {
                /* Media was swapped or the device vanished: the mount and the
                 * export.pdb behind it are stale. */
                djl_log(ctx, DJL_LOG_DEBUG,
                        "metadata: dropping stale NFS mount for player %u", host);
                nfs_cache_drop(ctx, i);
                break;
            }
            ctx->nfs_cache[i].last_use_ms = djl_now_ms();
            return ctx->nfs_cache[i].h;
        }
        uint64_t age = ctx->nfs_cache[i].h ? ctx->nfs_cache[i].last_use_ms : 0;
        if (age < oldest) { oldest = age; victim = i; }
    }

    djl_nfs *h = NULL;
    if (djl_nfs_open(ctx, host, slot, &h) != DJL_OK) return NULL;

    nfs_cache_drop(ctx, victim);
    ctx->nfs_cache[victim].h           = h;
    ctx->nfs_cache[victim].host        = host;
    ctx->nfs_cache[victim].slot        = (uint8_t)slot;
    ctx->nfs_cache[victim].gen         = gen;
    ctx->nfs_cache[victim].last_use_ms = djl_now_ms();
    return h;
}

static bool fetch_via_nfs(djl_context *ctx, uint8_t host, djl_slot slot,
                          djl_track_type type, uint32_t id, fetch_result *out)
{
    if (!djl_nfs_supported()) return false;
    if (!ctx->cfg.allow_media_access) return false;
    /* Only media that physically exists on disk. CD audio, streaming and
     * rekordbox-collection tracks have no export.pdb to read. */
    if (type != DJL_TRACK_REKORDBOX) return false;
    if (slot != DJL_SLOT_SD && slot != DJL_SLOT_USB) return false;

    djl_nfs *n = nfs_cache_get(ctx, host, slot);
    if (!n) {
        djl_log(ctx, DJL_LOG_DEBUG, "metadata: no NFS mount for player %u", host);
        return false;
    }
    /* Bound how long one job may spend in the NFS client, so a wedged or
     * rebooting player cannot pin the single worker thread. */
    djl_nfs_set_deadline(n, djl_now_ms() + DJL_NFS_JOB_BUDGET_MS);

    djl_nfs_track t;
    djl_err e = djl_nfs_fetch_track(n, id, &t);
    if (e != DJL_OK) {
        djl_log(ctx, DJL_LOG_DEBUG, "metadata: NFS track %u failed: %s",
                id, djl_strerror(e));
        /* A timeout or I/O error may have left the mount unusable; drop it so
         * the next attempt starts clean rather than inheriting the damage. */
        if (e == DJL_ERR_TIMEOUT || e == DJL_ERR_IO) {
            for (size_t i = 0; i < sizeof ctx->nfs_cache / sizeof ctx->nfs_cache[0]; i++)
                if (ctx->nfs_cache[i].h == n) { nfs_cache_drop(ctx, i); break; }
        }
        return false;
    }

    /* Move ownership of the parsed analysis into the result, filling only the
     * gaps. Every one of these owns heap memory, so overwriting a field an
     * earlier provider already populated would leak it -- which is exactly what
     * happened with the provider order {DBSERVER, NFS}, an order the public
     * API accepts. */
    if (!out->has_meta && t.has_meta) { out->has_meta = true; out->meta = t.meta; }
    if (!out->has_grid && t.anlz.has_grid) {
        out->has_grid = true; out->grid = t.anlz.grid;
        memset(&t.anlz.grid, 0, sizeof t.anlz.grid);
    }
    if (!out->has_cues && t.anlz.has_cues) {
        out->has_cues = true; out->cues = t.anlz.cues;
        memset(&t.anlz.cues, 0, sizeof t.anlz.cues);
    }
    /* The .DAT/.EXT carry the cue lists, so parsing them settles the question
     * even when the track simply has none. */
    if (t.anlz.has_grid || t.anlz.has_cues) out->cues_resolved = true;
    if (!out->has_ss && t.anlz.has_ss) {
        out->has_ss = true; out->ss = t.anlz.ss;
        memset(&t.anlz.ss, 0, sizeof t.anlz.ss);
    }
    if (!out->has_wave && t.anlz.has_preview) {
        out->has_wave = true; out->wave = t.anlz.preview;
        memset(&t.anlz.preview, 0, sizeof t.anlz.preview);
    }

    sign_if_possible(out, t.anlz.has_rgb_detail ? &t.anlz.rgb_detail : NULL);

    if (!out->has_art && out->has_meta && out->meta.artwork_id) {
        djl_blob art;
        if (djl_nfs_read_artwork(n, out->meta.artwork_id, &art) == DJL_OK) {
            out->has_art = true; out->art = art;
        }
    }

    djl_nfs_track_free(&t);
    /* The mount stays open in the cache for the next track load. */
    return result_usable(out);
}

/* ---- provider: dbserver (TCP; the only source for CD and streaming) ---- */

static bool fetch_via_dbserver(djl_context *ctx, uint8_t host, djl_slot slot,
                               djl_track_type type, uint32_t id, fetch_result *out)
{
    int own = djl_own_number(ctx);
    if (own < 1 || own > 6 || (uint8_t)own == host) return false;

    djl_db *db = NULL;
    if (djl_db_open(ctx, host, &db) != DJL_OK) {
        djl_log(ctx, DJL_LOG_DEBUG, "metadata: cannot open dbserver on player %u", host);
        return false;
    }

    if (!out->has_meta)
        out->has_meta = djl_db_track_metadata(db, slot, type, id, &out->meta) == DJL_OK;
    if (!out->has_wave)
        out->has_wave = djl_db_waveform(db, slot, type, id, DJL_WAVE_RGB, false, &out->wave) == DJL_OK;
    if (!out->has_grid)
        out->has_grid = djl_db_beat_grid(db, slot, type, id, &out->grid) == DJL_OK;
    if (!out->has_cues) {
        out->has_cues = djl_db_cue_list(db, slot, type, id, true, &out->cues) == DJL_OK;
        if (!out->has_cues)
            out->has_cues = djl_db_cue_list(db, slot, type, id, false, &out->cues) == DJL_OK;
        if (out->has_cues) out->cues_resolved = true;
    }
    if (!out->has_art && out->has_meta && out->meta.artwork_id)
        out->has_art = djl_db_album_art(db, slot, type, out->meta.artwork_id, &out->art) == DJL_OK;
    if (!out->has_ss && type == DJL_TRACK_REKORDBOX)
        out->has_ss = djl_db_song_structure(db, slot, type, id, &out->ss) == DJL_OK;

    /* Signature needs RGB detail + grid + names; only rekordbox tracks have the
     * .EXT detail, so only attempt it there. */
    if (!out->has_sig && type == DJL_TRACK_REKORDBOX && out->has_grid) {
        djl_waveform_blob det;
        if (djl_db_waveform(db, slot, type, id, DJL_WAVE_RGB, true, &det) == DJL_OK) {
            sign_if_possible(out, &det);
            djl_waveform_free(&det);
        }
    }

    djl_db_close(db);
    return result_usable(out);
}

static void perform_fetch(djl_context *ctx, uint8_t player, uint8_t host,
                          djl_slot slot, djl_track_type type, uint32_t id)
{
    /* All blocking work happens here, with no lock held. Providers are tried in
     * the configured order and may complement each other: NFS yields the whole
     * ANLZ set including PSSI, dbserver covers media with no files on disk. */
    djl_provider_kind order[DJL_MAX_PROVIDERS];
    size_t norder;
    pthread_mutex_lock(&ctx->lock);
    norder = ctx->provider_count;
    memcpy(order, ctx->providers, sizeof order);
    pthread_mutex_unlock(&ctx->lock);

    fetch_result res;
    memset(&res, 0, sizeof res);
    bool via_nfs = false, via_db = false;

    for (size_t i = 0; i < norder; i++) {
        bool ok = false;
        switch (order[i]) {
        case DJL_PROVIDER_NFS:
            ok = fetch_via_nfs(ctx, host, slot, type, id, &res);
            via_nfs = via_nfs || ok;
            break;
        case DJL_PROVIDER_DBSERVER:
            ok = fetch_via_dbserver(ctx, host, slot, type, id, &res);
            via_db = via_db || ok;
            break;
        default:
            break;
        }
        /* Stop as soon as we have a complete-enough picture; otherwise let the
         * next provider fill the gaps. */
        if (ok && res.has_meta && res.has_grid && res.cues_resolved) break;
    }
    const char *source = via_nfs ? (via_db ? "nfs+dbserver" : "nfs")
                                 : (via_db ? "dbserver" : "none");

    if (!result_usable(&res)) {
        djl_log(ctx, DJL_LOG_WARN, "metadata: no provider could serve player %u track %u",
                player, id);
        result_free(&res);
        return;
    }

    djl_track_info    meta = res.meta;
    djl_waveform_blob wave = res.wave;
    djl_beat_grid     grid = res.grid;
    djl_cue_list      cues = res.cues;
    djl_blob          art  = res.art;
    djl_song_structure ss  = res.ss;
    bool has_meta = res.has_meta, has_wave = res.has_wave, has_grid = res.has_grid;
    bool has_cues = res.has_cues, has_art = res.has_art, has_ss = res.has_ss;
    bool has_sig  = res.has_sig;
    uint8_t sig[20];
    memcpy(sig, res.sig, 20);
    djl_log(ctx, DJL_LOG_DEBUG, "metadata: player %u track %u served by %s",
            player, id, source);

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
        if (has_ss)   { e->has_ss   = true; e->ss   = ss; }

        /* Hand the fresh grid to the position tracker: this is what gives
         * pre-CDJ-3000 players an absolute playhead. */
        djl_pos_attach_grid(ctx, player);

        djl_log(ctx, DJL_LOG_INFO,
                "metadata p%u id=%u: %s%s%s%s%s%s", player, id,
                has_meta ? "meta " : "", has_wave ? "wave " : "",
                has_grid ? "grid " : "", has_cues ? "cues " : "", has_art ? "art " : "",
                has_ss ? "phrases " : "");

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
        if (has_ss)   { memset(&ev,0,sizeof ev); ev.device=player; ev.time_ms=djl_now_ms()-ctx->t0;
                        ev.kind = DJL_EV_SONG_STRUCTURE; ev.u.song_structure.player=player;
                        ev.u.song_structure.mood=(int)ss.mood; ev.u.song_structure.bank=ss.bank;
                        ev.u.song_structure.phrases=ss.count; djl_emit(ctx,&ev); }
    } else {
        /* nobody to store it for; drop the freshly-fetched data */
        if (has_wave) djl_waveform_free(&wave);
        if (has_grid) djl_beat_grid_free(&grid);
        if (has_cues) djl_cue_list_free(&cues);
        if (has_art)  djl_blob_free(&art);
        if (has_ss)   djl_song_structure_free(&ss);
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

/* ---- provider order ---- */

djl_err djl_metadata_set_provider_order(djl_context *ctx,
                                        const djl_provider_kind *order, size_t n)
{
    if (!ctx || !order || n == 0 || n > DJL_MAX_PROVIDERS) return DJL_ERR_INVAL;
    for (size_t i = 0; i < n; i++)
        if (order[i] != DJL_PROVIDER_DBSERVER && order[i] != DJL_PROVIDER_NFS)
            return DJL_ERR_INVAL;
    pthread_mutex_lock(&ctx->lock);
    memset(ctx->providers, 0, sizeof ctx->providers);
    memcpy(ctx->providers, order, n * sizeof *order);
    ctx->provider_count = n;
    pthread_mutex_unlock(&ctx->lock);
    return DJL_OK;
}

size_t djl_metadata_get_provider_order(djl_context *ctx, djl_provider_kind *out, size_t max)
{
    if (!ctx || !out) return 0;
    pthread_mutex_lock(&ctx->lock);
    size_t n = ctx->provider_count < max ? ctx->provider_count : max;
    for (size_t i = 0; i < n; i++) out[i] = ctx->providers[i];
    pthread_mutex_unlock(&ctx->lock);
    return n;
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

    /* The worker owns the NFS mounts, so they can only be released now that it
     * has exited. Each UMNTs the player before closing. */
    djl_nfs_cache_clear(ctx);

    /* The position trackers borrow grids out of this cache, so drop those
     * references before the storage goes away. */
    pthread_mutex_lock(&ctx->lock);
    for (int i = 0; i < 64; i++) {
        ctx->positions[i].grid = NULL;
        ctx->positions[i].grid_beat_known = false;
    }
    if (ctx->meta_cache) {
        for (int i = 0; i < 64; i++) entry_clear(&ctx->meta_cache[i]);
        free(ctx->meta_cache);
        ctx->meta_cache = NULL;
    }
    pthread_mutex_unlock(&ctx->lock);
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

djl_err djl_get_song_structure(djl_context *ctx, uint8_t player, djl_song_structure *out)
{
    if (!ctx || !out || player >= 64) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    djl_err r = DJL_ERR_NOT_FOUND;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->meta_cache && ctx->meta_cache[player].has_ss) {
        djl_song_structure *s = &ctx->meta_cache[player].ss;
        out->mood = s->mood; out->bank = s->bank; out->end_beat = s->end_beat;
        memcpy(out->sha1, s->sha1, 20);
        r = DJL_OK;
        if (s->count) {
            out->phrases = malloc(s->count * sizeof *s->phrases);
            if (out->phrases) { memcpy(out->phrases, s->phrases, s->count * sizeof *s->phrases);
                                out->count = s->count; }
            else r = DJL_ERR_NOMEM;
        }
        if (r == DJL_OK && s->raw && s->raw_len) {
            out->raw = malloc(s->raw_len);
            if (out->raw) { memcpy(out->raw, s->raw, s->raw_len); out->raw_len = s->raw_len; }
        }
    }
    pthread_mutex_unlock(&ctx->lock);
    return r;
}
