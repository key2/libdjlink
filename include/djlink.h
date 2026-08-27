/* libdjlink - Pioneer / AlphaTheta Pro DJ Link protocol library
 *
 * Public API. C11. See ARCHITECTURE.md for the protocol specification this
 * implements and for byte-level references into the dysentery analysis.
 */
#ifndef DJLINK_H
#define DJLINK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DJL_API

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    DJL_OK = 0,
    DJL_ERR_INVAL       = -1,   /* bad argument */
    DJL_ERR_NOMEM       = -2,
    DJL_ERR_IO          = -3,
    DJL_ERR_NO_IFACE    = -4,   /* interface not found / no address */
    DJL_ERR_NOT_FOUND   = -5,
    DJL_ERR_TIMEOUT     = -6,
    DJL_ERR_STATE       = -7,   /* wrong state for this call */
    DJL_ERR_CONFLICT    = -8,   /* device number conflict */
    DJL_ERR_SHORT       = -9,   /* packet too short for field */
    DJL_ERR_UNKNOWN     = -10,  /* unrecognized packet */
    DJL_ERR_UNAVAILABLE = -11
} djl_err;

DJL_API const char *djl_strerror(djl_err e);

/* Monotonic milliseconds, same clock the library uses for event timestamps. */
DJL_API uint64_t djl_now_ms(void);

/* ------------------------------------------------------------------ */
/* Protocol constants                                                  */
/* ------------------------------------------------------------------ */

#define DJL_PORT_ANNOUNCE 50000
#define DJL_PORT_BEAT     50001
#define DJL_PORT_STATUS   50002
#define DJL_PORT_AUDIO    50004

#define DJL_MAGIC_LEN      10
#define DJL_NAME_LEN       0x14   /* 20 */
#define DJL_MAX_PACKET     1500

/* Value of a pitch field representing 0% adjustment (normal speed). */
#define DJL_NEUTRAL_PITCH  1048576L   /* 0x100000 */

/* Device type, keep-alive byte 0x34 */
typedef enum {
    DJL_DEVTYPE_CDJ        = 0x01,
    DJL_DEVTYPE_MIXER      = 0x02,
    DJL_DEVTYPE_MIXER_MODERN = 0x03, /* DJM-A9 and later */
    DJL_DEVTYPE_STAGEHAND  = 0x05
} djl_device_type;

/* Packet kinds, disambiguated by port. */
typedef enum {
    DJL_PKT_UNKNOWN = 0,
    /* port 50000 */
    DJL_PKT_NUMBER_STAGE1,
    DJL_PKT_NUMBER_WILL_ASSIGN,
    DJL_PKT_NUMBER_STAGE2,
    DJL_PKT_NUMBER_ASSIGN,
    DJL_PKT_NUMBER_STAGE3,
    DJL_PKT_NUMBER_FINISHED,
    DJL_PKT_KEEP_ALIVE,
    DJL_PKT_NUMBER_IN_USE,
    DJL_PKT_DEVICE_HELLO,
    /* port 50001 */
    DJL_PKT_FADER_START,
    DJL_PKT_CHANNELS_ON_AIR,
    DJL_PKT_PRECISE_POSITION,
    DJL_PKT_MASTER_HANDOFF_REQ,
    DJL_PKT_MASTER_HANDOFF_RESP,
    DJL_PKT_BEAT,
    DJL_PKT_SYNC_CONTROL,
    DJL_PKT_BEAT_HEARTBEAT,      /* 0x6a AlphaTheta */
    /* port 50002 */
    DJL_PKT_MEDIA_QUERY,
    DJL_PKT_MEDIA_RESPONSE,
    DJL_PKT_CDJ_STATUS,
    DJL_PKT_RB_LIGHTING_HELLO,
    DJL_PKT_LOAD_TRACK,
    DJL_PKT_LOAD_TRACK_ACK,
    DJL_PKT_MIXER_STATUS,
    DJL_PKT_LOAD_SETTINGS,
    DJL_PKT_MIXER_STATE_A9,      /* 0x39 */
    DJL_PKT_OPUS_DATA_REQ,       /* 0x55 */
    DJL_PKT_OPUS_DATA_RESP,      /* 0x56 */
    DJL_PKT_VU_STREAM,           /* 0x58 */
    DJL_PKT_KUVO_GATEWAY,        /* 0x40 */
    /* port 50004 */
    DJL_PKT_AUDIO_DATA,
    DJL_PKT_AUDIO_HANDOVER,
    DJL_PKT_AUDIO_TIMING,
    /* rekordbox LINK control channel, port 50002. Undocumented publicly;
     * measured first-hand from a capture taken on the rekordbox host.
     * Appended here rather than grouped above so existing enum values, which
     * consumers may already have compiled against, keep their numbers. */
    DJL_PKT_RB_ANNOUNCE,         /* 0x11 rekordbox -> player, carries host name */
    DJL_PKT_RB_KEEPALIVE,        /* 0x16 rekordbox -> player, periodic */
    DJL_PKT_RB_MIXER_NOTIFY,     /* 0x30 mixer -> rekordbox */
    DJL_PKT_RB_MIXER_REPLY,      /* 0x31 rekordbox -> mixer */
    DJL_PKT_RB_PLAYER_REPLY,     /* 0x46 player -> rekordbox, answers 0x47 */
    DJL_PKT_RB_CONFIG,           /* 0x47 rekordbox -> player, settings block */
    DJL_PKT_RB_PLAYER_NOTIFY,    /* 0x80 player -> rekordbox */
    DJL_PKT__COUNT
} djl_packet_kind;

DJL_API const char *djl_packet_kind_name(djl_packet_kind k);

/* Track source slot, CDJ status byte 0x29 */
typedef enum {
    DJL_SLOT_NONE       = 0,
    DJL_SLOT_CD         = 1,
    DJL_SLOT_SD         = 2,
    DJL_SLOT_USB        = 3,
    DJL_SLOT_COLLECTION = 4,   /* rekordbox on a laptop */
    DJL_SLOT_UNKNOWN5   = 5,
    DJL_SLOT_STREAM_DP  = 6,   /* Streaming Direct Play */
    DJL_SLOT_USB2       = 7,   /* XDJ-AZ four-deck mode */
    DJL_SLOT_UNKNOWN8   = 8,
    DJL_SLOT_BEATPORT   = 9
} djl_slot;

DJL_API const char *djl_slot_name(djl_slot s);

/* Track type, CDJ status byte 0x2a */
typedef enum {
    DJL_TRACK_NONE       = 0,
    DJL_TRACK_REKORDBOX  = 1,
    DJL_TRACK_UNANALYZED = 2,
    DJL_TRACK_AUDIO_CD   = 5,
    DJL_TRACK_STREAMING  = 6
} djl_track_type;

DJL_API const char *djl_track_type_name(djl_track_type t);

/* Play mode, CDJ status byte 0x7b (P1) */
typedef enum {
    DJL_PLAY1_NO_TRACK      = 0x00,
    DJL_PLAY1_LOADING       = 0x02,
    DJL_PLAY1_PLAYING       = 0x03,
    DJL_PLAY1_LOOPING       = 0x04,
    DJL_PLAY1_PAUSED        = 0x05,
    DJL_PLAY1_PAUSED_CUE    = 0x06,
    DJL_PLAY1_CUE_PLAY      = 0x07,
    DJL_PLAY1_CUE_SCRATCH   = 0x08,
    DJL_PLAY1_SEARCHING     = 0x09,
    DJL_PLAY1_SPUN_DOWN     = 0x0e,
    DJL_PLAY1_ENDED         = 0x11,
    DJL_PLAY1_EMERGENCY     = 0x12
} djl_play_state1;

DJL_API const char *djl_play_state1_name(unsigned v);

/* CDJ status flag bits, byte 0x89 (F) */
#define DJL_F_PLAY    0x40
#define DJL_F_MASTER  0x20
#define DJL_F_SYNC    0x10
#define DJL_F_ON_AIR  0x08
#define DJL_F_BPM_ONLY 0x02

/* ------------------------------------------------------------------ */
/* Wire layer - pure, no I/O, no allocation. Usable standalone.         */
/* ------------------------------------------------------------------ */

/* True if buf begins with the 10-byte Pro DJ Link magic. */
DJL_API bool djl_wire_has_magic(const uint8_t *buf, size_t len);

/* Classify a received datagram. Returns DJL_PKT_UNKNOWN if unrecognized. */
DJL_API djl_packet_kind djl_wire_classify(uint16_t port, const uint8_t *buf, size_t len);

/* Raw kind byte at offset 0x0a. */
DJL_API int djl_wire_kind_byte(const uint8_t *buf, size_t len);

/* Which framing a packet uses. Announcement packets (port 50000) put the
 * device name at 0x0c; status/beat/control packets put it at 0x0b. */
DJL_API bool djl_wire_is_announce_framing(uint16_t port);

/* Extract the NUL-padded device name as UTF-8 (ASCII in practice).
 * out must hold at least DJL_NAME_LEN+1 bytes. */
DJL_API djl_err djl_wire_device_name(uint16_t port, const uint8_t *buf, size_t len,
                                    char *out, size_t outsz);

/* Device number. Announcement framing reads 0x24, status framing reads 0x21. */
DJL_API int djl_wire_device_number(uint16_t port, const uint8_t *buf, size_t len);

/* Big-endian / little-endian unsigned integer extraction with bounds checks.
 * Return 0 and set *ok=false on overrun. nbytes must be 1..8. */
DJL_API uint64_t djl_wire_be(const uint8_t *buf, size_t len, size_t off, size_t nbytes, bool *ok);
DJL_API uint64_t djl_wire_le(const uint8_t *buf, size_t len, size_t off, size_t nbytes, bool *ok);
DJL_API int      djl_wire_u8(const uint8_t *buf, size_t len, size_t off);

/* Pitch and tempo helpers. */
DJL_API double djl_pitch_percent(uint32_t raw);
DJL_API double djl_pitch_multiplier(uint32_t raw);
DJL_API uint32_t djl_percent_to_pitch(double percent);
DJL_API double djl_effective_bpm(uint32_t bpm_x100, uint32_t pitch_raw);

/* Half-frame conversions (75 frames/s, 150 half-frames/s). */
DJL_API int64_t djl_halfframe_to_ms(int64_t hf);
DJL_API int64_t djl_ms_to_halfframe(int64_t ms);

/* ------------------------------------------------------------------ */
/* Decoded packet views                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char      name[DJL_NAME_LEN + 1];
    uint8_t   number;          /* D */
    uint8_t   device_type;     /* byte 0x34 */
    uint8_t   model_code;      /* byte 0x35 */
    uint8_t   proto_version;   /* byte 0x21 */
    uint8_t   mac[6];
    uint8_t   ip[4];
    uint8_t   peer_count;      /* p, byte 0x30 */
    uint8_t   was_first;       /* byte 0x25: 2 = first on network, 1 = joined */
} djl_keep_alive;

DJL_API djl_err djl_decode_keep_alive(const uint8_t *buf, size_t len, djl_keep_alive *out);

typedef struct {
    uint8_t  number;
    char     name[DJL_NAME_LEN + 1];
    uint16_t packet_len;
    uint8_t  subtype;           /* byte 0x20: 0x03-0x06 documented, 0x08 on CDJ-3000X */

    /* presence / identity */
    uint8_t  activity;          /* A   0x27 */
    uint8_t  track_device;      /* Dr  0x28 */
    djl_slot track_slot;        /* Sr  0x29 */
    djl_track_type track_type;  /* Tr  0x2a */
    uint32_t rekordbox_id;      /*     0x2c */
    uint16_t track_number;      /*     0x32 */
    uint8_t  sort_mode;         /* tsrt 0x35 */
    uint8_t  track_source;      /* tsrc 0x37 */
    uint32_t category1;         /* tcat1 0x38 */
    uint32_t category2;         /* tcat2 0x3c */
    uint16_t list_count;        /* dn  0x46 */

    /* media presence */
    uint8_t  usb_local;         /* Ul  0x6f */
    uint8_t  sd_local;          /* Sl  0x73 */
    uint8_t  link_available;    /* L   0x75 */
    uint8_t  media_present;     /* Mp  0xb7 (CDJ-3000) */
    uint8_t  usb_unsafe_eject;  /* Ue  0xb8 */
    uint8_t  sd_unsafe_eject;   /* Se  0xb9 */
    uint8_t  emergency_loop;    /* el  0xba */

    /* playback */
    uint8_t  play_state1;       /* P1  0x7b */
    uint8_t  play_state2;       /* P2  0x8b */
    uint8_t  play_state3;       /* P3  0x9d */
    char     firmware[5];       /*     0x7c */
    uint32_t sync_counter;      /* Syncn 0x84 */
    uint8_t  flags;             /* F   0x89 */
    uint32_t pitch1, pitch2, pitch3, pitch4;
    uint16_t tempo_validity;    /* Mv  0x90 */
    uint16_t bpm_x100;          /* BPM 0x92 (0xffff = none) */
    uint8_t  master_meaningful; /* Mm  0x9e */
    uint8_t  master_handoff;    /* Mh  0x9f */
    int32_t  beat;              /* Beat 0xa0 (-1 if unknown) */
    uint16_t cue_countdown;     /* Cue 0xa4 */
    uint8_t  beat_within_bar;   /* Bb  0xa6 */
    uint32_t packet_counter;    /*     0xc8 */
    uint8_t  hardware_hint;     /* nx  0xcc */
    uint8_t  touch_audio_caps;  /* t   0xcd */

    /* CDJ-3000 extras (present only when packet_len is large enough) */
    bool     has_extended;
    uint8_t  master_tempo;      /* Mt  0x158 */
    uint8_t  key_note;          /*     0x15c */
    uint8_t  key_major;         /*     0x15d */
    uint8_t  key_accidental;    /*     0x15e */
    int64_t  key_shift_cents;   /*     0x164 */
    uint32_t loop_start_raw;    /* Loops 0x1b6 */
    uint32_t loop_end_raw;      /* Loope 0x1be */
    uint16_t loop_beats;        /* Loopb 0x1c8 */
    uint8_t  waveform_color;    /* settings block 1 +0x0a */
    uint8_t  waveform_position; /* settings block 1 +0x0d */

    /* derived */
    bool     playing, master, synced, on_air, bpm_only_sync;
    double   pitch_percent;
    double   effective_bpm;
} djl_cdj_status;

DJL_API djl_err djl_decode_cdj_status(const uint8_t *buf, size_t len, djl_cdj_status *out);

typedef struct {
    uint8_t  number;
    char     name[DJL_NAME_LEN + 1];
    uint8_t  flags;            /* F   0x27 */
    uint32_t pitch;            /*     0x28 */
    uint16_t bpm_x100;         /* BPM 0x2e */
    uint8_t  master_handoff;   /* Mh  0x36 */
    uint8_t  beat_within_bar;  /* Bb  0x37 */
    bool     master;
    double   effective_bpm;
} djl_mixer_status;

DJL_API djl_err djl_decode_mixer_status(const uint8_t *buf, size_t len, djl_mixer_status *out);

typedef struct {
    uint8_t  number;
    char     name[DJL_NAME_LEN + 1];
    uint32_t next_beat_ms, second_beat_ms, next_bar_ms;
    uint32_t fourth_beat_ms, second_bar_ms, eighth_beat_ms;
    uint32_t pitch;
    uint16_t bpm_x100;
    uint8_t  beat_within_bar;
    double   effective_bpm;
} djl_beat;

DJL_API djl_err djl_decode_beat(const uint8_t *buf, size_t len, djl_beat *out);

typedef struct {
    uint8_t  number;
    uint32_t track_length_s;
    uint32_t playhead_ms;
    int32_t  pitch_x100;       /* signed, /100 = percent */
    uint32_t bpm_x10;          /* 0xffffffff = unknown */
} djl_precise_position;

DJL_API djl_err djl_decode_precise_position(const uint8_t *buf, size_t len,
                                            djl_precise_position *out);

typedef struct {
    uint8_t  host_device;      /* Dr 0x27 */
    djl_slot slot;             /* Sr 0x2b */
    char     media_name[65];   /* UTF-8 from UTF-16BE at 0x2c */
    char     created[41];      /*             UTF-16BE at 0x6c */
    uint16_t track_count;      /*     0xa6 */
    uint8_t  color;            /* col 0xa8 */
    djl_track_type track_type; /* Tr  0xaa */
    bool     has_my_settings;  /* set 0xab */
    uint16_t playlist_count;   /*     0xae */
    uint64_t total_bytes;      /*     0xb0 */
    uint64_t free_bytes;       /*     0xb8 */
} djl_media_details;

DJL_API djl_err djl_decode_media_details(const uint8_t *buf, size_t len,
                                         djl_media_details *out);

/* ------------------------------------------------------------------ */
/* rekordbox LINK control channel (port 50002)                         */
/* ------------------------------------------------------------------ */

/* When the collection source is rekordbox rather than another player's USB,
 * players do not use the dbserver TCP protocol at all: metadata and files come
 * over NFS (with rekordbox as the *server*) plus this UDP control channel.
 * None of it is publicly documented; the field map below was measured from a
 * capture taken on the rekordbox host (see ARCHITECTURE.md section 1.11).
 *
 * All seven kinds share the ordinary port-50002 framing: name at 0x0b, then
 * 0x01 at 0x1f, a subtype at 0x20, the sender's device number at 0x21, and a
 * big-endian payload length at 0x22 covering everything after the 0x24-byte
 * header. */
typedef struct {
    uint8_t  kind;             /* raw kind byte at 0x0a */
    char     name[DJL_NAME_LEN + 1];
    uint8_t  subtype;          /* 0x20: 0 player->rekordbox, 1 rekordbox->player, 3 mixer */
    uint8_t  device;           /* 0x21: sender's device number (rekordbox is 0x11) */
    uint16_t payload_len;      /* 0x22, big-endian */
    bool     length_consistent;/* payload_len agrees with the datagram size */

    /* 0x11: the host computer name shown when browsing rekordbox on a player.
     * Stored UTF-16 *big*-endian, unlike the UTF-16LE that NFS paths use. */
    char     host_name[130];

    /* 0x80: the device number this notification refers to (rekordbox's). */
    uint8_t  referenced_device;
    /* 0x47: true when the 12 34 56 78 settings marker is present. */
    bool     has_settings_marker;
    /* 0x46: the 16-bit code the player answers with. */
    uint16_t reply_code;

    uint16_t payload_copied;   /* bytes available in payload below */
    uint8_t  payload[64];      /* start of the payload, for research */
} djl_rb_link;

DJL_API djl_err djl_decode_rb_link(const uint8_t *buf, size_t len, djl_rb_link *out);

/* ------------------------------------------------------------------ */
/* Context                                                             */
/* ------------------------------------------------------------------ */

typedef struct djl_context djl_context;

typedef enum {
    DJL_NUMBER_AUTO = 0,        /* set auto-assign flag, accept mixer assignment */
    DJL_NUMBER_FIXED,           /* claim exactly config.preferred_number */
    DJL_NUMBER_LOWEST_FREE      /* lowest free in 1..4, then 5..6 */
} djl_number_policy;

typedef enum {
    DJL_LOG_ERROR = 0, DJL_LOG_WARN, DJL_LOG_INFO, DJL_LOG_DEBUG, DJL_LOG_TRACE
} djl_log_level;

typedef void (*djl_log_fn)(djl_log_level lvl, const char *msg, void *ud);

typedef struct {
    const char       *interface_name;   /* required for now, e.g. "eth0" */
    const char       *device_name;      /* <= 20 bytes, default "libdjlink" */
    djl_number_policy number_policy;
    uint8_t           preferred_number;
    djl_device_type   advertise_as;     /* default DJL_DEVTYPE_CDJ */
    uint8_t           model_code;       /* byte 0x35, default 0x64 */
    uint8_t           proto_version;    /* byte 0x21, default 0x03 */
    bool              send_status;      /* emit our own CDJ status @200ms */
    bool              observe_only;     /* never transmit anything */
    bool              auto_metadata;    /* auto-fetch metadata/waveform on load */
    djl_log_fn        log;
    void             *log_ud;
    djl_log_level     log_level;
} djl_config;

DJL_API void    djl_config_defaults(djl_config *cfg);
DJL_API djl_err djl_context_create(const djl_config *cfg, djl_context **out);
DJL_API djl_err djl_context_start(djl_context *ctx);
DJL_API void    djl_context_stop(djl_context *ctx);
DJL_API void    djl_context_destroy(djl_context *ctx);

/* Our own state, once online. */
DJL_API int  djl_own_number(const djl_context *ctx);
DJL_API bool djl_is_online(const djl_context *ctx);

/* ------------------------------------------------------------------ */
/* Device roster                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  number;
    char     name[DJL_NAME_LEN + 1];
    uint8_t  ip[4];
    uint8_t  mac[6];
    uint8_t  device_type;
    uint8_t  model_code;
    uint8_t  proto_version;
    uint8_t  peer_count;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    bool     is_mixer;
    bool     is_cdj;
} djl_device_info;

#define DJL_MAX_DEVICES 32

DJL_API size_t djl_devices(djl_context *ctx, djl_device_info *out, size_t max);
DJL_API djl_err djl_device_by_number(djl_context *ctx, uint8_t number, djl_device_info *out);

/* Latest cached status per player, or DJL_ERR_NOT_FOUND. */
DJL_API djl_err djl_cdj_status_for(djl_context *ctx, uint8_t number, djl_cdj_status *out);
DJL_API int     djl_tempo_master(djl_context *ctx);
DJL_API double  djl_master_tempo(djl_context *ctx);

/* ------------------------------------------------------------------ */
/* Interpolated playback position                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  player;
    bool     valid;
    bool     definitive;      /* from a precise-position packet or a fresh beat */
    bool     playing;
    bool     reverse;
    int64_t  position_ms;     /* interpolated playhead */
    int32_t  beat;            /* -1 if unknown */
    uint8_t  beat_within_bar; /* 0 if unknown */
    double   pitch;           /* signed multiplier (negative = reverse) */
    double   effective_bpm;
    int64_t  track_length_ms; /* -1 if unknown */
} djl_position;

/* Interpolate a player's current playhead to now. On CDJ-3000-class players
 * this uses the 30 ms precise-position stream (absolute, survives scratch and
 * loop); otherwise it extrapolates from the last beat/status using pitch. */
DJL_API djl_err djl_get_position(djl_context *ctx, uint8_t player, djl_position *out);

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    DJL_EV_DEVICE_FOUND = 0,
    DJL_EV_DEVICE_LOST,
    DJL_EV_OWN_NUMBER_ASSIGNED,
    DJL_EV_NUMBER_CONFLICT,
    DJL_EV_CDJ_STATUS,
    DJL_EV_MIXER_STATUS,
    DJL_EV_BEAT,
    DJL_EV_PRECISE_POSITION,
    DJL_EV_MASTER_CHANGED,
    DJL_EV_TEMPO_CHANGED,
    DJL_EV_ON_AIR_CHANGED,
    DJL_EV_FADER_START,
    DJL_EV_MEDIA_DETAILS,
    DJL_EV_TRACK_LOADED,
    DJL_EV_AUDIO_TIMING,
    DJL_EV_POSITION,
    DJL_EV_TRACK_METADATA,
    DJL_EV_WAVEFORM,
    DJL_EV_BEAT_GRID,
    DJL_EV_CUE_LIST,
    DJL_EV_ALBUM_ART,
    DJL_EV_SIGNATURE,
    DJL_EV_SONG_STRUCTURE,
    DJL_EV_UNKNOWN_PACKET,
    DJL_EV_REKORDBOX_LINK,
    DJL_EV__COUNT
} djl_event_kind;

DJL_API const char *djl_event_kind_name(djl_event_kind k);

typedef struct {
    djl_event_kind kind;
    uint64_t       time_ms;      /* monotonic ms since context start */
    uint8_t        device;       /* device number this concerns, 0 if n/a */
    union {
        djl_device_info      device_found;
        djl_device_info      device_lost;
        djl_cdj_status       cdj_status;
        djl_mixer_status     mixer_status;
        djl_beat             beat;
        djl_precise_position precise;
        djl_media_details    media;
        struct { uint8_t old_master, new_master; }        master_changed;
        struct { double bpm; uint8_t from; }              tempo_changed;
        struct { uint8_t channel_mask; }                  on_air;
        struct { uint8_t commands[4]; }                   fader_start;
        struct { uint8_t number; }                        own_number;
        struct { uint8_t defended_by; uint8_t number; }   conflict;
        struct {
            uint8_t  player;
            uint8_t  source_player;
            djl_slot slot;
            djl_track_type type;
            uint32_t rekordbox_id;
        } track_loaded;
        struct { uint32_t counter; uint8_t link_cue; uint8_t elected; } audio_timing;
        djl_position position;
        /* For the fetched-data events below, the payload is a lightweight
         * notification; retrieve the actual bytes with the djl_get_* getters,
         * which hand back a copy the caller owns. */
        struct { uint8_t player; uint32_t rekordbox_id; } metadata;
        struct { uint8_t player; int style; bool detail; uint32_t segments; } waveform;
        struct { uint8_t player; uint32_t entries; } beat_grid;
        struct { uint8_t player; uint32_t entries; bool extended; } cue_list;
        struct { uint8_t player; uint32_t artwork_id; uint32_t length; } album_art;
        struct { uint8_t player; uint8_t sha1[20]; } signature;
        struct { uint8_t player; int mood; uint8_t bank; uint32_t phrases; } song_structure;
        struct {
            uint16_t port;
            uint8_t  kind_byte;
            uint16_t length;
            uint8_t  bytes[64];   /* first 64 bytes for research */
        } unknown;
        djl_rb_link rb_link;
    } u;
} djl_event;

/* Drain up to max events. Blocks up to timeout_ms (0 = non-blocking,
 * negative = wait indefinitely). Returns count, or negative djl_err. */
DJL_API int djl_poll(djl_context *ctx, djl_event *out, size_t max, int timeout_ms);

/* Raw packet hook: called for every datagram carrying valid Pro DJ Link magic,
 * before decoding, including packets the library does not understand. Invoked
 * on the I/O thread, so it must not block. The buffer is only valid for the
 * duration of the call. This is the escape hatch for protocol research. */
typedef void (*djl_raw_fn)(uint16_t port, djl_packet_kind kind,
                           const uint8_t *data, size_t len, void *ud);
DJL_API djl_err djl_set_raw_hook(djl_context *ctx, djl_raw_fn fn, void *ud);

/* ------------------------------------------------------------------ */
/* Control (all require an assigned device number)                      */
/* ------------------------------------------------------------------ */

DJL_API djl_err djl_query_media(djl_context *ctx, uint8_t player, djl_slot slot);
DJL_API djl_err djl_send_sync(djl_context *ctx, uint8_t player, bool on);
DJL_API djl_err djl_appoint_master(djl_context *ctx, uint8_t player);
DJL_API djl_err djl_send_on_air(djl_context *ctx, uint8_t channel_mask, bool six_channel);
DJL_API djl_err djl_send_fader_start(djl_context *ctx, uint8_t start_mask, uint8_t stop_mask);
DJL_API djl_err djl_load_track(djl_context *ctx, uint8_t target, uint8_t source_player,
                               djl_slot slot, djl_track_type type, uint32_t rekordbox_id);

/* Our advertised playback state, reflected in the status packets we send. */
DJL_API djl_err djl_set_tempo(djl_context *ctx, double bpm);
DJL_API djl_err djl_set_playing(djl_context *ctx, bool playing);
DJL_API djl_err djl_set_synced(djl_context *ctx, bool synced);
DJL_API djl_err djl_set_beat(djl_context *ctx, int32_t beat, uint8_t beat_within_bar);

/* ------------------------------------------------------------------ */
/* dbserver: metadata, waveforms, browsing (TCP)                       */
/* ------------------------------------------------------------------ */

typedef enum {
    DJL_WAVE_BLUE = 0,     /* legacy monochrome */
    DJL_WAVE_RGB,          /* nxs2 color (PWV4/PWV5) */
    DJL_WAVE_THREE_BAND    /* CDJ-3000 3-band (PWV6/PWV7) */
} djl_waveform_style;

typedef struct djl_db djl_db;

typedef struct {
    bool     found;              /* false if the track has no metadata menu */
    uint32_t rekordbox_id;
    djl_slot slot;
    djl_track_type type;
    char     title[256];
    char     artist[256];
    char     album[256];
    char     genre[128];
    char     comment[256];
    char     key[64];
    char     date_added[32];
    char     color_name[64];
    char     label[128];
    char     original_artist[256];
    char     remixer[256];
    uint32_t artist_id, album_id, genre_id, artwork_id, label_id;
    uint32_t duration_s;
    uint32_t tempo_x100;
    uint32_t year;
    uint32_t bitrate;
    uint8_t  rating;
    uint8_t  color_id;
} djl_track_info;

/* rekordbox hot-cue color code (1..0x3e) to RGB. Returns false for unknown/0. */
DJL_API bool djl_rekordbox_color(uint8_t code, uint8_t *r, uint8_t *g, uint8_t *b);

typedef struct {
    djl_waveform_style style;
    bool     detail;             /* false = preview, true = scrolling detail */
    uint32_t length;             /* bytes in data */
    uint8_t *data;               /* library-owned tag/segment bytes; free with djl_waveform_free */
} djl_waveform_blob;

typedef struct {
    uint32_t parent_id;
    uint32_t id;
    uint16_t item_type;          /* low 16 bits of the menu item type */
    char     label1[256];        /* title / filename / name */
    char     label2[256];        /* artist / secondary */
} djl_menu_row;

/* Open a dbserver connection to a player: TCP 12523 port lookup, greeting,
 * and the setup handshake posing as our own device number. Requires our number
 * to be 1..4 (or <=6 on CDJ-3000 for streaming). */
DJL_API djl_err  djl_db_open(djl_context *ctx, uint8_t player, djl_db **out);
DJL_API void     djl_db_close(djl_db *db);
DJL_API int      djl_db_target(const djl_db *db);        /* player we are talking to */
DJL_API int      djl_db_their_number(const djl_db *db);  /* number they reported in setup */
DJL_API uint16_t djl_db_port(const djl_db *db);          /* negotiated dbserver port */

/* rekordbox track metadata (message 0x2002). For non-rekordbox tracks use
 * djl_db_folder_menu / djl_db_track_list instead. */
DJL_API djl_err djl_db_track_metadata(djl_db *db, djl_slot slot, djl_track_type type,
                                      uint32_t rekordbox_id, djl_track_info *out);

/* Whole-track waveform. want selects the preferred style; the library falls
 * back to blue if the color/3-band tag is unavailable. */
DJL_API djl_err djl_db_waveform(djl_db *db, djl_slot slot, djl_track_type type,
                                uint32_t rekordbox_id, djl_waveform_style want,
                                bool detail, djl_waveform_blob *out);
DJL_API void    djl_waveform_free(djl_waveform_blob *wf);

/* Sample height 0..? and color of one waveform segment, decoded per style. */
DJL_API int     djl_waveform_segment_count(const djl_waveform_blob *wf);
/* Segment height. Range is 0..31 for blue; RGB/3-band are raw unsigned bytes,
 * so normalise against djl_waveform_max_height() for display. */
DJL_API int     djl_waveform_height(const djl_waveform_blob *wf, int segment);
DJL_API int     djl_waveform_max_height(const djl_waveform_blob *wf);
DJL_API void    djl_waveform_rgb(const djl_waveform_blob *wf, int segment,
                                 uint8_t *r, uint8_t *g, uint8_t *b);

/* Beat grid: time and bar position of every beat in the track. */
typedef struct {
    uint16_t beat_within_bar;   /* 1..4 */
    uint16_t tempo_x100;
    uint32_t time_ms;
} djl_beat_grid_entry;

typedef struct {
    uint32_t count;
    djl_beat_grid_entry *entries;   /* library-owned */
} djl_beat_grid;

DJL_API djl_err djl_db_beat_grid(djl_db *db, djl_slot slot, djl_track_type type,
                                 uint32_t rekordbox_id, djl_beat_grid *out);
DJL_API void    djl_beat_grid_free(djl_beat_grid *g);

/* Memory points, loops and hot cues. */
typedef struct {
    bool     is_loop;
    uint8_t  hot_cue;           /* 0 = memory point, 1..8 = A..H */
    uint32_t start_ms;
    uint32_t end_ms;            /* loop end, if is_loop */
    bool     has_color;
    uint8_t  color_id;          /* rekordbox color-table row (nxs2) */
    uint8_t  r, g, b;           /* embedded color, if present */
    char     comment[256];      /* nxs2 only */
} djl_cue_entry;

typedef struct {
    uint32_t count;
    bool     extended;          /* true if from the nxs2 request (colors/comments) */
    djl_cue_entry *entries;     /* library-owned */
} djl_cue_list;

DJL_API djl_err djl_db_cue_list(djl_db *db, djl_slot slot, djl_track_type type,
                                uint32_t rekordbox_id, bool extended, djl_cue_list *out);
DJL_API void    djl_cue_list_free(djl_cue_list *c);

/* Album art (JPEG). Use artwork_id from djl_track_info. */
typedef struct { uint32_t length; uint8_t *data; } djl_blob;
DJL_API djl_err djl_db_album_art(djl_db *db, djl_slot slot, djl_track_type type,
                                 uint32_t artwork_id, djl_blob *out);
DJL_API void    djl_blob_free(djl_blob *b);

/* Song structure / phrase analysis (PSSI). rekordbox 6+ phrase-analyzed tracks. */
typedef enum {
    DJL_MOOD_UNKNOWN = 0, DJL_MOOD_HIGH = 1, DJL_MOOD_MID = 2, DJL_MOOD_LOW = 3
} djl_track_mood;

typedef struct {
    uint16_t index;        /* phrase number, from 1 */
    uint16_t beat;         /* beat at which the phrase starts */
    uint16_t kind;         /* raw phrase kind id (interpretation depends on mood) */
    char     label[24];    /* human phrase name (Intro/Verse/Chorus/Up/Down/Bridge/Outro) */
} djl_phrase;

typedef struct {
    djl_track_mood mood;
    uint8_t   bank;        /* rekordbox Lighting stylistic bank (0..8) */
    uint16_t  end_beat;    /* beat at which the last phrase ends */
    uint32_t  count;
    djl_phrase *phrases;   /* library-owned */
    uint8_t   sha1[20];    /* SHA-1 of the raw tag body (for Opus/PSSI matching) */
    uint32_t  raw_len;     /* length of the deobfuscated body below */
    uint8_t  *raw;         /* deobfuscated song-structure body, library-owned */
} djl_song_structure;

DJL_API djl_err djl_db_song_structure(djl_db *db, djl_slot slot, djl_track_type type,
                                      uint32_t rekordbox_id, djl_song_structure *out);
DJL_API void    djl_song_structure_free(djl_song_structure *s);
DJL_API const char *djl_phrase_label(djl_track_mood mood, uint16_t kind);

/* Parse a raw PSSI tag body (len_entry_bytes, len_entries, then the possibly
 * XOR-masked song-structure body) into phrases. Deobfuscates when needed and
 * fills sha1 with the SHA-1 of the raw body (for matching). Pure; usable on a
 * PSSI blob obtained from any source (dbserver, NFS/ANLZ, or an Opus reply). */
DJL_API djl_err djl_parse_song_structure(const uint8_t *tag_body, size_t len,
                                         djl_song_structure *out);

/* Browse the raw filesystem (folder_id 0xffffffff = root) or the whole track
 * list of a slot. Fills up to max rows, sets *count. */
DJL_API djl_err djl_db_folder_menu(djl_db *db, djl_slot slot, djl_track_type type,
                                   uint32_t folder_id, djl_menu_row *out,
                                   size_t max, size_t *count);
DJL_API djl_err djl_db_track_list(djl_db *db, djl_slot slot, djl_track_type type,
                                  djl_menu_row *out, size_t max, size_t *count);

/* Top-level menu of a slot (Artist/Album/Track/Folder/...); confirms content
 * queries work and shows what the media offers. */
DJL_API djl_err djl_db_root_menu(djl_db *db, djl_slot slot, djl_track_type type,
                                 djl_menu_row *out, size_t max, size_t *count);

/* ------------------------------------------------------------------ */
/* Cached metadata (populated automatically when cfg.auto_metadata is set)   */
/* Each getter hands back a copy the caller owns; free the variable-size ones */
/* with the matching djl_*_free. Return DJL_ERR_NOT_FOUND if not fetched yet. */
/* ------------------------------------------------------------------ */

DJL_API djl_err djl_get_metadata (djl_context *ctx, uint8_t player, djl_track_info *out);
DJL_API djl_err djl_get_waveform (djl_context *ctx, uint8_t player, djl_waveform_blob *out);
DJL_API djl_err djl_get_beat_grid(djl_context *ctx, uint8_t player, djl_beat_grid *out);
DJL_API djl_err djl_get_cue_list (djl_context *ctx, uint8_t player, djl_cue_list *out);
DJL_API djl_err djl_get_album_art(djl_context *ctx, uint8_t player, djl_blob *out);
DJL_API djl_err djl_get_signature(djl_context *ctx, uint8_t player, uint8_t out_sha1[20]);
DJL_API djl_err djl_get_song_structure(djl_context *ctx, uint8_t player, djl_song_structure *out);

/* SHA-1 track signature over title, artist, duration, the RGB waveform detail,
 * and the beat grid (same inputs as beat-link's SignatureFinder, used for
 * track identification). Any of rgb_detail/grid may be NULL, which changes the
 * result; a full signature needs both. */
DJL_API djl_err djl_track_signature(const char *title, const char *artist,
                                    uint32_t duration_s,
                                    const djl_waveform_blob *rgb_detail,
                                    const djl_beat_grid *grid, uint8_t out_sha1[20]);

/* ------------------------------------------------------------------ */
/* ANLZ: rekordbox analysis files (.DAT / .EXT / .2EX). Pure parser.    */
/* ------------------------------------------------------------------ */

/* Everything a set of ANLZ files can tell us about one track. Parsing is
 * additive: call djl_anlz_parse once per file (.DAT, then .EXT, then .2EX) on
 * the same struct and the richer tags supersede the poorer ones the way
 * rekordbox intends (PCO2 over PCOB, 3-band/RGB over blue). */
typedef struct {
    bool has_grid;    djl_beat_grid      grid;      /* PQTZ */
    bool has_cues;    djl_cue_list       cues;      /* PCOB / PCO2 */
    bool has_ss;      djl_song_structure ss;        /* PSSI */
    bool has_preview; djl_waveform_blob  preview;   /* best: PWV6 > PWV4 > PWAV */
    bool has_detail;  djl_waveform_blob  detail;    /* best: PWV7 > PWV5 > PWV3 */
    /* The track signature is defined over the RGB detail waveform specifically,
     * so keep PWV5 even when the 3-band PWV7 supersedes it for display. */
    bool has_rgb_detail; djl_waveform_blob rgb_detail;
    char path[512];                                 /* PPTH: audio file path */
} djl_anlz;

/* Parse one PMAI file, merging into *inout (zero it before the first call). */
DJL_API djl_err djl_anlz_parse(const uint8_t *data, size_t len, djl_anlz *inout);
DJL_API void    djl_anlz_free(djl_anlz *a);

/* ------------------------------------------------------------------ */
/* DeviceSQL export.pdb reader. Pure; borrows the caller's buffer.      */
/* ------------------------------------------------------------------ */

typedef struct djl_pdb djl_pdb;

/* data must stay valid and unmodified until djl_pdb_close. */
DJL_API djl_err djl_pdb_open(const uint8_t *data, size_t len, djl_pdb **out);
DJL_API void    djl_pdb_close(djl_pdb *p);

DJL_API size_t  djl_pdb_track_count(const djl_pdb *p);
DJL_API djl_err djl_pdb_track_id_at(const djl_pdb *p, size_t index, uint32_t *out_id);

/* Full metadata for one track id, with cross-table names resolved. If
 * anlz_path is non-NULL it receives the .DAT analysis path from the row. */
DJL_API djl_err djl_pdb_track(const djl_pdb *p, uint32_t track_id,
                              djl_track_info *out, char *anlz_path, size_t anlz_path_sz);

/* Album-art file path for an artwork id (from djl_track_info.artwork_id). */
DJL_API djl_err djl_pdb_artwork_path(const djl_pdb *p, uint32_t artwork_id,
                                     char *out, size_t outsz);

/* ------------------------------------------------------------------ */
/* NFS client: read a player's own USB/SD directly (DJL_WITH_NFS)       */
/* ------------------------------------------------------------------ */

typedef struct djl_nfs djl_nfs;

/* False if the library was built without DJL_WITH_NFS, in which case every
 * djl_nfs_* call returns DJL_ERR_UNAVAILABLE. */
DJL_API bool djl_nfs_supported(void);

/* Mount a player's media over NFS. slot must be DJL_SLOT_SD or DJL_SLOT_USB.
 * Works at any device number, including when four real players occupy 1..4,
 * and needs no dbserver. Blocking; call from a worker thread. */
DJL_API djl_err djl_nfs_open(djl_context *ctx, uint8_t player, djl_slot slot, djl_nfs **out);
DJL_API djl_err djl_nfs_open_addr(const uint8_t ip[4], djl_slot slot, djl_nfs **out);
DJL_API void    djl_nfs_close(djl_nfs *n);

/* Read a whole file, e.g. "PIONEER/rekordbox/export.pdb". Paths are relative
 * to the slot root; the library handles the UTF-16LE encoding Pioneer's NFS
 * requires and the hidden ".PIONEER" folder on HFS+ media. */
DJL_API djl_err djl_nfs_read_file(djl_nfs *n, const char *path, djl_blob *out);

typedef struct {
    char     name[256];
    uint32_t fileid;
} djl_nfs_dirent;

DJL_API djl_err djl_nfs_list_dir(djl_nfs *n, const char *path,
                                 djl_nfs_dirent *out, size_t max, size_t *count);

/* The slot's export.pdb, downloaded and parsed on first use, then cached in
 * the handle. The returned reader stays owned by the handle. */
DJL_API djl_err djl_nfs_pdb(djl_nfs *n, const djl_pdb **out);

typedef struct {
    bool     has_meta;
    djl_track_info meta;
    djl_anlz anlz;
    char     anlz_path[512];   /* the .DAT path the PDB pointed at */
} djl_nfs_track;

/* Resolve a rekordbox track id through export.pdb, then download and parse its
 * .DAT, .EXT and .2EX analysis files. This is the one call that yields
 * metadata + beat grid + cues + phrases + waveforms from a single source. */
DJL_API djl_err djl_nfs_fetch_track(djl_nfs *n, uint32_t track_id, djl_nfs_track *out);
DJL_API void    djl_nfs_track_free(djl_nfs_track *t);

/* Album art (JPEG) for an artwork id, read straight off the media. */
DJL_API djl_err djl_nfs_read_artwork(djl_nfs *n, uint32_t artwork_id, djl_blob *out);

/* ------------------------------------------------------------------ */
/* Metadata provider order                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    DJL_PROVIDER_NONE     = 0,
    DJL_PROVIDER_DBSERVER = 1,   /* TCP; needs our number in 1..4 (or <=6) */
    DJL_PROVIDER_NFS      = 2    /* export.pdb + ANLZ; any device number */
} djl_provider_kind;

#define DJL_MAX_PROVIDERS 4

/* Order the auto-fetch manager tries providers in. Default is NFS then
 * dbserver: NFS works regardless of device number and returns PSSI reliably,
 * while dbserver is the only source for CD audio and streaming tracks. */
DJL_API djl_err djl_metadata_set_provider_order(djl_context *ctx,
                                                const djl_provider_kind *order, size_t n);
DJL_API size_t  djl_metadata_get_provider_order(djl_context *ctx,
                                                djl_provider_kind *out, size_t max);

#ifdef __cplusplus
}
#endif
#endif /* DJLINK_H */
