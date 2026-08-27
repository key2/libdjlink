/* djl-nfs: read a player's USB/SD directly over Pioneer's NFS server.
 *
 * Needs no device number and no dbserver, so it works with four real players
 * on the network. Lists the media, dumps export.pdb-derived metadata, and
 * fetches a track's ANLZ analysis (beat grid, cues, phrases, waveforms).
 */
#include "djlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void logger(djl_log_level lvl, const char *msg, void *ud)
{
    (void)ud;
    if (lvl <= DJL_LOG_WARN)
        fprintf(stderr, "[%s] %s\n", lvl == DJL_LOG_ERROR ? "ERROR" : "WARN", msg);
}

static void usage(const char *a0)
{
    fprintf(stderr,
        "usage: %s -i <iface> [-p player | -a A.B.C.D] [options]\n"
        "  -i <iface>   network interface (e.g. enx00e04c680292)\n"
        "  -p <n>       player number to read (discovered via DJ Link)\n"
        "  -a <ip>      talk straight to an IP, skipping discovery\n"
        "  -s <slot>    2 = SD, 3 = USB (default 3)\n"
        "  -l           list the collection from export.pdb\n"
        "  -t <id>      fetch one track id fully (metadata + ANLZ)\n"
        "  -d <path>    list a directory on the media\n"
        "  -f <path>    dump a file's first bytes as hex\n"
        "  -o <file>    with -f, write the whole file here instead\n", a0);
}

static void print_hex(const uint8_t *d, size_t n, size_t max)
{
    for (size_t i = 0; i < n && i < max; i += 16) {
        printf("  %08zx ", i);
        for (size_t k = 0; k < 16 && i + k < n && i + k < max; k++)
            printf("%02x%s", d[i + k], (k % 4 == 3) ? " " : "");
        printf("\n");
    }
}

static void show_track(const djl_nfs_track *t)
{
    const djl_track_info *m = &t->meta;
    printf("\n=== track %u ===\n", m->rekordbox_id);
    if (t->has_meta) {
        printf("  title        : %s\n", m->title);
        printf("  artist       : %s\n", m->artist);
        if (m->album[0])           printf("  album        : %s\n", m->album);
        if (m->genre[0])           printf("  genre        : %s\n", m->genre);
        if (m->label[0])           printf("  label        : %s\n", m->label);
        if (m->key[0])             printf("  key          : %s\n", m->key);
        if (m->original_artist[0]) printf("  orig. artist : %s\n", m->original_artist);
        if (m->remixer[0])         printf("  remixer      : %s\n", m->remixer);
        if (m->comment[0])         printf("  comment      : %s\n", m->comment);
        if (m->date_added[0])      printf("  added        : %s\n", m->date_added);
        if (m->color_name[0])      printf("  color        : %s (%u)\n", m->color_name, m->color_id);
        printf("  tempo        : %.2f BPM\n", m->tempo_x100 / 100.0);
        printf("  duration     : %u s\n", m->duration_s);
        if (m->bitrate) printf("  bit rate     : %u kbps\n", m->bitrate);
        if (m->year)    printf("  year         : %u\n", m->year);
        if (m->rating)  printf("  rating       : %u\n", m->rating);
        if (m->artwork_id) printf("  artwork id   : %u\n", m->artwork_id);
    }
    printf("  anlz path    : %s\n", t->anlz_path);
    if (t->anlz.path[0]) printf("  audio path   : %s\n", t->anlz.path);

    const djl_anlz *a = &t->anlz;
    if (a->has_grid) {
        printf("  beat grid    : %u beats", a->grid.count);
        if (a->grid.count) {
            printf(" (first %u ms bar-pos %u, last %u ms)",
                   a->grid.entries[0].time_ms, a->grid.entries[0].beat_within_bar,
                   a->grid.entries[a->grid.count - 1].time_ms);
        }
        printf("\n");
    }
    if (a->has_cues) {
        printf("  cues         : %u%s\n", a->cues.count,
               a->cues.extended ? " (extended)" : "");
        for (uint32_t i = 0; i < a->cues.count; i++) {
            const djl_cue_entry *c = &a->cues.entries[i];
            printf("      %-6s %8u ms", c->is_loop ? "loop" :
                   c->hot_cue ? "hot" : "memory", c->start_ms);
            if (c->is_loop) printf(" -> %u ms", c->end_ms);
            if (c->hot_cue) printf("  %c", 'A' + c->hot_cue - 1);
            if (c->has_color) printf("  #%02x%02x%02x", c->r, c->g, c->b);
            if (c->comment[0]) printf("  \"%s\"", c->comment);
            printf("\n");
        }
    }
    if (a->has_ss) {
        static const char *moods[] = { "unknown", "high", "mid", "low" };
        printf("  phrases      : %u  mood=%s bank=%u end_beat=%u\n",
               a->ss.count, moods[a->ss.mood <= 3 ? a->ss.mood : 0],
               a->ss.bank, a->ss.end_beat);
        for (uint32_t i = 0; i < a->ss.count; i++)
            printf("      #%-3u beat %-6u kind %-3u %s\n", a->ss.phrases[i].index,
                   a->ss.phrases[i].beat, a->ss.phrases[i].kind,
                   a->ss.phrases[i].label);
    }
    if (a->has_preview)
        printf("  wave preview : %d segments (%s)\n",
               djl_waveform_segment_count(&a->preview),
               a->preview.style == DJL_WAVE_RGB ? "RGB" :
               a->preview.style == DJL_WAVE_THREE_BAND ? "3-band" : "blue");
    if (a->has_detail)
        printf("  wave detail  : %d segments (%s)\n",
               djl_waveform_segment_count(&a->detail),
               a->detail.style == DJL_WAVE_RGB ? "RGB" :
               a->detail.style == DJL_WAVE_THREE_BAND ? "3-band" : "blue");
}

int main(int argc, char **argv)
{
    const char *iface = NULL, *dirpath = NULL, *filepath = NULL, *outfile = NULL;
    const char *addr = NULL;
    int player = -1, slot = 3, list = 0;
    long track = -1;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-i") && i + 1 < argc) iface = argv[++i];
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) addr = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) player = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) slot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) track = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dirpath = argv[++i];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) filepath = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outfile = argv[++i];
        else if (!strcmp(argv[i], "-l")) list = 1;
        else { usage(argv[0]); return 2; }
    }
    if (!addr && (!iface || player < 0)) { usage(argv[0]); return 2; }
    if (!djl_nfs_supported()) {
        fprintf(stderr, "this build has no NFS support (DJL_WITH_NFS=OFF)\n");
        return 1;
    }

    djl_context *ctx = NULL;
    djl_nfs *nfs = NULL;
    djl_err e;

    if (addr) {
        unsigned a, b, c, d;
        if (sscanf(addr, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
            fprintf(stderr, "bad IP %s\n", addr); return 2;
        }
        uint8_t ip[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
        printf("mounting %s slot %d over NFS ...\n", addr, slot);
        e = djl_nfs_open_addr(ip, (djl_slot)slot, &nfs);
    } else {
        /* Join the network only to resolve the player's address. */
        djl_config cfg;
        djl_config_defaults(&cfg);
        cfg.interface_name = iface;
        cfg.device_name    = "djl-nfs";
        cfg.observe_only   = true;      /* NFS needs no device number at all */
        cfg.auto_metadata  = false;
        cfg.log            = logger;
        if (djl_context_create(&cfg, &ctx) != DJL_OK) {
            fprintf(stderr, "context create failed\n"); return 1;
        }
        if (djl_context_start(ctx) != DJL_OK) {
            fprintf(stderr, "context start failed\n"); djl_context_destroy(ctx); return 1;
        }
        printf("watching for player %d ...\n", player);
        djl_device_info di;
        uint64_t deadline = djl_now_ms() + 8000;
        while (djl_now_ms() < deadline &&
               djl_device_by_number(ctx, (uint8_t)player, &di) != DJL_OK) {
            djl_event ev[16];
            djl_poll(ctx, ev, 16, 200);
        }
        if (djl_device_by_number(ctx, (uint8_t)player, &di) != DJL_OK) {
            fprintf(stderr, "player %d not found\n", player);
            djl_context_destroy(ctx); return 1;
        }
        printf("player %d is %u.%u.%u.%u '%s'; mounting slot %d over NFS ...\n",
               player, di.ip[0], di.ip[1], di.ip[2], di.ip[3], di.name, slot);
        e = djl_nfs_open(ctx, (uint8_t)player, (djl_slot)slot, &nfs);
    }

    if (e != DJL_OK) {
        fprintf(stderr, "NFS mount failed: %s\n", djl_strerror(e));
        if (ctx) djl_context_destroy(ctx);
        return 1;
    }
    printf("mounted.\n");

    int rc = 0;

    if (dirpath) {
        djl_nfs_dirent ents[256];
        size_t n = 0;
        e = djl_nfs_list_dir(nfs, dirpath, ents, 256, &n);
        if (e != DJL_OK) { fprintf(stderr, "list '%s': %s\n", dirpath, djl_strerror(e)); rc = 1; }
        else {
            printf("\n=== %s (%zu entries) ===\n", dirpath, n);
            for (size_t i = 0; i < n; i++)
                printf("  %-40s fileid=%u\n", ents[i].name, ents[i].fileid);
        }
    }

    if (filepath) {
        djl_blob f;
        e = djl_nfs_read_file(nfs, filepath, &f);
        if (e != DJL_OK) { fprintf(stderr, "read '%s': %s\n", filepath, djl_strerror(e)); rc = 1; }
        else {
            printf("\n=== %s: %u bytes ===\n", filepath, f.length);
            if (outfile) {
                FILE *fp = fopen(outfile, "wb");
                if (fp) { fwrite(f.data, 1, f.length, fp); fclose(fp);
                          printf("  written to %s\n", outfile); }
                else { fprintf(stderr, "cannot write %s\n", outfile); rc = 1; }
            } else {
                print_hex(f.data, f.length, 256);
            }
            djl_blob_free(&f);
        }
    }

    if (list) {
        const djl_pdb *pdb = NULL;
        e = djl_nfs_pdb(nfs, &pdb);
        if (e != DJL_OK) { fprintf(stderr, "export.pdb: %s\n", djl_strerror(e)); rc = 1; }
        else {
            size_t n = djl_pdb_track_count(pdb);
            printf("\n=== collection: %zu tracks ===\n", n);
            for (size_t i = 0; i < n; i++) {
                uint32_t id = 0;
                if (djl_pdb_track_id_at(pdb, i, &id) != DJL_OK) continue;
                djl_track_info ti;
                char anlz[512];
                if (djl_pdb_track(pdb, id, &ti, anlz, sizeof anlz) != DJL_OK) continue;
                printf("  %5u  %-34.34s %-24.24s %6.2f BPM  %s\n", id, ti.title,
                       ti.artist, ti.tempo_x100 / 100.0, ti.key);
            }
        }
    }

    if (track > 0) {
        djl_nfs_track t;
        e = djl_nfs_fetch_track(nfs, (uint32_t)track, &t);
        if (e != DJL_OK) { fprintf(stderr, "track %ld: %s\n", track, djl_strerror(e)); rc = 1; }
        else { show_track(&t); djl_nfs_track_free(&t); }
    }

    djl_nfs_close(nfs);
    if (ctx) djl_context_destroy(ctx);
    return rc;
}
