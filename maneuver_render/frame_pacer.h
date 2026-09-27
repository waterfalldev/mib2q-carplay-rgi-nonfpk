/* Even frame pacing for the render loop.
 *
 * After a rendered frame the loop sleeps to a deadline one period after the previous
 * deadline, so frames start on a fixed grid instead of whenever the swap returns.  The
 * swap interval stays as a ceiling, but the rate no longer depends on it: under
 * eglSwapInterval(2) the car showed frame-locked clicks at up to 53/s and a rate that
 * wandered between 20 and 30.  A frame that overruns restarts the grid at its end, so
 * frames are never closer than one period and a late frame is never followed by
 * catch-up frames.  Deadlines chain from the previous deadline, not from the wake-up,
 * so a sleep that rounds up to the kernel tick (QNX 6.5) does not accumulate.
 *
 * The statistics show on the car what the loop actually achieved: the gaps between
 * frame starts, how long the swap blocked (about 0 means the swap interval is not
 * pacing anything) and how many frames overran the period.
 *
 * Pure arithmetic on CLOCK_MONOTONIC nanoseconds; tests/frame_pacer_test.c drives it
 * with a fake clock. */
#ifndef CR_FRAME_PACER_H
#define CR_FRAME_PACER_H

#include <stdint.h>
#include <stdio.h>

/* One summary line when drawing stops, at most every 10 s (a line covers every run
 * since the last one), and every 30 s while drawing continues. */
#define CR_PACER_REPORT_IDLE_NS (10LL * 1000000000LL)
#define CR_PACER_REPORT_BUSY_NS (30LL * 1000000000LL)

typedef struct {
    int64_t period_ns;
    int64_t deadline_ns;    /* start of the next frame; meaningful while running */
    int     running;        /* the previous loop iteration rendered */
    /* Statistics since the last report. */
    long    frames, runs, gaps, overruns;
    int64_t prev_start_ns, window_start_ns;
    int64_t gap_sum_ns, gap_min_ns, gap_max_ns, swap_min_ns, swap_max_ns;
    int64_t last_report_ns; /* 0 until the first report */
} cr_frame_pacer_t;

static void cr_pacer_reset_stats(cr_frame_pacer_t *p) {
    p->frames = p->runs = p->gaps = p->overruns = 0;
    p->gap_sum_ns = p->gap_min_ns = p->gap_max_ns = 0;
    p->swap_min_ns = p->swap_max_ns = 0;
}

static void cr_pacer_init(cr_frame_pacer_t *p, int64_t period_ns) {
    p->period_ns = period_ns;
    p->deadline_ns = p->prev_start_ns = p->window_start_ns = p->last_report_ns = 0;
    p->running = 0;
    cr_pacer_reset_stats(p);
}

/* The frame that started at start_ns has been swapped; the swap took swap_ns and it is
 * now now_ns.  Returns how long to sleep before the next loop iteration. */
static int64_t cr_pacer_frame_done(cr_frame_pacer_t *p, int64_t start_ns,
                                   int64_t swap_ns, int64_t now_ns) {
    if (!p->frames) p->window_start_ns = start_ns;
    if (!p->running || !p->frames) p->runs++;  /* a run continues across a busy report */
    if (p->running) {
        int64_t gap = start_ns - p->prev_start_ns;
        if (!p->gaps || gap < p->gap_min_ns) p->gap_min_ns = gap;
        if (!p->gaps || gap > p->gap_max_ns) p->gap_max_ns = gap;
        p->gap_sum_ns += gap;
        p->gaps++;
        p->deadline_ns += p->period_ns;
    } else {
        p->deadline_ns = start_ns + p->period_ns;
    }
    if (!p->frames || swap_ns < p->swap_min_ns) p->swap_min_ns = swap_ns;
    if (!p->frames || swap_ns > p->swap_max_ns) p->swap_max_ns = swap_ns;
    p->frames++;
    p->prev_start_ns = start_ns;
    p->running = 1;
    if (p->deadline_ns < now_ns) {
        p->deadline_ns = now_ns;
        p->overruns++;
    }
    return p->deadline_ns - now_ns;
}

/* The loop iteration drew nothing: the next frame starts a new run. */
static void cr_pacer_idle(cr_frame_pacer_t *p) {
    p->running = 0;
}

/* When a summary is due, writes it to buf, resets the statistics and returns 1. */
static int cr_pacer_report(cr_frame_pacer_t *p, int64_t now_ns, char *buf, size_t size) {
    if (!p->frames) return 0;
    if (p->running ? now_ns - p->window_start_ns < CR_PACER_REPORT_BUSY_NS
                   : p->last_report_ns && now_ns - p->last_report_ns < CR_PACER_REPORT_IDLE_NS)
        return 0;
    if (p->gaps) {
        double avg_ms = (double)p->gap_sum_ns / p->gaps / 1e6;
        snprintf(buf, size,
                 "frames=%ld runs=%ld fps=%.1f gap_ms=%.1f/%.1f/%.1f(min/avg/max) "
                 "swap_ms=%.1f/%.1f(min/max) overruns=%ld",
                 p->frames, p->runs, 1000.0 / avg_ms, p->gap_min_ns / 1e6, avg_ms,
                 p->gap_max_ns / 1e6, p->swap_min_ns / 1e6, p->swap_max_ns / 1e6,
                 p->overruns);
    } else {
        snprintf(buf, size, "frames=%ld runs=%ld swap_ms=%.1f/%.1f(min/max) overruns=%ld",
                 p->frames, p->runs, p->swap_min_ns / 1e6, p->swap_max_ns / 1e6,
                 p->overruns);
    }
    p->last_report_ns = now_ns;
    cr_pacer_reset_stats(p);
    return 1;
}

#endif
