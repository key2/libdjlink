/* libdjlink wire codec: pure packet classification and field extraction.
 * No I/O, no allocation, no globals. Every accessor is bounds-checked.
 */
#include "djlink.h"
#include <string.h>

static const uint8_t DJL_MAGIC[DJL_MAGIC_LEN] = {
    0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c
};

#define KIND_OFF 0x0a

/* Device name offsets differ between the two framings. */
#define NAME_OFF_ANNOUNCE 0x0c
#define NAME_OFF_STATUS   0x0b

bool djl_wire_has_magic(const uint8_t *buf, size_t len)
{
    if (!buf || len < DJL_MAGIC_LEN) return false;
    return memcmp(buf, DJL_MAGIC, DJL_MAGIC_LEN) == 0;
}

int djl_wire_kind_byte(const uint8_t *buf, size_t len)
{
    if (!buf || len <= KIND_OFF) return -1;
    return buf[KIND_OFF];
}

bool djl_wire_is_announce_framing(uint16_t port)
{
    return port == DJL_PORT_ANNOUNCE;
}

djl_packet_kind djl_wire_classify(uint16_t port, const uint8_t *buf, size_t len)
{
    if (!djl_wire_has_magic(buf, len)) return DJL_PKT_UNKNOWN;
    int k = djl_wire_kind_byte(buf, len);
    if (k < 0) return DJL_PKT_UNKNOWN;

    switch (port) {
    case DJL_PORT_ANNOUNCE:
        switch (k) {
        case 0x00: return DJL_PKT_NUMBER_STAGE1;
        case 0x01: return DJL_PKT_NUMBER_WILL_ASSIGN;
        case 0x02: return DJL_PKT_NUMBER_STAGE2;
        case 0x03: return DJL_PKT_NUMBER_ASSIGN;
        case 0x04: return DJL_PKT_NUMBER_STAGE3;
        case 0x05: return DJL_PKT_NUMBER_FINISHED;
        case 0x06: return DJL_PKT_KEEP_ALIVE;
        case 0x08: return DJL_PKT_NUMBER_IN_USE;
        case 0x0a: return DJL_PKT_DEVICE_HELLO;
        default:   return DJL_PKT_UNKNOWN;
        }
    case DJL_PORT_BEAT:
        switch (k) {
        case 0x02: return DJL_PKT_FADER_START;
        case 0x03: return DJL_PKT_CHANNELS_ON_AIR;
        case 0x0b: return DJL_PKT_PRECISE_POSITION;
        case 0x26: return DJL_PKT_MASTER_HANDOFF_REQ;
        case 0x27: return DJL_PKT_MASTER_HANDOFF_RESP;
        case 0x28: return DJL_PKT_BEAT;
        case 0x2a: return DJL_PKT_SYNC_CONTROL;
        case 0x6a: return DJL_PKT_BEAT_HEARTBEAT;
        default:   return DJL_PKT_UNKNOWN;
        }
    case DJL_PORT_STATUS:
        switch (k) {
        case 0x05: return DJL_PKT_MEDIA_QUERY;
        case 0x06: return DJL_PKT_MEDIA_RESPONSE;
        case 0x0a: return DJL_PKT_CDJ_STATUS;
        case 0x10: return DJL_PKT_RB_LIGHTING_HELLO;
        case 0x19: return DJL_PKT_LOAD_TRACK;
        case 0x1a: return DJL_PKT_LOAD_TRACK_ACK;
        case 0x29: return DJL_PKT_MIXER_STATUS;
        case 0x34: return DJL_PKT_LOAD_SETTINGS;
        case 0x39: return DJL_PKT_MIXER_STATE_A9;
        case 0x40: return DJL_PKT_KUVO_GATEWAY;
        case 0x55: return DJL_PKT_OPUS_DATA_REQ;
        case 0x56: return DJL_PKT_OPUS_DATA_RESP;
        case 0x58: return DJL_PKT_VU_STREAM;
        default:   return DJL_PKT_UNKNOWN;
        }
    case DJL_PORT_AUDIO:
        switch (k) {
        case 0x1e: return DJL_PKT_AUDIO_DATA;
        case 0x1f: return DJL_PKT_AUDIO_HANDOVER;
        case 0x20: return DJL_PKT_AUDIO_TIMING;
        default:   return DJL_PKT_UNKNOWN;
        }
    default:
        return DJL_PKT_UNKNOWN;
    }
}

const char *djl_packet_kind_name(djl_packet_kind k)
{
    switch (k) {
    case DJL_PKT_NUMBER_STAGE1:      return "NumberClaimStage1";
    case DJL_PKT_NUMBER_WILL_ASSIGN: return "NumberWillAssign";
    case DJL_PKT_NUMBER_STAGE2:      return "NumberClaimStage2";
    case DJL_PKT_NUMBER_ASSIGN:      return "NumberAssign";
    case DJL_PKT_NUMBER_STAGE3:      return "NumberClaimStage3";
    case DJL_PKT_NUMBER_FINISHED:    return "NumberAssignFinished";
    case DJL_PKT_KEEP_ALIVE:         return "KeepAlive";
    case DJL_PKT_NUMBER_IN_USE:      return "NumberInUse";
    case DJL_PKT_DEVICE_HELLO:       return "DeviceHello";
    case DJL_PKT_FADER_START:        return "FaderStart";
    case DJL_PKT_CHANNELS_ON_AIR:    return "ChannelsOnAir";
    case DJL_PKT_PRECISE_POSITION:   return "PrecisePosition";
    case DJL_PKT_MASTER_HANDOFF_REQ: return "MasterHandoffRequest";
    case DJL_PKT_MASTER_HANDOFF_RESP:return "MasterHandoffResponse";
    case DJL_PKT_BEAT:               return "Beat";
    case DJL_PKT_SYNC_CONTROL:       return "SyncControl";
    case DJL_PKT_BEAT_HEARTBEAT:     return "BeatHeartbeat";
    case DJL_PKT_MEDIA_QUERY:        return "MediaQuery";
    case DJL_PKT_MEDIA_RESPONSE:     return "MediaResponse";
    case DJL_PKT_CDJ_STATUS:         return "CdjStatus";
    case DJL_PKT_RB_LIGHTING_HELLO:  return "RekordboxLightingHello";
    case DJL_PKT_LOAD_TRACK:         return "LoadTrack";
    case DJL_PKT_LOAD_TRACK_ACK:     return "LoadTrackAck";
    case DJL_PKT_MIXER_STATUS:       return "MixerStatus";
    case DJL_PKT_LOAD_SETTINGS:      return "LoadSettings";
    case DJL_PKT_MIXER_STATE_A9:     return "MixerStateA9";
    case DJL_PKT_OPUS_DATA_REQ:      return "OpusDataRequest";
    case DJL_PKT_OPUS_DATA_RESP:     return "OpusDataResponse";
    case DJL_PKT_VU_STREAM:          return "VuStream";
    case DJL_PKT_KUVO_GATEWAY:       return "KuvoGateway";
    case DJL_PKT_AUDIO_DATA:         return "AudioData";
    case DJL_PKT_AUDIO_HANDOVER:     return "AudioHandover";
    case DJL_PKT_AUDIO_TIMING:       return "AudioTiming";
    default:                         return "Unknown";
    }
}

/* ---------------- primitive accessors ---------------- */

uint64_t djl_wire_be(const uint8_t *buf, size_t len, size_t off, size_t nbytes, bool *ok)
{
    if (ok) *ok = false;
    if (!buf || nbytes == 0 || nbytes > 8) return 0;
    if (off + nbytes > len) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < nbytes; i++) v = (v << 8) | buf[off + i];
    if (ok) *ok = true;
    return v;
}

uint64_t djl_wire_le(const uint8_t *buf, size_t len, size_t off, size_t nbytes, bool *ok)
{
    if (ok) *ok = false;
    if (!buf || nbytes == 0 || nbytes > 8) return 0;
    if (off + nbytes > len) return 0;
    uint64_t v = 0;
    for (size_t i = nbytes; i-- > 0;) v = (v << 8) | buf[off + i];
    if (ok) *ok = true;
    return v;
}

int djl_wire_u8(const uint8_t *buf, size_t len, size_t off)
{
    if (!buf || off >= len) return -1;
    return buf[off];
}

djl_err djl_wire_device_name(uint16_t port, const uint8_t *buf, size_t len,
                             char *out, size_t outsz)
{
    if (!out || outsz < DJL_NAME_LEN + 1) return DJL_ERR_INVAL;
    size_t off = djl_wire_is_announce_framing(port) ? NAME_OFF_ANNOUNCE : NAME_OFF_STATUS;
    if (off + DJL_NAME_LEN > len) return DJL_ERR_SHORT;
    size_t n = 0;
    for (size_t i = 0; i < DJL_NAME_LEN; i++) {
        uint8_t c = buf[off + i];
        if (c == 0) break;
        /* keep it printable; the field is ASCII in all observed hardware */
        out[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    out[n] = '\0';
    /* trim trailing spaces */
    while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
    return DJL_OK;
}

int djl_wire_device_number(uint16_t port, const uint8_t *buf, size_t len)
{
    size_t off = djl_wire_is_announce_framing(port) ? 0x24 : 0x21;
    return djl_wire_u8(buf, len, off);
}

/* ---------------- numeric helpers ---------------- */

double djl_pitch_percent(uint32_t raw)
{
    return ((double)raw - (double)DJL_NEUTRAL_PITCH) / ((double)DJL_NEUTRAL_PITCH / 100.0);
}

double djl_pitch_multiplier(uint32_t raw)
{
    return (double)raw / (double)DJL_NEUTRAL_PITCH;
}

uint32_t djl_percent_to_pitch(double percent)
{
    double v = (percent * (double)DJL_NEUTRAL_PITCH / 100.0) + (double)DJL_NEUTRAL_PITCH;
    if (v < 0) v = 0;
    return (uint32_t)(v + 0.5);
}

double djl_effective_bpm(uint32_t bpm_x100, uint32_t pitch_raw)
{
    if (bpm_x100 == 0xffff) return 0.0;
    return (bpm_x100 / 100.0) * djl_pitch_multiplier(pitch_raw);
}

int64_t djl_halfframe_to_ms(int64_t hf) { return hf * 100 / 15; }
int64_t djl_ms_to_halfframe(int64_t ms) { return ms * 15 / 100; }

/* ---------------- UTF-16BE -> UTF-8 (BMP subset) ---------------- */

static void utf16be_to_utf8(const uint8_t *src, size_t src_bytes, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; i + 1 < src_bytes; i += 2) {
        uint32_t cp = ((uint32_t)src[i] << 8) | src[i + 1];
        if (cp == 0) break;
        if (cp < 0x80) {
            if (o + 1 >= outsz) break;
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            if (o + 2 >= outsz) break;
            out[o++] = (char)(0xc0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        } else {
            if (o + 3 >= outsz) break;
            out[o++] = (char)(0xe0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    out[o] = '\0';
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) out[--o] = '\0';
}

/* ---------------- structured decoders ---------------- */

djl_err djl_decode_keep_alive(const uint8_t *buf, size_t len, djl_keep_alive *out)
{
    if (!out) return DJL_ERR_INVAL;
    if (!djl_wire_has_magic(buf, len)) return DJL_ERR_UNKNOWN;
    if (len < 0x36) return DJL_ERR_SHORT;
    memset(out, 0, sizeof *out);

    djl_err e = djl_wire_device_name(DJL_PORT_ANNOUNCE, buf, len, out->name, sizeof out->name);
    if (e != DJL_OK) return e;

    out->proto_version = buf[0x21];
    out->number        = buf[0x24];
    out->was_first     = buf[0x25];
    memcpy(out->mac, buf + 0x26, 6);
    memcpy(out->ip,  buf + 0x2c, 4);
    out->peer_count    = buf[0x30];
    out->device_type   = buf[0x34];
    out->model_code    = buf[0x35];
    return DJL_OK;
}

/* Settings blocks are prefixed with 12 34 56 78. Returns true and fills the
 * two known settings if a valid block is found at the given offset. */
static bool settings_block(const uint8_t *buf, size_t len, size_t off,
                           uint8_t *color, uint8_t *position)
{
    if (off + 0x0e > len) return false;
    if (!(buf[off] == 0x12 && buf[off+1] == 0x34 &&
          buf[off+2] == 0x56 && buf[off+3] == 0x78)) return false;
    if (color)    *color    = buf[off + 0x0a];
    if (position) *position = buf[off + 0x0d];
    return true;
}

djl_err djl_decode_cdj_status(const uint8_t *buf, size_t len, djl_cdj_status *out)
{
    if (!out) return DJL_ERR_INVAL;
    if (!djl_wire_has_magic(buf, len)) return DJL_ERR_UNKNOWN;
    /* Oldest players send 0xd0 bytes. Anything shorter is not a status packet. */
    if (len < 0xd0) return DJL_ERR_SHORT;
    memset(out, 0, sizeof *out);
    out->packet_len = (uint16_t)len;

    djl_err e = djl_wire_device_name(DJL_PORT_STATUS, buf, len, out->name, sizeof out->name);
    if (e != DJL_OK) return e;

    bool ok;
    out->subtype      = buf[0x20];
    out->number       = buf[0x21];
    out->activity     = buf[0x27];
    out->track_device = buf[0x28];
    out->track_slot   = (djl_slot)buf[0x29];
    out->track_type   = (djl_track_type)buf[0x2a];
    out->rekordbox_id = (uint32_t)djl_wire_be(buf, len, 0x2c, 4, &ok);
    out->track_number = (uint16_t)djl_wire_be(buf, len, 0x32, 2, &ok);
    out->sort_mode    = buf[0x35];
    out->track_source = buf[0x37];
    out->category1    = (uint32_t)djl_wire_be(buf, len, 0x38, 4, &ok);
    out->category2    = (uint32_t)djl_wire_be(buf, len, 0x3c, 4, &ok);
    out->list_count   = (uint16_t)djl_wire_be(buf, len, 0x46, 2, &ok);

    out->usb_local      = buf[0x6f];
    out->sd_local       = buf[0x73];
    out->link_available = buf[0x75];

    out->play_state1  = buf[0x7b];
    memcpy(out->firmware, buf + 0x7c, 4);
    out->firmware[4] = '\0';
    for (int i = 0; i < 4; i++)
        if (out->firmware[i] < 0x20 || out->firmware[i] >= 0x7f) out->firmware[i] = '\0';

    out->sync_counter = (uint32_t)djl_wire_be(buf, len, 0x84, 4, &ok);
    out->flags        = buf[0x89];
    out->play_state2  = buf[0x8b];
    out->pitch1       = (uint32_t)djl_wire_be(buf, len, 0x8c, 4, &ok);
    out->tempo_validity = (uint16_t)djl_wire_be(buf, len, 0x90, 2, &ok);
    out->bpm_x100     = (uint16_t)djl_wire_be(buf, len, 0x92, 2, &ok);
    out->pitch2       = (uint32_t)djl_wire_be(buf, len, 0x98, 4, &ok);
    out->play_state3  = buf[0x9d];
    out->master_meaningful = buf[0x9e];
    out->master_handoff    = buf[0x9f];

    uint32_t beat_raw = (uint32_t)djl_wire_be(buf, len, 0xa0, 4, &ok);
    out->beat = (beat_raw == 0xffffffffu) ? -1 : (int32_t)beat_raw;
    out->cue_countdown  = (uint16_t)djl_wire_be(buf, len, 0xa4, 2, &ok);
    out->beat_within_bar = buf[0xa6];

    if (len > 0xb7) out->media_present     = buf[0xb7];
    if (len > 0xb8) out->usb_unsafe_eject  = buf[0xb8];
    if (len > 0xb9) out->sd_unsafe_eject   = buf[0xb9];
    if (len > 0xba) out->emergency_loop    = buf[0xba];

    if (len >= 0xc8) out->pitch3 = (uint32_t)djl_wire_be(buf, len, 0xc0, 4, &ok);
    if (len >= 0xc8) out->pitch4 = (uint32_t)djl_wire_be(buf, len, 0xc4, 4, &ok);
    if (len >= 0xcc) out->packet_counter = (uint32_t)djl_wire_be(buf, len, 0xc8, 4, &ok);
    if (len > 0xcc)  out->hardware_hint   = buf[0xcc];
    if (len > 0xcd)  out->touch_audio_caps = buf[0xcd];

    /* Settings blocks: block 1 at 0xd0, block 2 (CDJ-3000) at 0xff. */
    settings_block(buf, len, 0xd0, &out->waveform_color, &out->waveform_position);

    /* CDJ-3000 extended region. */
    if (len >= 0x200) {
        out->has_extended = true;
        if (len > 0x158) out->master_tempo    = buf[0x158];
        if (len > 0x15e) {
            out->key_note       = buf[0x15c];
            out->key_major      = buf[0x15d];
            out->key_accidental = buf[0x15e];
        }
        out->key_shift_cents = (int64_t)djl_wire_be(buf, len, 0x164, 8, &ok);
        out->loop_start_raw  = (uint32_t)djl_wire_be(buf, len, 0x1b6, 4, &ok);
        out->loop_end_raw    = (uint32_t)djl_wire_be(buf, len, 0x1be, 4, &ok);
        out->loop_beats      = (uint16_t)djl_wire_be(buf, len, 0x1c8, 2, &ok);
    }

    /* Derived. */
    out->playing       = (out->flags & DJL_F_PLAY)   != 0;
    out->master        = (out->flags & DJL_F_MASTER) != 0;
    out->synced        = (out->flags & DJL_F_SYNC)   != 0;
    out->on_air        = (out->flags & DJL_F_ON_AIR) != 0;
    out->bpm_only_sync = (out->flags & DJL_F_BPM_ONLY) != 0;
    out->pitch_percent = djl_pitch_percent(out->pitch1);
    out->effective_bpm = djl_effective_bpm(out->bpm_x100, out->pitch1);

    /* Pre-nexus players always send 0 for F; fall back to P1 for play state. */
    if (out->flags == 0)
        out->playing = (out->play_state1 == DJL_PLAY1_PLAYING ||
                        out->play_state1 == DJL_PLAY1_LOOPING ||
                        out->play_state1 == DJL_PLAY1_CUE_PLAY);
    return DJL_OK;
}

djl_err djl_decode_mixer_status(const uint8_t *buf, size_t len, djl_mixer_status *out)
{
    if (!out) return DJL_ERR_INVAL;
    if (!djl_wire_has_magic(buf, len)) return DJL_ERR_UNKNOWN;
    if (len < 0x38) return DJL_ERR_SHORT;
    memset(out, 0, sizeof *out);

    djl_err e = djl_wire_device_name(DJL_PORT_STATUS, buf, len, out->name, sizeof out->name);
    if (e != DJL_OK) return e;

    bool ok;
    out->number         = buf[0x21];
    out->flags          = buf[0x27];
    out->pitch          = (uint32_t)djl_wire_be(buf, len, 0x28, 4, &ok);
    out->bpm_x100       = (uint16_t)djl_wire_be(buf, len, 0x2e, 2, &ok);
    out->master_handoff = buf[0x36];
    out->beat_within_bar = buf[0x37];
    out->master         = (out->flags & DJL_F_MASTER) != 0;
    out->effective_bpm  = djl_effective_bpm(out->bpm_x100, out->pitch);
    return DJL_OK;
}

djl_err djl_decode_beat(const uint8_t *buf, size_t len, djl_beat *out)
{
    if (!out) return DJL_ERR_INVAL;
    if (!djl_wire_has_magic(buf, len)) return DJL_ERR_UNKNOWN;
    if (len < 0x60) return DJL_ERR_SHORT;
    memset(out, 0, sizeof *out);

    djl_err e = djl_wire_device_name(DJL_PORT_BEAT, buf, len, out->name, sizeof out->name);
    if (e != DJL_OK) return e;

    bool ok;
    out->number         = buf[0x21];
    out->next_beat_ms   = (uint32_t)djl_wire_be(buf, len, 0x24, 4, &ok);
    out->second_beat_ms = (uint32_t)djl_wire_be(buf, len, 0x28, 4, &ok);
    out->next_bar_ms    = (uint32_t)djl_wire_be(buf, len, 0x2c, 4, &ok);
    out->fourth_beat_ms = (uint32_t)djl_wire_be(buf, len, 0x30, 4, &ok);
    out->second_bar_ms  = (uint32_t)djl_wire_be(buf, len, 0x34, 4, &ok);
    out->eighth_beat_ms = (uint32_t)djl_wire_be(buf, len, 0x38, 4, &ok);
    out->pitch          = (uint32_t)djl_wire_be(buf, len, 0x54, 4, &ok);
    out->bpm_x100       = (uint16_t)djl_wire_be(buf, len, 0x5a, 2, &ok);
    out->beat_within_bar = buf[0x5c];
    out->effective_bpm  = djl_effective_bpm(out->bpm_x100, out->pitch);
    return DJL_OK;
}

djl_err djl_decode_precise_position(const uint8_t *buf, size_t len,
                                    djl_precise_position *out)
{
    if (!out) return DJL_ERR_INVAL;
    if (!djl_wire_has_magic(buf, len)) return DJL_ERR_UNKNOWN;
    if (len < 0x3c) return DJL_ERR_SHORT;
    memset(out, 0, sizeof *out);

    bool ok;
    out->number         = buf[0x21];
    out->track_length_s = (uint32_t)djl_wire_be(buf, len, 0x24, 4, &ok);
    out->playhead_ms    = (uint32_t)djl_wire_be(buf, len, 0x28, 4, &ok);
    out->pitch_x100     = (int32_t)(uint32_t)djl_wire_be(buf, len, 0x2c, 4, &ok);
    out->bpm_x10        = (uint32_t)djl_wire_be(buf, len, 0x38, 4, &ok);
    return DJL_OK;
}

djl_err djl_decode_media_details(const uint8_t *buf, size_t len, djl_media_details *out)
{
    if (!out) return DJL_ERR_INVAL;
    if (!djl_wire_has_magic(buf, len)) return DJL_ERR_UNKNOWN;
    if (len < 0xc0) return DJL_ERR_SHORT;
    memset(out, 0, sizeof *out);

    bool ok;
    out->host_device = buf[0x27];
    out->slot        = (djl_slot)buf[0x2b];
    utf16be_to_utf8(buf + 0x2c, 0x40, out->media_name, sizeof out->media_name);
    utf16be_to_utf8(buf + 0x6c, 0x28, out->created,    sizeof out->created);
    out->track_count     = (uint16_t)djl_wire_be(buf, len, 0xa6, 2, &ok);
    out->color           = buf[0xa8];
    out->track_type      = (djl_track_type)buf[0xaa];
    out->has_my_settings = buf[0xab] != 0;
    out->playlist_count  = (uint16_t)djl_wire_be(buf, len, 0xae, 2, &ok);
    out->total_bytes     = djl_wire_be(buf, len, 0xb0, 8, &ok);
    out->free_bytes      = djl_wire_be(buf, len, 0xb8, 8, &ok);
    return DJL_OK;
}

/* ---------------- name tables ---------------- */

const char *djl_strerror(djl_err e)
{
    switch (e) {
    case DJL_OK:              return "ok";
    case DJL_ERR_INVAL:       return "invalid argument";
    case DJL_ERR_NOMEM:       return "out of memory";
    case DJL_ERR_IO:          return "I/O error";
    case DJL_ERR_NO_IFACE:    return "network interface unavailable";
    case DJL_ERR_NOT_FOUND:   return "not found";
    case DJL_ERR_TIMEOUT:     return "timeout";
    case DJL_ERR_STATE:       return "invalid state";
    case DJL_ERR_CONFLICT:    return "device number conflict";
    case DJL_ERR_SHORT:       return "packet too short";
    case DJL_ERR_UNKNOWN:     return "unrecognized packet";
    case DJL_ERR_UNAVAILABLE: return "unavailable";
    default:                  return "unknown error";
    }
}

const char *djl_slot_name(djl_slot s)
{
    switch (s) {
    case DJL_SLOT_NONE:       return "none";
    case DJL_SLOT_CD:         return "CD";
    case DJL_SLOT_SD:         return "SD";
    case DJL_SLOT_USB:        return "USB";
    case DJL_SLOT_COLLECTION: return "rekordbox";
    case DJL_SLOT_STREAM_DP:  return "StreamingDirectPlay";
    case DJL_SLOT_USB2:       return "USB2";
    case DJL_SLOT_BEATPORT:   return "BeatportLINK";
    default:                  return "?";
    }
}

const char *djl_track_type_name(djl_track_type t)
{
    switch (t) {
    case DJL_TRACK_NONE:       return "none";
    case DJL_TRACK_REKORDBOX:  return "rekordbox";
    case DJL_TRACK_UNANALYZED: return "unanalyzed";
    case DJL_TRACK_AUDIO_CD:   return "audioCD";
    case DJL_TRACK_STREAMING:  return "streaming";
    default:                   return "?";
    }
}

const char *djl_play_state1_name(unsigned v)
{
    switch (v) {
    case DJL_PLAY1_NO_TRACK:    return "no track";
    case DJL_PLAY1_LOADING:     return "loading";
    case DJL_PLAY1_PLAYING:     return "playing";
    case DJL_PLAY1_LOOPING:     return "looping";
    case DJL_PLAY1_PAUSED:      return "paused";
    case DJL_PLAY1_PAUSED_CUE:  return "paused at cue";
    case DJL_PLAY1_CUE_PLAY:    return "cue play";
    case DJL_PLAY1_CUE_SCRATCH: return "cue scratch";
    case DJL_PLAY1_SEARCHING:   return "searching";
    case DJL_PLAY1_SPUN_DOWN:   return "spun down";
    case DJL_PLAY1_ENDED:       return "ended";
    case DJL_PLAY1_EMERGENCY:   return "emergency loop";
    default:                    return "?";
    }
}

const char *djl_event_kind_name(djl_event_kind k)
{
    switch (k) {
    case DJL_EV_DEVICE_FOUND:        return "DeviceFound";
    case DJL_EV_DEVICE_LOST:         return "DeviceLost";
    case DJL_EV_OWN_NUMBER_ASSIGNED: return "OwnNumberAssigned";
    case DJL_EV_NUMBER_CONFLICT:     return "NumberConflict";
    case DJL_EV_CDJ_STATUS:          return "CdjStatus";
    case DJL_EV_MIXER_STATUS:        return "MixerStatus";
    case DJL_EV_BEAT:                return "Beat";
    case DJL_EV_PRECISE_POSITION:    return "PrecisePosition";
    case DJL_EV_MASTER_CHANGED:      return "MasterChanged";
    case DJL_EV_TEMPO_CHANGED:       return "TempoChanged";
    case DJL_EV_ON_AIR_CHANGED:      return "OnAirChanged";
    case DJL_EV_FADER_START:         return "FaderStart";
    case DJL_EV_MEDIA_DETAILS:       return "MediaDetails";
    case DJL_EV_TRACK_LOADED:        return "TrackLoaded";
    case DJL_EV_AUDIO_TIMING:        return "AudioTiming";
    case DJL_EV_POSITION:            return "Position";
    case DJL_EV_TRACK_METADATA:      return "TrackMetadata";
    case DJL_EV_WAVEFORM:            return "Waveform";
    case DJL_EV_BEAT_GRID:           return "BeatGrid";
    case DJL_EV_CUE_LIST:            return "CueList";
    case DJL_EV_ALBUM_ART:           return "AlbumArt";
    case DJL_EV_SIGNATURE:           return "Signature";
    case DJL_EV_UNKNOWN_PACKET:      return "UnknownPacket";
    default:                         return "?";
    }
}
