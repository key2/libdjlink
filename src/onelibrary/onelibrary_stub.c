/* libdjlink: OneLibrary stubs for builds without DJL_WITH_ONELIBRARY.
 *
 * The feature needs libsqlite3, so on targets that lack it (or when it is
 * turned off) every entry point degrades to DJL_ERR_UNAVAILABLE while keeping
 * the public API shape identical.
 */
#include "djl_internal.h"

bool djl_onelibrary_supported(void) { return false; }

djl_err djl_onelibrary_decrypt(const uint8_t *enc, size_t len, djl_blob *out)
{
    (void)enc; (void)len;
    if (out) { out->data = NULL; out->length = 0; }
    return DJL_ERR_UNAVAILABLE;
}

djl_err djl_onelibrary_open(const uint8_t *enc, size_t len, djl_onelibrary **out)
{
    (void)enc; (void)len;
    if (out) *out = NULL;
    return DJL_ERR_UNAVAILABLE;
}

void djl_onelibrary_close(djl_onelibrary *o) { (void)o; }

size_t djl_onelibrary_track_count(const djl_onelibrary *o) { (void)o; return 0; }

djl_err djl_onelibrary_track_id_at(const djl_onelibrary *o, size_t index, uint32_t *out_id)
{
    (void)o; (void)index; (void)out_id;
    return DJL_ERR_UNAVAILABLE;
}

djl_err djl_onelibrary_track(const djl_onelibrary *o, uint32_t content_id,
                             djl_track_info *out, char *anlz_path, size_t anlz_path_sz)
{
    (void)o; (void)content_id; (void)out; (void)anlz_path; (void)anlz_path_sz;
    return DJL_ERR_UNAVAILABLE;
}

djl_err djl_onelibrary_artwork_path(const djl_onelibrary *o, uint32_t artwork_id,
                                    char *out, size_t outsz)
{
    (void)o; (void)artwork_id;
    if (out && outsz) out[0] = '\0';
    return DJL_ERR_UNAVAILABLE;
}
