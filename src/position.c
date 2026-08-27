/* libdjlink playback-position tracking.
 *
 * Three information sources, in descending order of quality (see TimeFinder in
 * beat-link and ARCHITECTURE.md section 7):
 *   1. precise-position packets (CDJ-3000+, 30 ms) - absolute playhead in ms.
 *   2. beat packets + a beat grid - exact beat, mapped to ms. This is the only
 *      way to get an absolute position out of a pre-CDJ-3000 player.
 *   3. CDJ status (200 ms) - beat number, pitch, play state.
 *
 * This module keeps the raw state and interpolates it to an arbitrary instant
 * using pitch and the monotonic clock. It performs no I/O and owns no memory:
 * the beat grid is borrowed from the metadata cache under ctx->lock.
 */
#include "djl_internal.h"
#include <string.h>

/* ---------------- beat grid helpers ---------------- */

/* Time of a 1-based beat number. Players can report beats past the end of the
 * grid when looping the tail of a track, so extrapolate using the final beat
 * interval instead of failing (matching beat-link's timeOfBeat). */
int64_t djl_grid_time_of_beat(const djl_beat_grid *g, int32_t beat)
{
    if (!g || g->count == 0 || beat < 1) return -1;
    if ((uint32_t)beat <= g->count)
        return (int64_t)g->entries[beat - 1].time_ms;
    if (g->count < 2)
        return (int64_t)g->entries[0].time_ms;
    int64_t last = (int64_t)g->entries[g->count - 1].time_ms;
    int64_t prev = (int64_t)g->entries[g->count - 2].time_ms;
    int64_t interval = last - prev;
    if (interval < 0) interval = 0;
    return last + interval * ((int64_t)beat - (int64_t)g->count);
}

/* The 1-based beat playing at a given time, or -1. Binary search: grids run to
 * tens of thousands of entries and this is called per status packet. */
int32_t djl_grid_beat_at_time(const djl_beat_grid *g, int64_t ms)
{
    if (!g || g->count == 0) return -1;
    if (ms < (int64_t)g->entries[0].time_ms) return 0;   /* before the first beat */
    uint32_t lo = 0, hi = g->count - 1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1) / 2;
        if ((int64_t)g->entries[mid].time_ms <= ms) lo = mid;
        else hi = mid - 1;
    }
    return (int32_t)lo + 1;
}

uint8_t djl_grid_beat_within_bar(const djl_beat_grid *g, int32_t beat)
{
    if (!g || g->count == 0 || beat < 1) return 0;
    uint32_t i = ((uint32_t)beat <= g->count) ? (uint32_t)beat - 1 : g->count - 1;
    uint16_t bib = g->entries[i].beat_within_bar;
    return (bib >= 1 && bib <= 4) ? (uint8_t)bib : 0;
}

/* How far our extrapolation may drift from the grid before we snap back to it.
 * Two beats' worth is beat-link's tolerance for deciding the player jumped. */
static bool grid_disagrees(const djl_beat_grid *g, int64_t interpolated, int32_t beat)
{
    int32_t at = djl_grid_beat_at_time(g, interpolated);
    if (at < 0) return false;
    int32_t d = at - beat;
    if (d < 0) d = -d;
    return d >= 2;
}

/* ---------------- update paths ---------------- */

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
     * the speed multiplier is 1 + pitch%/100 = 1 + pitch_x100/10000. Past
     * -100% this goes negative, which is a genuine backward scratch. */
    s->speed = 1.0 + pp->pitch_x100 / 10000.0;
    if (pp->bpm_x10 != 0xffffffffu) s->bpm = pp->bpm_x10 / 10.0;
    /* Deliberately NOT inferring play state here. This field is the pitch
     * *fader* position, not a velocity: a deck parked at the end of a track
     * with the fader at +0.15% still reports a non-zero pitch, and treating
     * that as motion made the playhead run away from a static position (seen
     * live on CDJ-3000X firmware 1.31, both decks stopped at track end).
     * Play state belongs to the status packet's P1/F flags. */

    /* Keep the beat number in step so a later beat packet has a base to count
     * from even on a player that also sends precise position. */
    if (s->grid) {
        int32_t b = djl_grid_beat_at_time(s->grid, s->position_ms);
        if (b > 0) {
            s->beat = b;
            s->grid_beat_known = true;
            uint8_t bib = djl_grid_beat_within_bar(s->grid, b);
            if (bib) s->beat_within_bar = bib;
        }
    }
}

/* A beat packet is an exact phase marker. With a grid it pins the absolute
 * playhead; without one it only updates tempo and phase. */
void djl_pos_apply_beat(djl_pos_state *s, const djl_beat *b, uint64_t now)
{
    /* Carry the interpolated position forward to now before resetting time. */
    if (s->valid && s->position_ms >= 0 && s->playing) {
        int64_t elapsed = (int64_t)(now - s->at_ms);
        s->position_ms += (int64_t)((double)elapsed * s->speed);
    }
    s->valid          = true;
    s->at_ms          = now;
    s->beat_within_bar = b->beat_within_bar;
    if (b->effective_bpm > 0) s->bpm = b->effective_bpm;
    /* Beat packets carry the fader pitch (a positive multiplier), never a
     * scratch direction, so take it as-is. */
    if (b->pitch) s->speed = b->pitch / (double)DJL_NEUTRAL_PITCH;
    s->definitive = true;

    /* Beat packets carry no beat *number*, so a grid alone is not enough: we
     * also need a beat number from a status packet to count from. Advance it
     * only once we are far enough into the current beat that this really is the
     * next one, rather than a beat packet that overtook the status describing
     * the same beat (beat-link uses one fifth of a beat). */
    if (s->grid && s->grid_beat_known && s->beat > 0) {
        bool advance = true;
        if (s->position_ms >= 0 && s->bpm > 0) {
            int64_t beat_start = djl_grid_time_of_beat(s->grid, s->beat);
            if (beat_start >= 0) {
                double into = (double)(s->position_ms - beat_start);
                double fifth = 60000.0 / s->bpm / 5.0;
                if (into < fifth) advance = false;
            }
        }
        int32_t next = s->beat + (advance ? 1 : 0);
        if ((uint32_t)next > s->grid->count && s->grid->count)
            next = (int32_t)s->grid->count;      /* looping the last beat */
        s->beat = next;
        int64_t t = djl_grid_time_of_beat(s->grid, next);
        if (t >= 0) {
            s->position_ms = t;
            /* Beats only arrive while playing forward. */
            if (s->speed < 0) s->speed = -s->speed;
        }
        uint8_t bib = djl_grid_beat_within_bar(s->grid, next);
        if (bib) s->beat_within_bar = bib;
    }
}

void djl_pos_apply_status(djl_pos_state *s, const djl_cdj_status *st, uint64_t now)
{
    /* Carry position forward using the previous sample before we overwrite. */
    int64_t carried = -1;
    if (s->valid && s->position_ms >= 0 && s->playing) {
        int64_t elapsed = (int64_t)(now - s->at_ms);
        carried = s->position_ms + (int64_t)((double)elapsed * s->speed);
    } else if (s->valid) {
        carried = s->position_ms;
    }
    s->position_ms = carried;

    bool was_playing = s->playing;
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

    /* With a grid, the status beat number is an absolute anchor. Use it when we
     * have nothing better, and to correct drift or an outright jump (cue, loop,
     * needle search) that extrapolation cannot see. Precise position, where
     * available, always wins: it survives scratching and sub-beat loops. */
    s->grid_beat_known = (st->beat > 0);
    if (s->grid && st->beat > 0 && !s->from_precise) {
        int64_t grid_ms = djl_grid_time_of_beat(s->grid, st->beat);
        if (grid_ms >= 0) {
            bool snap = s->position_ms < 0 ||
                        grid_disagrees(s->grid, s->position_ms, st->beat) ||
                        (was_playing && !st->playing);
            if (snap) s->position_ms = grid_ms;
            s->definitive = true;
        }
        uint8_t bib = djl_grid_beat_within_bar(s->grid, st->beat);
        if (!s->beat_within_bar && bib) s->beat_within_bar = bib;
    }

    if (s->track_length_ms < 0 && s->grid && s->grid->count)
        s->track_length_ms = djl_grid_time_of_beat(s->grid, (int32_t)s->grid->count);
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
        int64_t moved = (int64_t)((double)elapsed * s->speed);
        pos += moved;
        /* Clamp only our own forward extrapolation. A playhead the device
         * reported is taken at face value: track_length_s is whole seconds, so
         * a genuine position can legitimately sit just past it (191613 ms in a
         * "191 s" track, observed live). */
        if (moved > 0 && s->track_length_ms > 0 && pos > s->track_length_ms)
            pos = (s->position_ms > s->track_length_ms) ? s->position_ms
                                                       : s->track_length_ms;
    }
    if (pos < 0) pos = 0;
    out->position_ms = pos;

    /* Report the beat the interpolated time actually falls on, so consumers
     * driving lighting cues off beat numbers stay in step between packets. */
    if (s->grid && s->grid->count) {
        int32_t b = djl_grid_beat_at_time(s->grid, pos);
        if (b > 0) {
            out->beat = b;
            uint8_t bib = djl_grid_beat_within_bar(s->grid, b);
            if (bib) out->beat_within_bar = bib;
        }
    }
}
