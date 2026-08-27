/* libdjlink context: single-threaded event loop driving all protocol engines.
 *
 * Threading contract: the I/O thread owns all protocol state. Public query
 * and control functions take ctx->lock briefly. Events are copied into a
 * bounded ring that djl_poll() drains.
 */
#include "djl_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <math.h>

/* Timing constants (see ARCHITECTURE.md section 5). */
#define WATCH_PERIOD_MS   4000
#define CLAIM_GAP_MS       300
#define KEEPALIVE_MS      1500
#define STATUS_MS          200
#define EXPIRY_TICK_MS    1000
#define DEVICE_EXPIRY_MS 10000

/* ---------------- logging ---------------- */

void djl_log(djl_context *ctx, djl_log_level lvl, const char *fmt, ...)
{
    if (!ctx || !ctx->cfg.log || lvl > ctx->cfg.log_level) return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    ctx->cfg.log(lvl, msg, ctx->cfg.log_ud);
}

/* ---------------- event queue (caller must hold lock) ---------------- */

static bool ev_coalescible(djl_event_kind k)
{
    return k == DJL_EV_CDJ_STATUS || k == DJL_EV_PRECISE_POSITION ||
           k == DJL_EV_MIXER_STATUS || k == DJL_EV_POSITION;
}

/* Emit a coalesced position event for a player from its tracked state. */
static void emit_position(djl_context *ctx, uint8_t player, uint64_t now)
{
    if (player == 0 || player >= 64) return;
    djl_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind    = DJL_EV_POSITION;
    ev.device  = player;
    ev.time_ms = now - ctx->t0;
    djl_pos_interpolate(&ctx->positions[player], player, now, &ev.u.position);
    if (ev.u.position.valid) djl_emit(ctx, &ev);
}

void djl_emit(djl_context *ctx, const djl_event *ev)
{
    size_t next = (ctx->ev_tail + 1) % DJL_EVQ_SIZE;
    if (next == ctx->ev_head) {
        /* Full. Try to replace an existing coalescible event of the same
         * kind and device rather than dropping outright. */
        if (ev_coalescible(ev->kind)) {
            for (size_t i = ctx->ev_head; i != ctx->ev_tail; i = (i + 1) % DJL_EVQ_SIZE) {
                if (ctx->evq[i].kind == ev->kind && ctx->evq[i].device == ev->device) {
                    ctx->evq[i] = *ev;
                    return;
                }
            }
        }
        ctx->ev_dropped++;
        return;
    }
    ctx->evq[ctx->ev_tail] = *ev;
    ctx->ev_tail = next;
    pthread_cond_signal(&ctx->ev_cond);
}

static void emit_simple(djl_context *ctx, djl_event_kind k, uint8_t device)
{
    djl_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind    = k;
    ev.device  = device;
    ev.time_ms = djl_now_ms() - ctx->t0;
    djl_emit(ctx, &ev);
}

/* ---------------- roster ---------------- */

static djl_slot_entry *roster_find(djl_context *ctx, uint8_t number)
{
    for (size_t i = 0; i < DJL_MAX_DEVICES; i++)
        if (ctx->devices[i].used && ctx->devices[i].info.number == number)
            return &ctx->devices[i];
    return NULL;
}

static djl_slot_entry *roster_alloc(djl_context *ctx)
{
    for (size_t i = 0; i < DJL_MAX_DEVICES; i++)
        if (!ctx->devices[i].used) return &ctx->devices[i];
    return NULL;
}

static size_t roster_count(djl_context *ctx)
{
    size_t n = 0;
    for (size_t i = 0; i < DJL_MAX_DEVICES; i++)
        if (ctx->devices[i].used) n++;
    return n;
}

/* Returns true if this is a device we are not already tracking. */
static void roster_saw_keep_alive(djl_context *ctx, const djl_keep_alive *ka,
                                  const uint8_t src_ip[4], uint64_t now)
{
    if (ka->number == 0) return;
    /* Ignore our own keep-alives echoed back to us. */
    if (ctx->state == DJL_ST_ONLINE && ka->number == ctx->id.number &&
        memcmp(ka->mac, ctx->id.mac, 6) == 0)
        return;

    djl_slot_entry *e = roster_find(ctx, ka->number);
    bool is_new = false;
    if (!e) {
        e = roster_alloc(ctx);
        if (!e) return;
        memset(e, 0, sizeof *e);
        e->used = true;
        e->info.first_seen_ms = now - ctx->t0;
        is_new = true;
    }

    e->info.number        = ka->number;
    snprintf(e->info.name, sizeof e->info.name, "%s", ka->name);
    memcpy(e->info.ip,  src_ip[0] ? src_ip : ka->ip, 4);
    memcpy(e->info.mac, ka->mac, 6);
    e->info.device_type   = ka->device_type;
    e->info.model_code    = ka->model_code;
    e->info.proto_version = ka->proto_version;
    e->info.peer_count    = ka->peer_count;
    e->info.last_seen_ms  = now - ctx->t0;
    /* Mixers use device number 0x21 and/or device type 2/3. */
    e->info.is_mixer = (ka->device_type == DJL_DEVTYPE_MIXER ||
                        ka->device_type == DJL_DEVTYPE_MIXER_MODERN ||
                        ka->number == 0x21);
    e->info.is_cdj   = (ka->device_type == DJL_DEVTYPE_CDJ && ka->number <= 6);

    if (is_new) {
        djl_log(ctx, DJL_LOG_INFO,
                "device found: %u '%s' %u.%u.%u.%u type=0x%02x model=0x%02x proto=0x%02x",
                ka->number, ka->name, e->info.ip[0], e->info.ip[1],
                e->info.ip[2], e->info.ip[3],
                ka->device_type, ka->model_code, ka->proto_version);
        djl_event ev;
        memset(&ev, 0, sizeof ev);
        ev.kind    = DJL_EV_DEVICE_FOUND;
        ev.device  = ka->number;
        ev.time_ms = now - ctx->t0;
        ev.u.device_found = e->info;
        djl_emit(ctx, &ev);
    }
}

static void roster_expire(djl_context *ctx, uint64_t now)
{
    for (size_t i = 0; i < DJL_MAX_DEVICES; i++) {
        djl_slot_entry *e = &ctx->devices[i];
        if (!e->used) continue;
        if ((now - ctx->t0) - e->info.last_seen_ms > DEVICE_EXPIRY_MS) {
            djl_log(ctx, DJL_LOG_INFO, "device lost: %u '%s'",
                    e->info.number, e->info.name);
            djl_event ev;
            memset(&ev, 0, sizeof ev);
            ev.kind    = DJL_EV_DEVICE_LOST;
            ev.device  = e->info.number;
            ev.time_ms = now - ctx->t0;
            ev.u.device_lost = e->info;
            djl_emit(ctx, &ev);
            if (ctx->tempo_master == e->info.number) ctx->tempo_master = 0;
            /* Forget where a device that has gone away was playing. */
            if (e->info.number < 64)
                memset(&ctx->positions[e->info.number], 0,
                       sizeof ctx->positions[e->info.number]);
            memset(e, 0, sizeof *e);
        }
    }
}

/* Lowest free device number in [lo,hi], or 0 if none. */
static uint8_t lowest_free(djl_context *ctx, uint8_t lo, uint8_t hi)
{
    for (uint8_t n = lo; n <= hi; n++)
        if (!roster_find(ctx, n)) return n;
    return 0;
}

/* ---------------- transmit helpers ---------------- */

static void bcast(djl_context *ctx, djl_sock *s, const uint8_t *buf, size_t len)
{
    if (ctx->cfg.observe_only || len == 0) return;
    djl_err e = djl_sock_send(s, ctx->iface.broadcast, DJL_PORT_ANNOUNCE, buf, len);
    if (e != DJL_OK)
        djl_log(ctx, DJL_LOG_WARN, "broadcast failed: %s", djl_strerror(e));
}

static void unicast(djl_context *ctx, djl_sock *s, const uint8_t ip[4],
                    uint16_t port, const uint8_t *buf, size_t len)
{
    if (ctx->cfg.observe_only || len == 0) return;
    djl_err e = djl_sock_send(s, ip, port, buf, len);
    if (e != DJL_OK)
        djl_log(ctx, DJL_LOG_WARN, "unicast failed: %s", djl_strerror(e));
}

/* ---------------- device number state machine ---------------- */

static void enter_state(djl_context *ctx, djl_state st, uint64_t now)
{
    ctx->state       = st;
    ctx->state_since = now;
    ctx->claim_step  = 0;
}

static void go_online(djl_context *ctx, uint64_t now)
{
    ctx->id.number = ctx->claiming;
    enter_state(ctx, DJL_ST_ONLINE, now);
    ctx->next_keepalive = now;
    ctx->next_status    = now;
    djl_log(ctx, DJL_LOG_INFO, "online as device number %u", ctx->id.number);

    djl_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind    = DJL_EV_OWN_NUMBER_ASSIGNED;
    ev.device  = ctx->id.number;
    ev.time_ms = now - ctx->t0;
    ev.u.own_number.number = ctx->id.number;
    djl_emit(ctx, &ev);
}

static uint8_t pick_number(djl_context *ctx)
{
    switch (ctx->cfg.number_policy) {
    case DJL_NUMBER_FIXED:
        return ctx->cfg.preferred_number;
    case DJL_NUMBER_AUTO:
        return 0;
    case DJL_NUMBER_LOWEST_FREE:
    default: {
        if (ctx->cfg.preferred_number &&
            !roster_find(ctx, ctx->cfg.preferred_number))
            return ctx->cfg.preferred_number;
        uint8_t n = lowest_free(ctx, 1, 4);
        if (!n) n = lowest_free(ctx, 5, 6);
        return n;
    }
    }
}

static void numbering_tick(djl_context *ctx, uint64_t now)
{
    uint8_t buf[DJL_MAX_PACKET];
    size_t  n;

    switch (ctx->state) {
    case DJL_ST_WATCHING:
        if (now - ctx->state_since >= WATCH_PERIOD_MS) {
            if (ctx->cfg.observe_only) {
                djl_log(ctx, DJL_LOG_INFO,
                        "observe-only: %zu device(s) seen, not claiming a number",
                        roster_count(ctx));
                enter_state(ctx, DJL_ST_ONLINE, now);
                return;
            }
            ctx->claiming = pick_number(ctx);
            if (ctx->claiming == 0 && ctx->cfg.number_policy != DJL_NUMBER_AUTO) {
                djl_log(ctx, DJL_LOG_ERROR,
                        "no free device number available in 1..6");
                emit_simple(ctx, DJL_EV_NUMBER_CONFLICT, 0);
                enter_state(ctx, DJL_ST_ONLINE, now);   /* degrade to observer */
                return;
            }
            if (ctx->claiming > 4)
                djl_log(ctx, DJL_LOG_WARN,
                        "device number %u is outside 1..4; dbserver metadata "
                        "queries will not work", ctx->claiming);
            djl_log(ctx, DJL_LOG_INFO, "claiming device number %u", ctx->claiming);
            enter_state(ctx, DJL_ST_ANNOUNCING, now);
        }
        return;

    case DJL_ST_ANNOUNCING:
        if (now - ctx->state_since >= (uint64_t)ctx->claim_step * CLAIM_GAP_MS) {
            if (ctx->claim_step >= 3) { enter_state(ctx, DJL_ST_CLAIM1, now); return; }
            n = djl_build_hello(buf, sizeof buf, &ctx->id);
            bcast(ctx, &ctx->sock_announce, buf, n);
            ctx->claim_step++;
        }
        return;

    case DJL_ST_CLAIM1:
        if (now - ctx->state_since >= (uint64_t)ctx->claim_step * CLAIM_GAP_MS) {
            if (ctx->claim_step >= 3) { enter_state(ctx, DJL_ST_CLAIM2, now); return; }
            n = djl_build_claim1(buf, sizeof buf, &ctx->id, (uint8_t)(ctx->claim_step + 1));
            bcast(ctx, &ctx->sock_announce, buf, n);
            ctx->claim_step++;
        }
        return;

    case DJL_ST_CLAIM2:
        if (now - ctx->state_since >= (uint64_t)ctx->claim_step * CLAIM_GAP_MS) {
            if (ctx->claim_step >= 3) { enter_state(ctx, DJL_ST_CLAIM3, now); return; }
            n = djl_build_claim2(buf, sizeof buf, &ctx->id, ctx->claiming,
                                 (uint8_t)(ctx->claim_step + 1),
                                 ctx->cfg.number_policy == DJL_NUMBER_AUTO);
            bcast(ctx, &ctx->sock_announce, buf, n);
            ctx->claim_step++;
        }
        return;

    case DJL_ST_CLAIM3:
        if (now - ctx->state_since >= (uint64_t)ctx->claim_step * CLAIM_GAP_MS) {
            if (ctx->claim_step >= 3) { go_online(ctx, now); return; }
            n = djl_build_claim3(buf, sizeof buf, &ctx->id, ctx->claiming,
                                 (uint8_t)(ctx->claim_step + 1));
            bcast(ctx, &ctx->sock_announce, buf, n);
            ctx->claim_step++;
        }
        return;

    default:
        return;
    }
}

/* ---------------- inbound packet handling ---------------- */

static void handle_announce(djl_context *ctx, const uint8_t *buf, size_t len,
                            const uint8_t src_ip[4], uint64_t now)
{
    djl_packet_kind k = djl_wire_classify(DJL_PORT_ANNOUNCE, buf, len);

    switch (k) {
    case DJL_PKT_KEEP_ALIVE: {
        djl_keep_alive ka;
        if (djl_decode_keep_alive(buf, len, &ka) == DJL_OK)
            roster_saw_keep_alive(ctx, &ka, src_ip, now);
        return;
    }
    case DJL_PKT_NUMBER_IN_USE: {
        int defended = djl_wire_u8(buf, len, 0x24);
        if (defended > 0 && ctx->state >= DJL_ST_ANNOUNCING &&
            ctx->state <= DJL_ST_CLAIM3 && (uint8_t)defended == ctx->claiming) {
            djl_log(ctx, DJL_LOG_WARN,
                    "device number %d is defended by another device", defended);
            djl_event ev;
            memset(&ev, 0, sizeof ev);
            ev.kind    = DJL_EV_NUMBER_CONFLICT;
            ev.device  = (uint8_t)defended;
            ev.time_ms = now - ctx->t0;
            ev.u.conflict.number = (uint8_t)defended;
            djl_emit(ctx, &ev);
            /* Pick another number and restart the claim sequence. */
            ctx->cfg.preferred_number = 0;
            uint8_t next = pick_number(ctx);
            if (next && next != ctx->claiming) {
                ctx->claiming = next;
                djl_log(ctx, DJL_LOG_INFO, "retrying with device number %u", next);
                enter_state(ctx, DJL_ST_CLAIM1, now);
            } else {
                enter_state(ctx, DJL_ST_ONLINE, now);
            }
        }
        return;
    }
    case DJL_PKT_NUMBER_WILL_ASSIGN:
        /* A mixer intends to assign us a number. Cross-check later against
         * the roster before accepting (XDJ-XZ hands out numbers already in
         * use when reached via its laptop port). */
        djl_log(ctx, DJL_LOG_INFO, "a mixer offered to assign our device number");
        return;

    case DJL_PKT_NUMBER_ASSIGN: {
        int assigned = djl_wire_u8(buf, len, 0x24);
        if (assigned > 0 && assigned <= 6) {
            if (roster_find(ctx, (uint8_t)assigned)) {
                djl_log(ctx, DJL_LOG_WARN,
                        "mixer assigned device number %d but it is already in "
                        "use; ignoring", assigned);
                return;
            }
            djl_log(ctx, DJL_LOG_INFO, "mixer assigned device number %d", assigned);
            ctx->claiming = (uint8_t)assigned;
            ctx->assigned_by_mixer = 1;
            uint8_t buf2[DJL_MAX_PACKET];
            size_t n = djl_build_claim3(buf2, sizeof buf2, &ctx->id, ctx->claiming, 1);
            bcast(ctx, &ctx->sock_announce, buf2, n);
            go_online(ctx, now);
        }
        return;
    }
    case DJL_PKT_NUMBER_FINISHED:
        if (ctx->state == DJL_ST_CLAIM3) {
            djl_log(ctx, DJL_LOG_INFO,
                    "another device confirmed our number assignment");
            go_online(ctx, now);
        }
        return;

    case DJL_PKT_NUMBER_STAGE2: {
        /* Someone is claiming a number. Defend ours if it matches. */
        int claim = djl_wire_u8(buf, len, 0x2e);
        if (ctx->state == DJL_ST_ONLINE && ctx->id.number &&
            claim == (int)ctx->id.number && !ctx->cfg.observe_only) {
            uint8_t out[DJL_MAX_PACKET];
            size_t n = djl_build_number_in_use(out, sizeof out, &ctx->id,
                                               ctx->id.number, ctx->iface.ip);
            unicast(ctx, &ctx->sock_announce, src_ip, DJL_PORT_ANNOUNCE, out, n);
            djl_log(ctx, DJL_LOG_INFO, "defended our device number %u",
                    ctx->id.number);
        }
        return;
    }
    default:
        return;
    }
}

static void note_master(djl_context *ctx, uint8_t number, bool is_master,
                        double bpm, uint64_t now)
{
    if (is_master && ctx->tempo_master != number) {
        uint8_t old = ctx->tempo_master;
        ctx->tempo_master = number;
        djl_event ev;
        memset(&ev, 0, sizeof ev);
        ev.kind    = DJL_EV_MASTER_CHANGED;
        ev.device  = number;
        ev.time_ms = now - ctx->t0;
        ev.u.master_changed.old_master = old;
        ev.u.master_changed.new_master = number;
        djl_emit(ctx, &ev);
    }
    if (is_master && bpm > 0.0 && fabs(bpm - ctx->master_tempo) > 0.005) {
        ctx->master_tempo = bpm;
        djl_event ev;
        memset(&ev, 0, sizeof ev);
        ev.kind    = DJL_EV_TEMPO_CHANGED;
        ev.device  = number;
        ev.time_ms = now - ctx->t0;
        ev.u.tempo_changed.bpm  = bpm;
        ev.u.tempo_changed.from = number;
        djl_emit(ctx, &ev);
    }
}

static void handle_status(djl_context *ctx, const uint8_t *buf, size_t len,
                          uint64_t now)
{
    djl_packet_kind k = djl_wire_classify(DJL_PORT_STATUS, buf, len);

    switch (k) {
    case DJL_PKT_CDJ_STATUS: {
        djl_cdj_status st;
        if (djl_decode_cdj_status(buf, len, &st) != DJL_OK) return;
        if (ctx->state == DJL_ST_ONLINE && st.number == ctx->id.number) return;

        djl_slot_entry *e = roster_find(ctx, st.number);
        uint32_t prev_id = 0;
        uint8_t  prev_slot = 0, prev_type = 0;
        bool had_status = false;
        if (e) {
            had_status = e->has_status;
            prev_id    = e->status.rekordbox_id;
            prev_slot  = (uint8_t)e->status.track_slot;
            prev_type  = (uint8_t)e->status.track_type;
            e->status     = st;
            e->has_status = true;
            e->info.last_seen_ms = now - ctx->t0;
        }

        if (st.number < 64) {
            djl_pos_apply_status(&ctx->positions[st.number], &st, now);
            emit_position(ctx, st.number, now);
        }

        djl_event ev;
        memset(&ev, 0, sizeof ev);
        ev.kind    = DJL_EV_CDJ_STATUS;
        ev.device  = st.number;
        ev.time_ms = now - ctx->t0;
        ev.u.cdj_status = st;
        djl_emit(ctx, &ev);

        /* Only report a load when we have a genuinely playable reference.
         * Players clear these fields when a track ends (observed on
         * CDJ-3000X firmware 1.31, where P1 goes to 0x11 and Dr/Sr/Tr/id all
         * drop to zero), so a transition to zero must not look like a load,
         * and a transition back must not re-fire for the same track. */
        bool ref_changed = st.rekordbox_id != prev_id ||
                           (uint8_t)st.track_slot != prev_slot ||
                           (uint8_t)st.track_type != prev_type;
        if (had_status && ref_changed && st.rekordbox_id != 0 &&
            st.track_type != DJL_TRACK_NONE && st.track_slot != DJL_SLOT_NONE) {
            djl_event tl;
            memset(&tl, 0, sizeof tl);
            tl.kind    = DJL_EV_TRACK_LOADED;
            tl.device  = st.number;
            tl.time_ms = now - ctx->t0;
            tl.u.track_loaded.player        = st.number;
            tl.u.track_loaded.source_player = st.track_device;
            tl.u.track_loaded.slot          = st.track_slot;
            tl.u.track_loaded.type          = st.track_type;
            tl.u.track_loaded.rekordbox_id  = st.rekordbox_id;
            djl_emit(ctx, &tl);

            /* Kick off an automatic metadata fetch from the hosting player.
             * No device-number requirement here: the NFS provider works at any
             * number (or none), and the dbserver provider checks the 1..6
             * constraint itself. We only refuse to query ourselves. */
            if (ctx->cfg.auto_metadata && !ctx->cfg.observe_only &&
                st.track_device != ctx->id.number)
                djl_meta_enqueue(ctx, st.number, st.track_device,
                                 st.track_slot, st.track_type, st.rekordbox_id);
        }
        note_master(ctx, st.number, st.master, st.effective_bpm, now);
        return;
    }
    case DJL_PKT_MIXER_STATUS: {
        djl_mixer_status ms;
        if (djl_decode_mixer_status(buf, len, &ms) != DJL_OK) return;
        djl_slot_entry *e = roster_find(ctx, ms.number);
        if (e) e->info.last_seen_ms = now - ctx->t0;
        djl_event ev;
        memset(&ev, 0, sizeof ev);
        ev.kind    = DJL_EV_MIXER_STATUS;
        ev.device  = ms.number;
        ev.time_ms = now - ctx->t0;
        ev.u.mixer_status = ms;
        djl_emit(ctx, &ev);
        note_master(ctx, ms.number, ms.master, ms.effective_bpm, now);
        return;
    }
    case DJL_PKT_MEDIA_RESPONSE: {
        djl_media_details md;
        if (djl_decode_media_details(buf, len, &md) != DJL_OK) return;
        djl_event ev;
        memset(&ev, 0, sizeof ev);
        ev.kind    = DJL_EV_MEDIA_DETAILS;
        ev.device  = md.host_device;
        ev.time_ms = now - ctx->t0;
        ev.u.media = md;
        djl_emit(ctx, &ev);
        return;
    }
    case DJL_PKT_RB_ANNOUNCE:
    case DJL_PKT_RB_KEEPALIVE:
    case DJL_PKT_RB_MIXER_NOTIFY:
    case DJL_PKT_RB_MIXER_REPLY:
    case DJL_PKT_RB_PLAYER_REPLY:
    case DJL_PKT_RB_CONFIG:
    case DJL_PKT_RB_PLAYER_NOTIFY:
    case DJL_PKT_RB_LIGHTING_HELLO: {
        /* rekordbox LINK control traffic. We do not act on it yet, but a
         * consumer watching a rekordbox-sourced network needs to see it. */
        djl_rb_link rb;
        if (djl_decode_rb_link(buf, len, &rb) != DJL_OK) return;
        djl_event ev;
        memset(&ev, 0, sizeof ev);
        ev.kind    = DJL_EV_REKORDBOX_LINK;
        ev.device  = rb.device;
        ev.time_ms = now - ctx->t0;
        ev.u.rb_link = rb;
        djl_emit(ctx, &ev);
        return;
    }
    case DJL_PKT_UNKNOWN: {
        djl_event ev;
        memset(&ev, 0, sizeof ev);
        ev.kind    = DJL_EV_UNKNOWN_PACKET;
        ev.time_ms = now - ctx->t0;
        ev.u.unknown.port      = DJL_PORT_STATUS;
        int kb = djl_wire_kind_byte(buf, len);
        ev.u.unknown.kind_byte = (uint8_t)(kb < 0 ? 0 : kb);
        ev.u.unknown.length    = (uint16_t)len;
        memcpy(ev.u.unknown.bytes, buf, len < 64 ? len : 64);
        djl_emit(ctx, &ev);
        return;
    }
    default:
        return;
    }
}

static void handle_beat(djl_context *ctx, const uint8_t *buf, size_t len,
                        uint64_t now)
{
    djl_packet_kind k = djl_wire_classify(DJL_PORT_BEAT, buf, len);

    switch (k) {
    case DJL_PKT_BEAT: {
        djl_beat b;
        if (djl_decode_beat(buf, len, &b) != DJL_OK) return;
        if (b.number < 64) djl_pos_apply_beat(&ctx->positions[b.number], &b, now);
        djl_event ev;
        memset(&ev, 0, sizeof ev);
        ev.kind    = DJL_EV_BEAT;
        ev.device  = b.number;
        ev.time_ms = now - ctx->t0;
        ev.u.beat  = b;
        djl_emit(ctx, &ev);
        return;
    }
    case DJL_PKT_PRECISE_POSITION: {
        djl_precise_position pp;
        if (djl_decode_precise_position(buf, len, &pp) != DJL_OK) return;
        if (pp.number < 64) {
            djl_pos_apply_precise(&ctx->positions[pp.number], &pp, now);
            emit_position(ctx, pp.number, now);
        }
        djl_event ev;
        memset(&ev, 0, sizeof ev);
        ev.kind     = DJL_EV_PRECISE_POSITION;
        ev.device   = pp.number;
        ev.time_ms  = now - ctx->t0;
        ev.u.precise = pp;
        djl_emit(ctx, &ev);
        return;
    }
    case DJL_PKT_CHANNELS_ON_AIR: {
        uint8_t mask = 0;
        for (int i = 0; i < 4; i++)
            if (djl_wire_u8(buf, len, 0x24 + (size_t)i) == 1) mask |= (uint8_t)(1u << i);
        if (len >= 0x35) {
            if (djl_wire_u8(buf, len, 0x2e) == 1) mask |= 0x10;
            if (djl_wire_u8(buf, len, 0x2f) == 1) mask |= 0x20;
        }
        if (mask != ctx->on_air_mask) {
            ctx->on_air_mask = mask;
            djl_event ev;
            memset(&ev, 0, sizeof ev);
            ev.kind    = DJL_EV_ON_AIR_CHANGED;
            ev.time_ms = now - ctx->t0;
            ev.u.on_air.channel_mask = mask;
            djl_emit(ctx, &ev);
        }
        /* Track our own on-air state if we hold a channel number. */
        if (ctx->id.number >= 1 && ctx->id.number <= 6)
            ctx->adv_on_air = (mask & (1u << (ctx->id.number - 1))) != 0;
        return;
    }
    case DJL_PKT_FADER_START: {
        djl_event ev;
        memset(&ev, 0, sizeof ev);
        ev.kind    = DJL_EV_FADER_START;
        ev.time_ms = now - ctx->t0;
        for (int i = 0; i < 4; i++) {
            int v = djl_wire_u8(buf, len, 0x24 + (size_t)i);
            ev.u.fader_start.commands[i] = (uint8_t)(v < 0 ? 2 : v);
        }
        djl_emit(ctx, &ev);
        return;
    }
    case DJL_PKT_UNKNOWN: {
        djl_event ev;
        memset(&ev, 0, sizeof ev);
        ev.kind    = DJL_EV_UNKNOWN_PACKET;
        ev.time_ms = now - ctx->t0;
        ev.u.unknown.port      = DJL_PORT_BEAT;
        int kb = djl_wire_kind_byte(buf, len);
        ev.u.unknown.kind_byte = (uint8_t)(kb < 0 ? 0 : kb);
        ev.u.unknown.length    = (uint16_t)len;
        memcpy(ev.u.unknown.bytes, buf, len < 64 ? len : 64);
        djl_emit(ctx, &ev);
        return;
    }
    default:
        return;
    }
}

static void handle_audio(djl_context *ctx, const uint8_t *buf, size_t len,
                         uint64_t now)
{
    if (djl_wire_classify(DJL_PORT_AUDIO, buf, len) != DJL_PKT_AUDIO_TIMING) return;
    bool ok;
    djl_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind    = DJL_EV_AUDIO_TIMING;
    ev.device  = (uint8_t)djl_wire_u8(buf, len, 0x21);
    ev.time_ms = now - ctx->t0;
    ev.u.audio_timing.counter  = (uint32_t)djl_wire_be(buf, len, 0x24, 4, &ok);
    ev.u.audio_timing.link_cue = (uint8_t)djl_wire_u8(buf, len, 0x28);
    ev.u.audio_timing.elected  = (uint8_t)djl_wire_u8(buf, len, 0x29);
    djl_emit(ctx, &ev);
}

/* ---------------- periodic transmit ---------------- */

static void send_keep_alive(djl_context *ctx)
{
    uint8_t buf[DJL_MAX_PACKET];
    ctx->id.peer_count = (uint8_t)(roster_count(ctx) + 1);
    size_t n = djl_build_keep_alive(buf, sizeof buf, &ctx->id);
    bcast(ctx, &ctx->sock_announce, buf, n);
}

static void send_status_to_all(djl_context *ctx)
{
    if (!ctx->cfg.send_status || ctx->id.number == 0) return;
    uint8_t buf[DJL_MAX_PACKET];
    size_t n = djl_build_status(buf, sizeof buf, &ctx->id,
                                ctx->adv_playing, ctx->adv_master,
                                ctx->adv_synced, ctx->adv_on_air,
                                ctx->adv_tempo, ctx->adv_beat, ctx->adv_bib,
                                ctx->sync_counter, ctx->next_master,
                                ++ctx->packet_counter);
    if (!n) return;
    for (size_t i = 0; i < DJL_MAX_DEVICES; i++) {
        if (!ctx->devices[i].used) continue;
        unicast(ctx, &ctx->sock_status, ctx->devices[i].info.ip,
                DJL_PORT_STATUS, buf, n);
    }
}

/* ---------------- main loop ---------------- */

static void *io_thread(void *arg)
{
    djl_context *ctx = (djl_context *)arg;
    uint8_t buf[DJL_MAX_PACKET];

    djl_sock *const socks[4] = {
        &ctx->sock_announce, &ctx->sock_beat, &ctx->sock_status, &ctx->sock_audio
    };

    while (ctx->running) {
        bool ready[4];
        int r = djl_sock_poll(socks, 4, 20, ready);
        uint64_t now = djl_now_ms();

        if (r > 0) {
            pthread_mutex_lock(&ctx->lock);
            for (int i = 0; i < 4; i++) {
                if (!ready[i]) continue;
                djl_sock *s = socks[i];
                for (;;) {
                    uint8_t src[4] = {0,0,0,0};
                    int n = djl_sock_recv(s, buf, sizeof buf, src);
                    if (n <= 0) break;
                    if (!djl_wire_has_magic(buf, (size_t)n)) continue;
                    if (ctx->raw_hook)
                        ctx->raw_hook(s->port,
                                      djl_wire_classify(s->port, buf, (size_t)n),
                                      buf, (size_t)n, ctx->raw_ud);
                    switch (s->port) {
                    case DJL_PORT_ANNOUNCE:
                        handle_announce(ctx, buf, (size_t)n, src, now); break;
                    case DJL_PORT_BEAT:
                        handle_beat(ctx, buf, (size_t)n, now); break;
                    case DJL_PORT_STATUS:
                        handle_status(ctx, buf, (size_t)n, now); break;
                    case DJL_PORT_AUDIO:
                        handle_audio(ctx, buf, (size_t)n, now); break;
                    default: break;
                    }
                }
            }
            pthread_mutex_unlock(&ctx->lock);
        }

        /* Timers. */
        pthread_mutex_lock(&ctx->lock);
        now = djl_now_ms();

        if (ctx->state != DJL_ST_ONLINE) {
            numbering_tick(ctx, now);
        } else {
            if (!ctx->cfg.observe_only && now >= ctx->next_keepalive) {
                send_keep_alive(ctx);
                ctx->next_keepalive = now + KEEPALIVE_MS;
            }
            if (!ctx->cfg.observe_only && now >= ctx->next_status) {
                send_status_to_all(ctx);
                ctx->next_status = now + STATUS_MS;
            }
        }
        if (now >= ctx->next_expiry) {
            roster_expire(ctx, now);
            ctx->next_expiry = now + EXPIRY_TICK_MS;
        }
        pthread_mutex_unlock(&ctx->lock);
    }
    return NULL;
}

/* ---------------- public lifecycle ---------------- */

void djl_config_defaults(djl_config *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->device_name    = "libdjlink";
    cfg->number_policy  = DJL_NUMBER_LOWEST_FREE;
    cfg->advertise_as   = DJL_DEVTYPE_CDJ;
    cfg->model_code     = 0x64;   /* CDJ-3000-compatible; see startup.adoc */
    cfg->proto_version  = 0x03;
    cfg->send_status    = true;
    cfg->auto_metadata  = true;
    cfg->log_level      = DJL_LOG_INFO;
}

djl_err djl_context_create(const djl_config *cfg, djl_context **out)
{
    if (!cfg || !out) return DJL_ERR_INVAL;
    if (!cfg->interface_name) return DJL_ERR_INVAL;

    djl_context *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return DJL_ERR_NOMEM;

    ctx->cfg = *cfg;
    snprintf(ctx->name_buf, sizeof ctx->name_buf, "%s",
             cfg->device_name ? cfg->device_name : "libdjlink");

    djl_err e = djl_iface_lookup(cfg->interface_name, &ctx->iface);
    if (e != DJL_OK) { free(ctx); return e; }

    memset(&ctx->id, 0, sizeof ctx->id);
    memcpy(ctx->id.name, ctx->name_buf, strnlen(ctx->name_buf, DJL_NAME_LEN));
    memcpy(ctx->id.mac, ctx->iface.mac, 6);
    memcpy(ctx->id.ip,  ctx->iface.ip,  4);
    ctx->id.device_type   = (uint8_t)(cfg->advertise_as ? cfg->advertise_as
                                                        : DJL_DEVTYPE_CDJ);
    ctx->id.model_code    = cfg->model_code;
    ctx->id.proto_version = cfg->proto_version ? cfg->proto_version : 0x03;
    ctx->id.peer_count    = 1;

    ctx->adv_tempo = 120.0;
    ctx->adv_bib   = 1;
    ctx->adv_beat  = 0;
    ctx->next_master = 0xff;

    /* NFS first: it works at any device number, returns the complete ANLZ set
     * (including PSSI, which the dbserver tag query serves unreliably), and
     * does not compete for the four dbserver-capable slots. dbserver follows
     * as the only source for audio CDs and streaming tracks. */
    ctx->providers[0]   = DJL_PROVIDER_NFS;
    ctx->providers[1]   = DJL_PROVIDER_DBSERVER;
    ctx->provider_count = 2;

    pthread_mutex_init(&ctx->lock, NULL);
    pthread_cond_init(&ctx->ev_cond, NULL);

    ctx->sock_announce.fd = ctx->sock_beat.fd = -1;
    ctx->sock_status.fd   = ctx->sock_audio.fd = -1;

    *out = ctx;
    return DJL_OK;
}

djl_err djl_context_start(djl_context *ctx)
{
    if (!ctx) return DJL_ERR_INVAL;
    if (ctx->running) return DJL_ERR_STATE;

    const char *ifn = ctx->cfg.interface_name;
    djl_err e;
    if ((e = djl_sock_open(&ctx->sock_announce, DJL_PORT_ANNOUNCE, ifn)) != DJL_OK) goto fail;
    if ((e = djl_sock_open(&ctx->sock_beat,     DJL_PORT_BEAT,     ifn)) != DJL_OK) goto fail;
    if ((e = djl_sock_open(&ctx->sock_status,   DJL_PORT_STATUS,   ifn)) != DJL_OK) goto fail;
    if ((e = djl_sock_open(&ctx->sock_audio,    DJL_PORT_AUDIO,    ifn)) != DJL_OK) goto fail;

    ctx->t0 = djl_now_ms();
    ctx->next_expiry = ctx->t0 + EXPIRY_TICK_MS;
    enter_state(ctx, DJL_ST_WATCHING, ctx->t0);
    ctx->running = true;

    if (ctx->cfg.auto_metadata && !ctx->cfg.observe_only) {
        if (djl_meta_start(ctx) != DJL_OK)
            djl_log(ctx, DJL_LOG_WARN, "could not start metadata worker");
    }

    djl_log(ctx, DJL_LOG_INFO,
            "interface %s ip %u.%u.%u.%u/%d bcast %u.%u.%u.%u "
            "mac %02x:%02x:%02x:%02x:%02x:%02x",
            ctx->iface.name,
            ctx->iface.ip[0], ctx->iface.ip[1], ctx->iface.ip[2], ctx->iface.ip[3],
            ctx->iface.prefix_len,
            ctx->iface.broadcast[0], ctx->iface.broadcast[1],
            ctx->iface.broadcast[2], ctx->iface.broadcast[3],
            ctx->iface.mac[0], ctx->iface.mac[1], ctx->iface.mac[2],
            ctx->iface.mac[3], ctx->iface.mac[4], ctx->iface.mac[5]);

    if (pthread_create(&ctx->thread, NULL, io_thread, ctx) != 0) {
        ctx->running = false;
        e = DJL_ERR_IO;
        goto fail;
    }
    ctx->thread_started = true;
    return DJL_OK;

fail:
    djl_sock_close(&ctx->sock_announce);
    djl_sock_close(&ctx->sock_beat);
    djl_sock_close(&ctx->sock_status);
    djl_sock_close(&ctx->sock_audio);
    return e;
}

void djl_context_stop(djl_context *ctx)
{
    if (!ctx || !ctx->running) return;
    ctx->running = false;
    /* Join the I/O thread before the metadata cache is torn down: the position
     * trackers it drives hold borrowed pointers into that cache. */
    if (ctx->thread_started) {
        pthread_join(ctx->thread, NULL);
        ctx->thread_started = false;
    }
    djl_meta_stop(ctx);
    djl_sock_close(&ctx->sock_announce);
    djl_sock_close(&ctx->sock_beat);
    djl_sock_close(&ctx->sock_status);
    djl_sock_close(&ctx->sock_audio);
}

void djl_context_destroy(djl_context *ctx)
{
    if (!ctx) return;
    djl_context_stop(ctx);
    pthread_mutex_destroy(&ctx->lock);
    pthread_cond_destroy(&ctx->ev_cond);
    free(ctx);
}

int  djl_own_number(const djl_context *ctx) { return ctx ? ctx->id.number : -1; }
bool djl_is_online(const djl_context *ctx)  { return ctx && ctx->state == DJL_ST_ONLINE; }

/* ---------------- public queries ---------------- */

size_t djl_devices(djl_context *ctx, djl_device_info *out, size_t max)
{
    if (!ctx || !out) return 0;
    pthread_mutex_lock(&ctx->lock);
    size_t n = 0;
    for (size_t i = 0; i < DJL_MAX_DEVICES && n < max; i++)
        if (ctx->devices[i].used) out[n++] = ctx->devices[i].info;
    pthread_mutex_unlock(&ctx->lock);
    return n;
}

djl_err djl_device_by_number(djl_context *ctx, uint8_t number, djl_device_info *out)
{
    if (!ctx || !out) return DJL_ERR_INVAL;
    pthread_mutex_lock(&ctx->lock);
    djl_slot_entry *e = roster_find(ctx, number);
    if (e) *out = e->info;
    pthread_mutex_unlock(&ctx->lock);
    return e ? DJL_OK : DJL_ERR_NOT_FOUND;
}

djl_err djl_cdj_status_for(djl_context *ctx, uint8_t number, djl_cdj_status *out)
{
    if (!ctx || !out) return DJL_ERR_INVAL;
    pthread_mutex_lock(&ctx->lock);
    djl_slot_entry *e = roster_find(ctx, number);
    bool have = e && e->has_status;
    if (have) *out = e->status;
    pthread_mutex_unlock(&ctx->lock);
    return have ? DJL_OK : DJL_ERR_NOT_FOUND;
}

int djl_tempo_master(djl_context *ctx)
{
    if (!ctx) return -1;
    pthread_mutex_lock(&ctx->lock);
    int m = ctx->tempo_master;
    pthread_mutex_unlock(&ctx->lock);
    return m;
}

djl_err djl_get_position(djl_context *ctx, uint8_t player, djl_position *out)
{
    if (!ctx || !out || player == 0 || player >= 64) return DJL_ERR_INVAL;
    pthread_mutex_lock(&ctx->lock);
    bool valid = ctx->positions[player].valid;
    djl_pos_interpolate(&ctx->positions[player], player, djl_now_ms(), out);
    pthread_mutex_unlock(&ctx->lock);
    return valid ? DJL_OK : DJL_ERR_NOT_FOUND;
}

double djl_master_tempo(djl_context *ctx)
{
    if (!ctx) return 0.0;
    pthread_mutex_lock(&ctx->lock);
    double t = ctx->master_tempo;
    pthread_mutex_unlock(&ctx->lock);
    return t;
}

int djl_poll(djl_context *ctx, djl_event *out, size_t max, int timeout_ms)
{
    if (!ctx || !out || max == 0) return DJL_ERR_INVAL;
    pthread_mutex_lock(&ctx->lock);

    if (ctx->ev_head == ctx->ev_tail && timeout_ms != 0) {
        if (timeout_ms < 0) {
            while (ctx->ev_head == ctx->ev_tail && ctx->running)
                pthread_cond_wait(&ctx->ev_cond, &ctx->lock);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += timeout_ms / 1000;
            ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            while (ctx->ev_head == ctx->ev_tail && ctx->running) {
                if (pthread_cond_timedwait(&ctx->ev_cond, &ctx->lock, &ts) != 0) break;
            }
        }
    }

    size_t n = 0;
    while (n < max && ctx->ev_head != ctx->ev_tail) {
        out[n++] = ctx->evq[ctx->ev_head];
        ctx->ev_head = (ctx->ev_head + 1) % DJL_EVQ_SIZE;
    }
    pthread_mutex_unlock(&ctx->lock);
    return (int)n;
}

djl_err djl_set_raw_hook(djl_context *ctx, djl_raw_fn fn, void *ud)
{
    if (!ctx) return DJL_ERR_INVAL;
    pthread_mutex_lock(&ctx->lock);
    ctx->raw_hook = fn;
    ctx->raw_ud   = ud;
    pthread_mutex_unlock(&ctx->lock);
    return DJL_OK;
}

/* ---------------- public control ---------------- */

static djl_err need_online(djl_context *ctx)
{
    if (!ctx) return DJL_ERR_INVAL;
    if (ctx->cfg.observe_only) return DJL_ERR_STATE;
    if (ctx->state != DJL_ST_ONLINE || ctx->id.number == 0) return DJL_ERR_STATE;
    return DJL_OK;
}

djl_err djl_query_media(djl_context *ctx, uint8_t player, djl_slot slot)
{
    djl_err e = need_online(ctx);
    if (e != DJL_OK) return e;
    pthread_mutex_lock(&ctx->lock);
    djl_slot_entry *t = roster_find(ctx, player);
    if (!t) { pthread_mutex_unlock(&ctx->lock); return DJL_ERR_NOT_FOUND; }
    uint8_t buf[DJL_MAX_PACKET];
    size_t n = djl_build_media_query(buf, sizeof buf, &ctx->id, player, (uint8_t)slot);
    unicast(ctx, &ctx->sock_status, t->info.ip, DJL_PORT_STATUS, buf, n);
    pthread_mutex_unlock(&ctx->lock);
    return DJL_OK;
}

static djl_err send_sync_byte(djl_context *ctx, uint8_t player, uint8_t s)
{
    djl_err e = need_online(ctx);
    if (e != DJL_OK) return e;
    pthread_mutex_lock(&ctx->lock);
    djl_slot_entry *t = roster_find(ctx, player);
    if (!t) { pthread_mutex_unlock(&ctx->lock); return DJL_ERR_NOT_FOUND; }
    uint8_t buf[DJL_MAX_PACKET];
    size_t n = djl_build_sync_control(buf, sizeof buf, &ctx->id, s);
    unicast(ctx, &ctx->sock_beat, t->info.ip, DJL_PORT_BEAT, buf, n);
    pthread_mutex_unlock(&ctx->lock);
    return DJL_OK;
}

djl_err djl_send_sync(djl_context *ctx, uint8_t player, bool on)
{
    return send_sync_byte(ctx, player, on ? 0x10 : 0x20);
}

djl_err djl_appoint_master(djl_context *ctx, uint8_t player)
{
    return send_sync_byte(ctx, player, 0x01);
}

djl_err djl_send_on_air(djl_context *ctx, uint8_t channel_mask, bool six_channel)
{
    djl_err e = need_online(ctx);
    if (e != DJL_OK) return e;
    pthread_mutex_lock(&ctx->lock);
    uint8_t buf[DJL_MAX_PACKET];
    size_t n = djl_build_on_air(buf, sizeof buf, &ctx->id, channel_mask, six_channel);
    /* Broadcasting works: each player interprets its own flag. */
    if (!ctx->cfg.observe_only && n)
        djl_sock_send(&ctx->sock_beat, ctx->iface.broadcast, DJL_PORT_BEAT, buf, n);
    pthread_mutex_unlock(&ctx->lock);
    return DJL_OK;
}

djl_err djl_send_fader_start(djl_context *ctx, uint8_t start_mask, uint8_t stop_mask)
{
    djl_err e = need_online(ctx);
    if (e != DJL_OK) return e;
    uint8_t cmds[4];
    for (int i = 0; i < 4; i++) {
        if (stop_mask & (1u << i))       cmds[i] = 0x01;  /* stop wins */
        else if (start_mask & (1u << i)) cmds[i] = 0x00;
        else                             cmds[i] = 0x02;  /* no change */
    }
    pthread_mutex_lock(&ctx->lock);
    uint8_t buf[DJL_MAX_PACKET];
    size_t n = djl_build_fader_start(buf, sizeof buf, &ctx->id, cmds);
    if (!ctx->cfg.observe_only && n)
        djl_sock_send(&ctx->sock_beat, ctx->iface.broadcast, DJL_PORT_BEAT, buf, n);
    pthread_mutex_unlock(&ctx->lock);
    return DJL_OK;
}

djl_err djl_load_track(djl_context *ctx, uint8_t target, uint8_t source_player,
                       djl_slot slot, djl_track_type type, uint32_t rekordbox_id)
{
    djl_err e = need_online(ctx);
    if (e != DJL_OK) return e;
    pthread_mutex_lock(&ctx->lock);
    djl_slot_entry *t = roster_find(ctx, target);
    if (!t) { pthread_mutex_unlock(&ctx->lock); return DJL_ERR_NOT_FOUND; }
    uint8_t buf[DJL_MAX_PACKET];
    size_t n = djl_build_load_track(buf, sizeof buf, &ctx->id, target,
                                    source_player, (uint8_t)slot,
                                    (uint8_t)type, rekordbox_id);
    unicast(ctx, &ctx->sock_status, t->info.ip, DJL_PORT_STATUS, buf, n);
    pthread_mutex_unlock(&ctx->lock);
    return DJL_OK;
}

djl_err djl_set_tempo(djl_context *ctx, double bpm)
{
    if (!ctx || bpm < 0.0 || bpm > 999.0) return DJL_ERR_INVAL;
    pthread_mutex_lock(&ctx->lock);
    ctx->adv_tempo = bpm;
    pthread_mutex_unlock(&ctx->lock);
    return DJL_OK;
}

djl_err djl_set_playing(djl_context *ctx, bool playing)
{
    if (!ctx) return DJL_ERR_INVAL;
    pthread_mutex_lock(&ctx->lock);
    ctx->adv_playing = playing;
    pthread_mutex_unlock(&ctx->lock);
    return DJL_OK;
}

djl_err djl_set_synced(djl_context *ctx, bool synced)
{
    if (!ctx) return DJL_ERR_INVAL;
    pthread_mutex_lock(&ctx->lock);
    ctx->adv_synced = synced;
    pthread_mutex_unlock(&ctx->lock);
    return DJL_OK;
}

djl_err djl_set_beat(djl_context *ctx, int32_t beat, uint8_t beat_within_bar)
{
    if (!ctx || beat_within_bar > 4) return DJL_ERR_INVAL;
    pthread_mutex_lock(&ctx->lock);
    ctx->adv_beat = beat;
    ctx->adv_bib  = beat_within_bar ? beat_within_bar : 1;
    pthread_mutex_unlock(&ctx->lock);
    return DJL_OK;
}
