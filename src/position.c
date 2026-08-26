/* libdjlink playback-position tracking.
 *
 * Three information sources, in descending order of quality (see TimeFinder in
 * beat-link and ARCHITECTURE.md section 7):
 *   1. precise-position packets (CDJ-3000+, 30 ms) - absolute playhead in ms.
 *   2. beat packets + a beat grid - exact beat, mapped to ms (grid supplied by
 *      the metadata layer; without it, beats give phase/tempo only).
 *   3. CDJ status (200 ms) - beat, pitch, play state.
 *
 * This module keeps the raw state and interpolates it to an arbitrary instant
 * using pitch and the monotonic clock. It performs no I/O.
 */
#include "djl_internal.h"
#include <string.h>

/* CDJ-3000 precise position gives us the absolute playhead directly. */
void djl_pos_apply_precise(djl_pos_state *s, const djl_precise_position *pp, uint64_t now)
{
    s->valid          = true;
    s->at_ms          = now;
    s->position_ms    = (int64_t)pp->playhead_ms;
    s->definitive     = true;
    s->from_precise   = true;
    s->track_length_ms = (pp->track_length_s == 0 || pp->track_length_s == 0xffffffffu)
                         ? -1 : (int64_t)pp->track_length_s * 1000;
    /* pitch_x100 is a signed pitch *percentage* x100 (e.g. -50 = -0.50%), so
     * the speed multiplier is 1 + pitch%/100 = 1 + pitch_x100/10000. This is
     * naturally negative during a backward scratch, which is real reverse. */
    s->speed = 1.0 + pp->pitch_x100 / 10000.0;
    if (pp->bpm_x10 != 0xffffffffu) s->bpm = pp->bpm_x10 / 10.0;
    /* A precise-position stream only flows while the deck is active; treat a
     * non-zero speed as playing, and keep the last known state otherwise. */
    if (s->speed != 0.0) s->playing = true;
}

/* A beat packet is an exact phase marker; without a grid it does not give an
 * absolute time, so we update tempo/phase and mark the sample definitive but
 * leave position_ms to be carried/extrapolated. */
void djl_pos_apply_beat(djl_pos_state *s, const djl_beat *b, uint64_t now)
{
    /* Carry the interpolated position forward to now before resetting time. */
    if (s->valid && s->position_ms >= 0 && s->playing) {
        int64_t elapsed = (int64_t)(now - s->at_ms);
        s->position_ms += (int64_t)(elapsed * s->speed);
    }
    s->valid          = true;
    s->at_ms          = now;
    s->beat_within_bar = b->beat_within_bar;
    if (b->effective_bpm > 0) s->bpm = b->effective_bpm;
    /* Beat packets carry the fader pitch (a positive multiplier), never a
     * scratch direction, so take it as-is. */
    if (b->pitch) s->speed = b->pitch / (double)DJL_NEUTRAL_PITCH;
    s->definitive = true;
}

void djl_pos_apply_status(djl_pos_state *s, const djl_cdj_status *st, uint64_t now)
{
    /* Carry position forward using the previous sample before we overwrite. */
    if (s->valid && s->position_ms >= 0 && s->playing) {
        int64_t elapsed = (int64_t)(now - s->at_ms);
        s->position_ms += (int64_t)(elapsed * s->speed);
    }
    s->at_ms   = now;
    s->valid   = true;
    s->beat    = st->beat;
    s->beat_within_bar = st->beat_within_bar;
    s->playing = st->playing;
    if (st->bpm_x100 != 0xffff) s->bpm = st->effective_bpm;
    /* Status carries the fader pitch as a positive multiplier (no direction). */
    s->speed = st->pitch1 / (double)DJL_NEUTRAL_PITCH;
    /* A status packet is not an absolute position fix. */
    s->definitive = s->from_precise;
    if (!st->playing) s->definitive = true;   /* stopped position is exact */
}

void djl_pos_interpolate(const djl_pos_state *s, uint8_t player, uint64_t now,
                         djl_position *out)
{
    memset(out, 0, sizeof *out);
    out->player = player;
    out->beat = -1;
    out->track_length_ms = -1;
    if (!s || !s->valid) return;

    out->valid           = true;
    out->playing         = s->playing;
    out->reverse         = s->speed < 0;
    out->pitch           = s->speed;
    out->effective_bpm   = s->bpm;
    out->beat            = s->beat;
    out->beat_within_bar = s->beat_within_bar;
    out->track_length_ms = s->track_length_ms;
    out->definitive      = s->definitive;

    if (s->position_ms < 0) { out->beat = s->beat; return; }  /* time unknown */

    int64_t pos = s->position_ms;
    if (s->playing) {
        int64_t elapsed = (int64_t)(now - s->at_ms);
        pos += (int64_t)(elapsed * s->speed);
    }
    if (pos < 0) pos = 0;
    if (s->track_length_ms > 0 && pos > s->track_length_ms) pos = s->track_length_ms;
    out->position_ms = pos;
}
