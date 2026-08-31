/* libdjlink Stagehand persona + remote-control builder tests, and streaming
 * source classification.
 *
 * The handshake/keepalive golden vectors are REAL bytes captured off the wire
 * on 2026-08-31 while the library posed as a Stagehand iPad (device type 0x05,
 * model 0x20). They were verified byte-for-byte against the reference
 * alphatheta-connect implementation, which emits the identical join sequence.
 * See ARCHITECTURE.md section 1.14.
 *
 * The transport (0x07) and preference-write (0x6b) builders have no live golden
 * capture (this rig's DJM-A9 gates the Stagehand push, so nothing on the wire
 * exercises the return channel), so those are asserted against the documented
 * byte offsets, with a distinct value at each field so a transposition fails.
 */
#include "djl_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern int djl_test_checks;
extern int djl_test_failures;

#define CHECK(cond, ...) do {                              \
    djl_test_checks++;                                     \
    if (!(cond)) {                                         \
        djl_test_failures++;                               \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
        printf(__VA_ARGS__); printf("\n");                 \
    }                                                      \
} while (0)

#define CHECK_EQ_U(a, b) do {                                          \
    djl_test_checks++;                                                 \
    unsigned long long _a = (unsigned long long)(a);                   \
    unsigned long long _b = (unsigned long long)(b);                   \
    if (_a != _b) {                                                    \
        djl_test_failures++;                                           \
        printf("FAIL %s:%d: %s = %llu (0x%llx), expected %llu (0x%llx)\n", \
               __FILE__, __LINE__, #a, _a, _a, _b, _b);                \
    }                                                                  \
} while (0)

/* Identity matching the live capture: name "Stagehand", runtime number 173
 * (0xad), AlphaTheta-OUI MAC c8:3d:fc:53:83:f3, IP 169.254.241.203. */
static djl_identity sh_id(void)
{
    djl_identity id;
    memset(&id, 0, sizeof id);
    memcpy(id.name, "Stagehand", 9);
    id.number       = 0xad;
    id.device_type  = 0x05;
    id.model_code   = 0x20;
    id.proto_version = 0x03;
    static const uint8_t mac[6] = { 0xc8, 0x3d, 0xfc, 0x53, 0x83, 0xf3 };
    static const uint8_t ip[4]  = { 169, 254, 241, 203 };
    memcpy(id.mac, mac, 6);
    memcpy(id.ip,  ip,  4);
    id.peer_count = 1;
    return id;
}

static void check_bytes(const char *what, const uint8_t *got, size_t n,
                        const uint8_t *want, size_t wn)
{
    djl_test_checks++;
    if (n != wn) {
        djl_test_failures++;
        printf("FAIL %s: length %zu, expected %zu\n", what, n, wn);
        return;
    }
    if (memcmp(got, want, n) != 0) {
        djl_test_failures++;
        printf("FAIL %s: byte mismatch\n  got : ", what);
        for (size_t i = 0; i < n; i++) printf("%02x", got[i]);
        printf("\n  want: ");
        for (size_t i = 0; i < wn; i++) printf("%02x", want[i]);
        printf("\n");
    }
}

/* Golden: the 54-byte 0x06 keep-alive as captured off the wire. */
static const uint8_t GOLD_KA[54] = {
    0x51,0x73,0x70,0x74,0x31,0x57,0x6d,0x4a,0x4f,0x4c,
    0x06,0x00,
    'S','t','a','g','e','h','a','n','d',0,0,0,0,0,0,0,0,0,0,0,
    0x01,0x03, 0x00,0x36,
    0xad, 0x01,
    0xc8,0x3d,0xfc,0x53,0x83,0xf3,
    0xa9,0xfe,0xf1,0xcb,
    0x01,0x00,0x00,0x00,
    0x05, 0x20
};

/* Golden: the 50-byte 0x02 claim (iteration counter 2), as captured. */
static const uint8_t GOLD_CLAIM2[50] = {
    0x51,0x73,0x70,0x74,0x31,0x57,0x6d,0x4a,0x4f,0x4c,
    0x02,0x00,
    'S','t','a','g','e','h','a','n','d',0,0,0,0,0,0,0,0,0,0,0,
    0x01,0x03, 0x00,0x32,
    0xa9,0xfe,0xf1,0xcb,
    0xc8,0x3d,0xfc,0x53,0x83,0xf3,
    0x3a, 0x02,
    0x05, 0x01
};

/* Golden: the 37-byte 0x0a announce (computed; same form the reference emits). */
static const uint8_t GOLD_ANNOUNCE[37] = {
    0x51,0x73,0x70,0x74,0x31,0x57,0x6d,0x4a,0x4f,0x4c,
    0x0a,0x00,
    'S','t','a','g','e','h','a','n','d',0,0,0,0,0,0,0,0,0,0,0,
    0x01,0x03, 0x00,0x25,
    0x05
};

static void test_stagehand_handshake(void)
{
    djl_identity id = sh_id();
    uint8_t buf[256];
    size_t n;

    n = djl_build_stagehand_announce(buf, sizeof buf, &id);
    check_bytes("stagehand announce", buf, n, GOLD_ANNOUNCE, sizeof GOLD_ANNOUNCE);

    n = djl_build_stagehand_claim(buf, sizeof buf, &id, 2);
    check_bytes("stagehand claim #2", buf, n, GOLD_CLAIM2, sizeof GOLD_CLAIM2);

    n = djl_build_stagehand_keep_alive(buf, sizeof buf, &id);
    check_bytes("stagehand keep-alive", buf, n, GOLD_KA, sizeof GOLD_KA);

    /* The claim counter is the only thing that varies across the 3 claims. */
    n = djl_build_stagehand_claim(buf, sizeof buf, &id, 1);
    CHECK_EQ_U(buf[0x2f], 1);
    n = djl_build_stagehand_claim(buf, sizeof buf, &id, 3);
    CHECK_EQ_U(buf[0x2f], 3);
    CHECK_EQ_U(buf[0x2e], 0x3a);   /* symbolic number is fixed */
    (void)n;

    /* Capacity guards. */
    CHECK_EQ_U(djl_build_stagehand_keep_alive(buf, 10, &id), 0);
    CHECK_EQ_U(djl_build_stagehand_claim(buf, 10, &id, 1), 0);
    CHECK_EQ_U(djl_build_stagehand_announce(buf, 10, &id), 0);
}

static void test_transport_packet(void)
{
    djl_identity id = sh_id();
    uint8_t buf[64];
    size_t n = djl_build_transport(buf, sizeof buf, &id, 2 /*target*/,
                                   0x0f /*play*/, true, 0x5a /*corr*/);
    CHECK_EQ_U(n, 0x38);                 /* 56 bytes */
    CHECK(djl_wire_has_magic(buf, n), "magic");
    CHECK_EQ_U(buf[0x0a], 0x07);         /* kind */
    CHECK(memcmp(buf + 0x0b, "Stagehand", 9) == 0, "name at 0x0b");
    CHECK_EQ_U(buf[0x1e], 2);            /* target device number */
    CHECK_EQ_U(buf[0x1f], 0x01);
    CHECK_EQ_U(buf[0x20], 0x03);         /* proto marker */
    CHECK_EQ_U(buf[0x21], 0x5a);         /* correlation byte */
    CHECK_EQ_U(buf[0x22], 0x00);         /* len_r hi */
    CHECK_EQ_U(buf[0x23], 0x30);         /* len_r lo */
    CHECK_EQ_U(buf[0x28], 0x3a);         /* sub-id */
    CHECK_EQ_U(buf[0x2b], 0x0f);         /* action */
    CHECK_EQ_U(buf[0x2d], 0x01);         /* press */

    n = djl_build_transport(buf, sizeof buf, &id, 2, 0x14, false, 0x5a);
    CHECK_EQ_U(buf[0x2b], 0x14);
    CHECK_EQ_U(buf[0x2d], 0x00);         /* release */

    CHECK_EQ_U(djl_build_transport(buf, 10, &id, 2, 0x0f, true, 0), 0);
}

static void test_pref_write_packet(void)
{
    djl_identity id = sh_id();
    uint8_t buf[160];

    /* On-air ON. */
    size_t n = djl_build_pref_write(buf, sizeof buf, &id, 1 /*target*/,
                                    0x81 /*on*/, 0x00);
    CHECK_EQ_U(n, 0x7c);                 /* 124 bytes */
    CHECK(djl_wire_has_magic(buf, n), "magic");
    CHECK_EQ_U(buf[0x0a], 0x6b);
    CHECK(memcmp(buf + 0x0b, "Stagehand", 9) == 0, "name at 0x0b");
    CHECK_EQ_U(buf[0x1e], 1);            /* target */
    CHECK_EQ_U(buf[0x1f], 0x01);
    CHECK_EQ_U(buf[0x20], 0x03);
    CHECK_EQ_U(buf[0x21], 0x3a);         /* sub-id */
    CHECK_EQ_U(buf[0x22], 0x00);
    CHECK_EQ_U(buf[0x23], 0x50);         /* body length */
    CHECK_EQ_U(buf[0x24], 0x01);         /* write flag */
    CHECK_EQ_U(buf[0x2c], 0x81);         /* on-air ON */
    CHECK_EQ_U(buf[0x3c], 0x00);         /* quantize untouched */

    /* On-air OFF. */
    n = djl_build_pref_write(buf, sizeof buf, &id, 1, 0x80, 0x00);
    CHECK_EQ_U(buf[0x2c], 0x80);

    /* Quantize index 2 (=1/4): wire value 0x82. */
    n = djl_build_pref_write(buf, sizeof buf, &id, 1, 0x00, 0x82);
    CHECK_EQ_U(buf[0x2c], 0x00);         /* on-air untouched */
    CHECK_EQ_U(buf[0x3c], 0x82);
    (void)n;

    CHECK_EQ_U(djl_build_pref_write(buf, 20, &id, 1, 0x81, 0), 0);
}

static void test_streaming_source(void)
{
    /* Local media is never a streaming source. */
    CHECK_EQ_U(djl_streaming_source_of(DJL_TRACK_REKORDBOX, DJL_SLOT_USB),
               DJL_STREAM_NONE);
    CHECK_EQ_U(djl_streaming_source_of(DJL_TRACK_REKORDBOX, DJL_SLOT_SD),
               DJL_STREAM_NONE);
    CHECK_EQ_U(djl_streaming_source_of(DJL_TRACK_AUDIO_CD, DJL_SLOT_CD),
               DJL_STREAM_NONE);
    CHECK(!djl_slot_is_streaming(DJL_SLOT_USB), "usb not streaming");
    CHECK(!djl_slot_is_streaming(DJL_SLOT_COLLECTION), "rb not streaming");

    /* Named streaming slots. */
    CHECK_EQ_U(djl_streaming_source_of(DJL_TRACK_STREAMING, DJL_SLOT_BEATPORT),
               DJL_STREAM_BEATPORT);
    CHECK_EQ_U(djl_streaming_source_of(DJL_TRACK_STREAMING, DJL_SLOT_STREAM_DP),
               DJL_STREAM_DIRECT_PLAY);
    CHECK_EQ_U(djl_streaming_source_of(DJL_TRACK_STREAMING, DJL_SLOT_STREAM8),
               DJL_STREAM_CLOUD_DIRECT);
    /* Unnamed streaming slots collapse to generic. */
    CHECK_EQ_U(djl_streaming_source_of(DJL_TRACK_STREAMING, DJL_SLOT_STREAM5),
               DJL_STREAM_GENERIC);
    CHECK_EQ_U(djl_streaming_source_of(DJL_TRACK_STREAMING, DJL_SLOT_STREAM7),
               DJL_STREAM_GENERIC);
    /* Streaming track type with an ordinary slot id still reads as streaming. */
    CHECK_EQ_U(djl_streaming_source_of(DJL_TRACK_STREAMING, DJL_SLOT_USB),
               DJL_STREAM_GENERIC);

    CHECK(djl_slot_is_streaming(DJL_SLOT_BEATPORT), "beatport streaming");
    CHECK(djl_slot_is_streaming(DJL_SLOT_STREAM_DP), "sdp streaming");

    /* Names and back-compat aliases. */
    CHECK(strcmp(djl_streaming_source_name(DJL_STREAM_BEATPORT), "BeatportLINK") == 0,
          "beatport name");
    CHECK_EQ_U(DJL_SLOT_UNKNOWN5, DJL_SLOT_STREAM5);
    CHECK_EQ_U(DJL_SLOT_USB2,     DJL_SLOT_STREAM7);
    CHECK_EQ_U(DJL_SLOT_UNKNOWN8, DJL_SLOT_STREAM8);
}

void djl_test_stagehand(void);
void djl_test_stagehand(void)
{
    test_stagehand_handshake();
    test_transport_packet();
    test_pref_write_packet();
    test_streaming_source();
}
