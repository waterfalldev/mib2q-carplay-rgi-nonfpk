/* The slowest frame of each pacing window, attributed.
 *
 * The pacer (frame_pacer.h) reports the longest gap between frame starts.  On the car that
 * gap was 150-200 ms once per window since nfc20, and the swap wait did not explain it.  This
 * splits every loop iteration by stage (main.c's watchdog stage marks) and tags it with what
 * it did.  When the gap that follows a rendered iteration is the longest of the window, that
 * iteration is kept: its time by stage, the sleep the pacer asked for, and how much longer
 * the gap was than work plus sleep (a late wake-up).  Gaps are measured as the pacer measures
 * them: between consecutive rendered iterations, a run restarting after an idle one.
 *
 * Pure arithmetic on CLOCK_MONOTONIC nanoseconds; tests/frame_profile_test.c drives it
 * with a fake clock. */
#ifndef CR_FRAME_PROFILE_H
#define CR_FRAME_PROFILE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CR_PROFILE_STAGES 32
#define CR_PROFILE_TOP    3     /* stages named in a report, longest first */

typedef struct {
    int64_t  stage_ns[CR_PROFILE_STAGES];
    int64_t  start_ns, work_ns, sleep_ns;
    unsigned events;            /* caller-defined bits */
    unsigned masks;             /* transition-mask layers painted */
} cr_profile_iter_t;

typedef struct {
    int      stage;             /* stage being timed, since mark_ns */
    int64_t  mark_ns;
    cr_profile_iter_t cur;      /* the iteration in progress */
    cr_profile_iter_t prev;     /* the last rendered iteration, while a run continues */
    int      have_prev;
    cr_profile_iter_t worst;    /* the iteration before the window's longest gap */
    int64_t  worst_gap_ns;      /* 0: no gap yet this window */
} cr_frame_profile_t;

static void cr_profile_init(cr_frame_profile_t *p) {
    memset(p, 0, sizeof(*p));
    p->stage = -1;
}

/* Time since the last mark goes to the stage being left. */
static void cr_profile_mark(cr_frame_profile_t *p, int stage, int64_t now_ns) {
    if (p->stage >= 0 && p->stage < CR_PROFILE_STAGES)
        p->cur.stage_ns[p->stage] += now_ns - p->mark_ns;
    p->stage = stage;
    p->mark_ns = now_ns;
}

/* A loop iteration starts at now_ns in stage. */
static void cr_profile_begin(cr_frame_profile_t *p, int stage, int64_t now_ns) {
    memset(&p->cur, 0, sizeof(p->cur));
    p->cur.start_ns = now_ns;
    p->stage = stage;
    p->mark_ns = now_ns;
}

static void cr_profile_event(cr_frame_profile_t *p, unsigned bits) {
    p->cur.events |= bits;
}

/* The iteration ends at now_ns, before its sleep: rendered as for the pacer, masks painted
 * during it, sleep_ns what the pacer asked for. */
static void cr_profile_end(cr_frame_profile_t *p, int rendered, unsigned masks,
                           int64_t sleep_ns, int64_t now_ns) {
    cr_profile_mark(p, p->stage, now_ns);
    p->cur.work_ns = now_ns - p->cur.start_ns;
    p->cur.sleep_ns = sleep_ns;
    p->cur.masks = masks;
    if (!rendered) {
        p->have_prev = 0;
        return;
    }
    if (p->have_prev) {
        int64_t gap = p->cur.start_ns - p->prev.start_ns;
        if (gap > p->worst_gap_ns) {
            p->worst_gap_ns = gap;
            p->worst = p->prev;
        }
    }
    p->prev = p->cur;
    p->have_prev = 1;
}

/* When the window had a gap, writes its attribution to buf, starts a new window and
 * returns 1.  stage_name names a stage index; event_names[i] names bit i, NULL-terminated. */
static int cr_profile_report(cr_frame_profile_t *p, int64_t now_ns,
                             const char *(*stage_name)(int), const char *const *event_names,
                             char *buf, size_t size) {
    const cr_profile_iter_t *w = &p->worst;
    int top[CR_PROFILE_TOP], used[CR_PROFILE_STAGES], n, i;
    size_t len;
    if (!p->worst_gap_ns) return 0;
    memset(used, 0, sizeof(used));
    for (n = 0; n < CR_PROFILE_TOP; n++) {
        int best = -1;
        for (i = 0; i < CR_PROFILE_STAGES; i++)
            if (!used[i] && w->stage_ns[i] > 0 && (best < 0 || w->stage_ns[i] > w->stage_ns[best]))
                best = i;
        if (best < 0) break;
        used[best] = 1;
        top[n] = best;
    }
    snprintf(buf, size, "gap=%.1fms %.1fs ago: work=%.1fms (",
             p->worst_gap_ns / 1e6, (now_ns - w->start_ns) / 1e9, w->work_ns / 1e6);
    for (i = 0; i < n; i++) {
        len = strlen(buf);
        snprintf(buf + len, size - len, "%s%s=%.1f", i ? " " : "", stage_name(top[i]),
                 w->stage_ns[top[i]] / 1e6);
    }
    len = strlen(buf);
    snprintf(buf + len, size - len, ") sleep=%.1fms late=%.1fms masks=%u events=",
             w->sleep_ns / 1e6, (p->worst_gap_ns - w->work_ns - w->sleep_ns) / 1e6, w->masks);
    for (i = 0; event_names[i]; i++) {
        if (!(w->events & (1u << i))) continue;
        len = strlen(buf);
        snprintf(buf + len, size - len, "%s%s", buf[len - 1] == '=' ? "" : ",", event_names[i]);
    }
    len = strlen(buf);
    if (buf[len - 1] == '=') snprintf(buf + len, size - len, "none");
    p->worst_gap_ns = 0;
    return 1;
}

#endif
