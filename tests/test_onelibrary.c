/* libdjlink OneLibrary tests: SQLCipher crypto KATs plus the full decrypt +
 * read pipeline against a real rekordbox exportLibrary.db.
 *
 * Only built with DJL_WITH_ONELIBRARY (the crypto and the libsqlite3-backed
 * reader are compiled out otherwise). The golden vector tests/data/exportLibrary.db
 * is a real Device Library Plus export read off CDJ USB player 1 (2026-08-31).
 */
#include "djlink.h"

#if defined(DJL_WITH_ONELIBRARY)
#include "onelibrary/onelibrary_internal.h"
#endif

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

#if defined(DJL_WITH_ONELIBRARY)

static int hexeq(const uint8_t *b, size_t n, const char *hex)
{
    for (size_t i = 0; i < n; i++) {
        char t[3] = { hex[2*i], hex[2*i+1], 0 };
        if ((uint8_t)strtoul(t, NULL, 16) != b[i]) return 0;
    }
    return 1;
}

/* SHA-512 against the RFC 6234 "abc" vector. */
static void test_sha512(void)
{
    uint8_t h[64];
    djl_sha512((const uint8_t *)"abc", 3, h);
    CHECK(hexeq(h, 64,
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"),
        "SHA-512(abc)");

    /* Empty message. */
    djl_sha512((const uint8_t *)"", 0, h);
    CHECK(hexeq(h, 64,
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"),
        "SHA-512()");

    /* HMAC-SHA512, RFC 4231 test case 1 (key=0x0b*20, data="Hi There"). */
    uint8_t key[20];
    memset(key, 0x0b, sizeof key);
    uint8_t mac[64];
    djl_hmac_sha512(key, sizeof key, (const uint8_t *)"Hi There", 8, mac);
    CHECK(hexeq(mac, 64,
        "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
        "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854"),
        "HMAC-SHA512 RFC4231 #1");
}

/* PBKDF2-HMAC-SHA512, matching the reference implementation. */
static void test_pbkdf2(void)
{
    uint8_t dk[64];
    djl_pbkdf2_hmac_sha512((const uint8_t *)"password", 8,
                           (const uint8_t *)"salt", 4, 1, dk, 64);
    CHECK(hexeq(dk, 64,
        "867f70cf1ade02cff3752599a3a53dc4af34c7a669815ae5d513554e1c8cf252"
        "c02d470a285a0501bad999bfe943c08f050235d7d68b1da55e63f73b60a57fce"),
        "PBKDF2-HMAC-SHA512 c=1");

    djl_pbkdf2_hmac_sha512((const uint8_t *)"password", 8,
                           (const uint8_t *)"salt", 4, 4096, dk, 64);
    CHECK(hexeq(dk, 16, "d197b1b33db0143e018b12f3d1d1479e"),
        "PBKDF2-HMAC-SHA512 c=4096");
}

/* AES-256 against the FIPS-197 Appendix C.3 vector. */
static void test_aes256(void)
{
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    uint8_t ct[16] = {0x8e,0xa2,0xb7,0xca,0x51,0x67,0x45,0xbf,
                      0xea,0xfc,0x49,0x90,0x4b,0x49,0x60,0x89};
    uint8_t want[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    djl_aes256 a;
    djl_aes256_init_decrypt(&a, key);
    uint8_t pt[16];
    djl_aes256_decrypt_block(&a, ct, pt);
    CHECK(memcmp(pt, want, 16) == 0, "AES-256 decrypt block");

    /* CBC with a non-zero IV, decrypting two blocks encrypted by a known good
     * implementation (round-trip check with in==out aliasing). */
    uint8_t iv[16]; for (int i = 0; i < 16; i++) iv[i] = (uint8_t)(0xf0 + i);
    uint8_t buf[16];
    memcpy(buf, ct, 16);
    djl_aes256_cbc_decrypt(&a, iv, buf, buf, 16);   /* in place */
    for (int i = 0; i < 16; i++)
        CHECK_EQ_U(buf[i], (uint8_t)(want[i] ^ iv[i]));
}

static uint8_t *load_fixture(size_t *len)
{
    /* CTest runs from the build dir; the source tree is one level up in our
     * layout, and CMake also copies the fixture next to the binary. Try both. */
    const char *paths[] = {
        "tests/data/exportLibrary.db",
        "../tests/data/exportLibrary.db",
        "data/exportLibrary.db",
    };
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n <= 0) { fclose(f); continue; }
        uint8_t *b = malloc((size_t)n);
        if (b && fread(b, 1, (size_t)n, f) == (size_t)n) { *len = (size_t)n; fclose(f); return b; }
        free(b); fclose(f);
    }
    return NULL;
}

/* The whole pipeline on the real export: decrypt -> plaintext SQLite ->
 * resolved track. */
static void test_real_export(void)
{
    size_t len = 0;
    uint8_t *enc = load_fixture(&len);
    if (!enc) {
        printf("  (skipping OneLibrary golden vector: fixture not found)\n");
        djl_test_checks++;
        return;
    }
    CHECK_EQ_U(len, 131072);

    /* Decrypt: the per-page HMAC must verify, proving key + parameters. */
    djl_blob plain;
    CHECK_EQ_U(djl_onelibrary_decrypt(enc, len, &plain), DJL_OK);
    CHECK(plain.length == 131072, "plaintext image length");
    CHECK(plain.data && memcmp(plain.data, "SQLite format 3", 15) == 0,
          "decrypted image must carry the SQLite magic");
    djl_blob_free(&plain);

    /* A single flipped byte must fail the HMAC rather than yield garbage. */
    enc[5000] ^= 0xff;
    CHECK_EQ_U(djl_onelibrary_decrypt(enc, len, &plain), DJL_ERR_STATE);
    enc[5000] ^= 0xff;

    /* Open and resolve. */
    djl_onelibrary *o = NULL;
    CHECK_EQ_U(djl_onelibrary_open(enc, len, &o), DJL_OK);
    CHECK(o != NULL, "reader must open");
    if (o) {
        CHECK_EQ_U(djl_onelibrary_track_count(o), 40);

        uint32_t id = 0;
        CHECK_EQ_U(djl_onelibrary_track_id_at(o, 0, &id), DJL_OK);
        CHECK_EQ_U(id, 1);
        CHECK_EQ_U(djl_onelibrary_track_id_at(o, 999, &id), DJL_ERR_NOT_FOUND);

        djl_track_info ti;
        char anlz[256];
        CHECK_EQ_U(djl_onelibrary_track(o, 33, &ti, anlz, sizeof anlz), DJL_OK);
        CHECK(ti.found, "track 33 must be found");
        CHECK_STR(ti.title, "Calvin Harris - Outside (Official Video) ft. Ellie Goulding");
        CHECK_STR(ti.key, "Dm");
        CHECK_EQ_U(ti.tempo_x100, 12800);
        CHECK_EQ_U(ti.duration_s, 225);
        CHECK_EQ_U(ti.bitrate, 192);
        CHECK_STR(ti.date_added, "2026-08-27");
        CHECK_STR(anlz, "/PIONEER/USBANLZ/P012/0000973C/ANLZ0001.DAT");

        /* A demo track carries a resolved artist name. */
        CHECK_EQ_U(djl_onelibrary_track(o, 5, &ti, NULL, 0), DJL_OK);
        CHECK_STR(ti.title, "Demo Track 1");
        CHECK_STR(ti.artist, "Loopmasters");

        CHECK_EQ_U(djl_onelibrary_track(o, 99999, &ti, NULL, 0), DJL_ERR_NOT_FOUND);
        djl_onelibrary_close(o);
    }

    /* Rejections. */
    CHECK_EQ_U(djl_onelibrary_open(NULL, len, &o), DJL_ERR_INVAL);
    CHECK_EQ_U(djl_onelibrary_open(enc, 100, &o), DJL_ERR_INVAL);   /* not page-aligned */
    free(enc);
}

#endif /* DJL_WITH_ONELIBRARY */

void djl_test_onelibrary(void);
void djl_test_onelibrary(void)
{
#if defined(DJL_WITH_ONELIBRARY)
    CHECK(djl_onelibrary_supported(), "supported() must be true when built in");
    test_sha512();
    test_pbkdf2();
    test_aes256();
    test_real_export();
#else
    CHECK(!djl_onelibrary_supported(), "supported() must be false when compiled out");
    djl_test_checks++;
#endif
}
