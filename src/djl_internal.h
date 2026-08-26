#ifndef DJL_INTERNAL_H
#define DJL_INTERNAL_H

#include "djlink.h"
#include <pthread.h>

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

/* A bound UDP socket. */
typedef struct {
    int      fd;
    uint16_t port;
} djl_sock;

djl_err djl_sock_open(djl_sock *s, uint16_t port, const char *ifname);
void    djl_sock_close(djl_sock *s);
int     djl_sock_recv(djl_sock *s, uint8_t *buf, size_t cap, uint8_t src_ip[4]);
djl_err djl_sock_send(djl_sock *s, const uint8_t ip[4], uint16_t port,
                      const uint8_t *buf, size_t len);

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
} djl_pos_state;

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
};

/* Metadata manager (metadata.c). enqueue must be called with ctx->lock held. */
djl_err djl_meta_start(djl_context *ctx);
void    djl_meta_stop(djl_context *ctx);
void    djl_meta_enqueue(djl_context *ctx, uint8_t player, uint8_t host,
                         djl_slot slot, djl_track_type type, uint32_t id);

#endif /* DJL_INTERNAL_H */
