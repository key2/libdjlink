/* libdjlink DJM-A9 / V10 mixer-state (0x39) and VU-meter (0x58) decoder tests.
 *
 * No capture of these exists yet: a DJM only unicasts them to a subscribed
 * bridge, so there was nothing to capture until this milestone. The fixtures
 * are therefore synthetic, built from the field map in ARCHITECTURE.md 1.10
 * (ported from SuperTimecodeConverter), with a distinct value at every offset
 * so a transposed field fails loudly. The live rig validates the offsets for
 * real; these lock them against regression.
 */
#include "djlink.h"

#include <stdio.h>
#include <string.h>

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
        printf("FAIL %s:%d: %s = %llu, expected %llu\n",               \
               __FILE__, __LINE__, #a, _a, _b);                        \
    }                                                                  \
} while (0)

static const uint8_t MAGIC[10] = {0x51,0x73,0x70,0x74,0x31,0x57,0x6d,0x4a,0x4f,0x4c};

/* Build a 0x39 packet: magic, kind, name at 0x0b, mixer number at 0x21. */
static size_t build_mixer(uint8_t *b, size_t cap, uint8_t number)
{
    const size_t total = 248;
    if (cap < total) return 0;
    memset(b, 0, total);
    memcpy(b, MAGIC, 10);
    b[0x0a] = 0x39;
    memcpy(b + 0x0b, "DJM-A9", 6);
    b[0x21] = number;
    return total;
}

static void test_mixer_channels(void)
{
    uint8_t b[248];
    build_mixer(b, sizeof b, 0x21);

    /* Distinct value per byte of CH1's 13-byte strip at 0x24. */
    static const uint16_t base[6] = { 0x24, 0x3c, 0x54, 0x6c, 0x84, 0x9c };
    for (int c = 0; c < 6; c++)
        for (int i = 0; i < 13; i++)
            b[base[c] + i] = (uint8_t)(0x10 * (c + 1) + i);

    djl_djm_mixer m;
    CHECK_EQ_U(djl_decode_djm_mixer(b, sizeof b, 4, &m), DJL_OK);
    CHECK_EQ_U(m.number, 0x21);
    CHECK_EQ_U(m.channels, 4);
    CHECK(strcmp(m.name, "DJM-A9") == 0, "name must decode");

    /* CH1 fields in order. */
    CHECK_EQ_U(m.ch[0].input_src, 0x10);
    CHECK_EQ_U(m.ch[0].trim,      0x11);
    CHECK_EQ_U(m.ch[0].comp,      0x12);
    CHECK_EQ_U(m.ch[0].eq_hi,     0x13);
    CHECK_EQ_U(m.ch[0].eq_mid,    0x14);
    CHECK_EQ_U(m.ch[0].eq_lo_mid, 0x15);
    CHECK_EQ_U(m.ch[0].eq_lo,     0x16);
    CHECK_EQ_U(m.ch[0].color,     0x17);
    CHECK_EQ_U(m.ch[0].send,      0x18);
    CHECK_EQ_U(m.ch[0].cue,       0x19);
    CHECK_EQ_U(m.ch[0].cue_b,     0x1a);
    CHECK_EQ_U(m.ch[0].fader,     0x1b);
    CHECK_EQ_U(m.ch[0].xf_assign, 0x1c);

    /* CH4 sits at 0x6c; its trim is the second byte there. */
    CHECK_EQ_U(m.ch[3].input_src, 0x40);
    CHECK_EQ_U(m.ch[3].fader,     0x4b);

    /* Requesting 4 channels must not populate CH5/CH6. */
    CHECK_EQ_U(m.ch[4].input_src, 0);
    CHECK_EQ_U(m.ch[5].input_src, 0);

    /* Now as a 6-channel V10: CH5 at 0x84, CH6 at 0x9c. */
    CHECK_EQ_U(djl_decode_djm_mixer(b, sizeof b, 6, &m), DJL_OK);
    CHECK_EQ_U(m.channels, 6);
    CHECK_EQ_U(m.ch[4].input_src, 0x50);
    CHECK_EQ_U(m.ch[5].input_src, 0x60);
    CHECK_EQ_U(m.ch[5].xf_assign, 0x6c);
}

static void test_mixer_globals(void)
{
    uint8_t b[248];
    build_mixer(b, sizeof b, 0x21);

    b[0x0b4] = 0xb4;  b[0x0b5] = 0xb5;  b[0x0b6] = 0xb6;  b[0x0b7] = 0xb7;
    b[0x0b9] = 0xb9;  b[0x0ba] = 0xba;  b[0x0bb] = 0xbb;  b[0x0bc] = 0xbc;
    b[0x0bd] = 0xbd;  b[0x0be] = 0xbe;  b[0x0bf] = 0xbf;  b[0x0c0] = 0xc0;
    b[0x0c1] = 0xc1;
    b[0x0c4] = 0xc4;  b[0x0c5] = 0xc5;
    b[0x0c6] = 0xc6;  b[0x0c7] = 0xc7;  b[0x0c8] = 0xc8;  b[0x0c9] = 0xc9;
    b[0x0ca] = 0xca;  b[0x0cb] = 0xcb;  b[0x0cc] = 0xcc;  b[0x0ce] = 0xce;
    b[0x0cf] = 0xcf;
    b[0x0d6] = 0xd6;  b[0x0d7] = 0xd7;  b[0x0d8] = 0xd8;  b[0x0d9] = 0xd9;
    b[0x0da] = 0xda;  b[0x0db] = 0xdb;  b[0x0dc] = 0xdc;  b[0x0dd] = 0xdd;
    b[0x0e2] = 0xe2;  b[0x0e3] = 0xe3;  b[0x0e4] = 0xe4;  b[0x0e5] = 0xe5;
    b[0x0e6] = 0xe6;  b[0x0e7] = 0xe7;

    djl_djm_mixer m;
    CHECK_EQ_U(djl_decode_djm_mixer(b, sizeof b, 4, &m), DJL_OK);

    CHECK_EQ_U(m.crossfader,   0xb4);
    CHECK_EQ_U(m.fader_curve,  0xb5);
    CHECK_EQ_U(m.xf_curve,     0xb6);
    CHECK_EQ_U(m.master_fader, 0xb7);
    CHECK_EQ_U(m.master_cue,   0xb9);
    CHECK_EQ_U(m.master_cue_b, 0xba);
    CHECK_EQ_U(m.isolator_on,  0xbb);
    CHECK_EQ_U(m.isolator_hi,  0xbc);
    CHECK_EQ_U(m.isolator_mid, 0xbd);
    CHECK_EQ_U(m.isolator_lo,  0xbe);
    CHECK_EQ_U(m.booth,        0xbf);
    CHECK_EQ_U(m.booth_eq_hi,  0xc0);
    CHECK_EQ_U(m.booth_eq_lo,  0xc1);
    CHECK_EQ_U(m.hp_cue_link,  0xc4);
    CHECK_EQ_U(m.hp_cue_link_b,0xc5);
    CHECK_EQ_U(m.hp_mixing,    0xe3);
    CHECK_EQ_U(m.hp_level,     0xe4);
    CHECK_EQ_U(m.booth_eq,     0xe5);
    CHECK_EQ_U(m.hp_mixing_b,  0xe6);
    CHECK_EQ_U(m.hp_level_b,   0xe7);
    CHECK_EQ_U(m.fx_freq_lo,   0xc6);
    CHECK_EQ_U(m.fx_freq_mid,  0xc7);
    CHECK_EQ_U(m.fx_freq_hi,   0xc8);
    CHECK_EQ_U(m.beat_fx_select,0xc9);
    CHECK_EQ_U(m.beat_fx_assign,0xca);
    CHECK_EQ_U(m.beat_fx_level, 0xcb);
    CHECK_EQ_U(m.beat_fx_on,    0xcc);
    CHECK_EQ_U(m.multi_io_select,0xce);
    CHECK_EQ_U(m.multi_io_level, 0xcf);
    CHECK_EQ_U(m.send_return,    0xcf);
    CHECK_EQ_U(m.color_fx_select,0xdb);
    CHECK_EQ_U(m.color_fx_param, 0xe2);
    CHECK_EQ_U(m.send_ext1,      0xdc);
    CHECK_EQ_U(m.send_ext2,      0xdd);
    CHECK_EQ_U(m.mic_eq_hi,      0xd6);
    CHECK_EQ_U(m.mic_eq_lo,      0xd7);
    CHECK_EQ_U(m.filter_lpf,     0xd8);
    CHECK_EQ_U(m.filter_hpf,     0xd9);
    CHECK_EQ_U(m.filter_reso,    0xda);
}

static void test_mixer_rejects(void)
{
    uint8_t b[248];
    build_mixer(b, sizeof b, 0x21);
    djl_djm_mixer m;

    CHECK_EQ_U(djl_decode_djm_mixer(NULL, sizeof b, 4, &m), DJL_ERR_INVAL);
    CHECK_EQ_U(djl_decode_djm_mixer(b, sizeof b, 4, NULL), DJL_ERR_INVAL);
    /* Wrong kind byte. */
    b[0x0a] = 0x29;
    CHECK_EQ_U(djl_decode_djm_mixer(b, sizeof b, 4, &m), DJL_ERR_UNKNOWN);
    b[0x0a] = 0x39;
    /* Truncated below the guaranteed block. */
    CHECK_EQ_U(djl_decode_djm_mixer(b, 0x40, 4, &m), DJL_ERR_SHORT);
    /* channels=0 defaults to 4, not 0. */
    CHECK_EQ_U(djl_decode_djm_mixer(b, sizeof b, 0, &m), DJL_OK);
    CHECK_EQ_U(m.channels, 4);
}

/* Build a 0x58 VU packet, big-endian u16 segments. */
static size_t build_vu(uint8_t *b, size_t cap, uint8_t number)
{
    const size_t total = 0x200;
    if (cap < total) return 0;
    memset(b, 0, total);
    memcpy(b, MAGIC, 10);
    b[0x0a] = 0x58;
    memcpy(b + 0x0b, "DJM-A9", 6);
    b[0x21] = number;
    return total;
}

static void put_seg(uint8_t *b, uint16_t base, int seg, uint16_t v)
{
    b[base + seg * 2]     = (uint8_t)(v >> 8);
    b[base + seg * 2 + 1] = (uint8_t)v;
}

static void test_vu(void)
{
    uint8_t b[0x200];
    build_vu(b, sizeof b, 0x21);

    static const uint16_t ch[6]  = { 0x02c, 0x068, 0x0a4, 0x0e0, 0x194, 0x1d0 };
    static const uint16_t mas[2] = { 0x11c, 0x158 };

    /* Give each meter a ramp so its peak is the last segment, distinct per meter. */
    for (int c = 0; c < 4; c++)
        for (int s = 0; s < DJL_VU_SEGMENTS; s++)
            put_seg(b, ch[c], s, (uint16_t)(0x0100 * (c + 1) + s));
    for (int mm = 0; mm < 2; mm++)
        for (int s = 0; s < DJL_VU_SEGMENTS; s++)
            put_seg(b, mas[mm], s, (uint16_t)(0x0700 + mm * 0x0100 + s));

    djl_vu_meters v;
    CHECK_EQ_U(djl_decode_vu_meters(b, sizeof b, 4, &v), DJL_OK);
    CHECK_EQ_U(v.number, 0x21);
    CHECK_EQ_U(v.channels, 4);

    CHECK_EQ_U(v.channel_peak[0], 0x0100 + 14);
    CHECK_EQ_U(v.channel_peak[3], 0x0400 + 14);
    CHECK_EQ_U(v.master_peak[0],  0x0700 + 14);
    CHECK_EQ_U(v.master_peak[1],  0x0800 + 14);
    /* Raw ladder preserved. */
    CHECK_EQ_U(v.channel_seg[0][0], 0x0100);
    CHECK_EQ_U(v.channel_seg[0][14], 0x0100 + 14);
    CHECK_EQ_U(v.master_seg[1][7],  0x0800 + 7);

    /* 6-channel V10: CH5 at 0x194, CH6 at 0x1d0. */
    for (int c = 4; c < 6; c++)
        for (int s = 0; s < DJL_VU_SEGMENTS; s++)
            put_seg(b, ch[c], s, (uint16_t)(0x0100 * (c + 1) + s));
    CHECK_EQ_U(djl_decode_vu_meters(b, sizeof b, 6, &v), DJL_OK);
    CHECK_EQ_U(v.channels, 6);
    CHECK_EQ_U(v.channel_peak[4], 0x0500 + 14);
    CHECK_EQ_U(v.channel_peak[5], 0x0600 + 14);

    /* Rejections. */
    CHECK_EQ_U(djl_decode_vu_meters(NULL, sizeof b, 4, &v), DJL_ERR_INVAL);
    b[0x0a] = 0x39;
    CHECK_EQ_U(djl_decode_vu_meters(b, sizeof b, 4, &v), DJL_ERR_UNKNOWN);
    b[0x0a] = 0x58;
    CHECK_EQ_U(djl_decode_vu_meters(b, 0x100, 4, &v), DJL_ERR_SHORT);
}

void djl_test_djm(void);
void djl_test_djm(void)
{
    test_mixer_channels();
    test_mixer_globals();
    test_mixer_rejects();
    test_vu();
}
