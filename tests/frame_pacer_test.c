/* Frame pacer arithmetic (maneuver_render/frame_pacer.h) on a fake clock: an even grid,
 * no drift from late wake-ups, no catch-up after an overrun, a fresh grid after idle, and
 * the summary line and its rate limit. */
#include "frame_pacer.h"

#include <assert.h>
#include <string.h>

#define MS 1000000LL
#define S  1000000000LL

/* One rendered iteration starting at start: draw for work, swap for swap, then sleep what
 * the pacer asks plus late (tick rounding).  Returns when the next iteration starts. */
static int64_t frame(cr_frame_pacer_t *p, int64_t start, int64_t work, int64_t swap,
                     int64_t late, int64_t *wait_out) {
    int64_t now = start + work + swap;
    int64_t wait = cr_pacer_frame_done(p, start, swap, now);
    assert(wait >= 0);
    if (wait_out) *wait_out = wait;
    return now + wait + late;
}

static void test_even_grid(void) {
    cr_frame_pacer_t p;
    int64_t t = 5 * S, wait, first = t;
    int i;
    cr_pacer_init(&p, 50 * MS);
    for (i = 0; i < 100; i++) {
        int64_t next = frame(&p, t, 8 * MS + (i % 7) * MS, (i % 3) * MS, 0, &wait);
        assert(next - t == 50 * MS);  /* uneven work, even starts */
        t = next;
    }
    assert(t - first == 100 * 50 * MS);
    assert(p.gap_min_ns == 50 * MS && p.gap_max_ns == 50 * MS && p.overruns == 0);
}

static void test_late_wakeups_do_not_drift(void) {
    cr_frame_pacer_t p;
    int64_t t = 0, first = 0;
    int i;
    cr_pacer_init(&p, 50 * MS);
    for (i = 0; i < 200; i++)
        t = frame(&p, t, 10 * MS, 0, 1 * MS, NULL);  /* every sleep wakes 1 ms late */
    /* Frame 200 starts 1 ms (one wake-up) after its slot, not 200 ms. */
    assert(t - first == 200 * 50 * MS + 1 * MS);
}

static void test_overrun_restarts_grid_without_catch_up(void) {
    cr_frame_pacer_t p;
    int64_t t = 0, wait, next;
    cr_pacer_init(&p, 50 * MS);
    t = frame(&p, t, 10 * MS, 0, 0, NULL);        /* 0 -> 50 */
    assert(t == 50 * MS);
    next = frame(&p, t, 80 * MS, 0, 0, &wait);    /* overruns: no sleep */
    assert(wait == 0 && p.overruns == 1 && next == 130 * MS);
    t = next;
    next = frame(&p, t, 10 * MS, 0, 0, &wait);    /* grid restarted at 130 */
    assert(next - t == 50 * MS);
    t = next;
    next = frame(&p, t, 10 * MS, 0, 0, NULL);
    assert(next - t == 50 * MS && p.gap_min_ns == 50 * MS);
}

static void test_idle_starts_fresh_grid_and_keeps_spacing(void) {
    cr_frame_pacer_t p;
    int64_t t = 0, wait;
    cr_pacer_init(&p, 50 * MS);
    t = frame(&p, t, 10 * MS, 0, 0, NULL);
    t = frame(&p, t, 10 * MS, 0, 0, &wait);
    assert(wait == 40 * MS);  /* the last frame of a run still sleeps its period */
    cr_pacer_idle(&p);
    t += 700 * MS;            /* idle polling, then a new command */
    frame(&p, t, 10 * MS, 0, 0, &wait);
    assert(wait == 40 * MS && p.runs == 2);
    assert(p.gaps == 1);      /* no gap measured across the idle stretch */
}

static void test_report_policy_and_line(void) {
    cr_frame_pacer_t p;
    char line[160];
    int64_t t = 1 * S;
    int i;
    cr_pacer_init(&p, 50 * MS);
    assert(!cr_pacer_report(&p, t, line, sizeof(line)));  /* nothing drawn */
    for (i = 0; i < 30; i++) t = frame(&p, t, 10 * MS, 2 * MS, 0, NULL);
    assert(!cr_pacer_report(&p, t, line, sizeof(line)));  /* still drawing, under 30 s */
    cr_pacer_idle(&p);
    assert(cr_pacer_report(&p, t, line, sizeof(line)));   /* first run ends: report */
    assert(strstr(line, "frames=30 runs=1 fps=20.0 gap_ms=50.0/50.0/50.0(min/avg/max) "
                        "swap_ms=2.0/2.0(min/max) overruns=0"));
    assert(p.frames == 0);

    t += 2 * S;                                            /* a second run 2 s later */
    for (i = 0; i < 5; i++) t = frame(&p, t, 10 * MS, 0, 0, NULL);
    cr_pacer_idle(&p);
    assert(!cr_pacer_report(&p, t, line, sizeof(line)));  /* under 10 s: held back */
    t += 9 * S;                                            /* a third run */
    t = frame(&p, t, 60 * MS, 0, 0, NULL);
    cr_pacer_idle(&p);
    assert(cr_pacer_report(&p, t, line, sizeof(line)));   /* merged line */
    assert(strstr(line, "frames=6 runs=2 ") && strstr(line, "overruns=1"));

    t += 20 * S;                                           /* a one-frame run: no gaps */
    t = frame(&p, t, 10 * MS, 0, 0, NULL);
    cr_pacer_idle(&p);
    assert(cr_pacer_report(&p, t, line, sizeof(line)));
    assert(!strcmp(line, "frames=1 runs=1 swap_ms=0.0/0.0(min/max) overruns=0"));
}

static void test_long_run_reports_every_30_s(void) {
    cr_frame_pacer_t p;
    char line[160];
    int64_t t = 0;
    int i, lines = 0;
    cr_pacer_init(&p, 50 * MS);
    for (i = 0; i < 20 * 95; i++) {  /* 95 s of continuous drawing */
        t = frame(&p, t, 10 * MS, 0, 0, NULL);
        if (cr_pacer_report(&p, t, line, sizeof(line))) {
            lines++;
            assert(strstr(line, "runs=1 fps=20.0"));  /* the run carries over */
        }
    }
    assert(lines == 3);
}

int main(void) {
    test_even_grid();
    test_late_wakeups_do_not_drift();
    test_overrun_restarts_grid_without_catch_up();
    test_idle_starts_fresh_grid_and_keeps_spacing();
    test_report_policy_and_line();
    test_long_run_reports_every_30_s();
    puts("frame_pacer: even grid/no drift/no catch-up/fresh grid after idle/report policy PASS");
    return 0;
}
