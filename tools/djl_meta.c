/* djl-meta: connect to a player's dbserver and dump metadata + waveform.
 *
 * Joins the DJ Link network as a virtual CDJ (needed to get a device number in
 * 1..4 and to be reachable), waits until online, then opens a dbserver
 * connection to the requested player. If no track id is given, it reads the
 * player's status to find the currently-loaded track.
 */
#include "djlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int s){ (void)s; g_stop = 1; }

static void logger(djl_log_level lvl, const char *msg, void *ud)
{
    (void)ud;
    if (lvl <= DJL_LOG_WARN) fprintf(stderr, "[%s] %s\n",
        lvl==DJL_LOG_ERROR?"ERROR":"WARN", msg);
}

static const char *wave_style_name(djl_waveform_style s)
{
    return s==DJL_WAVE_RGB?"RGB":s==DJL_WAVE_THREE_BAND?"3-band":"blue";
}

static void render_waveform(const djl_waveform_blob *wf, int cols)
{
    int n = djl_waveform_segment_count(wf);
    if (n <= 0) { printf("  (no waveform segments)\n"); return; }
    const char *ramp = " .:-=+*#%@";
    int rows = 8;
    /* Downsample to cols columns, take the peak height in each bucket. */
    for (int row = rows; row >= 1; row--) {
        printf("  ");
        for (int c = 0; c < cols; c++) {
            long a = (long)c * n / cols, b = (long)(c+1) * n / cols;
            if (b <= a) b = a+1;
            int peak = 0;
            for (long i = a; i < b && i < n; i++) {
                int h = djl_waveform_height(wf, (int)i);
                if (h > peak) peak = h;
            }
            int level = peak * rows / 31;          /* 0..rows */
            putchar(level >= row ? ramp[9] : ' ');
        }
        putchar('\n');
    }
    printf("  0%*s%d segments (%s%s)\n", cols-10, "", n,
           wave_style_name(wf->style), wf->detail?" detail":" preview");
}

int main(int argc, char **argv)
{
    const char *iface = NULL;
    int number = 0, player = 0;
    long track_id = -1;
    int slot = DJL_SLOT_USB, type = DJL_TRACK_REKORDBOX;
    int do_list = 0, cols = 100, want_rgb = 0;

    int opt;
    while ((opt = getopt(argc, argv, "i:n:p:t:s:T:lRc:h")) != -1) {
        switch (opt) {
        case 'i': iface = optarg; break;
        case 'n': number = atoi(optarg); break;
        case 'p': player = atoi(optarg); break;
        case 't': track_id = strtol(optarg, NULL, 0); break;
        case 's': slot = atoi(optarg); break;
        case 'T': type = atoi(optarg); break;
        case 'l': do_list = 1; break;
        case 'R': want_rgb = 1; break;
        case 'c': cols = atoi(optarg); break;
        case 'h': default:
            fprintf(stderr,
                "usage: %s -i <iface> -p <player> [options]\n"
                "  -i <iface>  DJ Link interface (required)\n"
                "  -p <n>      target player number (required)\n"
                "  -n <n>      our device number (default lowest free 1..4)\n"
                "  -t <id>     track id (default: read from player's status)\n"
                "  -s <slot>   slot 2=SD 3=USB (default 3)\n"
                "  -T <type>   track type 1=rekordbox 2=unanalyzed (default 1)\n"
                "  -R          prefer RGB waveform\n"
                "  -l          list the slot's folder + track menu\n"
                "  -c <cols>   waveform render width (default 100)\n", argv[0]);
            return 2;
        }
    }
    if (!iface || player <= 0) { fprintf(stderr, "need -i and -p\n"); return 2; }

    signal(SIGINT, on_sigint);

    djl_config cfg;
    djl_config_defaults(&cfg);
    cfg.interface_name = iface;
    cfg.device_name = "djl-meta";
    if (number > 0) cfg.preferred_number = (uint8_t)number;
    cfg.send_status = true;
    cfg.log = logger;

    djl_context *ctx = NULL;
    if (djl_context_create(&cfg, &ctx) != DJL_OK) { fprintf(stderr,"create failed\n"); return 1; }
    if (djl_context_start(ctx) != DJL_OK) { fprintf(stderr,"start failed\n"); djl_context_destroy(ctx); return 1; }

    /* Wait until we are online and have seen the target player. */
    printf("joining network, waiting to come online ...\n");
    uint64_t deadline = djl_now_ms() + 12000;
    djl_event ev[32];
    while (!g_stop && djl_now_ms() < deadline) {
        int k = djl_poll(ctx, ev, 32, 200);
        (void)k;
        if (djl_is_online(ctx)) {
            djl_device_info di;
            if (djl_device_by_number(ctx, (uint8_t)player, &di) == DJL_OK) break;
        }
    }
    if (!djl_is_online(ctx)) { fprintf(stderr,"did not come online\n"); goto done; }
    printf("online as device %d\n", djl_own_number(ctx));

    /* If no track id given, read it from the player's latest status. */
    if (track_id < 0) {
        for (int tries = 0; tries < 25 && !g_stop; tries++) {
            djl_cdj_status st;
            if (djl_cdj_status_for(ctx, (uint8_t)player, &st) == DJL_OK && st.rekordbox_id) {
                track_id = st.rekordbox_id;
                slot = st.track_slot; type = st.track_type;
                printf("player %d has track id=%ld slot=%s type=%s\n",
                       player, track_id, djl_slot_name(slot), djl_track_type_name(type));
                break;
            }
            djl_poll(ctx, ev, 32, 200);
        }
    }

    printf("opening dbserver on player %d ...\n", player);
    djl_db *db = NULL;
    djl_err e = djl_db_open(ctx, (uint8_t)player, &db);
    if (e != DJL_OK) { fprintf(stderr,"db open failed: %s\n", djl_strerror(e)); goto done; }
    printf("connected: dbserver port %u, they report device %d\n",
           djl_db_port(db), djl_db_their_number(db));

    if (do_list) {
        djl_menu_row rows[256]; size_t n = 0;
        printf("\n=== folder menu (root) ===\n");
        e = djl_db_folder_menu(db, slot, type, 0xffffffffu, rows, 256, &n);
        if (e == DJL_OK) for (size_t i=0;i<n;i++)
            printf("  [%04x] %-40s %s\n", rows[i].item_type, rows[i].label1, rows[i].label2);
        else printf("  folder menu: %s\n", djl_strerror(e));

        n = 0;
        printf("\n=== track list ===\n");
        e = djl_db_track_list(db, slot, type, rows, 256, &n);
        if (e == DJL_OK) for (size_t i=0;i<n;i++)
            printf("  id=%-8u %-40s %s\n", rows[i].id, rows[i].label1, rows[i].label2);
        else printf("  track list: %s\n", djl_strerror(e));
    }

    if (track_id >= 0) {
        printf("\n=== metadata (id %ld, %s, %s) ===\n", track_id,
               djl_slot_name(slot), djl_track_type_name(type));
        djl_track_info ti;
        e = djl_db_track_metadata(db, slot, type, (uint32_t)track_id, &ti);
        if (e == DJL_OK && ti.found) {
            printf("  Title:    %s\n", ti.title);
            printf("  Artist:   %s\n", ti.artist);
            printf("  Album:    %s\n", ti.album);
            printf("  Genre:    %s\n", ti.genre);
            printf("  Key:      %s\n", ti.key);
            printf("  Comment:  %s\n", ti.comment);
            printf("  Duration: %u s   Tempo: %.2f BPM   Rating: %u\n",
                   ti.duration_s, ti.tempo_x100/100.0, ti.rating);
            printf("  Added:    %s   Color: %s   Artwork id: %u\n",
                   ti.date_added, ti.color_name, ti.artwork_id);
        } else {
            printf("  no rekordbox metadata (%s) - trying track list for a filename\n",
                   djl_strerror(e));
        }

        printf("\n=== waveform (id %ld) ===\n", track_id);
        djl_waveform_blob wf;
        e = djl_db_waveform(db, slot, type, (uint32_t)track_id,
                            want_rgb?DJL_WAVE_RGB:DJL_WAVE_BLUE, false, &wf);
        if (e == DJL_OK) { render_waveform(&wf, cols); djl_waveform_free(&wf); }
        else printf("  waveform preview: %s\n", djl_strerror(e));
    }

    djl_db_close(db);
done:
    djl_context_destroy(ctx);
    return 0;
}
