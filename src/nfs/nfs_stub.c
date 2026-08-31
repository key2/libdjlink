/* libdjlink: NFS client stubs for builds without DJL_WITH_NFS.
 *
 * The PDB and ANLZ readers are pure and always available (they are useful on a
 * local rekordbox export archive), so only the networked half is stubbed. This
 * keeps the public API shape identical on microcontroller-class targets.
 */
#include "djl_internal.h"

bool djl_nfs_supported(void) { return false; }

djl_err djl_nfs_open(djl_context *ctx, uint8_t player, djl_slot slot, djl_nfs **out)
{
    (void)ctx; (void)player; (void)slot;
    if (out) *out = NULL;
    return DJL_ERR_UNAVAILABLE;
}

djl_err djl_nfs_open_addr(const uint8_t ip[4], djl_slot slot, djl_nfs **out)
{
    (void)ip; (void)slot;
    if (out) *out = NULL;
    return DJL_ERR_UNAVAILABLE;
}

void djl_nfs_close(djl_nfs *n) { (void)n; }

void djl_nfs_set_deadline(djl_nfs *n, uint64_t deadline_ms)
{
    (void)n; (void)deadline_ms;
}


djl_err djl_nfs_read_file(djl_nfs *n, const char *path, djl_blob *out)
{
    (void)n; (void)path; (void)out;
    return DJL_ERR_UNAVAILABLE;
}

djl_err djl_nfs_list_dir(djl_nfs *n, const char *path, djl_nfs_dirent *out,
                         size_t max, size_t *count)
{
    (void)n; (void)path; (void)out; (void)max;
    if (count) *count = 0;
    return DJL_ERR_UNAVAILABLE;
}

djl_err djl_nfs_pdb(djl_nfs *n, const djl_pdb **out)
{
    (void)n;
    if (out) *out = NULL;
    return DJL_ERR_UNAVAILABLE;
}

djl_err djl_nfs_onelibrary(djl_nfs *n, const djl_onelibrary **out)
{
    (void)n;
    if (out) *out = NULL;
    return DJL_ERR_UNAVAILABLE;
}

djl_err djl_nfs_fetch_track(djl_nfs *n, uint32_t track_id, djl_nfs_track *out)
{
    (void)n; (void)track_id; (void)out;
    return DJL_ERR_UNAVAILABLE;
}

void djl_nfs_track_free(djl_nfs_track *t) { (void)t; }

djl_err djl_nfs_read_artwork(djl_nfs *n, uint32_t artwork_id, djl_blob *out)
{
    (void)n; (void)artwork_id; (void)out;
    return DJL_ERR_UNAVAILABLE;
}
