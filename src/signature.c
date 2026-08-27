/* libdjlink track signature: SHA-1 over the same inputs as beat-link's
 * SignatureFinder, so a track can be identified independent of its id/slot.
 *
 * Digest order (SignatureFinder.java): title UTF-8, 0x00, artist UTF-8, 0x00,
 * duration as a 4-byte big-endian int, the RGB waveform-detail bytes, then for
 * every beat: beat-within-bar (4-byte BE) and time-within-track ms (4-byte BE).
 *
 * Includes a small public-domain-style SHA-1 so the library needs no crypto
 * dependency.
 */
#include "djlink.h"
#include <string.h>

/* Internal, shared with dbserver.c (PSSI fingerprint). Declared here to keep
 * this file free of the pthread-pulling internal header. */
void djl_sha1(const uint8_t *data, size_t len, uint8_t out[20]);

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_ctx;

static uint32_t rol(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }

static void sha1_block(sha1_ctx *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);

    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if      (i < 20) { f = (b & c) | (~b & d);            k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8f1bbcdc; }
        else             { f = b ^ c ^ d;                     k = 0xca62c1d6; }
        uint32_t t = rol(a,5) + f + e + k + w[i];
        e = d; d = c; c = rol(b,30); b = a; a = t;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e;
}

static void sha1_init(sha1_ctx *s)
{
    s->h[0]=0x67452301; s->h[1]=0xefcdab89; s->h[2]=0x98badcfe;
    s->h[3]=0x10325476; s->h[4]=0xc3d2e1f0; s->len=0; s->n=0;
}

static void sha1_update(sha1_ctx *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->len += len;
    while (len) {
        size_t take = 64 - s->n;
        if (take > len) take = len;
        memcpy(s->buf + s->n, p, take);
        s->n += take; p += take; len -= take;
        if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
    }
}

static void sha1_final(sha1_ctx *s, uint8_t out[20])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->n != 56) sha1_update(s, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i*8));
    sha1_update(s, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(s->h[i] >> 24);
        out[i*4+1] = (uint8_t)(s->h[i] >> 16);
        out[i*4+2] = (uint8_t)(s->h[i] >> 8);
        out[i*4+3] = (uint8_t)(s->h[i]);
    }
}

static void put_be32(sha1_ctx *s, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v>>24), (uint8_t)(v>>16), (uint8_t)(v>>8), (uint8_t)v };
    sha1_update(s, b, 4);
}

/* One-shot SHA-1, exposed internally (e.g. PSSI fingerprinting). */
void djl_sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    sha1_ctx s; sha1_init(&s); sha1_update(&s, data, len); sha1_final(&s, out);
}

djl_err djl_track_signature(const char *title, const char *artist,
                            uint32_t duration_s, const djl_waveform_blob *rgb_detail,
                            const djl_beat_grid *grid, uint8_t out_sha1[20])
{
    if (!out_sha1) return DJL_ERR_INVAL;
    sha1_ctx s;
    sha1_init(&s);

    sha1_update(&s, title ? title : "", title ? strlen(title) : 0);
    uint8_t z = 0; sha1_update(&s, &z, 1);
    sha1_update(&s, artist ? artist : "", artist ? strlen(artist) : 0);
    sha1_update(&s, &z, 1);
    put_be32(&s, duration_s);

    if (rgb_detail && rgb_detail->data && rgb_detail->length)
        sha1_update(&s, rgb_detail->data, rgb_detail->length);

    if (grid) {
        for (uint32_t i = 0; i < grid->count; i++) {
            put_be32(&s, grid->entries[i].beat_within_bar);
            put_be32(&s, grid->entries[i].time_ms);
        }
    }

    sha1_final(&s, out_sha1);
    return DJL_OK;
}
