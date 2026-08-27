/* djl-monitor: live Pro DJ Link network monitor built on libdjlink.
 *
 * Usage: djl-monitor -i <iface> [options]
 */
#include "djlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static int  g_verbose   = 0;
static int  g_show_beat = 1;
static int  g_show_pos  = 0;
static int  g_show_stat = 0;
static int  g_dump_kind = -1;   /* kind byte to hexdump, -1 = none */
static int  g_dumped    = 0;
static int  g_dump_max  = 2;
static int  g_dump_port = -1;   /* port filter for -X, -1 = any */
static int  g_query_media = 0;

static void hexdump(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i += 16) {
        printf("  %04zx  ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < n) printf("%02x ", d[i + j]);
            else           printf("   ");
            if (j == 7) printf(" ");
        }
        printf(" |");
        for (size_t j = 0; j < 16 && i + j < n; j++) {
            uint8_t c = d[i + j];
            putchar((c >= 0x20 && c < 0x7f) ? c : '.');
        }
        printf("|\n");
    }
}

static void raw_hook(uint16_t port, djl_packet_kind kind,
                     const uint8_t *data, size_t len, void *ud)
{
    (void)ud;
    if (g_dump_kind < 0 || g_dumped >= g_dump_max) return;
    if (g_dump_port >= 0 && port != (uint16_t)g_dump_port) return;
    if (len <= 0x0a || data[0x0a] != (uint8_t)g_dump_kind) return;
    g_dumped++;
    printf("\n=== raw %s port %u kind 0x%02x len %zu (0x%zx) ===\n",
           djl_packet_kind_name(kind), port, data[0x0a], len, len);
    hexdump(data, len);
    printf("\n");
    fflush(stdout);
}

static const char *lvl_name(djl_log_level l)
{
    switch (l) {
    case DJL_LOG_ERROR: return "ERROR";
    case DJL_LOG_WARN:  return "WARN ";
    case DJL_LOG_INFO:  return "INFO ";
    case DJL_LOG_DEBUG: return "DEBUG";
    default:            return "TRACE";
    }
}

static void logger(djl_log_level lvl, const char *msg, void *ud)
{
    (void)ud;
    fprintf(stderr, "[%s] %s\n", lvl_name(lvl), msg);
}

static void print_flags(uint8_t f)
{
    printf("%s%s%s%s%s",
           (f & DJL_F_PLAY)   ? "PLAY "   : "",
           (f & DJL_F_MASTER) ? "MASTER " : "",
           (f & DJL_F_SYNC)   ? "SYNC "   : "",
           (f & DJL_F_ON_AIR) ? "ONAIR "  : "",
           (f & DJL_F_BPM_ONLY) ? "BPMONLY " : "");
}

static void print_key(const djl_cdj_status *s)
{
    static const char *notes[12] = { "A","B\u266d","B","C","C\u266f","D",
                                     "E\u266d","E","F","F\u266f","G","A\u266d" };
    static const char *majors[12] = { "C","D\u266d","D","E\u266d","E","F",
                                      "F\u266f","G","A\u266d","A","B\u266d","B" };
    if (s->key_note > 11) return;
    if (s->key_accidental == 0x64) { printf(" key=out-of-key"); return; }
    if (s->key_major) printf(" key=%s", majors[s->key_note]);
    else              printf(" key=%sm", notes[s->key_note]);
}

static void handle(const djl_event *ev, djl_context *ctx)
{
    (void)ctx;
    switch (ev->kind) {
    case DJL_EV_DEVICE_FOUND: {
        const djl_device_info *d = &ev->u.device_found;
        printf("+ device %-3u %-16s %u.%u.%u.%u  "
               "mac=%02x:%02x:%02x:%02x:%02x:%02x type=0x%02x model=0x%02x proto=0x%02x %s\n",
               d->number, d->name, d->ip[0], d->ip[1], d->ip[2], d->ip[3],
               d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
               d->device_type, d->model_code, d->proto_version,
               d->is_mixer ? "[mixer]" : (d->is_cdj ? "[cdj]" : ""));
        break;
    }
    case DJL_EV_DEVICE_LOST:
        printf("- device %-3u %s\n", ev->u.device_lost.number, ev->u.device_lost.name);
        break;

    case DJL_EV_OWN_NUMBER_ASSIGNED:
        printf("* we are device number %u\n", ev->u.own_number.number);
        break;

    case DJL_EV_NUMBER_CONFLICT:
        printf("! device number conflict on %u\n", ev->u.conflict.number);
        break;

    case DJL_EV_MASTER_CHANGED:
        printf("* tempo master: %u -> %u\n",
               ev->u.master_changed.old_master, ev->u.master_changed.new_master);
        break;

    case DJL_EV_TEMPO_CHANGED:
        printf("* master tempo %.2f BPM (from player %u)\n",
               ev->u.tempo_changed.bpm, ev->u.tempo_changed.from);
        break;

    case DJL_EV_ON_AIR_CHANGED: {
        uint8_t m = ev->u.on_air.channel_mask;
        printf("* on-air:");
        for (int i = 0; i < 6; i++) if (m & (1u << i)) printf(" ch%d", i + 1);
        if (!m) printf(" (none)");
        printf("\n");
        break;
    }
    case DJL_EV_FADER_START:
        printf("* fader start: %02x %02x %02x %02x\n",
               ev->u.fader_start.commands[0], ev->u.fader_start.commands[1],
               ev->u.fader_start.commands[2], ev->u.fader_start.commands[3]);
        break;

    case DJL_EV_BEAT: {
        if (!g_show_beat) break;
        const djl_beat *b = &ev->u.beat;
        printf("beat  p%-2u bar-pos %u  %.2f BPM  next=%ums bar=%ums\n",
               b->number, b->beat_within_bar, b->effective_bpm,
               b->next_beat_ms, b->next_bar_ms);
        break;
    }
    case DJL_EV_PRECISE_POSITION: {
        if (!g_show_pos) break;
        const djl_precise_position *p = &ev->u.precise;
        printf("pos   p%-2u %8u ms / %us  pitch %+.2f%%  %.1f BPM\n",
               p->number, p->playhead_ms, p->track_length_s,
               p->pitch_x100 / 100.0,
               p->bpm_x10 == 0xffffffffu ? 0.0 : p->bpm_x10 / 10.0);
        break;
    }
    case DJL_EV_TRACK_LOADED: {
        const typeof(ev->u.track_loaded) *t = &ev->u.track_loaded;
        printf("load  p%-2u id=%-8u from p%u %s (%s)\n",
               t->player, t->rekordbox_id, t->source_player,
               djl_slot_name(t->slot), djl_track_type_name(t->type));
        break;
    }
    case DJL_EV_MEDIA_DETAILS: {
        const djl_media_details *m = &ev->u.media;
        printf("media p%u %s: '%s' created=%s tracks=%u playlists=%u "
               "type=%s settings=%s %.1f/%.1f GB free\n",
               m->host_device, djl_slot_name(m->slot),
               m->media_name[0] ? m->media_name : "(unnamed)",
               m->created[0] ? m->created : "?",
               m->track_count, m->playlist_count,
               djl_track_type_name(m->track_type),
               m->has_my_settings ? "yes" : "no",
               m->free_bytes / 1e9, m->total_bytes / 1e9);
        break;
    }
    case DJL_EV_CDJ_STATUS: {
        if (!g_show_stat) break;
        const djl_cdj_status *s = &ev->u.cdj_status;
        printf("stat  p%-2u %-13s len=%-4u ", s->number, s->name, s->packet_len);
        print_flags(s->flags);
        printf("P1=%-14s beat=%-6d bar=%u %.2f BPM pitch=%+.2f%%",
               djl_play_state1_name(s->play_state1), s->beat,
               s->beat_within_bar, s->effective_bpm, s->pitch_percent);
        if (s->rekordbox_id)
            printf(" track=%u@p%u/%s", s->rekordbox_id, s->track_device,
                   djl_slot_name(s->track_slot));
        if (s->has_extended) print_key(s);
        if (s->firmware[0]) printf(" fw=%s", s->firmware);
        printf("\n");
        break;
    }
    case DJL_EV_AUDIO_TIMING:
        if (g_verbose)
            printf("audio timing from %u: counter=%u linkcue=%u elected=%u\n",
                   ev->device, ev->u.audio_timing.counter,
                   ev->u.audio_timing.link_cue, ev->u.audio_timing.elected);
        break;

    case DJL_EV_TRACK_METADATA: {
        djl_track_info ti;
        if (djl_get_metadata(ctx, ev->u.metadata.player, &ti) == DJL_OK)
            printf("meta  p%-2u \"%s\"%s%s  dur=%us tempo=%.2f key=%s\n",
                   ev->u.metadata.player, ti.title,
                   ti.artist[0] ? " - " : "", ti.artist,
                   ti.duration_s, ti.tempo_x100 / 100.0, ti.key);
        break;
    }
    case DJL_EV_WAVEFORM:
        printf("wave  p%-2u %u segments (style %d%s)\n", ev->u.waveform.player,
               ev->u.waveform.segments, ev->u.waveform.style,
               ev->u.waveform.detail ? " detail" : "");
        break;
    case DJL_EV_BEAT_GRID:
        printf("grid  p%-2u %u beats\n", ev->u.beat_grid.player, ev->u.beat_grid.entries);
        break;
    case DJL_EV_CUE_LIST:
        printf("cues  p%-2u %u entries%s\n", ev->u.cue_list.player,
               ev->u.cue_list.entries, ev->u.cue_list.extended ? " (ext)" : "");
        break;
    case DJL_EV_ALBUM_ART:
        printf("art   p%-2u %u bytes\n", ev->u.album_art.player, ev->u.album_art.length);
        break;
    case DJL_EV_SIGNATURE:
        printf("sig   p%-2u ", ev->u.signature.player);
        for (int i = 0; i < 20; i++) printf("%02x", ev->u.signature.sha1[i]);
        printf("\n");
        break;
    case DJL_EV_REKORDBOX_LINK: {
        const djl_rb_link *r = &ev->u.rb_link;
        printf("rblnk 0x%02x %-13s dev=%-3u sub=%u len=%u%s",
               r->kind, r->name, r->device, r->subtype, r->payload_len,
               r->length_consistent ? "" : " (len mismatch)");
        if (r->host_name[0])        printf(" host='%s'", r->host_name);
        if (r->referenced_device)   printf(" ref=dev%u", r->referenced_device);
        if (r->has_settings_marker) printf(" [settings]");
        if (r->reply_code)          printf(" code=0x%04x", r->reply_code);
        printf("\n");
        break;
    }
    case DJL_EV_POSITION:
        if (g_verbose) {
            const djl_position *p = &ev->u.position;
            printf("pos   p%-2u %8lld ms beat=%-5d bar=%u %.2f BPM %s%s\n",
                   p->player, (long long)p->position_ms, p->beat, p->beat_within_bar,
                   p->effective_bpm, p->playing ? "play" : "stop",
                   p->reverse ? " rev" : "");
        }
        break;

    case DJL_EV_UNKNOWN_PACKET: {
        if (!g_verbose) break;
        printf("?     port %u kind 0x%02x len %u:",
               ev->u.unknown.port, ev->u.unknown.kind_byte, ev->u.unknown.length);
        unsigned show = ev->u.unknown.length < 32 ? ev->u.unknown.length : 32;
        for (unsigned i = 0; i < show; i++) printf(" %02x", ev->u.unknown.bytes[i]);
        printf("\n");
        break;
    }
    default:
        break;
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s -i <interface> [options]\n"
        "  -i <iface>   network interface carrying DJ Link traffic (required)\n"
        "  -n <num>     preferred device number (default: lowest free in 1..4)\n"
        "  -N <name>    device name to advertise (default: libdjlink)\n"
        "  -m <hex>     keep-alive model code byte 0x35 (default 0x64)\n"
        "  -o           observe only, transmit nothing\n"
        "  -q           do not send our own status packets\n"
        "  -s           show CDJ status packets\n"
        "  -p           show precise position packets\n"
        "  -B           hide beat packets\n"
        "  -v           verbose: unknown packets, audio timing, debug log\n"
        "  -X <hex>     hexdump the first packets whose kind byte matches\n"
        "  -C <n>       how many packets -X should dump (default 2)\n"
        "  -P <port>    restrict -X to one port\n"
        "  -M           query every media slot on every player once online\n"
        "  -t <sec>     run for N seconds then exit (default: until Ctrl-C)\n"
        "  -h           this help\n", argv0);
}

int main(int argc, char **argv)
{
    const char *iface = NULL, *name = NULL;
    int number = 0, run_secs = 0, observe = 0, quiet_status = 0;
    unsigned model = 0x64;

    int opt;
    while ((opt = getopt(argc, argv, "i:n:N:m:oqspBvt:X:C:P:Mh")) != -1) {
        switch (opt) {
        case 'i': iface = optarg; break;
        case 'n': number = atoi(optarg); break;
        case 'N': name = optarg; break;
        case 'm': model = (unsigned)strtoul(optarg, NULL, 16); break;
        case 'o': observe = 1; break;
        case 'q': quiet_status = 1; break;
        case 's': g_show_stat = 1; break;
        case 'p': g_show_pos = 1; break;
        case 'B': g_show_beat = 0; break;
        case 'v': g_verbose = 1; break;
        case 'X': g_dump_kind = (int)strtoul(optarg, NULL, 16); break;
        case 'C': g_dump_max = atoi(optarg); break;
        case 'P': g_dump_port = atoi(optarg); break;
        case 'M': g_query_media = 1; break;
        case 't': run_secs = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (!iface) { usage(argv[0]); return 2; }

    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);

    djl_config cfg;
    djl_config_defaults(&cfg);
    cfg.interface_name = iface;
    if (name) cfg.device_name = name;
    if (number > 0) {
        cfg.preferred_number = (uint8_t)number;
        cfg.number_policy    = DJL_NUMBER_LOWEST_FREE;
    }
    cfg.model_code   = (uint8_t)model;
    cfg.observe_only = observe != 0;
    cfg.send_status  = !quiet_status && !observe;
    cfg.log          = logger;
    cfg.log_level    = g_verbose ? DJL_LOG_DEBUG : DJL_LOG_INFO;

    djl_context *ctx = NULL;
    djl_err e = djl_context_create(&cfg, &ctx);
    if (e != DJL_OK) {
        fprintf(stderr, "context create failed: %s\n", djl_strerror(e));
        return 1;
    }
    e = djl_context_start(ctx);
    if (e != DJL_OK) {
        fprintf(stderr, "context start failed: %s\n", djl_strerror(e));
        fprintf(stderr, "note: binding ports 50000-50004 may require "
                        "CAP_NET_BIND_SERVICE or root\n");
        djl_context_destroy(ctx);
        return 1;
    }

    if (g_dump_kind >= 0) djl_set_raw_hook(ctx, raw_hook, NULL);

    printf("monitoring %s%s ... Ctrl-C to stop\n",
           iface, observe ? " (observe only)" : "");

    uint64_t deadline = run_secs > 0 ? djl_now_ms() + (uint64_t)run_secs * 1000 : 0;
    int queried = 0;
    djl_event evs[64];
    while (!g_stop) {
        int n = djl_poll(ctx, evs, 64, 200);
        for (int i = 0; i < n; i++) handle(&evs[i], ctx);

        if (g_query_media && !queried && djl_is_online(ctx)) {
            queried = 1;
            djl_device_info d[DJL_MAX_DEVICES];
            size_t nd = djl_devices(ctx, d, DJL_MAX_DEVICES);
            static const djl_slot slots[] = { DJL_SLOT_SD, DJL_SLOT_USB, DJL_SLOT_CD };
            for (size_t k = 0; k < nd; k++) {
                if (d[k].is_mixer) continue;
                for (size_t j = 0; j < sizeof slots / sizeof slots[0]; j++) {
                    djl_err qe = djl_query_media(ctx, d[k].number, slots[j]);
                    printf("query p%u %s -> %s\n", d[k].number,
                           djl_slot_name(slots[j]), djl_strerror(qe));
                }
            }
            fflush(stdout);
        }
        fflush(stdout);
        if (deadline && djl_now_ms() >= deadline) break;
    }

    printf("\n--- final roster ---\n");
    djl_device_info devs[DJL_MAX_DEVICES];
    size_t nd = djl_devices(ctx, devs, DJL_MAX_DEVICES);
    for (size_t i = 0; i < nd; i++) {
        printf("  %-3u %-16s %u.%u.%u.%u  type=0x%02x model=0x%02x\n",
               devs[i].number, devs[i].name,
               devs[i].ip[0], devs[i].ip[1], devs[i].ip[2], devs[i].ip[3],
               devs[i].device_type, devs[i].model_code);
    }
    int m = djl_tempo_master(ctx);
    if (m > 0) printf("  tempo master: %d at %.2f BPM\n", m, djl_master_tempo(ctx));

    djl_context_destroy(ctx);
    return 0;
}
