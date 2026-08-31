/* djl-onelibrary: read a rekordbox OneLibrary (exportLibrary.db).
 *
 * Works on a file already pulled off the media, or fetches it over NFS from a
 * player. Decrypts the SQLCipher database and lists / resolves tracks, or dumps
 * a plaintext copy any SQLite tool can open.
 */
#include "djlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *a0)
{
    fprintf(stderr,
        "usage: %s [-f exportLibrary.db | -a A.B.C.D [-s slot]] [options]\n"
        "  -f <file>   read a local exportLibrary.db\n"
        "  -a <ip>     fetch exportLibrary.db from a player over NFS\n"
        "  -s <slot>   2 = SD, 3 = USB (default 3), with -a\n"
        "  -l          list the collection\n"
        "  -t <id>     show one content id in full\n"
        "  -d <out>    decrypt to a plaintext SQLite file\n", a0);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *len = (size_t)n;
    return b;
}

static void show_track(const djl_onelibrary *o, uint32_t id)
{
    djl_track_info ti;
    char anlz[512];
    if (djl_onelibrary_track(o, id, &ti, anlz, sizeof anlz) != DJL_OK) {
        printf("  content %u: not found\n", id);
        return;
    }
    printf("\n=== content %u ===\n", id);
    printf("  title        : %s\n", ti.title);
    if (ti.artist[0])          printf("  artist       : %s\n", ti.artist);
    if (ti.album[0])           printf("  album        : %s\n", ti.album);
    if (ti.genre[0])           printf("  genre        : %s\n", ti.genre);
    if (ti.label[0])           printf("  label        : %s\n", ti.label);
    if (ti.key[0])             printf("  key          : %s\n", ti.key);
    if (ti.remixer[0])         printf("  remixer      : %s\n", ti.remixer);
    if (ti.original_artist[0]) printf("  orig. artist : %s\n", ti.original_artist);
    if (ti.comment[0])         printf("  comment      : %s\n", ti.comment);
    if (ti.color_name[0])      printf("  color        : %s\n", ti.color_name);
    printf("  tempo        : %.2f BPM\n", ti.tempo_x100 / 100.0);
    printf("  duration     : %u s\n", ti.duration_s);
    if (ti.bitrate)    printf("  bit rate     : %u kbps\n", ti.bitrate);
    if (ti.year)       printf("  year         : %u\n", ti.year);
    if (ti.rating)     printf("  rating       : %u\n", ti.rating);
    if (ti.date_added[0]) printf("  added        : %s\n", ti.date_added);
    if (ti.artwork_id) printf("  artwork id   : %u\n", ti.artwork_id);
    if (anlz[0])       printf("  anlz path    : %s\n", anlz);
}

int main(int argc, char **argv)
{
    const char *file = NULL, *addr = NULL, *decout = NULL;
    int slot = 3, list = 0;
    long track = -1;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-f") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) addr = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) slot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) track = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) decout = argv[++i];
        else if (!strcmp(argv[i], "-l")) list = 1;
        else { usage(argv[0]); return 2; }
    }
    if (!file && !addr) { usage(argv[0]); return 2; }
    if (!djl_onelibrary_supported()) {
        fprintf(stderr, "this build has no OneLibrary support (DJL_WITH_ONELIBRARY=OFF)\n");
        return 1;
    }

    /* Get the encrypted bytes, from disk or off the player. */
    uint8_t *enc = NULL;
    size_t enclen = 0;
    djl_nfs *nfs = NULL;
    if (file) {
        enc = read_file(file, &enclen);
        if (!enc) { fprintf(stderr, "cannot read %s\n", file); return 1; }
    } else {
        unsigned a, b, c, d;
        if (sscanf(addr, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
            fprintf(stderr, "bad IP %s\n", addr); return 2;
        }
        uint8_t ip[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
        printf("mounting %s slot %d over NFS ...\n", addr, slot);
        if (djl_nfs_open_addr(ip, (djl_slot)slot, &nfs) != DJL_OK) {
            fprintf(stderr, "NFS mount failed\n"); return 1;
        }
        djl_blob raw;
        if (djl_nfs_read_file(nfs, "PIONEER/rekordbox/exportLibrary.db", &raw) != DJL_OK) {
            fprintf(stderr, "no exportLibrary.db on this media\n");
            djl_nfs_close(nfs); return 1;
        }
        enclen = raw.length;
        enc = malloc(enclen);
        if (enc) memcpy(enc, raw.data, enclen);
        djl_blob_free(&raw);
    }

    int rc = 0;

    if (decout) {
        djl_blob plain;
        if (djl_onelibrary_decrypt(enc, enclen, &plain) == DJL_OK) {
            FILE *fp = fopen(decout, "wb");
            if (fp) { fwrite(plain.data, 1, plain.length, fp); fclose(fp);
                      printf("decrypted %u bytes -> %s\n", plain.length, decout); }
            else { fprintf(stderr, "cannot write %s\n", decout); rc = 1; }
            djl_blob_free(&plain);
        } else { fprintf(stderr, "decrypt failed (wrong key/params?)\n"); rc = 1; }
    }

    djl_onelibrary *o = NULL;
    if (list || track > 0) {
        if (djl_onelibrary_open(enc, enclen, &o) != DJL_OK) {
            fprintf(stderr, "open failed\n");
            free(enc); if (nfs) djl_nfs_close(nfs); return 1;
        }
    }

    if (list && o) {
        size_t n = djl_onelibrary_track_count(o);
        printf("\n=== collection: %zu tracks ===\n", n);
        for (size_t i = 0; i < n; i++) {
            uint32_t id = 0;
            if (djl_onelibrary_track_id_at(o, i, &id) != DJL_OK) continue;
            djl_track_info ti;
            if (djl_onelibrary_track(o, id, &ti, NULL, 0) != DJL_OK) continue;
            printf("  %5u  %-40.40s %-22.22s %6.2f BPM  %s\n", id, ti.title,
                   ti.artist, ti.tempo_x100 / 100.0, ti.key);
        }
    }

    if (track > 0 && o) show_track(o, (uint32_t)track);

    if (o) djl_onelibrary_close(o);
    free(enc);
    if (nfs) djl_nfs_close(nfs);
    return rc;
}
