/* libdjlink outbound packet templates.
 *
 * Static byte templates patched in place, mirroring beat-link's approach so
 * that output can be compared byte-for-byte against hardware captures.
 * All offsets are absolute packet addresses as used in the dysentery docs.
 */
#include "djl_internal.h"
#include <string.h>
#include <math.h>

#define MAGIC0 0x51,0x73,0x70,0x74,0x31,0x57,0x6d,0x4a,0x4f,0x4c

/* Announcement framing: magic, kind, subtype, then 20-byte name at 0x0c. */
static size_t hdr_announce(uint8_t *buf, size_t cap, uint8_t kind, uint8_t subtype,
                           const djl_identity *id)
{
    if (cap < 0x20) return 0;
    static const uint8_t m[DJL_MAGIC_LEN] = { MAGIC0 };
    memcpy(buf, m, DJL_MAGIC_LEN);
    buf[0x0a] = kind;
    buf[0x0b] = subtype;
    memset(buf + 0x0c, 0, DJL_NAME_LEN);
    memcpy(buf + 0x0c, id->name, strnlen(id->name, DJL_NAME_LEN));
    return 0x20;
}

/* Status/beat/control framing: magic, kind, then 20-byte name at 0x0b. */
static size_t hdr_status(uint8_t *buf, size_t cap, uint8_t kind,
                         const djl_identity *id)
{
    if (cap < 0x1f) return 0;
    static const uint8_t m[DJL_MAGIC_LEN] = { MAGIC0 };
    memcpy(buf, m, DJL_MAGIC_LEN);
    buf[0x0a] = kind;
    memset(buf + 0x0b, 0, DJL_NAME_LEN);
    memcpy(buf + 0x0b, id->name, strnlen(id->name, DJL_NAME_LEN));
    return 0x1f;
}

static void put_be16(uint8_t *b, size_t off, uint16_t v)
{
    b[off]     = (uint8_t)(v >> 8);
    b[off + 1] = (uint8_t)(v & 0xff);
}

static void put_be32(uint8_t *b, size_t off, uint32_t v)
{
    b[off]     = (uint8_t)(v >> 24);
    b[off + 1] = (uint8_t)(v >> 16);
    b[off + 2] = (uint8_t)(v >> 8);
    b[off + 3] = (uint8_t)(v & 0xff);
}

/* ---------------- port 50000 ---------------- */

size_t djl_build_hello(uint8_t *buf, size_t cap, const djl_identity *id)
{
    /* CDJ-3000-compatible hello: one byte longer than the legacy form,
     * byte 0x21 = 0x04, trailing 0x01 0x40. See startup.adoc. */
    const size_t total = 0x26;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_announce(buf, cap, 0x0a, 0x00, id)) return 0;
    buf[0x20] = 0x01;
    buf[0x21] = 0x04;
    put_be16(buf, 0x22, (uint16_t)total);
    buf[0x24] = 0x01;
    buf[0x25] = 0x40;
    return total;
}

size_t djl_build_claim1(uint8_t *buf, size_t cap, const djl_identity *id, uint8_t n)
{
    const size_t total = 0x2c;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_announce(buf, cap, 0x00, 0x00, id)) return 0;
    buf[0x20] = 0x01;
    buf[0x21] = id->proto_version;
    put_be16(buf, 0x22, (uint16_t)total);
    buf[0x24] = n;              /* packet counter 1..3 */
    buf[0x25] = 0x01;           /* CDJ */
    memcpy(buf + 0x26, id->mac, 6);
    return total;
}

size_t djl_build_claim2(uint8_t *buf, size_t cap, const djl_identity *id,
                        uint8_t claim, uint8_t n, bool auto_assign)
{
    const size_t total = 0x32;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_announce(buf, cap, 0x02, 0x00, id)) return 0;
    buf[0x20] = 0x01;
    buf[0x21] = id->proto_version;
    put_be16(buf, 0x22, (uint16_t)total);
    memcpy(buf + 0x24, id->ip, 4);
    memcpy(buf + 0x28, id->mac, 6);
    buf[0x2e] = claim;
    buf[0x2f] = n;
    buf[0x30] = 0x01;
    buf[0x31] = auto_assign ? 0x01 : 0x02;
    return total;
}

size_t djl_build_claim3(uint8_t *buf, size_t cap, const djl_identity *id,
                        uint8_t claim, uint8_t n)
{
    const size_t total = 0x26;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_announce(buf, cap, 0x04, 0x00, id)) return 0;
    buf[0x20] = 0x01;
    buf[0x21] = id->proto_version;
    put_be16(buf, 0x22, (uint16_t)total);
    buf[0x24] = claim;
    buf[0x25] = n;
    return total;
}

size_t djl_build_keep_alive(uint8_t *buf, size_t cap, const djl_identity *id)
{
    const size_t total = 0x36;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_announce(buf, cap, 0x06, 0x00, id)) return 0;
    buf[0x20] = 0x01;
    buf[0x21] = 0x02;               /* keep-alive stays at 0x02 even for 3000 */
    put_be16(buf, 0x22, (uint16_t)total);
    buf[0x24] = id->number;
    buf[0x25] = 0x01;               /* we joined an existing network */
    memcpy(buf + 0x26, id->mac, 6);
    memcpy(buf + 0x2c, id->ip, 4);
    buf[0x30] = id->peer_count ? id->peer_count : 1;
    buf[0x34] = id->device_type;
    buf[0x35] = id->model_code;
    return total;
}

/* Pro DJ Link Bridge keepalive (0x06, 54 bytes, broadcast on 50000).
 *
 * A DJM only starts streaming its fader-status (0x39) and VU (0x58) once it has
 * seen this bridge identity broadcast and then received a subscribe (below).
 * The DJM-A9 is strict: player byte must be 0xF9 and byte 0x30 must be 0x04, or
 * it silently ignores the subscribe (SuperTimecodeConverter, verified live).
 * Device type 0x01 marks us as a bridge/lighting controller, distinct from our
 * virtual-CDJ keepalive, so both identities can coexist from one host. */
#define DJL_BRIDGE_PLAYER 0xF9

size_t djl_build_bridge_keep_alive(uint8_t *buf, size_t cap, const djl_identity *id)
{
    const size_t total = 0x36;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_announce(buf, cap, 0x06, 0x00, id)) return 0;
    buf[0x20] = 0x01;
    buf[0x21] = 0x01;               /* subtype: bridge */
    put_be16(buf, 0x22, (uint16_t)total);
    buf[0x24] = DJL_BRIDGE_PLAYER;
    buf[0x25] = 0x00;
    memcpy(buf + 0x26, id->mac, 6);
    memcpy(buf + 0x2c, id->ip, 4);
    buf[0x30] = 0x04;              /* A9 requires 0x04 here, not 0x03 */
    buf[0x34] = 0x05;
    buf[0x35] = 0x20;
    return total;
}

/* Bridge subscribe (0x57, 40 bytes, unicast to a DJM's port 50001).
 *
 * Byte 0x21 is the subscription bitmask; 0x87 requests faders + VU (the value
 * the reference bridge uses on non-Apple hosts). Should be sent from an
 * ephemeral source port, not 50001/50002, because some DJM firmware ignores a
 * subscribe whose source port matches a port it also sends data on. */
#define DJL_BRIDGE_SUB_MASK 0x87

size_t djl_build_bridge_subscribe(uint8_t *buf, size_t cap, const djl_identity *id)
{
    const size_t total = 0x28;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_status(buf, cap, 0x57, id)) return 0;   /* name at 0x0b */
    buf[0x1f] = 0x01;
    buf[0x20] = 0x00;
    buf[0x21] = DJL_BRIDGE_SUB_MASK;
    buf[0x22] = 0x00;
    buf[0x23] = 0x04;             /* subtype */
    buf[0x24] = 0x01;             /* subscribe = 1 */
    return total;
}

/* ---------------- Stagehand persona (port 50000, broadcast) ----------------
 *
 * Reproduces the Pioneer Stagehand iPad join, byte-for-byte per dysentery's
 * stagehand.adoc and alphatheta-connect's virtualcdj/stagehand.ts. A DJM-A9
 * unicasts its 0x39 fader status and 0x58 VU streams to any device that
 * announces this persona (device type 0x05, model code 0x20) — no subscribe
 * packet is needed, unlike the 0xF9 bridge. See ARCHITECTURE.md section 1.14. */

#define DJL_STAGEHAND_TYPE   0x05   /* keep-alive byte 0x34 / trailing announce byte */
#define DJL_STAGEHAND_MODEL  0x20   /* keep-alive byte 0x35 */
#define DJL_STAGEHAND_SYMBOL 0x3a   /* symbolic device number claimed at 0x2e */
#define DJL_STAGEHAND_PROTO  0x03   /* byte 0x21: CDJ-3000-era protocol marker */

/* Initial announcement (0x0a, 37 bytes). Structurally a pre-3000 announce but
 * byte 0x21 = 0x03 and the trailing device-type byte 0x24 = 0x05. */
size_t djl_build_stagehand_announce(uint8_t *buf, size_t cap, const djl_identity *id)
{
    const size_t total = 0x25;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_announce(buf, cap, 0x0a, 0x00, id)) return 0;
    buf[0x20] = 0x01;
    buf[0x21] = DJL_STAGEHAND_PROTO;
    put_be16(buf, 0x22, (uint16_t)total);
    buf[0x24] = DJL_STAGEHAND_TYPE;
    return total;
}

/* Device-number claim (0x02, 50 bytes). Byte 0x2e is always the symbolic 0x3a,
 * 0x2f is the 1..3 iteration counter, 0x30 the device type, 0x31 = 0x01. */
size_t djl_build_stagehand_claim(uint8_t *buf, size_t cap, const djl_identity *id,
                                 uint8_t n)
{
    const size_t total = 0x32;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_announce(buf, cap, 0x02, 0x00, id)) return 0;
    buf[0x20] = 0x01;
    buf[0x21] = DJL_STAGEHAND_PROTO;
    put_be16(buf, 0x22, (uint16_t)total);
    memcpy(buf + 0x24, id->ip, 4);
    memcpy(buf + 0x28, id->mac, 6);
    buf[0x2e] = DJL_STAGEHAND_SYMBOL;
    buf[0x2f] = n;
    buf[0x30] = DJL_STAGEHAND_TYPE;
    buf[0x31] = 0x01;
    return total;
}

/* Keep-alive (0x06, 54 bytes). Same envelope as a CDJ keep-alive but the
 * runtime device number (141..211) sits at 0x24, byte 0x34 = 0x05 and byte
 * 0x35 = 0x20. Byte 0x30 (peer count slot) is 1 and 0x33..0x30 = 01 00 00 00. */
size_t djl_build_stagehand_keep_alive(uint8_t *buf, size_t cap, const djl_identity *id)
{
    const size_t total = 0x36;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_announce(buf, cap, 0x06, 0x00, id)) return 0;
    buf[0x20] = 0x01;
    buf[0x21] = DJL_STAGEHAND_PROTO;
    put_be16(buf, 0x22, (uint16_t)total);
    buf[0x24] = id->number;             /* runtime number, not the symbolic 0x3a */
    buf[0x25] = 0x01;                   /* joined an existing network */
    memcpy(buf + 0x26, id->mac, 6);
    memcpy(buf + 0x2c, id->ip, 4);
    buf[0x30] = 0x01;
    buf[0x34] = DJL_STAGEHAND_TYPE;
    buf[0x35] = DJL_STAGEHAND_MODEL;
    return total;
}

/* ---------------- Stagehand remote control ----------------
 *
 * Transport (0x07, 56 bytes, port 50001) and preference write (0x6b, 124
 * bytes, port 50002). Status framing: name at 0x0b. Offsets follow dysentery's
 * stagehand.adoc single-control captures. The target CDJ's device number sits
 * at the unicast peer-marker slot 0x1e, mirroring the Stagehand->A9 command
 * frame. */

/* The target CDJ is addressed by the unicast destination IP, not by any field
 * in the packet (the reference alphatheta-connect carries no device number here).
 * Offsets match alphatheta-connect exactly: dysentery's stagehand.adoc listed the
 * action one byte lower, but the working reference and live testing put it at
 * 0x2c / 0x2e. */
size_t djl_build_transport(uint8_t *buf, size_t cap, const djl_identity *id,
                           uint8_t op, bool press, uint8_t corr)
{
    const size_t total = 0x38;          /* 56 bytes */
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_status(buf, cap, 0x07, id)) return 0;   /* name at 0x0b */
    buf[0x1f] = 0x01;
    buf[0x20] = DJL_STAGEHAND_PROTO;    /* 0x03 */
    buf[0x21] = corr;                   /* per-session correlation / view hash */
    put_be16(buf, 0x22, 0x0030);        /* len_r: 48 bytes follow */
    buf[0x28] = DJL_STAGEHAND_SYMBOL;   /* 0x3a sub-id */
    buf[0x2c] = op;                     /* action byte */
    buf[0x2e] = press ? 0x01 : 0x00;    /* press / release */
    return total;
}

size_t djl_build_pref_write(uint8_t *buf, size_t cap, const djl_identity *id,
                            uint8_t on_air, uint8_t quantize)
{
    const size_t total = 0x7c;          /* 124 bytes */
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_status(buf, cap, 0x6b, id)) return 0;   /* name at 0x0b */
    buf[0x1e] = DJL_STAGEHAND_PROTO;    /* 0x03: reference forces the last name byte */
    buf[0x1f] = 0x01;
    buf[0x20] = DJL_STAGEHAND_PROTO;    /* 0x03 */
    buf[0x21] = DJL_STAGEHAND_SYMBOL;   /* 0x3a sub-id */
    put_be16(buf, 0x22, 0x0050);        /* body length: 80 bytes */
    buf[0x24] = 0x01;                   /* transaction flag: write */
    if (on_air)   buf[0x2c] = on_air;   /* 0x80 OFF, 0x81 ON, 0 = untouched */
    if (quantize) buf[0x3c] = quantize; /* 0x80 | enum_index, 0 = untouched */
    return total;
}

size_t djl_build_number_in_use(uint8_t *buf, size_t cap, const djl_identity *id,
                               uint8_t defended, const uint8_t ip[4])
{
    const size_t total = 0x29;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_announce(buf, cap, 0x08, 0x00, id)) return 0;
    buf[0x20] = 0x01;
    buf[0x21] = 0x02;
    put_be16(buf, 0x22, (uint16_t)total);
    buf[0x24] = defended;
    memcpy(buf + 0x25, ip, 4);
    return total;
}

/* ---------------- port 50002: CDJ status ---------------- */

/* 253-byte payload following the device name, giving a 284 (0x11c) byte
 * packet: an nxs2-class status packet. Transcribed from beat-link's
 * VirtualCdj.STATUS_PAYLOAD. Payload index = packet address - 0x1f. */
static const uint8_t STATUS_PAYLOAD[253] = {
    0x01,
    /* 0x020 */ 0x04,0x00,0x00,0xf8,0x00,0x00,0x01,0x00,0x00,0x03,0x01,0x00,0x00,0x00,0x00,0x01,
    /* 0x030 */ 0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0xa0,0x00,0x00,0x00,0x00,0x00,
    /* 0x040 */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* 0x050 */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* 0x060 */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x04,0x04,0x00,0x00,0x00,0x04,
    /* 0x070 */ 0x00,0x00,0x00,0x04,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x31,0x2e,0x34,0x33,
    /* 0x080 */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x00,0x00,0x10,0x00,0x00,
    /* 0x090 */ 0x80,0x00,0x00,0x00,0x7f,0xff,0xff,0xff,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,
    /* 0x0a0 */ 0x00,0x00,0x00,0x00,0x01,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* 0x0b0 */ 0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* 0x0c0 */ 0x00,0x10,0x00,0x00,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x0f,0x01,0x00,0x00,
    /* 0x0d0 */ 0x12,0x34,0x56,0x78,0x00,0x00,0x00,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,
    /* 0x0e0 */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* 0x0f0 */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* 0x100 */ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* 0x110 */ 0x00,0x00,0x00,0x15,0x00,0x00,0x07,0x61,0x00,0x00,0x06,0x2f
};

/* Patch a byte using the absolute packet address from the protocol docs. */
static void set_at(uint8_t *pkt, size_t addr, uint8_t v) { pkt[addr] = v; }

size_t djl_build_status(uint8_t *buf, size_t cap, const djl_identity *id,
                        bool playing, bool master, bool synced, bool on_air,
                        double tempo, int32_t beat, uint8_t beat_within_bar,
                        uint32_t sync_counter, uint8_t next_master,
                        uint32_t packet_counter)
{
    const size_t total = 0x1f + sizeof STATUS_PAYLOAD;   /* 0x11c = 284 */
    if (cap < total) return 0;
    if (!hdr_status(buf, cap, 0x0a, id)) return 0;
    memcpy(buf + 0x1f, STATUS_PAYLOAD, sizeof STATUS_PAYLOAD);

    set_at(buf, 0x21, id->number);                    /* D */
    set_at(buf, 0x24, id->number);                    /* D (redundant copy) */
    set_at(buf, 0x27, playing ? 1 : 0);               /* A  activity */
    set_at(buf, 0x28, id->number);                    /* Dr track source device */
    set_at(buf, 0x7b, playing ? 3 : 5);               /* P1 */

    put_be32(buf, 0x84, sync_counter);                /* Sync_n */

    uint8_t f = 0x84;
    if (playing) f |= DJL_F_PLAY;
    if (master)  f |= DJL_F_MASTER;
    if (synced)  f |= DJL_F_SYNC;
    if (on_air)  f |= DJL_F_ON_AIR;
    set_at(buf, 0x89, f);                             /* F */

    set_at(buf, 0x8b, playing ? 0x7a : 0x7e);         /* P2 */

    long bpm = lround(tempo * 100.0);
    if (bpm < 0) bpm = 0;
    if (bpm > 0xfffe) bpm = 0xfffe;
    put_be16(buf, 0x92, (uint16_t)bpm);               /* BPM */

    set_at(buf, 0x9d, playing ? 9 : 1);               /* P3 */
    set_at(buf, 0x9e, master ? 1 : 0);                /* Mm */
    set_at(buf, 0x9f, next_master);                   /* Mh */

    put_be32(buf, 0xa0, (uint32_t)(beat < 0 ? 0 : beat));   /* Beat */
    set_at(buf, 0xa6, beat_within_bar);                     /* Bb */

    put_be32(buf, 0xc8, packet_counter);              /* packet counter */
    return total;
}

/* ---------------- port 50001 / 50002 control ---------------- */

size_t djl_build_media_query(uint8_t *buf, size_t cap, const djl_identity *id,
                             uint8_t target, uint8_t slot)
{
    const size_t total = 0x30;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_status(buf, cap, 0x05, id)) return 0;
    buf[0x1f] = 0x01;
    buf[0x20] = 0x00;
    buf[0x21] = id->number;
    put_be16(buf, 0x22, 0x000c);       /* len_r */
    memcpy(buf + 0x24, id->ip, 4);     /* where to send the response */
    buf[0x2b] = target;                /* Dr */
    buf[0x2f] = slot;                  /* Sr */
    return total;
}

size_t djl_build_sync_control(uint8_t *buf, size_t cap, const djl_identity *id, uint8_t s)
{
    const size_t total = 0x2c;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_status(buf, cap, 0x2a, id)) return 0;
    buf[0x1f] = 0x01;
    buf[0x20] = 0x00;
    buf[0x21] = id->number;
    put_be16(buf, 0x22, 0x0008);       /* len_r */
    buf[0x27] = id->number;
    buf[0x2b] = s;                     /* 0x10 on, 0x20 off, 0x01 become master */
    return total;
}

size_t djl_build_on_air(uint8_t *buf, size_t cap, const djl_identity *id,
                        uint8_t mask, bool six)
{
    const size_t total = six ? 0x35 : 0x2d;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_status(buf, cap, 0x03, id)) return 0;
    buf[0x1f] = 0x01;
    buf[0x20] = six ? 0x03 : 0x00;
    buf[0x21] = id->number;
    put_be16(buf, 0x22, six ? 0x0011 : 0x0009);
    for (int i = 0; i < 4; i++)
        buf[0x24 + i] = (mask & (1u << i)) ? 0x01 : 0x00;
    if (six) {
        buf[0x2e] = (mask & (1u << 4)) ? 0x01 : 0x00;
        buf[0x2f] = (mask & (1u << 5)) ? 0x01 : 0x00;
    }
    return total;
}

size_t djl_build_fader_start(uint8_t *buf, size_t cap, const djl_identity *id,
                             const uint8_t cmds[4])
{
    const size_t total = 0x2d;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_status(buf, cap, 0x02, id)) return 0;
    buf[0x1f] = 0x01;
    buf[0x20] = 0x00;
    buf[0x21] = id->number;
    put_be16(buf, 0x22, 0x0004);
    memcpy(buf + 0x24, cmds, 4);
    return total;
}

size_t djl_build_load_track(uint8_t *buf, size_t cap, const djl_identity *id,
                            uint8_t target, uint8_t src_player, uint8_t slot,
                            uint8_t type, uint32_t rekordbox_id)
{
    const size_t total = 0x53;
    if (cap < total) return 0;
    memset(buf, 0, total);
    if (!hdr_status(buf, cap, 0x19, id)) return 0;
    buf[0x1f] = 0x01;
    buf[0x20] = 0x00;
    buf[0x21] = id->number;
    put_be16(buf, 0x22, 0x0034);       /* len_r */
    buf[0x24] = id->number;
    buf[0x28] = src_player;            /* Dr */
    buf[0x29] = slot;                  /* Sr */
    buf[0x2a] = type;                  /* Tr */
    put_be32(buf, 0x2c, rekordbox_id);
    buf[0x33] = 0x32;
    buf[0x40] = (uint8_t)(target > 0 ? target - 1 : 0);   /* zero-based dest */
    return total;
}
