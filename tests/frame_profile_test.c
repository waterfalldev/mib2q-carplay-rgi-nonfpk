/* Slowest-frame attribution (maneuver_render/frame_profile.h) on a fake clock: stage times,
 * the iteration kept for the window's longest gap, runs broken by an idle iteration, the
 * late wake-up, events and masks, and the report line. */
#include "frame_profile.h"

#include <assert.h>
#include <string.h>

#define MS 1000000LL
#define S  1000000000LL

enum { POLL, DRAW, SWAP, PROBE, STAGES };
static const char *name(int stage) {
    static const char *names[] = { "poll", "draw", "swap", "probe" };
    return stage >= 0 && stage < STAGES ? names[stage] : "?";
}
static const char *const events[] = { "maneuver", "commit", "capture", NULL };

/* One iteration from start: poll 1 ms, draw, swap, probe; sleep asked, then late on waking.
 * Returns when the next iteration starts. */
static int64_t iter(cr_frame_profile_t *p, int64_t start, int rendered, int64_t draw, int64_t swap,
                    int64_t probe, int64_t sleep, int64_t late, unsigned ev, unsigned masks) {
    int64_t t = start;
    cr_profile_begin(p, POLL, t);
    t += 1 * MS; cr_profile_mark(p, DRAW, t);
    cr_profile_event(p, ev);
    t += draw;   cr_profile_mark(p, SWAP, t);
    t += swap;   cr_profile_mark(p, PROBE, t);
    t += probe;
    cr_profile_end(p, rendered, masks, sleep, t);
    return t + sleep + late;
}

static void test_stage_times(void) {
    cr_frame_profile_t p;
    cr_profile_init(&p);
    iter(&p, 10 * S, 1, 5 * MS, 7 * MS, 2 * MS, 18 * MS, 0, 0, 0);
    assert(p.prev.stage_ns[POLL] == 1 * MS && p.prev.stage_ns[DRAW] == 5 * MS);
    assert(p.prev.stage_ns[SWAP] == 7 * MS && p.prev.stage_ns[PROBE] == 2 * MS);
    assert(p.prev.work_ns == 15 * MS && p.prev.sleep_ns == 18 * MS);
    assert(p.worst_gap_ns == 0);                  /* one frame: no gap yet */
}

static void test_longest_gap_keeps_the_iteration_before_it(void) {
    cr_frame_profile_t p;
    char line[256];
    int64_t t = 0;
    cr_profile_init(&p);
    t = iter(&p, t, 1, 5 * MS, 5 * MS, 1 * MS, 22 * MS, 0, 1, 0);          /* gap 34 */
    t = iter(&p, t, 1, 5 * MS, 30 * MS, 140 * MS, 0, 4 * MS, 2 | 4, 2);    /* gap 180: a slow probe */
    t = iter(&p, t, 1, 5 * MS, 5 * MS, 1 * MS, 22 * MS, 0, 0, 0);          /* gap 34 */
    iter(&p, t, 1, 5 * MS, 5 * MS, 1 * MS, 22 * MS, 0, 0, 0);
    assert(p.worst_gap_ns == 180 * MS);
    assert(p.worst.stage_ns[PROBE] == 140 * MS && p.worst.masks == 2);
    assert(cr_profile_report(&p, p.worst.start_ns + 2 * S, name, events, line, sizeof(line)));
    assert(!strcmp(line, "gap=180.0ms 2.0s ago: work=176.0ms (probe=140.0 swap=30.0 draw=5.0) "
                         "sleep=0.0ms late=4.0ms masks=2 events=commit,capture"));
    /* The report starts a new window: nothing more until another gap. */
    assert(!cr_profile_report(&p, t, name, events, line, sizeof(line)));
}

static void test_idle_iteration_breaks_the_run(void) {
    cr_frame_profile_t p;
    char line[256];
    int64_t t = 0;
    cr_profile_init(&p);
    t = iter(&p, t, 1, 5 * MS, 5 * MS, 0, 23 * MS, 0, 0, 0);
    t = iter(&p, t, 0, 0, 0, 0, 900 * MS, 0, 0, 0);       /* idle: no frame, a long sleep */
    t = iter(&p, t, 1, 5 * MS, 5 * MS, 0, 23 * MS, 0, 0, 0);
    assert(p.worst_gap_ns == 0);                          /* no gap across the idle iteration */
    iter(&p, t, 1, 5 * MS, 5 * MS, 0, 23 * MS, 0, 0, 0);
    assert(p.worst_gap_ns == 34 * MS);
    assert(cr_profile_report(&p, t, name, events, line, sizeof(line)));
    assert(strstr(line, "gap=34.0ms ") && strstr(line, " events=none"));
}

static void test_equal_gap_keeps_the_first(void) {
    cr_frame_profile_t p;
    int64_t t = 0;
    cr_profile_init(&p);
    t = iter(&p, t, 1, 5 * MS, 5 * MS, 0, 23 * MS, 0, 0, 0);
    t = iter(&p, t, 1, 5 * MS, 5 * MS, 0, 23 * MS, 0, 0, 0);
    iter(&p, t, 1, 5 * MS, 5 * MS, 0, 23 * MS, 0, 0, 0);
    assert(p.worst_gap_ns == 34 * MS && p.worst.start_ns == 0);
}

static void test_short_buffer_stays_terminated(void) {
    cr_frame_profile_t p;
    char line[24];
    int64_t t = 0;
    cr_profile_init(&p);
    t = iter(&p, t, 1, 5 * MS, 5 * MS, 0, 23 * MS, 0, 7, 3);
    iter(&p, t, 1, 5 * MS, 5 * MS, 0, 23 * MS, 0, 0, 0);
    assert(cr_profile_report(&p, t, name, events, line, sizeof(line)));
    assert(strlen(line) == sizeof(line) - 1);
}

int main(void) {
    test_stage_times();
    test_longest_gap_keeps_the_iteration_before_it();
    test_idle_iteration_breaks_the_run();
    test_equal_gap_keeps_the_first();
    test_short_buffer_stays_terminated();
    printf("PASS\n");
    return 0;
}
