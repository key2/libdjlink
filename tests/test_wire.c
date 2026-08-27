/* libdjlink wire codec tests.
 *
 * Golden vectors captured from live hardware on 2026-08-26:
 * two CDJ-3000X players and a DJM-A9 mixer.
 */
#include "djlink.h"
#include "vector_cdj3000x.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int djl_test_failures = 0;
int djl_test_checks = 0;
#define failures djl_test_failures
#define checks   djl_test_checks

/* Implemented in test_nfs.c: NFS/RPC/XDR, PDB and ANLZ codecs. */
void djl_test_nfs(void);
/* Implemented in test_position.c: beat-grid position tracking. */
void djl_test_position(void);
/* Implemented in test_rblink.c: rekordbox LINK control channel. */
void djl_test_rblink(void);

#define CHECK(cond, ...) do {                            \
    checks++;                                            \
    if (!(cond)) {                                       \
        failures++;                                      \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);       \
        printf(__VA_ARGS__);                             \
        printf("\n");                                    \
    }                                                    \
} while (0)

#define CHECK_EQ_U(a, b) do {                                          \
    checks++;                                                          \
    unsigned long long _a = (unsigned long long)(a);                   \
    unsigned long long _b = (unsigned long long)(b);                   \
    if (_a != _b) {                                                    \
        failures++;                                                    \
        printf("FAIL %s:%d: %s = %llu, expected %llu\n",                \
               __FILE__, __LINE__, #a, _a, _b);                        \
    }                                                                  \
} while (0)

#define CHECK_STR(a, b) do {                                           \
    checks++;                                                          \
    if (strcmp((a), (b)) != 0) {                                       \
        failures++;                                                    \
        printf("FAIL %s:%d: %s = \"%s\", expected \"%s\"\n",            \
               __FILE__, __LINE__, #a, (a), (b));                      \
    }                                                                  \
} while (0)

static size_t unhex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = hex; p[0] && p[1] && n < cap; p += 2) {
        char b[3] = { p[0], p[1], 0 };
        out[n++] = (uint8_t)strtoul(b, NULL, 16);
    }
    return n;
}

/* ---- real keep-alive from CDJ-3000X player 1, 169.254.7.185 ---- */
static const char *KA_CDJ1 =
    "5173707431576d4a4f4c060043444a2d3330303058000000000000000000000001"
    "03003601012497ed3e4c79a9fe07b90300000001e4";

/* ---- real keep-alive from DJM-A9 mixer, 169.254.116.4 ---- */
static const char *KA_MIXER =
    "5173707431576d4a4f4c0600444a4d2d41390000000000000000000000000000"
    "010200362102c83dfc1f7404a9fe74040300000003b1";

/* ---- real keep-alive from CDJ-3000X player 2, 169.254.7.162 ---- */
static const char *KA_CDJ2 =
    "5173707431576d4a4f4c060043444a2d3330303058000000000000000000000001"
    "03003602012497ed3e546ea9fe07a20300000001e4";

static void test_magic(void)
{
    uint8_t buf[64];
    size_t n = unhex(KA_CDJ1, buf, sizeof buf);
    CHECK(djl_wire_has_magic(buf, n), "magic should be recognized");

    uint8_t bad[16];
    memcpy(bad, buf, 16);
    bad[3] ^= 0xff;
    CHECK(!djl_wire_has_magic(bad, 16), "corrupted magic must be rejected");

    /* Too short must not read out of bounds. */
    CHECK(!djl_wire_has_magic(buf, 5), "short buffer must be rejected");
    CHECK(!djl_wire_has_magic(NULL, 0), "NULL must be rejected");
}

static void test_classify(void)
{
    uint8_t buf[64];
    size_t n = unhex(KA_CDJ1, buf, sizeof buf);

    CHECK_EQ_U(djl_wire_classify(DJL_PORT_ANNOUNCE, buf, n), DJL_PKT_KEEP_ALIVE);

    /* Same kind byte 0x06 on port 50002 is a Media Response, not a keep-alive.
     * This is the two-level (port, kind) dispatch requirement. */
    CHECK_EQ_U(djl_wire_classify(DJL_PORT_STATUS, buf, n), DJL_PKT_MEDIA_RESPONSE);

    /* Kind 0x0a: Device Hello on 50000, CDJ Status on 50002. */
    buf[0x0a] = 0x0a;
    CHECK_EQ_U(djl_wire_classify(DJL_PORT_ANNOUNCE, buf, n), DJL_PKT_DEVICE_HELLO);
    CHECK_EQ_U(djl_wire_classify(DJL_PORT_STATUS,   buf, n), DJL_PKT_CDJ_STATUS);

    /* Kind 0x28 is only meaningful on the beat port. */
    buf[0x0a] = 0x28;
    CHECK_EQ_U(djl_wire_classify(DJL_PORT_BEAT,     buf, n), DJL_PKT_BEAT);
    CHECK_EQ_U(djl_wire_classify(DJL_PORT_ANNOUNCE, buf, n), DJL_PKT_UNKNOWN);

    /* Unknown port yields UNKNOWN rather than misclassifying. */
    CHECK_EQ_U(djl_wire_classify(9999, buf, n), DJL_PKT_UNKNOWN);
}

static void test_keep_alive_cdj(void)
{
    uint8_t buf[64];
    size_t n = unhex(KA_CDJ1, buf, sizeof buf);
    CHECK_EQ_U(n, 0x36);

    djl_keep_alive ka;
    CHECK_EQ_U(djl_decode_keep_alive(buf, n, &ka), DJL_OK);

    CHECK_STR(ka.name, "CDJ-3000X");
    CHECK_EQ_U(ka.number, 1);
    CHECK_EQ_U(ka.proto_version, 0x03);
    CHECK_EQ_U(ka.device_type, DJL_DEVTYPE_CDJ);
    CHECK_EQ_U(ka.model_code, 0xe4);
    CHECK_EQ_U(ka.peer_count, 3);
    CHECK_EQ_U(ka.was_first, 1);       /* joined an occupied network */
    CHECK_EQ_U(ka.ip[0], 169); CHECK_EQ_U(ka.ip[1], 254);
    CHECK_EQ_U(ka.ip[2], 7);   CHECK_EQ_U(ka.ip[3], 185);
    CHECK_EQ_U(ka.mac[0], 0x24); CHECK_EQ_U(ka.mac[1], 0x97);
    CHECK_EQ_U(ka.mac[2], 0xed); CHECK_EQ_U(ka.mac[3], 0x3e);
    CHECK_EQ_U(ka.mac[4], 0x4c); CHECK_EQ_U(ka.mac[5], 0x79);
}

static void test_keep_alive_mixer(void)
{
    uint8_t buf[64];
    size_t n = unhex(KA_MIXER, buf, sizeof buf);
    CHECK_EQ_U(n, 0x36);

    djl_keep_alive ka;
    CHECK_EQ_U(djl_decode_keep_alive(buf, n, &ka), DJL_OK);

    CHECK_STR(ka.name, "DJM-A9");
    CHECK_EQ_U(ka.number, 0x21);                     /* 33 */
    CHECK_EQ_U(ka.proto_version, 0x02);              /* legacy marker */
    CHECK_EQ_U(ka.device_type, DJL_DEVTYPE_MIXER_MODERN);
    CHECK_EQ_U(ka.model_code, 0xb1);
    CHECK_EQ_U(ka.ip[3], 4);
    /* AlphaTheta OUI c8:3d:fc */
    CHECK_EQ_U(ka.mac[0], 0xc8); CHECK_EQ_U(ka.mac[1], 0x3d);
    CHECK_EQ_U(ka.mac[2], 0xfc);
}

static void test_keep_alive_cdj2(void)
{
    uint8_t buf[64];
    size_t n = unhex(KA_CDJ2, buf, sizeof buf);
    djl_keep_alive ka;
    CHECK_EQ_U(djl_decode_keep_alive(buf, n, &ka), DJL_OK);
    CHECK_EQ_U(ka.number, 2);
    CHECK_EQ_U(ka.ip[3], 162);
    CHECK_EQ_U(ka.model_code, 0xe4);
}

static void test_bounds(void)
{
    uint8_t buf[64];
    size_t n = unhex(KA_CDJ1, buf, sizeof buf);

    /* Truncate progressively; nothing may crash and short reads must fail. */
    for (size_t len = 0; len < n; len++) {
        djl_keep_alive ka;
        djl_err e = djl_decode_keep_alive(buf, len, &ka);
        CHECK(e != DJL_OK, "truncated keep-alive at len %zu must not decode OK", len);
    }

    bool ok = true;
    (void)djl_wire_be(buf, n, n - 2, 4, &ok);
    CHECK(!ok, "big-endian read past end must set ok=false");
    ok = true;
    (void)djl_wire_le(buf, n, n - 1, 8, &ok);
    CHECK(!ok, "little-endian read past end must set ok=false");

    /* Reading at or past the end reports -1 rather than reading memory. */
    CHECK(djl_wire_u8(buf, n, n) == -1, "u8 at end must return -1");
    CHECK(djl_wire_u8(buf, n, n + 100) == -1, "u8 past end must return -1");
    CHECK(djl_wire_u8(buf, n, n - 1) >= 0, "u8 at last byte must succeed");

    /* nbytes outside 1..8 must be rejected. */
    ok = true; (void)djl_wire_be(buf, n, 0, 0, &ok);
    CHECK(!ok, "zero-width read rejected");
    ok = true; (void)djl_wire_be(buf, n, 0, 9, &ok);
    CHECK(!ok, "over-wide read rejected");
}

static void test_endianness(void)
{
    const uint8_t b[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    bool ok;
    CHECK_EQ_U(djl_wire_be(b, 8, 0, 4, &ok), 0x01020304u);
    CHECK(ok, "be ok");
    CHECK_EQ_U(djl_wire_le(b, 8, 0, 4, &ok), 0x04030201u);
    CHECK(ok, "le ok");
    CHECK_EQ_U(djl_wire_be(b, 8, 0, 2, &ok), 0x0102u);
    CHECK_EQ_U(djl_wire_le(b, 8, 0, 2, &ok), 0x0201u);
    CHECK_EQ_U(djl_wire_be(b, 8, 0, 8, &ok), 0x0102030405060708ull);
}

static void test_pitch_math(void)
{
    /* 0x100000 is 0%; 0x200000 is +100%; 0 is -100%. */
    CHECK(djl_pitch_percent(0x100000) == 0.0, "neutral pitch is 0%%");
    CHECK(djl_pitch_percent(0x200000) == 100.0, "double speed is +100%%");
    CHECK(djl_pitch_percent(0x000000) == -100.0, "stopped is -100%%");
    CHECK(djl_pitch_multiplier(0x100000) == 1.0, "neutral multiplier is 1.0");
    CHECK(djl_pitch_multiplier(0x200000) == 2.0, "double multiplier is 2.0");

    CHECK_EQ_U(djl_percent_to_pitch(0.0), 0x100000);
    CHECK_EQ_U(djl_percent_to_pitch(100.0), 0x200000);
    CHECK_EQ_U(djl_percent_to_pitch(-100.0), 0);

    /* 128.00 BPM at 0% is 128.00; at +100% is 256.00. */
    double e1 = djl_effective_bpm(12800, 0x100000);
    CHECK(e1 > 127.99 && e1 < 128.01, "128 BPM at 0%% -> %.4f", e1);
    double e2 = djl_effective_bpm(12800, 0x200000);
    CHECK(e2 > 255.99 && e2 < 256.01, "128 BPM at +100%% -> %.4f", e2);
    /* 0xffff means "no track"; must not produce a tempo. */
    CHECK(djl_effective_bpm(0xffff, 0x100000) == 0.0, "no-track BPM is 0");
}

static void test_halfframes(void)
{
    /* 150 half-frames per second. */
    CHECK_EQ_U(djl_halfframe_to_ms(150), 1000);
    CHECK_EQ_U(djl_ms_to_halfframe(1000), 150);
    CHECK_EQ_U(djl_halfframe_to_ms(0), 0);
    /* round trip on a non-trivial value */
    CHECK_EQ_U(djl_ms_to_halfframe(djl_halfframe_to_ms(3000)), 3000);
}

static void test_name_extraction(void)
{
    uint8_t buf[64];
    size_t n = unhex(KA_CDJ1, buf, sizeof buf);
    char name[DJL_NAME_LEN + 1];

    /* Announcement framing reads the name at 0x0c. */
    CHECK_EQ_U(djl_wire_device_name(DJL_PORT_ANNOUNCE, buf, n, name, sizeof name), DJL_OK);
    CHECK_STR(name, "CDJ-3000X");

    /* Status framing reads at 0x0b, one byte earlier, so on this
     * announcement packet it must produce something different. */
    CHECK_EQ_U(djl_wire_device_name(DJL_PORT_STATUS, buf, n, name, sizeof name), DJL_OK);
    CHECK(strcmp(name, "CDJ-3000X") != 0,
          "status framing must not accidentally match announce framing");

    /* Undersized output buffer must be rejected, not overflowed. */
    char tiny[4];
    CHECK_EQ_U(djl_wire_device_name(DJL_PORT_ANNOUNCE, buf, n, tiny, sizeof tiny),
               DJL_ERR_INVAL);
}

static void test_device_number(void)
{
    uint8_t buf[64];
    size_t n = unhex(KA_CDJ1, buf, sizeof buf);
    /* Announcement framing: D at 0x24. */
    CHECK_EQ_U(djl_wire_device_number(DJL_PORT_ANNOUNCE, buf, n), 1);
    /* Status framing: D at 0x21, which on this packet holds the proto version. */
    CHECK_EQ_U(djl_wire_device_number(DJL_PORT_STATUS, buf, n), 0x03);
}

static void test_synthetic_cdj_status(void)
{
    /* Build a minimal but well-formed 0x11c status packet and verify the
     * decoder reads the fields back from the documented offsets. */
    uint8_t p[0x11c];
    memset(p, 0, sizeof p);
    static const uint8_t magic[10] =
        { 0x51,0x73,0x70,0x74,0x31,0x57,0x6d,0x4a,0x4f,0x4c };
    memcpy(p, magic, 10);
    p[0x0a] = 0x0a;
    memcpy(p + 0x0b, "TestPlayer", 10);
    p[0x1f] = 0x01;
    p[0x20] = 0x03;
    p[0x21] = 3;                    /* D */
    p[0x22] = 0x00; p[0x23] = 0xf8; /* len_r */
    p[0x24] = 3;
    p[0x27] = 1;                    /* A activity */
    p[0x28] = 2;                    /* Dr */
    p[0x29] = DJL_SLOT_USB;         /* Sr */
    p[0x2a] = DJL_TRACK_REKORDBOX;  /* Tr */
    p[0x2c] = 0; p[0x2d] = 0; p[0x2e] = 0x04; p[0x2f] = 0xd2;  /* id 1234 */
    p[0x7b] = DJL_PLAY1_PLAYING;    /* P1 */
    memcpy(p + 0x7c, "1.43", 4);    /* firmware */
    p[0x89] = 0x84 | DJL_F_PLAY | DJL_F_MASTER | DJL_F_SYNC;   /* F */
    p[0x8c] = 0x00; p[0x8d] = 0x10; p[0x8e] = 0x00; p[0x8f] = 0x00; /* pitch 0% */
    p[0x92] = 0x32; p[0x93] = 0x00; /* BPM 128.00 -> 12800 = 0x3200 */
    p[0x9e] = 1;                    /* Mm */
    p[0x9f] = 0xff;                 /* Mh */
    p[0xa0] = 0; p[0xa1] = 0; p[0xa2] = 0x01; p[0xa3] = 0x00;  /* beat 256 */
    p[0xa6] = 3;                    /* Bb */

    djl_cdj_status s;
    CHECK_EQ_U(djl_decode_cdj_status(p, sizeof p, &s), DJL_OK);
    CHECK_STR(s.name, "TestPlayer");
    CHECK_EQ_U(s.number, 3);
    CHECK_EQ_U(s.activity, 1);
    CHECK_EQ_U(s.track_device, 2);
    CHECK_EQ_U(s.track_slot, DJL_SLOT_USB);
    CHECK_EQ_U(s.track_type, DJL_TRACK_REKORDBOX);
    CHECK_EQ_U(s.rekordbox_id, 1234);
    CHECK_EQ_U(s.play_state1, DJL_PLAY1_PLAYING);
    CHECK_STR(s.firmware, "1.43");
    CHECK_EQ_U(s.bpm_x100, 12800);
    CHECK_EQ_U(s.beat, 256);
    CHECK_EQ_U(s.beat_within_bar, 3);
    CHECK_EQ_U(s.master_handoff, 0xff);
    CHECK(s.playing, "F play bit decoded");
    CHECK(s.master,  "F master bit decoded");
    CHECK(s.synced,  "F sync bit decoded");
    CHECK(!s.on_air, "F on-air bit clear");
    CHECK(s.effective_bpm > 127.99 && s.effective_bpm < 128.01,
          "effective bpm %.4f", s.effective_bpm);

    /* A packet shorter than 0xd0 is not a status packet. */
    CHECK(djl_decode_cdj_status(p, 0x80, &s) != DJL_OK,
          "short status packet must be rejected");
}

static void test_synthetic_beat(void)
{
    uint8_t p[0x60];
    memset(p, 0, sizeof p);
    static const uint8_t magic[10] =
        { 0x51,0x73,0x70,0x74,0x31,0x57,0x6d,0x4a,0x4f,0x4c };
    memcpy(p, magic, 10);
    p[0x0a] = 0x28;
    memcpy(p + 0x0b, "CDJ-3000X", 9);
    p[0x1f] = 0x01;
    p[0x20] = 0x00;
    p[0x21] = 2;                       /* D */
    p[0x22] = 0x00; p[0x23] = 0x3c;    /* len_r = 0x003c */
    /* nextBeat = 469 ms (0x01d5) */
    p[0x24] = 0; p[0x25] = 0; p[0x26] = 0x01; p[0x27] = 0xd5;
    /* nextBar = 1407 ms (0x057f) */
    p[0x2c] = 0; p[0x2d] = 0; p[0x2e] = 0x05; p[0x2f] = 0x7f;
    p[0x54] = 0x00; p[0x55] = 0x10; p[0x56] = 0x00; p[0x57] = 0x00;  /* 0% */
    p[0x5a] = 0x32; p[0x5b] = 0x00;    /* 128.00 BPM */
    p[0x5c] = 2;                       /* Bb */
    p[0x5f] = 2;                       /* redundant D */

    djl_beat b;
    CHECK_EQ_U(djl_decode_beat(p, sizeof p, &b), DJL_OK);
    CHECK_EQ_U(b.number, 2);
    CHECK_STR(b.name, "CDJ-3000X");
    CHECK_EQ_U(b.next_beat_ms, 469);
    CHECK_EQ_U(b.next_bar_ms, 1407);
    CHECK_EQ_U(b.bpm_x100, 12800);
    CHECK_EQ_U(b.beat_within_bar, 2);
    CHECK(b.effective_bpm > 127.99 && b.effective_bpm < 128.01, "beat bpm");

    CHECK(djl_decode_beat(p, 0x40, &b) != DJL_OK, "short beat rejected");
}

static void test_synthetic_precise_position(void)
{
    uint8_t p[0x3c];
    memset(p, 0, sizeof p);
    static const uint8_t magic[10] =
        { 0x51,0x73,0x70,0x74,0x31,0x57,0x6d,0x4a,0x4f,0x4c };
    memcpy(p, magic, 10);
    p[0x0a] = 0x0b;
    memcpy(p + 0x0b, "CDJ-3000X", 9);
    p[0x1f] = 0x02;                    /* note: 0x02, not 0x01 */
    p[0x21] = 1;
    /* track length 372 s */
    p[0x24] = 0; p[0x25] = 0; p[0x26] = 0x01; p[0x27] = 0x74;
    /* playhead 61 000 ms = 0xEE48 */
    p[0x28] = 0; p[0x29] = 0; p[0x2a] = 0xee; p[0x2b] = 0x48;
    /* pitch +3.26% -> 326 = 0x146 */
    p[0x2c] = 0; p[0x2d] = 0; p[0x2e] = 0x01; p[0x2f] = 0x46;
    /* bpm 120.2 -> 1202 = 0x4b2 */
    p[0x38] = 0; p[0x39] = 0; p[0x3a] = 0x04; p[0x3b] = 0xb2;

    djl_precise_position pp;
    CHECK_EQ_U(djl_decode_precise_position(p, sizeof p, &pp), DJL_OK);
    CHECK_EQ_U(pp.number, 1);
    CHECK_EQ_U(pp.track_length_s, 372);
    CHECK_EQ_U(pp.playhead_ms, 61000);
    CHECK_EQ_U(pp.pitch_x100, 326);
    CHECK_EQ_U(pp.bpm_x10, 1202);
}

static void test_media_details(void)
{
    uint8_t p[0xc0];
    memset(p, 0, sizeof p);
    static const uint8_t magic[10] =
        { 0x51,0x73,0x70,0x74,0x31,0x57,0x6d,0x4a,0x4f,0x4c };
    memcpy(p, magic, 10);
    p[0x0a] = 0x06;
    p[0x27] = 2;                     /* host device */
    p[0x2b] = DJL_SLOT_USB;          /* slot */
    /* media name "Gig" in UTF-16BE */
    p[0x2c] = 0x00; p[0x2d] = 'G';
    p[0x2e] = 0x00; p[0x2f] = 'i';
    p[0x30] = 0x00; p[0x31] = 'g';
    p[0xa6] = 0x02; p[0xa7] = 0x8b;  /* 651 tracks */
    p[0xa8] = 5;                     /* green */
    p[0xaa] = DJL_TRACK_REKORDBOX;
    p[0xab] = 1;                     /* has My Settings */
    p[0xae] = 0x00; p[0xaf] = 0x0c;  /* 12 playlists */
    p[0xb7] = 0x10;                  /* total 16 bytes (toy value) */
    p[0xbf] = 0x08;                  /* free 8 */

    djl_media_details m;
    CHECK_EQ_U(djl_decode_media_details(p, sizeof p, &m), DJL_OK);
    CHECK_EQ_U(m.host_device, 2);
    CHECK_EQ_U(m.slot, DJL_SLOT_USB);
    CHECK_STR(m.media_name, "Gig");
    CHECK_EQ_U(m.track_count, 651);
    CHECK_EQ_U(m.color, 5);
    CHECK_EQ_U(m.track_type, DJL_TRACK_REKORDBOX);
    CHECK(m.has_my_settings, "My Settings flag");
    CHECK_EQ_U(m.playlist_count, 12);
    CHECK_EQ_U(m.total_bytes, 16);
    CHECK_EQ_U(m.free_bytes, 8);

    /* An empty name with a non-zero track count still means a real database.
     * Verify the decoder does not conflate the two. */
    memset(p + 0x2c, 0, 0x40);
    CHECK_EQ_U(djl_decode_media_details(p, sizeof p, &m), DJL_OK);
    CHECK_STR(m.media_name, "");
    CHECK_EQ_U(m.track_count, 651);
}

/* Regression against a real CDJ-3000X status packet (firmware 1.31).
 * This is the layout that motivated widening the decoder past 0x200. */
static void test_real_cdj3000x_status(void)
{
    djl_cdj_status s;
    CHECK_EQ_U(djl_decode_cdj_status(CDJ3000X_STATUS, CDJ3000X_STATUS_LEN, &s), DJL_OK);

    CHECK_STR(s.name, "CDJ-3000X");
    CHECK_EQ_U(s.packet_len, 1152);
    CHECK_EQ_U(s.subtype, 0x08);          /* undocumented value */
    CHECK_EQ_U(s.number, 1);
    CHECK_STR(s.firmware, "1.31");

    /* len_r must describe the rest of the packet: 0x24 + len_r == total. */
    bool ok;
    uint64_t len_r = djl_wire_be(CDJ3000X_STATUS, CDJ3000X_STATUS_LEN, 0x22, 2, &ok);
    CHECK(ok, "len_r readable");
    CHECK_EQ_U(0x24 + len_r, CDJ3000X_STATUS_LEN);

    /* Player 1 was tempo master and on-air, with a finished track. */
    CHECK_EQ_U(s.flags, 0xac);
    CHECK(s.master,   "master bit");
    CHECK(s.on_air,   "on-air bit");
    CHECK(!s.synced,  "sync bit clear");
    CHECK(!s.playing, "play bit clear");
    CHECK_EQ_U(s.play_state1, DJL_PLAY1_ENDED);
    CHECK_EQ_U(s.play_state2, 0xfe);       /* nxs2-style stopped */
    CHECK_EQ_U(s.master_meaningful, 1);
    CHECK_EQ_U(s.master_handoff, 0xff);

    CHECK_EQ_U(s.tempo_validity, 0x8000);  /* rekordbox-analyzed source */
    CHECK_EQ_U(s.bpm_x100, 12801);
    CHECK_EQ_U(s.beat, 482);
    CHECK_EQ_U(s.beat_within_bar, 2);
    CHECK_EQ_U(s.sync_counter, 12);
    CHECK_EQ_U(s.cue_countdown, 0x01ff);   /* no upcoming cue */
    CHECK_EQ_U(s.hardware_hint, 0x1f);     /* CDJ-3000 class */
    CHECK(s.touch_audio_caps & 0x20, "touch audio capable (bit 5 of 0xcd)");

    /* Local USB was mounted (0x00) and SD was absent (0x04). */
    CHECK_EQ_U(s.usb_local, 0x00);
    CHECK_EQ_U(s.sd_local, 0x04);

    /* Extended CDJ-3000 region must be recognized at this length. */
    CHECK(s.has_extended, "extended region parsed");
    CHECK_EQ_U(s.master_tempo, 0);         /* Master Tempo off */
    CHECK_EQ_U(s.key_shift_cents, 0);
    CHECK_EQ_U(s.loop_start_raw, 0);       /* not looping */
    CHECK_EQ_U(s.loop_beats, 0);
    /* Master Tempo off with the slider off zero yields the out-of-key marker. */
    CHECK_EQ_U(s.key_accidental, 0x64);

    /* This firmware carries no 12 34 56 78 settings block, so the settings
     * fields must stay at their safe defaults rather than reading garbage. */
    CHECK_EQ_U(s.waveform_color, 0);
    CHECK_EQ_U(s.waveform_position, 0);

    /* Pitch: -0.50% as shown by the player. */
    CHECK(s.pitch_percent < -0.4 && s.pitch_percent > -0.6,
          "pitch %.3f%% should be about -0.5%%", s.pitch_percent);
    CHECK(s.effective_bpm > 127.3 && s.effective_bpm < 127.5,
          "effective bpm %.3f", s.effective_bpm);

    /* Truncating the real packet must never produce a bogus success. */
    for (size_t len = 0; len < 0xd0; len++) {
        djl_cdj_status t;
        CHECK(djl_decode_cdj_status(CDJ3000X_STATUS, len, &t) != DJL_OK,
              "truncated real status at %zu must fail", len);
    }
    /* At >=0xd0 it decodes, but extended fields must be gated off. */
    djl_cdj_status t;
    CHECK_EQ_U(djl_decode_cdj_status(CDJ3000X_STATUS, 0xd0, &t), DJL_OK);
    CHECK(!t.has_extended, "extended must be gated off below 0x200");
    CHECK_EQ_U(t.key_accidental, 0);
}

static void test_signature(void)
{
    /* Known-answer for the digest order (title, 0, artist, 0, duration BE). */
    uint8_t sig[20];
    CHECK_EQ_U(djl_track_signature("Test", "DJ", 225, NULL, NULL, sig), DJL_OK);
    static const uint8_t expect[20] = {
        0xc1,0x7a,0xd8,0xb7,0x62,0xf2,0xfb,0x2e,0x9f,0xff,
        0x64,0xb5,0x59,0xbc,0x9a,0xe0,0x74,0x39,0xa9,0x3e };
    for (int i = 0; i < 20; i++)
        CHECK(sig[i] == expect[i], "signature byte %d = %02x, expected %02x",
              i, sig[i], expect[i]);

    /* Deterministic and input-sensitive. */
    uint8_t sig2[20];
    djl_track_signature("Test", "DJ", 226, NULL, NULL, sig2);
    CHECK(memcmp(sig, sig2, 20) != 0, "signature must change with duration");
}

static void test_song_structure(void)
{
    /* Build an unmasked song_structure_body: mood(2)=1(high), pad6, end_beat(2)=256,
     * pad2, bank(1)=3, pad1, then two 24-byte phrase entries. */
    uint8_t sb[14 + 48];
    memset(sb, 0, sizeof sb);
    sb[0]=0x00; sb[1]=0x01;                 /* mood = high */
    sb[8]=0x01; sb[9]=0x00;                 /* end_beat = 256 */
    sb[12]=0x03;                            /* bank = 3 */
    /* entry 0: index 1, beat 1, kind 1 (Intro) */
    uint8_t *e0 = sb + 14;
    e0[0]=0; e0[1]=1; e0[2]=0; e0[3]=1; e0[4]=0; e0[5]=1;
    /* entry 1: index 2, beat 33, kind 5 (Chorus in high mood) */
    uint8_t *e1 = sb + 14 + 24;
    e1[0]=0; e1[1]=2; e1[2]=0; e1[3]=33; e1[4]=0; e1[5]=5;

    static const uint8_t MASK[19] = {
        0xCB,0xE1,0xEE,0xFA,0xE5,0xEE,0xAD,0xEE,0xE9,0xD2,
        0xE9,0xEB,0xE1,0xE9,0xF3,0xE8,0xE9,0xF4,0xE1 };
    const uint16_t cnt = 2;

    /* Masked variant: tag body = esz(4)=24, cnt(2)=2, then XOR-masked body. */
    uint8_t body[6 + sizeof sb];
    body[0]=0; body[1]=0; body[2]=0; body[3]=24;
    body[4]=0; body[5]=(uint8_t)cnt;
    for (size_t i = 0; i < sizeof sb; i++)
        body[6+i] = (uint8_t)(sb[i] ^ ((MASK[i % 19] + cnt) & 0xff));

    /* raw_mood (first u2 of masked region) must look masked (>20). */
    CHECK(((body[6]<<8)|body[7]) > 20, "synthetic PSSI should read as masked");

    djl_song_structure ss;
    CHECK_EQ_U(djl_parse_song_structure(body, sizeof body, &ss), DJL_OK);
    CHECK_EQ_U(ss.mood, DJL_MOOD_HIGH);
    CHECK_EQ_U(ss.bank, 3);
    CHECK_EQ_U(ss.end_beat, 256);
    CHECK_EQ_U(ss.count, 2);
    if (ss.count == 2) {
        CHECK_EQ_U(ss.phrases[0].index, 1);
        CHECK_EQ_U(ss.phrases[0].beat, 1);
        CHECK_EQ_U(ss.phrases[0].kind, 1);
        CHECK_STR(ss.phrases[0].label, "Intro");
        CHECK_EQ_U(ss.phrases[1].beat, 33);
        CHECK_STR(ss.phrases[1].label, "Chorus");
    }
    djl_song_structure_free(&ss);

    /* Unmasked variant: raw_mood <= 20, so no deobfuscation should occur. */
    uint8_t body2[6 + sizeof sb];
    body2[0]=0; body2[1]=0; body2[2]=0; body2[3]=24;
    body2[4]=0; body2[5]=(uint8_t)cnt;
    memcpy(body2 + 6, sb, sizeof sb);
    djl_song_structure ss2;
    CHECK_EQ_U(djl_parse_song_structure(body2, sizeof body2, &ss2), DJL_OK);
    CHECK_EQ_U(ss2.mood, DJL_MOOD_HIGH);
    CHECK_EQ_U(ss2.count, 2);
    if (ss2.count == 2) CHECK_STR(ss2.phrases[1].label, "Chorus");
    djl_song_structure_free(&ss2);

    /* Label mapping sanity. */
    CHECK_STR(djl_phrase_label(DJL_MOOD_MID, 8), "Bridge");
    CHECK_STR(djl_phrase_label(DJL_MOOD_LOW, 10), "Outro");
    CHECK_STR(djl_phrase_label(DJL_MOOD_HIGH, 2), "Up");
}

/* Verify the rekordbox hot-cue color LUT matches known reference values. */
static void test_cue_color_lut(void)
{
    uint8_t r,g,b;
    CHECK(djl_rekordbox_color(0x01,&r,&g,&b) && r==0x30 && g==0x5a && b==0xff, "color 0x01");
    CHECK(djl_rekordbox_color(0x3e,&r,&g,&b) && r==0x64 && g==0x73 && b==0xff, "color 0x3e");
    CHECK(!djl_rekordbox_color(0x00,&r,&g,&b), "color 0 = none");
    CHECK(!djl_rekordbox_color(0x3f,&r,&g,&b), "color 0x3f out of range");
}

static void test_fuzz_no_crash(void)
{
    /* Feed random bytes at every length to every decoder. The contract is
     * total: never crash, never read out of bounds. */
    uint8_t buf[600];
    srand(12345);
    for (int iter = 0; iter < 20000; iter++) {
        size_t len = (size_t)(rand() % (int)sizeof buf);
        for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(rand() & 0xff);
        /* Half the time, install a valid magic so we get deeper coverage. */
        if ((iter & 1) && len >= 11) {
            static const uint8_t magic[10] =
                { 0x51,0x73,0x70,0x74,0x31,0x57,0x6d,0x4a,0x4f,0x4c };
            memcpy(buf, magic, 10);
        }
        djl_keep_alive ka; djl_cdj_status st; djl_mixer_status ms;
        djl_beat bt; djl_precise_position pp; djl_media_details md;
        char name[DJL_NAME_LEN + 1];

        (void)djl_wire_classify(DJL_PORT_ANNOUNCE, buf, len);
        (void)djl_wire_classify(DJL_PORT_BEAT, buf, len);
        (void)djl_wire_classify(DJL_PORT_STATUS, buf, len);
        (void)djl_wire_classify(DJL_PORT_AUDIO, buf, len);
        (void)djl_decode_keep_alive(buf, len, &ka);
        (void)djl_decode_cdj_status(buf, len, &st);
        (void)djl_decode_mixer_status(buf, len, &ms);
        (void)djl_decode_beat(buf, len, &bt);
        (void)djl_decode_precise_position(buf, len, &pp);
        (void)djl_decode_media_details(buf, len, &md);
        (void)djl_wire_device_name(DJL_PORT_ANNOUNCE, buf, len, name, sizeof name);
        (void)djl_wire_device_name(DJL_PORT_STATUS, buf, len, name, sizeof name);

        djl_song_structure ss;
        if (djl_parse_song_structure(buf, len, &ss) == DJL_OK) djl_song_structure_free(&ss);
    }
    checks++;
    printf("  fuzz: 20000 iterations completed without crashing\n");
}

int main(void)
{
    printf("libdjlink wire codec tests\n");
    test_magic();
    test_classify();
    test_keep_alive_cdj();
    test_keep_alive_mixer();
    test_keep_alive_cdj2();
    test_bounds();
    test_endianness();
    test_pitch_math();
    test_halfframes();
    test_name_extraction();
    test_device_number();
    test_synthetic_cdj_status();
    test_synthetic_beat();
    test_synthetic_precise_position();
    test_media_details();
    test_real_cdj3000x_status();
    test_signature();
    test_song_structure();
    test_cue_color_lut();
    test_fuzz_no_crash();
    djl_test_nfs();
    djl_test_position();
    djl_test_rblink();

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
