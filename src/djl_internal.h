#ifndef DJL_INTERNAL_H
#define DJL_INTERNAL_H

#include "djlink.h"
#include <pthread.h>
#include <stdint.h>

/* ---------------- OSAL ---------------- */

typedef struct {
    char     name[64];
    uint8_t  mac[6];
    uint8_t  ip[4];
    uint8_t  netmask[4];
    uint8_t  broadcast[4];
    int      prefix_len;
} djl_iface;

djl_err  djl_iface_lookup(const char *name, djl_iface *out);
/* declared in djlink.h */
void     djl_sleep_ms(unsigned ms);

/* A socket handle. Windows SOCKET is a UINT_PTR, which does not fit in an int
 * on 64-bit, so carry it as intptr_t: INVALID_SOCKET round-trips to -1 and a
 * POSIX descriptor fits unchanged, so "fd < 0 means invalid" holds on both. */
typedef intptr_t djl_fd;
#define DJL_BAD_FD ((djl_fd)-1)

/* A bound UDP socket. */
typedef struct {
    djl_fd   fd;
    uint16_t port;
} djl_sock;

djl_err djl_sock_open(djl_sock *s, uint16_t port, const char *ifname);
void    djl_sock_close(djl_sock *s);
int     djl_sock_recv(djl_sock *s, uint8_t *buf, size_t cap, uint8_t src_ip[4]);
djl_err djl_sock_send(djl_sock *s, const uint8_t ip[4], uint16_t port,
                      const uint8_t *buf, size_t len);

/* Wait until at least one of n sockets is readable. ready[i] is set for each
 * readable socket. Returns the count ready, 0 on timeout, negative on error. */
int     djl_sock_poll(djl_sock *const *socks, size_t n, unsigned timeout_ms,
                      bool *ready);

/* Client UDP socket bound to an ephemeral port, for the NFS/RPC client.
 * Pioneer's NFS does not require a privileged source port. */
djl_err djl_udp_open(djl_sock *s);
/* Blocking receive with a millisecond timeout. Returns bytes, 0 on timeout,
 * negative on error. */
int     djl_udp_recv_wait(djl_sock *s, uint8_t *buf, size_t cap, unsigned timeout_ms);

/* Blocking TCP client, for the dbserver protocol. Keeps every socket call in
 * the OSAL so dbserver.c stays platform-independent. */
typedef struct { djl_fd fd; } djl_tcp;

djl_err djl_tcp_connect(djl_tcp *t, const uint8_t ip[4], uint16_t port, int timeout_ms);
void    djl_tcp_close(djl_tcp *t);
djl_err djl_tcp_send_all(djl_tcp *t, const uint8_t *buf, size_t len);
djl_err djl_tcp_recv_exact(djl_tcp *t, uint8_t *buf, size_t len);

/* ---------------- packet templates ---------------- */

/* Each builder writes into buf (cap must be >= the documented size) and
 * returns the number of bytes written, or 0 on error. */

typedef struct {
    char    name[DJL_NAME_LEN];
    uint8_t number;
    uint8_t device_type;
    uint8_t model_code;
    uint8_t proto_version;
    uint8_t mac[6];
    uint8_t ip[4];
    uint8_t peer_count;
} djl_identity;

size_t djl_build_hello(uint8_t *buf, size_t cap, const djl_identity *id);
size_t djl_build_claim1(uint8_t *buf, size_t cap, const djl_identity *id, uint8_t n);
size_t djl_build_claim2(uint8_t *buf, size_t cap, const djl_identity *id,
                        uint8_t claim, uint8_t n, bool auto_assign);
size_t djl_build_claim3(uint8_t *buf, size_t cap, const djl_identity *id,
                        uint8_t claim, uint8_t n);
size_t djl_build_keep_alive(uint8_t *buf, size_t cap, const djl_identity *id);
size_t djl_build_number_in_use(uint8_t *buf, size_t cap, const djl_identity *id,
                               uint8_t defended, const uint8_t ip[4]);

size_t djl_build_status(uint8_t *buf, size_t cap, const djl_identity *id,
                        bool playing, bool master, bool synced, bool on_air,
                        double tempo, int32_t beat, uint8_t beat_within_bar,
                        uint32_t sync_counter, uint8_t next_master,
                        uint32_t packet_counter);

size_t djl_build_media_query(uint8_t *buf, size_t cap, const djl_identity *id,
                             uint8_t target, uint8_t slot);
size_t djl_build_sync_control(uint8_t *buf, size_t cap, const djl_identity *id, uint8_t s);
size_t djl_build_on_air(uint8_t *buf, size_t cap, const djl_identity *id,
                        uint8_t mask, bool six);
size_t djl_build_fader_start(uint8_t *buf, size_t cap, const djl_identity *id,
                            const uint8_t cmds[4]);
size_t djl_build_load_track(uint8_t *buf, size_t cap, const djl_identity *id,
                            uint8_t target, uint8_t src_player, uint8_t slot,
                            uint8_t type, uint32_t rekordbox_id);

/* ---------------- context ---------------- */

#define DJL_EVQ_SIZE 512

typedef enum {
    DJL_ST_IDLE = 0,
    DJL_ST_WATCHING,
    DJL_ST_ANNOUNCING,
    DJL_ST_CLAIM1,
    DJL_ST_CLAIM2,
    DJL_ST_CLAIM3,
    DJL_ST_ONLINE
} djl_state;

typedef struct {
    djl_device_info info;
    bool            used;
    /* last status cache */
    bool            has_status;
    djl_cdj_status  status;
} djl_slot_entry;

/* Per-player playback-position tracking state (see position.c). */
typedef struct {
    bool     valid;
    uint64_t at_ms;            /* monotonic ms when captured */
    int64_t  position_ms;      /* known playhead at at_ms (-1 if unknown) */
    int32_t  beat;
    uint8_t  beat_within_bar;
    double   speed;            /* signed pitch multiplier (negative = reverse) */
    double   bpm;              /* effective */
    bool     playing;
    bool     definitive;       /* last update was precise position or a beat */
    bool     from_precise;
    int64_t  track_length_ms;  /* -1 if unknown */

    /* Beat-grid anchoring, for players with no precise-position stream. The
     * grid is BORROWED from the metadata cache, never owned here: staleness is
     * detected by pointer identity in djl_pos_attach_grid, and the pointer is
     * cleared on track change, device loss and cache teardown. */
    const djl_beat_grid *grid;
    bool     grid_beat_known;  /* last beat number came from a status packet */
} djl_pos_state;

/* Beat-grid helpers (pure). */
int64_t djl_grid_time_of_beat(const djl_beat_grid *g, int32_t beat);
int32_t djl_grid_beat_at_time(const djl_beat_grid *g, int64_t ms);
uint8_t djl_grid_beat_within_bar(const djl_beat_grid *g, int32_t beat);

void djl_pos_apply_precise(djl_pos_state *s, const djl_precise_position *pp, uint64_t now);
void djl_pos_apply_beat(djl_pos_state *s, const djl_beat *b, uint64_t now);
void djl_pos_apply_status(djl_pos_state *s, const djl_cdj_status *st, uint64_t now);
void djl_pos_interpolate(const djl_pos_state *s, uint8_t player, uint64_t now,
                         djl_position *out);

struct djl_context {
    djl_config      cfg;
    char            name_buf[DJL_NAME_LEN + 1];
    djl_iface       iface;
    djl_identity    id;

    djl_sock        sock_announce;
    djl_sock        sock_beat;
    djl_sock        sock_status;
    djl_sock        sock_audio;

    volatile bool   running;
    pthread_t       thread;
    bool            thread_started;

    pthread_mutex_t lock;       /* guards roster, status cache, event queue */
    pthread_cond_t  ev_cond;

    djl_state       state;
    uint64_t        t0;         /* context start, ms */
    uint64_t        state_since;
    int             claim_step;
    uint8_t         claiming;
    uint8_t         assigned_by_mixer;

    djl_slot_entry  devices[DJL_MAX_DEVICES];
    djl_pos_state   positions[64];   /* indexed by device number */

    /* metadata auto-fetch manager (metadata.c) */
    bool            meta_running;
    bool            meta_started;
    pthread_t       meta_thread;
    pthread_cond_t  meta_cond;
    struct { uint8_t player, host; djl_slot slot; djl_track_type type; uint32_t id; }
                    meta_jobs[16];
    size_t          meta_job_head, meta_job_tail;
    struct djl_meta_entry *meta_cache;   /* [64], heap to keep this struct lean */
    djl_provider_kind providers[DJL_MAX_PROVIDERS];
    size_t          provider_count;

    /* Bumped by the I/O thread whenever a player's media changes or the device
     * disappears, so the metadata worker knows to drop any NFS mount and parsed
     * export.pdb it cached for that player. Read/written under ctx->lock. */
    uint32_t        media_gen[64];
    uint32_t        media_sig[64];   /* fingerprint of the last media details */

    /* NFS mounts cached across fetches, so a track load does not re-mount and
     * re-download export.pdb every time. Touched ONLY by the metadata worker
     * thread, which is why it needs no lock; djl_nfs itself is not thread-safe.
     * Sized for the four player slots plus headroom. */
    struct {
        djl_nfs *h;
        uint8_t  host;
        uint8_t  slot;
        uint32_t gen;        /* media_gen value this mount was opened at */
        uint64_t last_use_ms;
    } nfs_cache[6];

    /* our advertised state */
    bool            adv_playing, adv_master, adv_synced, adv_on_air;
    double          adv_tempo;
    int32_t         adv_beat;
    uint8_t         adv_bib;
    uint32_t        sync_counter;
    uint8_t         next_master;
    uint32_t        packet_counter;

    /* observed network state */
    uint8_t         tempo_master;
    double          master_tempo;
    uint8_t         on_air_mask;

    /* timers */
    uint64_t        next_keepalive;
    uint64_t        next_status;
    uint64_t        next_expiry;

    /* raw research hook */
    djl_raw_fn      raw_hook;
    void           *raw_ud;

    /* event queue */
    djl_event       evq[DJL_EVQ_SIZE];
    size_t          ev_head, ev_tail;
    uint64_t        ev_dropped;
};

void djl_log(djl_context *ctx, djl_log_level lvl, const char *fmt, ...);
void djl_emit(djl_context *ctx, const djl_event *ev);
void djl_sha1(const uint8_t *data, size_t len, uint8_t out[20]);

/* Per-player fetched-metadata cache entry. */
struct djl_meta_entry {
    bool     valid;
    uint8_t  host; djl_slot slot; djl_track_type type; uint32_t id;
    bool has_meta; djl_track_info meta;
    bool has_wave; djl_waveform_blob wave;
    bool has_grid; djl_beat_grid grid;
    bool has_cues; djl_cue_list cues;
    bool has_art;  djl_blob art;
    bool has_sig;  uint8_t sig[20];
    bool has_ss;   djl_song_structure ss;
};

/* Wall-clock budget for one metadata job's NFS work. Without a cap, four RPC
 * attempts at a doubling timeout (6 s) times the chunk-shrink retries times a
 * handful of files let one unreachable player pin the worker for minutes and
 * block djl_context_stop behind it. */
#define DJL_NFS_JOB_BUDGET_MS 8000

/* Close and forget every cached NFS mount (metadata.c). Worker thread only. */
void    djl_nfs_cache_clear(djl_context *ctx);

/* Refuse to start new NFS round trips after this monotonic timestamp. 0 clears
 * the limit. Applies to the handle's subsequent calls. */
void    djl_nfs_set_deadline(djl_nfs *n, uint64_t deadline_ms);

/* Metadata manager (metadata.c). enqueue must be called with ctx->lock held. */
djl_err djl_meta_start(djl_context *ctx);
void    djl_meta_stop(djl_context *ctx);
void    djl_meta_enqueue(djl_context *ctx, uint8_t player, uint8_t host,
                         djl_slot slot, djl_track_type type, uint32_t id);
/* Point a player's position tracker at its cached beat grid, or clear it when
 * the cache no longer holds one. Caller must hold ctx->lock. */
void    djl_pos_attach_grid(djl_context *ctx, uint8_t player);
/* Drop the borrowed grid, e.g. when the loaded track changes and the cached
 * grid still describes the previous one. Caller must hold ctx->lock. */
void    djl_pos_detach_grid(djl_context *ctx, uint8_t player);

#endif /* DJL_INTERNAL_H */
