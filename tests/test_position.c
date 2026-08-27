/* libdjlink beat-grid position tracking tests.
 *
 * These cover the path that gives pre-CDJ-3000 players an absolute playhead:
 * a status packet's beat number anchored against a beat grid, then beat packets
 * advancing it, with extrapolation in between. Grid values match the shape of a
 * real 128 BPM track read off the rig (first beat at 236 ms, 468.75 ms/beat).
 */
#include "djlink.h"
#include "djl_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern int djl_test_checks;
extern int djl_test_failures;

#define CHECK(cond, ...) do {                              \
    djl_test_checks++;                                     \
    if (!(cond)) {                                         \
        djl_test_failures++;                               \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
        printf(__VA_ARGS__);                               \
        printf("\n");                                      \
    }                                                      \
} while (0)

#define CHECK_EQ_I(a, b) do {                                          \
    djl_test_checks++;                                                 \
    long long _a = (long long)(a);                                     \
    long long _b = (long long)(b);                                     \
    if (_a != _b) {                                                    \
        djl_test_failures++;                                           \
        printf("FAIL %s:%d: %s = %lld, expected %lld\n",               \
               __FILE__, __LINE__, #a, _a, _b);                        \
    }                                                                  \
} while (0)

#define CHECK_NEAR(a, b, tol) do {                                     \
    djl_test_checks++;                                                 \
    long long _a = (long long)(a), _b = (long long)(b);                \
    long long _d = _a > _b ? _a - _b : _b - _a;                        \
    if (_d > (tol)) {                                                  \
        djl_test_failures++;                                           \
        printf("FAIL %s:%d: %s = %lld, expected %lld +/- %lld\n",      \
               __FILE__, __LINE__, #a, _a, _b, (long long)(tol));      \
    }                                                                  \
} while (0)

/* A 128 BPM grid: beat 1 at 236 ms, 468.75 ms per beat, 4/4. */
#define GRID_BEATS 64
static djl_beat_grid make_grid(djl_beat_grid_entry *storage, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        storage[i].beat_within_bar = (uint16_t)((i % 4) + 1);
        storage[i].tempo_x100      = 12800;
        storage[i].time_ms         = (uint32_t)(236.0 + 468.75 * (double)i);
    }
    djl_beat_grid g = { n, storage };
    return g;
}

static void test_grid_lookup(void)
{
    djl_beat_grid_entry st[GRID_BEATS];
    djl_beat_grid g = make_grid(st, GRID_BEATS);

    CHECK_EQ_I(djl_grid_time_of_beat(&g, 1), 236);
    CHECK_EQ_I(djl_grid_time_of_beat(&g, 2), 704);      /* 236 + 468.75 */
    CHECK_EQ_I(djl_grid_time_of_beat(&g, GRID_BEATS), st[GRID_BEATS - 1].time_ms);
    CHECK_EQ_I(djl_grid_time_of_beat(&g, 0), -1);       /* beats are 1-based */
    CHECK_EQ_I(djl_grid_time_of_beat(NULL, 1), -1);

    /* Players report beats past the end when looping the tail; extrapolate by
     * the final interval instead of failing. */
    int64_t last = djl_grid_time_of_beat(&g, GRID_BEATS);
    CHECK_NEAR(djl_grid_time_of_beat(&g, GRID_BEATS + 2), last + 2 * 468, 4);

    CHECK_EQ_I(djl_grid_beat_at_time(&g, 236), 1);
    CHECK_EQ_I(djl_grid_beat_at_time(&g, 700), 1);      /* still inside beat 1 */
    CHECK_EQ_I(djl_grid_beat_at_time(&g, 704), 2);
    CHECK_EQ_I(djl_grid_beat_at_time(&g, 0), 0);        /* before the first beat */
    CHECK_EQ_I(djl_grid_beat_at_time(&g, 100000), GRID_BEATS);
    CHECK_EQ_I(djl_grid_beat_at_time(NULL, 0), -1);

    /* Bar position must cycle 1..4 and survive out-of-range beats. */
    CHECK_EQ_I(djl_grid_beat_within_bar(&g, 1), 1);
    CHECK_EQ_I(djl_grid_beat_within_bar(&g, 4), 4);
    CHECK_EQ_I(djl_grid_beat_within_bar(&g, 5), 1);
    CHECK_EQ_I(djl_grid_beat_within_bar(&g, 0), 0);

    /* An empty grid must never be dereferenced. */
    djl_beat_grid empty = { 0, NULL };
    CHECK_EQ_I(djl_grid_time_of_beat(&empty, 1), -1);
    CHECK_EQ_I(djl_grid_beat_at_time(&empty, 500), -1);
    CHECK_EQ_I(djl_grid_beat_within_bar(&empty, 1), 0);

    /* A one-beat grid is degenerate but must not divide by zero or read [-1]. */
    djl_beat_grid_entry one_st[1];
    djl_beat_grid one = make_grid(one_st, 1);
    CHECK_EQ_I(djl_grid_time_of_beat(&one, 1), 236);
    CHECK_EQ_I(djl_grid_time_of_beat(&one, 9), 236);
}

/* Build a status packet view directly; we are testing position.c, not the
 * wire decoder. */
static djl_cdj_status status_at(int32_t beat, bool playing, double bpm)
{
    djl_cdj_status st;
    memset(&st, 0, sizeof st);
    st.number   = 2;
    st.beat     = beat;
    st.playing  = playing;
    st.bpm_x100 = (uint16_t)(bpm * 100.0);
    st.pitch1   = (uint32_t)DJL_NEUTRAL_PITCH;
    st.effective_bpm = bpm;
    st.beat_within_bar = (uint8_t)(beat > 0 ? ((beat - 1) % 4) + 1 : 0);
    return st;
}

static void test_status_anchors_to_grid(void)
{
    djl_beat_grid_entry st_store[GRID_BEATS];
    djl_beat_grid g = make_grid(st_store, GRID_BEATS);

    djl_pos_state ps;
    memset(&ps, 0, sizeof ps);
    ps.grid = &g;
    ps.position_ms = -1;

    /* Without a grid a status packet gives no absolute time; with one, the beat
     * number pins it exactly. This is the whole point of the feature. */
    djl_cdj_status s = status_at(9, true, 128.0);
    djl_pos_apply_status(&ps, &s, 1000);

    djl_position out;
    djl_pos_interpolate(&ps, 2, 1000, &out);
    CHECK(out.valid, "position must be valid once anchored");
    CHECK_EQ_I(out.position_ms, djl_grid_time_of_beat(&g, 9));
    CHECK_EQ_I(out.beat, 9);
    CHECK_EQ_I(out.beat_within_bar, 1);
    CHECK(out.definitive, "a grid-anchored status is a definitive fix");

    /* Between packets the playhead advances at pitch. 234 ms at 1.0x is half a
     * beat, so we stay on beat 9. */
    djl_pos_interpolate(&ps, 2, 1234, &out);
    CHECK_EQ_I(out.position_ms, djl_grid_time_of_beat(&g, 9) + 234);
    CHECK_EQ_I(out.beat, 9);
    /* A full beat later we must have rolled onto beat 10. */
    djl_pos_interpolate(&ps, 2, 1000 + 469, &out);
    CHECK_EQ_I(out.beat, 10);
    CHECK_EQ_I(out.beat_within_bar, 2);
}

static void test_pitch_scales_interpolation(void)
{
    djl_beat_grid_entry st_store[GRID_BEATS];
    djl_beat_grid g = make_grid(st_store, GRID_BEATS);
    djl_pos_state ps;
    memset(&ps, 0, sizeof ps);
    ps.grid = &g;
    ps.position_ms = -1;

    /* +8% pitch: 1000 ms of wall clock advances the playhead 1080 ms. */
    djl_cdj_status s = status_at(1, true, 138.24);
    s.pitch1 = djl_percent_to_pitch(8.0);
    djl_pos_apply_status(&ps, &s, 5000);
    djl_position out;
    djl_pos_interpolate(&ps, 2, 6000, &out);
    CHECK_NEAR(out.position_ms, 236 + 1080, 2);

    /* Paused decks must not drift at all. */
    djl_cdj_status p = status_at(5, false, 128.0);
    djl_pos_apply_status(&ps, &p, 7000);
    djl_pos_interpolate(&ps, 2, 9000, &out);
    CHECK_EQ_I(out.position_ms, djl_grid_time_of_beat(&g, 5));
    CHECK(!out.playing, "paused deck must report not playing");
}

static void test_beats_advance_position(void)
{
    djl_beat_grid_entry st_store[GRID_BEATS];
    djl_beat_grid g = make_grid(st_store, GRID_BEATS);
    djl_pos_state ps;
    memset(&ps, 0, sizeof ps);
    ps.grid = &g;
    ps.position_ms = -1;

    /* Anchor on beat 4, then feed beat packets: each should step exactly one
     * grid beat, which is how a pre-CDJ-3000 player stays in time. */
    djl_cdj_status s = status_at(4, true, 128.0);
    djl_pos_apply_status(&ps, &s, 10000);

    djl_beat b;
    memset(&b, 0, sizeof b);
    b.number = 2;
    b.pitch  = (uint32_t)DJL_NEUTRAL_PITCH;
    b.effective_bpm = 128.0;
    b.beat_within_bar = 1;

    /* A beat packet arriving a full beat later is genuinely the next beat. */
    djl_pos_apply_beat(&ps, &b, 10469);
    djl_position out;
    djl_pos_interpolate(&ps, 2, 10469, &out);
    CHECK_EQ_I(out.beat, 5);
    CHECK_EQ_I(out.position_ms, djl_grid_time_of_beat(&g, 5));

    djl_pos_apply_beat(&ps, &b, 10938);
    djl_pos_interpolate(&ps, 2, 10938, &out);
    CHECK_EQ_I(out.beat, 6);
    CHECK_EQ_I(out.position_ms, djl_grid_time_of_beat(&g, 6));

    /* A beat packet that overtakes the status describing the same beat (less
     * than a fifth of a beat in) must not double-count. */
    djl_pos_state ps2;
    memset(&ps2, 0, sizeof ps2);
    ps2.grid = &g;
    ps2.position_ms = -1;
    djl_cdj_status s2 = status_at(20, true, 128.0);
    djl_pos_apply_status(&ps2, &s2, 20000);
    djl_pos_apply_beat(&ps2, &b, 20010);          /* 10 ms in: same beat */
    djl_pos_interpolate(&ps2, 2, 20010, &out);
    CHECK_EQ_I(out.beat, 20);

    /* Beats never arrive while playing backwards, so the sign must flip back. */
    ps2.speed = -1.0;
    djl_pos_apply_beat(&ps2, &b, 20500);
    CHECK(ps2.speed > 0, "a beat packet implies forward playback");
}

static void test_jump_correction(void)
{
    djl_beat_grid_entry st_store[GRID_BEATS];
    djl_beat_grid g = make_grid(st_store, GRID_BEATS);
    djl_pos_state ps;
    memset(&ps, 0, sizeof ps);
    ps.grid = &g;
    ps.position_ms = -1;

    djl_cdj_status s = status_at(2, true, 128.0);
    djl_pos_apply_status(&ps, &s, 1000);

    /* The DJ hits a hot cue: the next status reports a distant beat. Our
     * extrapolation cannot see the jump, so the grid must correct it rather
     * than drift on from the old position. */
    djl_cdj_status jump = status_at(40, true, 128.0);
    djl_pos_apply_status(&ps, &jump, 1200);
    djl_position out;
    djl_pos_interpolate(&ps, 2, 1200, &out);
    CHECK_EQ_I(out.position_ms, djl_grid_time_of_beat(&g, 40));
    CHECK_EQ_I(out.beat, 40);

    /* A status that merely confirms where we already are must NOT snap, so the
     * sub-beat interpolation stays smooth instead of stuttering back. */
    djl_cdj_status same = status_at(41, true, 128.0);
    djl_pos_apply_status(&ps, &same, 1200 + 300);
    djl_pos_interpolate(&ps, 2, 1500, &out);
    int64_t beat41 = djl_grid_time_of_beat(&g, 41);
    CHECK(out.position_ms >= beat41 - 200 && out.position_ms <= beat41 + 500,
          "a confirming status must not snap the playhead backwards");
}

static void test_precise_position_wins(void)
{
    djl_beat_grid_entry st_store[GRID_BEATS];
    djl_beat_grid g = make_grid(st_store, GRID_BEATS);
    djl_pos_state ps;
    memset(&ps, 0, sizeof ps);
    ps.grid = &g;
    ps.position_ms = -1;

    /* A CDJ-3000 tells us the playhead directly; the grid must not override it,
     * because precise position survives scratching and sub-beat loops. */
    djl_precise_position pp;
    memset(&pp, 0, sizeof pp);
    pp.number = 2;
    pp.playhead_ms = 5000;
    pp.track_length_s = 225;
    pp.pitch_x100 = 0;
    pp.bpm_x10 = 1280;
    djl_pos_apply_precise(&ps, &pp, 1000);

    djl_position out;
    djl_pos_interpolate(&ps, 2, 1000, &out);
    CHECK_EQ_I(out.position_ms, 5000);
    CHECK_EQ_I(out.track_length_ms, 225000);
    /* The beat number should still be derived from the grid. */
    CHECK_EQ_I(out.beat, djl_grid_beat_at_time(&g, 5000));

    /* A status packet arriving afterwards must not drag us onto its beat. */
    djl_cdj_status s = status_at(2, true, 128.0);
    djl_pos_apply_status(&ps, &s, 1010);
    djl_pos_interpolate(&ps, 2, 1010, &out);
    CHECK(out.position_ms > 4900, "precise position must outrank the grid");

    /* Pitch is a percentage offset from normal speed, so -100% is a dead stop,
     * not reverse: speed = 1 + pitch%/100. */
    pp.pitch_x100 = -10000;          /* -100.00% */
    djl_pos_apply_precise(&ps, &pp, 2000);
    djl_pos_interpolate(&ps, 2, 2100, &out);
    CHECK(!out.reverse, "-100%% is stopped, not reverse");
    CHECK_EQ_I(out.position_ms, 5000);

    /* Past -100% the deck really is running backwards, which is what a
     * backward scratch on a CDJ-3000 looks like. */
    pp.pitch_x100 = -20000;          /* -200.00% = -1.0x */
    djl_pos_apply_precise(&ps, &pp, 3000);
    djl_pos_interpolate(&ps, 2, 3100, &out);
    CHECK(out.reverse, "beyond -100%% must report reverse");
    CHECK_NEAR(out.position_ms, 4900, 2);   /* 100 ms back from 5000 */
}

/* Regression: a deck parked at the end of a track, with the pitch fader off
 * zero, must stay put. The precise-position pitch field is a fader reading, not
 * a velocity, and inferring "playing" from it made the playhead run away.
 * Reproduced live on CDJ-3000X firmware 1.31 (191613 ms in a 191 s track,
 * fader +0.15%, deck stopped). */
static void test_stopped_at_track_end_does_not_drift(void)
{
    djl_pos_state ps;
    memset(&ps, 0, sizeof ps);
    ps.position_ms = -1;

    djl_precise_position pp;
    memset(&pp, 0, sizeof pp);
    pp.number = 2;
    pp.playhead_ms = 191613;
    pp.track_length_s = 191;
    pp.pitch_x100 = 15;             /* +0.15% on the fader */
    pp.bpm_x10 = 1282;
    djl_pos_apply_precise(&ps, &pp, 1000);

    /* Nothing has told us the deck is playing, so it must not be assumed. */
    CHECK(!ps.playing, "precise position must not invent a play state");

    djl_position out;
    djl_pos_interpolate(&ps, 2, 3000, &out);
    CHECK_EQ_I(out.position_ms, 191613);
    CHECK(!out.playing, "a stopped deck must report stopped");

    /* The reported playhead may sit just past a whole-second track length; that
     * must not be clamped away. */
    CHECK(out.position_ms > 191000, "a device-reported playhead is authoritative");

    /* Once a status packet says it is playing, extrapolation resumes but stops
     * at the end of the track rather than growing without bound. */
    djl_cdj_status s = status_at(-1, true, 128.2);
    s.beat = -1;
    djl_pos_apply_status(&ps, &s, 3000);
    djl_pos_interpolate(&ps, 2, 60000, &out);
    CHECK(out.position_ms <= 191613 + 1,
          "extrapolation past the end must be clamped, got %lld",
          (long long)out.position_ms);
}

static void test_no_grid_still_works(void)
{
    /* Without a grid we must degrade gracefully to the old behaviour rather
     * than reporting a bogus absolute time. */
    djl_pos_state ps;
    memset(&ps, 0, sizeof ps);
    ps.position_ms = -1;

    djl_cdj_status s = status_at(9, true, 128.0);
    djl_pos_apply_status(&ps, &s, 1000);
    djl_position out;
    djl_pos_interpolate(&ps, 2, 1500, &out);
    CHECK(out.valid, "state is still valid without a grid");
    CHECK_EQ_I(out.position_ms, 0);      /* unknown, reported as zero */
    CHECK_EQ_I(out.beat, 9);             /* the beat number still comes through */

    djl_beat b;
    memset(&b, 0, sizeof b);
    b.number = 2;
    b.pitch = (uint32_t)DJL_NEUTRAL_PITCH;
    b.effective_bpm = 128.0;
    b.beat_within_bar = 3;
    djl_pos_apply_beat(&ps, &b, 1600);
    djl_pos_interpolate(&ps, 2, 1600, &out);
    CHECK_EQ_I(out.beat_within_bar, 3);
}

void djl_test_position(void);
void djl_test_position(void)
{
    test_grid_lookup();
    test_status_anchors_to_grid();
    test_pitch_scales_interpolation();
    test_beats_advance_position();
    test_jump_correction();
    test_precise_position_wins();
    test_stopped_at_track_end_does_not_drift();
    test_no_grid_still_works();
}
