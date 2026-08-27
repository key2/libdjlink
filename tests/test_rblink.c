/* libdjlink rekordbox LINK control-channel tests.
 *
 * Golden vectors are real packets, captured on 2026-08-27 on the rekordbox host
 * itself (so its unicast is visible), with rekordbox as device 0x11, two
 * CDJ-3000X players and a DJM-A9. See ARCHITECTURE.md section 1.11.
 */
#include "djlink.h"

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
        printf(__VA_ARGS__);                               \
        printf("\n");                                      \
    }                                                      \
} while (0)

#define CHECK_EQ_U(a, b) do {                                          \
    djl_test_checks++;                                                 \
    unsigned long long _a = (unsigned long long)(a);                   \
    unsigned long long _b = (unsigned long long)(b);                   \
    if (_a != _b) {                                                    \
        djl_test_failures++;                                           \
        printf("FAIL %s:%d: %s = %llu, expected %llu\n",               \
               __FILE__, __LINE__, #a, _a, _b);                        \
    }                                                                  \
} while (0)

#define CHECK_STR(a, b) do {                                           \
    djl_test_checks++;                                                 \
    if (strcmp((a), (b)) != 0) {                                       \
        djl_test_failures++;                                           \
        printf("FAIL %s:%d: %s = \"%s\", expected \"%s\"\n",           \
               __FILE__, __LINE__, #a, (a), (b));                      \
    }                                                                  \
} while (0)

static size_t rb_unhex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = hex; p[0] && p[1] && n < cap; p += 2) {
        char b[3] = { p[0], p[1], 0 };
        out[n++] = (uint8_t)strtoul(b, NULL, 16);
    }
    return n;
}

/* rekordbox -> CDJ-3000X player 1: announce carrying the host computer name. */
static const char *RB_ANNOUNCE_0x11 =
    "5173707431576d4a4f4c1172656b6f7264626f78000000000000000000000001"
    "0111010411010000004400450053004b0054004f0050002d00330041004f0050"
    "004b005600320000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000";

static const char *RB_KEEPALIVE_0x16 =
    "5173707431576d4a4f4c1672656b6f7264626f78000000000000000000000001"
    "01110000000000000000000000000000";

static const char *RB_MIXER_NOTIFY_0x30 =
    "5173707431576d4a4f4c30444a4d2d4139000000000000000000000000000001"
    "03210000";

static const char *RB_MIXER_REPLY_0x31 =
    "5173707431576d4a4f4c3172656b6f7264626f78000000000000000000000001"
    "031100080600000000000000";

static const char *RB_PLAYER_REPLY_0x46 =
    "5173707431576d4a4f4c4643444a2d3330303058000000000000000000000001"
    "00020004020400a0";

static const char *RB_CONFIG_0x47 =
    "5173707431576d4a4f4c4772656b6f7264626f78000000000000000000000001"
    "0111002411040000123456780000000101010101010101010100000000000000"
    "0000000000000000";

static const char *RB_PLAYER_NOTIFY_0x80 =
    "5173707431576d4a4f4c8043444a2d3330303058000000000000000000000001"
    "000100080001110000000000";

static const char *RB_HELLO_0x10 =
    "5173707431576d4a4f4c1043444a2d3330303058000000000000000000000001"
    "00010000";

static void test_rb_classify(void)
{
    uint8_t buf[512];
    size_t n = rb_unhex(RB_ANNOUNCE_0x11, buf, sizeof buf);
    CHECK_EQ_U(n, 296);
    CHECK_EQ_U(djl_wire_classify(DJL_PORT_STATUS, buf, n), DJL_PKT_RB_ANNOUNCE);

    /* These kinds are only meaningful on 50002. Kind 0x11 on the announcement
     * port is part of the device-numbering dance, not a rekordbox announce. */
    CHECK(djl_wire_classify(DJL_PORT_ANNOUNCE, buf, n) != DJL_PKT_RB_ANNOUNCE,
          "0x11 on 50000 must not classify as a rekordbox announce");

    n = rb_unhex(RB_KEEPALIVE_0x16, buf, sizeof buf);
    CHECK_EQ_U(djl_wire_classify(DJL_PORT_STATUS, buf, n), DJL_PKT_RB_KEEPALIVE);
    n = rb_unhex(RB_MIXER_NOTIFY_0x30, buf, sizeof buf);
    CHECK_EQ_U(djl_wire_classify(DJL_PORT_STATUS, buf, n), DJL_PKT_RB_MIXER_NOTIFY);
    n = rb_unhex(RB_MIXER_REPLY_0x31, buf, sizeof buf);
    CHECK_EQ_U(djl_wire_classify(DJL_PORT_STATUS, buf, n), DJL_PKT_RB_MIXER_REPLY);
    n = rb_unhex(RB_PLAYER_REPLY_0x46, buf, sizeof buf);
    CHECK_EQ_U(djl_wire_classify(DJL_PORT_STATUS, buf, n), DJL_PKT_RB_PLAYER_REPLY);
    n = rb_unhex(RB_CONFIG_0x47, buf, sizeof buf);
    CHECK_EQ_U(djl_wire_classify(DJL_PORT_STATUS, buf, n), DJL_PKT_RB_CONFIG);
    n = rb_unhex(RB_PLAYER_NOTIFY_0x80, buf, sizeof buf);
    CHECK_EQ_U(djl_wire_classify(DJL_PORT_STATUS, buf, n), DJL_PKT_RB_PLAYER_NOTIFY);

    /* Every kind must have a name, or the research tooling prints blanks. */
    for (int k = 0; k < DJL_PKT__COUNT; k++) {
        const char *nm = djl_packet_kind_name((djl_packet_kind)k);
        CHECK(nm && nm[0], "packet kind %d must have a name", k);
    }
    for (int k = 0; k < DJL_EV__COUNT; k++) {
        const char *nm = djl_event_kind_name((djl_event_kind)k);
        CHECK(nm && nm[0], "event kind %d must have a name", k);
    }
}

static void test_rb_announce(void)
{
    uint8_t buf[512];
    size_t n = rb_unhex(RB_ANNOUNCE_0x11, buf, sizeof buf);

    djl_rb_link rb;
    CHECK_EQ_U(djl_decode_rb_link(buf, n, &rb), DJL_OK);
    CHECK_EQ_U(rb.kind, 0x11);
    CHECK_STR(rb.name, "rekordbox");
    CHECK_EQ_U(rb.subtype, 0x01);          /* 1 = rekordbox -> player */
    CHECK_EQ_U(rb.device, 0x11);           /* rekordbox sits at device 17 */
    CHECK_EQ_U(rb.payload_len, 260);
    CHECK(rb.length_consistent, "0x24 + 260 must equal the 296-byte datagram");

    /* The whole point of this packet: the computer name a player shows when
     * browsing rekordbox. It is UTF-16 BIG-endian, unlike the UTF-16LE that
     * NFS uses on the very same wire. */
    CHECK_STR(rb.host_name, "DESKTOP-3AOPKV2");
}

static void test_rb_other_kinds(void)
{
    uint8_t buf[512];
    djl_rb_link rb;

    /* 0x16 is the one kind whose declared length disagrees with the datagram:
     * it says zero but carries twelve zero bytes. Report, do not assume. */
    size_t n = rb_unhex(RB_KEEPALIVE_0x16, buf, sizeof buf);
    CHECK_EQ_U(djl_decode_rb_link(buf, n, &rb), DJL_OK);
    CHECK_EQ_U(rb.device, 0x11);
    CHECK_EQ_U(rb.payload_len, 0);
    CHECK(!rb.length_consistent, "0x16 declares 0 but carries 12 bytes");
    CHECK_EQ_U(rb.payload_copied, 12);

    /* Mixer pair: both directions use subtype 3, and the mixer identifies as
     * device 0x21 (33), which is where DJM mixers live. */
    n = rb_unhex(RB_MIXER_NOTIFY_0x30, buf, sizeof buf);
    CHECK_EQ_U(djl_decode_rb_link(buf, n, &rb), DJL_OK);
    CHECK_STR(rb.name, "DJM-A9");
    CHECK_EQ_U(rb.subtype, 0x03);
    CHECK_EQ_U(rb.device, 0x21);
    CHECK(rb.length_consistent, "0x30 carries no payload");

    n = rb_unhex(RB_MIXER_REPLY_0x31, buf, sizeof buf);
    CHECK_EQ_U(djl_decode_rb_link(buf, n, &rb), DJL_OK);
    CHECK_EQ_U(rb.subtype, 0x03);
    CHECK_EQ_U(rb.device, 0x11);
    CHECK_EQ_U(rb.payload_len, 8);
    CHECK(rb.length_consistent, "0x31 length must check out");
    CHECK_EQ_U(rb.payload[0], 0x06);

    /* Player reply carries a 16-bit code. */
    n = rb_unhex(RB_PLAYER_REPLY_0x46, buf, sizeof buf);
    CHECK_EQ_U(djl_decode_rb_link(buf, n, &rb), DJL_OK);
    CHECK_STR(rb.name, "CDJ-3000X");
    CHECK_EQ_U(rb.subtype, 0x00);          /* 0 = player -> rekordbox */
    CHECK_EQ_U(rb.device, 0x02);
    CHECK_EQ_U(rb.payload_len, 4);
    CHECK_EQ_U(rb.reply_code, 0x00a0);

    /* Config carries the same 12 34 56 78 marker that opens the settings block
     * in a player status packet. */
    n = rb_unhex(RB_CONFIG_0x47, buf, sizeof buf);
    CHECK_EQ_U(djl_decode_rb_link(buf, n, &rb), DJL_OK);
    CHECK_EQ_U(rb.payload_len, 36);
    CHECK(rb.length_consistent, "0x47 length must check out");
    CHECK(rb.has_settings_marker, "0x47 must carry the settings marker");

    /* Player notify references rekordbox's own device number. */
    n = rb_unhex(RB_PLAYER_NOTIFY_0x80, buf, sizeof buf);
    CHECK_EQ_U(djl_decode_rb_link(buf, n, &rb), DJL_OK);
    CHECK_EQ_U(rb.device, 0x01);
    CHECK_EQ_U(rb.payload_len, 8);
    CHECK_EQ_U(rb.referenced_device, 0x11);

    /* The player's hello, which pairs with the 0x11 announce. */
    n = rb_unhex(RB_HELLO_0x10, buf, sizeof buf);
    CHECK_EQ_U(djl_decode_rb_link(buf, n, &rb), DJL_OK);
    CHECK_EQ_U(rb.kind, 0x10);
    CHECK_EQ_U(rb.device, 0x01);
    CHECK(rb.length_consistent, "0x10 carries no payload");
}

static void test_rb_rejects(void)
{
    uint8_t buf[512];
    size_t n = rb_unhex(RB_ANNOUNCE_0x11, buf, sizeof buf);
    djl_rb_link rb;

    CHECK_EQ_U(djl_decode_rb_link(NULL, n, &rb), DJL_ERR_INVAL);
    CHECK_EQ_U(djl_decode_rb_link(buf, n, NULL), DJL_ERR_INVAL);

    /* A kind this decoder does not own must be refused, not guessed at. */
    buf[0x0a] = 0x0a;                     /* CDJ status */
    CHECK_EQ_U(djl_decode_rb_link(buf, n, &rb), DJL_ERR_UNKNOWN);
    buf[0x0a] = 0x11;

    /* Corrupt magic must be refused. */
    uint8_t save = buf[3];
    buf[3] ^= 0xff;
    CHECK_EQ_U(djl_decode_rb_link(buf, n, &rb), DJL_ERR_UNKNOWN);
    buf[3] = save;

    /* Truncation at every length must be handled without reading past the end;
     * anything shorter than the 0x24-byte header must be rejected outright. */
    for (size_t cut = 0; cut < n; cut++) {
        djl_err e = djl_decode_rb_link(buf, cut, &rb);
        if (cut < 0x24) CHECK(e != DJL_OK, "short packet must not decode");
    }

    /* A declared length far larger than the datagram must not be trusted: the
     * copy is bounded by what actually arrived. */
    buf[0x22] = 0xff; buf[0x23] = 0xff;
    CHECK_EQ_U(djl_decode_rb_link(buf, 0x24 + 8, &rb), DJL_OK);
    CHECK(!rb.length_consistent, "a lying length must be flagged");
    CHECK_EQ_U(rb.payload_copied, 8);
}

void djl_test_rblink(void);
void djl_test_rblink(void)
{
    test_rb_classify();
    test_rb_announce();
    test_rb_other_kinds();
    test_rb_rejects();
}
