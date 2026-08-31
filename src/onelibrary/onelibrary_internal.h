/* libdjlink OneLibrary internals: the SQLCipher-4 crypto needed to open a
 * rekordbox exportLibrary.db, plus the decrypt entry point.
 *
 * The SQL reading is done with libsqlite3 (sqlite3_deserialize on the decrypted
 * image), so nothing here touches a b-tree. Only the crypto is hand-rolled, to
 * keep the feature's sole external dependency libsqlite3 rather than a whole
 * crypto library. See ARCHITECTURE.md section 1.13.
 */
#ifndef DJL_ONELIBRARY_INTERNAL_H
#define DJL_ONELIBRARY_INTERNAL_H

#include "djl_internal.h"

/* ---------------- SHA-512 / HMAC / PBKDF2 ---------------- */

typedef struct {
    uint64_t h[8];
    uint64_t len;          /* total message length in bytes (< 2^64) */
    uint8_t  buf[128];
    size_t   buf_used;
} djl_sha512_ctx;

void djl_sha512_init(djl_sha512_ctx *c);
void djl_sha512_update(djl_sha512_ctx *c, const uint8_t *data, size_t len);
void djl_sha512_final(djl_sha512_ctx *c, uint8_t out[64]);
void djl_sha512(const uint8_t *data, size_t len, uint8_t out[64]);

typedef struct { djl_sha512_ctx inner, outer; } djl_hmac_sha512_ctx;

void djl_hmac_sha512_init(djl_hmac_sha512_ctx *h, const uint8_t *key, size_t klen);
void djl_hmac_sha512_update(djl_hmac_sha512_ctx *h, const uint8_t *data, size_t len);
void djl_hmac_sha512_final(djl_hmac_sha512_ctx *h, uint8_t out[64]);
void djl_hmac_sha512(const uint8_t *key, size_t klen,
                     const uint8_t *data, size_t len, uint8_t out[64]);

void djl_pbkdf2_hmac_sha512(const uint8_t *pass, size_t plen,
                            const uint8_t *salt, size_t slen,
                            uint32_t iters, uint8_t *out, size_t dklen);

/* ---------------- AES-256-CBC (decrypt only) ---------------- */

typedef struct { uint32_t rk[60]; } djl_aes256;   /* 14 rounds -> 60 words */

void djl_aes256_init_decrypt(djl_aes256 *a, const uint8_t key[32]);
/* Decrypt one 16-byte block (ECB primitive). */
void djl_aes256_decrypt_block(const djl_aes256 *a, const uint8_t in[16], uint8_t out[16]);
/* CBC decrypt in place-capable: len must be a multiple of 16. */
void djl_aes256_cbc_decrypt(const djl_aes256 *a, const uint8_t iv[16],
                            const uint8_t *in, uint8_t *out, size_t len);

/* ---------------- SQLCipher 4 ---------------- */

/* Decrypt a rekordbox OneLibrary image into a plaintext SQLite image.
 * Allocates *out (caller frees with djl_blob_free). Returns DJL_ERR_STATE if
 * the per-page HMAC does not verify (wrong key/params/corrupt). */
djl_err djl_onelibrary_sqlcipher_decrypt(const uint8_t *enc, size_t len, djl_blob *out);

#endif /* DJL_ONELIBRARY_INTERNAL_H */
