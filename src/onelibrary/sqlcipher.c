/* libdjlink: SQLCipher 4 page decryption for rekordbox OneLibrary databases.
 *
 * rekordbox's exportLibrary.db (Device Library Plus / "OneLibrary") is a
 * SQLCipher-4 database. The passphrase is not machine- or license-specific: it
 * is a single constant shared by every Device Library Plus export, published
 * via pyrekordbox and others. We embed the already-deobfuscated key directly
 * (it deobfuscates from base85 -> XOR -> zlib to this 64-character ASCII
 * string), so this file needs no base85/zlib and no secret handling.
 *
 * Format, confirmed by HMAC verification against a real CDJ USB export
 * (2026-08-31): 4096-byte pages, PBKDF2-HMAC-SHA512 x 256000 to derive the AES
 * key, a second 2-iteration PBKDF2 over the salt XOR 0x3a for the HMAC key,
 * AES-256-CBC per page, and an HMAC-SHA512 per page over
 * ciphertext || iv || page_number(LE32). Page 1's first 16 bytes are the KDF
 * salt in place of the "SQLite format 3\0" magic. Standard SQLCipher-4 layout.
 *
 * We rebuild a clean plaintext SQLite image and hand it to libsqlite3, so this
 * is the only place OneLibrary's encryption is touched.
 */
#include "onelibrary_internal.h"

#include <stdlib.h>
#include <string.h>

/* The shared OneLibrary SQLCipher passphrase (deobfuscated). 64 ASCII bytes,
 * used as a passphrase (run through PBKDF2), not as a raw key. */
static const char ONELIB_KEY[] =
    "r8gddnr4k847830ar6cqzbkk0el6qytmb3trbbx805jm74vez64i5o8fnrqryqls";

#define SQLCIPHER_PAGE   4096
#define SQLCIPHER_KDFIT  256000
#define SQLCIPHER_HMACIT 2
#define AES_KEYLEN       32
#define IV_LEN           16
#define HMAC_LEN         64
#define SALT_LEN         16
/* IV + HMAC, already a multiple of the 16-byte AES block. */
#define RESERVE          (IV_LEN + HMAC_LEN)

static const uint8_t SQLITE_MAGIC[16] = {
    'S','Q','L','i','t','e',' ','f','o','r','m','a','t',' ','3','\0'
};

djl_err djl_onelibrary_sqlcipher_decrypt(const uint8_t *enc, size_t len, djl_blob *out)
{
    if (!enc || !out) return DJL_ERR_INVAL;
    memset(out, 0, sizeof *out);
    if (len < SQLCIPHER_PAGE || (len % SQLCIPHER_PAGE) != 0) return DJL_ERR_INVAL;

    const uint8_t *salt = enc;                 /* first 16 bytes of the file */

    /* Derive the AES key from the passphrase, and the HMAC key from that. */
    uint8_t aes_key[AES_KEYLEN];
    djl_pbkdf2_hmac_sha512((const uint8_t *)ONELIB_KEY, strlen(ONELIB_KEY),
                           salt, SALT_LEN, SQLCIPHER_KDFIT, aes_key, AES_KEYLEN);

    uint8_t hmac_salt[SALT_LEN];
    for (int i = 0; i < SALT_LEN; i++) hmac_salt[i] = (uint8_t)(salt[i] ^ 0x3a);
    uint8_t hmac_key[AES_KEYLEN];
    djl_pbkdf2_hmac_sha512(aes_key, AES_KEYLEN, hmac_salt, SALT_LEN,
                           SQLCIPHER_HMACIT, hmac_key, AES_KEYLEN);

    djl_aes256 aes;
    djl_aes256_init_decrypt(&aes, aes_key);

    size_t npages = len / SQLCIPHER_PAGE;
    uint8_t *plain = malloc(len);
    if (!plain) return DJL_ERR_NOMEM;

    for (size_t p = 0; p < npages; p++) {
        const uint8_t *page = enc + p * SQLCIPHER_PAGE;
        uint8_t *dst = plain + p * SQLCIPHER_PAGE;

        /* Page 1 keeps its 16-byte salt as cleartext; every page's ciphertext
         * ends before the reserved IV+HMAC tail. */
        size_t ct_start = (p == 0) ? SALT_LEN : 0;
        size_t ct_end   = SQLCIPHER_PAGE - RESERVE;
        const uint8_t *ct  = page + ct_start;
        size_t ct_len      = ct_end - ct_start;
        const uint8_t *iv  = page + ct_end;
        const uint8_t *mac = page + ct_end + IV_LEN;

        /* Authenticate before trusting the ciphertext: HMAC over
         * ciphertext || iv || page_no(LE32). A mismatch on page 1 means the
         * key or parameters are wrong; on a later page, corruption. */
        uint8_t page_le[4];
        uint32_t page_no = (uint32_t)(p + 1);
        page_le[0] = (uint8_t)page_no;        page_le[1] = (uint8_t)(page_no >> 8);
        page_le[2] = (uint8_t)(page_no >> 16); page_le[3] = (uint8_t)(page_no >> 24);

        uint8_t calc[HMAC_LEN];
        djl_hmac_sha512_ctx hm;
        djl_hmac_sha512_init(&hm, hmac_key, AES_KEYLEN);
        djl_hmac_sha512_update(&hm, ct, ct_len);
        djl_hmac_sha512_update(&hm, iv, IV_LEN);
        djl_hmac_sha512_update(&hm, page_le, 4);
        djl_hmac_sha512_final(&hm, calc);

        unsigned diff = 0;
        for (int i = 0; i < HMAC_LEN; i++) diff |= (unsigned)(calc[i] ^ mac[i]);
        if (diff != 0) { free(plain); return DJL_ERR_STATE; }

        djl_aes256_cbc_decrypt(&aes, iv, ct, dst + ct_start, ct_len);

        if (p == 0) {
            /* Restore the standard SQLite header magic in place of the salt so
             * libsqlite3 accepts the image. */
            memcpy(dst, SQLITE_MAGIC, SALT_LEN);
        }
        /* Zero the reserved tail we did not decrypt, keeping the page size and
         * therefore every b-tree pointer intact. */
        memset(dst + ct_end, 0, RESERVE);
    }

    out->data   = plain;
    out->length = (uint32_t)len;
    return DJL_OK;
}
