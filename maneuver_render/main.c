/*
 * CarPlay Cluster Renderer -- main entry point.
 *
 * Command-driven rendering engine with TCP server.
 * Receives maneuver commands, handles all animation/transitions internally.
 *
 * macOS: GLFW window for development.
 * QNX: own managed displayable 98 in Java-selected cluster contexts.
 * See protocol.h for DisplayManager routing and ownership.
 *
 * Copyright (c) 2026 LuKa (@LuKa_dev)
 */

#include "log_stamp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#ifdef PLATFORM_QNX
#include <sys/neutrino.h>     /* setprio() */
#endif

#include "platform.h"

/* TARGET_FPS is the unit of every per-frame animation constant (render_frame_step) and the
 * first idle poll rate.  Frames are drawn at PACED_FPS, evenly spaced (frame_pacer.h): nfc19
 * held 20 on the car; nfc20 paces at 30. */
#define TARGET_FPS     30
#define FRAME_TIME_NS  (1000000000L / TARGET_FPS)
#define PACED_FPS      30
#include "frame_pacer.h"
#include "frame_profile.h"
#include "gl_compat.h"
#include "render.h"
#include "arrow_progress.h"
#include "protocol.h"
#include "maneuver_command.h"
#include "maneuver.h"
#include "server.h"
#include "scene/scene.h"
#include "lane_guidance.h"
#include "lane_panel.h"
#include "map_layer.h"
#include "cluster_video.h"

#define WINDOW_W CR_DEFAULT_WIDTH
#define WINDOW_H CR_DEFAULT_HEIGHT
#define FLAG_ATLAS_LOCAL_PATH      "./flag_atlas.rgba"
#define FLAG_ATLAS_PRODUCTION_PATH "/mnt/app/root/hooks/flag_atlas.rgba"

static int timespec_elapsed_at_least(const struct timespec *now,
                                     const struct timespec *last,
                                     long seconds) {
    long ds;

    if (last->tv_sec == 0) return 1;

    ds = (long)(now->tv_sec - last->tv_sec);
    if (ds > seconds) return 1;
    if (ds < seconds) return 0;

    return now->tv_nsec >= last->tv_nsec;
}

static int64_t timespec_ns(const struct timespec *t) {
    return (int64_t)t->tv_sec * 1000000000LL + t->tv_nsec;
}

/* Where the slowest frame of each pacing window went (frame_profile.h): the loop's stages,
 * timed at the watchdog's stage marks, and these events. */
static cr_frame_profile_t g_profile;
enum {
    EV_MANEUVER = 1, EV_PROMOTE = 2, EV_COMMIT = 4, EV_REFRESH = 8, EV_REVEAL = 16,
    EV_CLEAR = 32, EV_SCENE = 64, EV_CAPTURE = 128, EV_OUTPUT = 256
};
static const char *const g_event_names[] = {
    "maneuver", "promote", "commit", "refresh", "reveal", "clear", "scene", "capture", "output", NULL
};
static const char *watch_stage_name(int stage);

/* One pacing line, with the transition-mask layers painted since the previous one (each
 * set's layers once per push, not every frame) and the most painted in one frame (one set's
 * while the next maneuver is invisible), then the slowest frame of the same window. */
static unsigned g_frame_masks_peak;
static void log_pacing(const char *line, int64_t now_ns) {
    static unsigned last;
    char slowest[256];
    unsigned painted = render_mask_paint_count();
    fprintf(stderr, "%s pacing: %s masks=%u peak=%u\n", log_stamp(), line, painted - last,
            g_frame_masks_peak);
    last = painted;
    g_frame_masks_peak = 0;
    if (cr_profile_report(&g_profile, now_ns, watch_stage_name, g_event_names, slowest, sizeof(slowest)))
        fprintf(stderr, "%s slowest: %s\n", log_stamp(), slowest);
}

static void sleep_ns(int64_t ns) {
    struct timespec ts = { (time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL) };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

/* ================================================================
 * Engine state machine
 *
 * IDLE:       showing current maneuver, slide=1.0
 * PUSHING:    slide 1->2, current pushes out + next slides in
 * SLIDING_IN: commit next->current, slide 0->1
 * ================================================================ */

typedef enum {
    ENGINE_IDLE,
    ENGINE_PUSHING,
    ENGINE_SLIDING_IN
} engine_phase_t;

typedef struct {
    maneuver_state_t current;
    maneuver_state_t next;
    maneuver_state_t pending;
    cr_scene_t *current_scene, *next_scene;
    int              has_current;   /* invariant: 1 after clear_maneuver() initializes FOLLOW_STREET */
    int              has_next;
    int              has_pending;   /* queued during PUSHING */
    engine_phase_t   phase;
    int              dirty;
} cr_engine_t;

static cr_engine_t g_engine;
// Independent panel input. Never passed into route geometry or animation.
static cr_lane_guidance_t g_lane_guidance;
static cr_lane_decoder_t g_lane_decoder;
static cr_lane_panel_t *g_lane_panel;

static cr_scene_t *engine_scene(const maneuver_state_t *m) {
    if(m==&g_engine.current)return g_engine.current_scene;
    if(m==&g_engine.next)return g_engine.next_scene;
    return NULL;
}
static int scene_handles(void *ctx,const maneuver_state_t *m) {
    cr_scene_t *s=engine_scene(m);(void)ctx;
    return s && cr_scene_route(s) && !cr_scene_is_native(s);
}
static void scene_route(void *ctx,const maneuver_state_t *m,route_path_t *out) {
    const route_path_t *p=cr_scene_route(engine_scene(m));(void)ctx;
    if(p)*out=*p;else memset(out,0,sizeof(*out));
}
static float scene_elevation(void *ctx,const maneuver_state_t *m) {
    const cr_scene_info_t *info=cr_scene_info(engine_scene(m));(void)ctx;
    return info?info->route_elevation:0;
}
static void scene_paint(void *ctx,const maneuver_state_t *m,float x,float y,float c,float s) {
    (void)ctx;cr_scene_paint(engine_scene(m),x,y,c,s);
}
static void prepare_engine_scene(cr_scene_t *scene,const maneuver_state_t *m,const cr_scene_view_t *view) {
    int next=m==&g_engine.next;
    if(!scene)return;
    cr_scene_input_t in;
    memset(&in,0,sizeof(in));in.maneuver=*m;
    unsigned before=cr_scene_info(scene)->builds;
    if(!cr_scene_prepare(scene,&in,view)) {
        fprintf(stderr,"scene: invalid input; ordinary maneuver fallback\n");return;
    }
    const cr_scene_info_t *info=cr_scene_info(scene);
    if(info->builds!=before) {
        fprintf(stderr,"scene: kind=%d fallback=%d commands=%u\n",
            info->kind,info->fallback,info->command_count);
        /* A rebuilt scene paints different masks; cached mask sets must not outlive it.  The
         * next maneuver's scene paints set 1 only; the current one's also moves set 1. */
        if(next)render_invalidate_next_masks();else render_invalidate_masks();
        cr_profile_event(&g_profile,EV_SCENE);
    }
}
static void prepare_engine_scenes(void) {
    cr_scene_view_t view;
    /* The canonical small composition paints the entire source in both stages. */
    view.compact=1;render_get_layout_matrix(view.projection);
    prepare_engine_scene(g_engine.current_scene,&g_engine.current,&view);
    if(g_engine.has_next)prepare_engine_scene(g_engine.next_scene,&g_engine.next,&view);
}

/* eglSwapBuffers on the old Adreno/WFD stack has no timeout. A wedged process
 * still has a live PID, so the external supervisor cannot distinguish it from
 * a healthy always-on renderer. This watchdog never calls EGL or touches the
 * render state; it only observes loop progress and exits this isolated process
 * before a stuck graphics client can linger indefinitely. */
/* A breadcrumb is only read on timeout.  No per-frame logging or graphics
 * calls from the watchdog: a blocked EGL/Screen client may hold libc locks. */
typedef enum {
    WATCH_STARTUP, WATCH_POLL, WATCH_COMMANDS, WATCH_ENGINE, WATCH_LANE_UPDATE,
    WATCH_LANE_FRAMING, WATCH_PREPARE, WATCH_BEGIN_FRAME,
    WATCH_SCENE_DRAW, WATCH_LANE_DRAW, WATCH_END_FRAME,
    WATCH_SCREENSHOT, WATCH_SWAP, WATCH_WINDOW_PROBE,
    WATCH_HEARTBEAT, WATCH_SLEEP, WATCH_IDLE,
    WATCH_SCENE_PREPARE, WATCH_CAPTURE, WATCH_YIELD, WATCH_OUTPUT_CHECK
} renderer_watch_stage_t;
static unsigned long g_loop_progress;
static int g_watch_stage = WATCH_STARTUP;
static volatile int g_renderer_running = 1;
static volatile int g_render_loop_started;

static void watch_stage(renderer_watch_stage_t stage) {
    struct timespec now;
    __sync_lock_test_and_set(&g_watch_stage, (int)stage);
    clock_gettime(CLOCK_MONOTONIC, &now);
    cr_profile_mark(&g_profile, (int)stage, timespec_ns(&now));
}

static const char *watch_stage_name(int stage) {
    switch (stage) {
    case WATCH_STARTUP: return "startup";
    case WATCH_POLL: return "poll";
    case WATCH_COMMANDS: return "commands";
    case WATCH_ENGINE: return "engine";
    case WATCH_LANE_UPDATE: return "lane-update";
    case WATCH_LANE_FRAMING: return "lane-framing";
    case WATCH_PREPARE: return "prepare";
    case WATCH_BEGIN_FRAME: return "begin-frame";
    case WATCH_SCENE_DRAW: return "scene-draw";
    case WATCH_LANE_DRAW: return "lane-draw";
    case WATCH_END_FRAME: return "end-frame";
    case WATCH_SCREENSHOT: return "screenshot";
    case WATCH_SWAP: return "egl-swap";
    case WATCH_WINDOW_PROBE: return "window-probe";
    case WATCH_HEARTBEAT: return "heartbeat";
    case WATCH_SLEEP: return "sleep";
    case WATCH_IDLE: return "idle";
    case WATCH_SCENE_PREPARE: return "scene-prepare";
    case WATCH_CAPTURE: return "most-capture";
    case WATCH_YIELD: return "yield";
    case WATCH_OUTPUT_CHECK: return "output-check";
    default: return "unknown";
    }
}

static void *renderer_watchdog_main(void *unused) {
    unsigned long last = __sync_fetch_and_add(&g_loop_progress, 0);
    int stalledSeconds = 0;
    (void)unused;
    while (g_renderer_running) {
        struct timespec pause = { 1, 0 };
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) { }
        if (!g_renderer_running) break;
        unsigned long progress = __sync_fetch_and_add(&g_loop_progress, 0);
        if (progress != last) {
            last = progress;
            stalledSeconds = 0;
        } else if (++stalledSeconds >= (g_render_loop_started ? 5 : 15)) {
            static const char msg[] = "maneuver_render: progress watchdog TIMEOUT phase=";
            static const char newline[] = "\n";
            const char *phase = watch_stage_name(__sync_fetch_and_add(&g_watch_stage, 0));
            (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
            (void)write(STDERR_FILENO, phase, strlen(phase));
            (void)write(STDERR_FILENO, newline, sizeof(newline) - 1);
            _exit(89);
        }
    }
    return NULL;
}

/* Presentation alpha.  CLEAR keeps a deterministic FOLLOW_STREET frame prepared
 * underneath the compositor at alpha 0; the next real presentation fades it in. */
static float g_fade_alpha = 1.0f;
static int   g_fade_active = 0;
static int g_anim_prev_rendered;
#define FADE_SPEED 0.125f  /* per-frame step (~0.27s / 8 frames at 30fps) */
static int   g_cleared = 1;

/* Progress is rendered directly on the route arrow. */
static cr_arrow_progress_t g_arrow;
static int g_pending_progress_level=16,g_pending_progress_state=CR_PROGRESS_OFF;
static int   g_persp_deferred = 0;       /* pending perspective change after push */
static int   g_persp_deferred_value = 1; /* 0=2D, 1=3D */
/* Apply a new maneuver to the engine */
static void engine_apply_maneuver(const maneuver_state_t *state,double now) {
    if (state->icon == ICON_NONE || g_engine.current.icon == ICON_NONE) {
        g_engine.current = *state;

        g_engine.has_current = 1;
        g_engine.has_next = g_engine.has_pending = 0;
        g_engine.phase = ENGINE_IDLE;
        cr_progress_reset(&g_arrow);
        maneuver_set_slide(1.0f);
        render_invalidate_masks();
        cr_profile_event(&g_profile, EV_MANEUVER);
        g_engine.dirty = 1;
        return;
    }
    if (g_engine.phase == ENGINE_PUSHING || g_engine.phase == ENGINE_SLIDING_IN) {
        /* Mid-transition: queue into pending slot, don't disturb current animation */
        g_engine.pending = *state;

        g_engine.has_pending = 1;
        g_pending_progress_level=16;g_pending_progress_state=CR_PROGRESS_OFF;
        fprintf(stderr, "engine: queued icon=%d (phase=%d)\n",
                state->icon, g_engine.phase);
        return;
    }

    /* IDLE: store as next, start push */
    g_engine.next = *state;

    g_engine.has_next = 1;
    cr_progress_begin_handoff(&g_arrow,now);
    maneuver_start_push();
    g_engine.phase = ENGINE_PUSHING;
    render_invalidate_next_masks();     /* the current maneuver's set 0 stays valid */
    cr_profile_event(&g_profile, EV_MANEUVER);
    g_engine.dirty = 1;
    fprintf(stderr, "engine: new maneuver icon=%d\n", state->icon);
}

/* A roads-only update belongs to the latest accepted command. During a push
 * that is next (or pending), never the outgoing current maneuver. */
static void engine_refresh_maneuver(const maneuver_state_t *state,double now) {
    if (state->icon == ICON_NONE || g_engine.current.icon == ICON_NONE) {
        engine_apply_maneuver(state,now);
        return;
    }
    /* Only painted masks go stale: a pending maneuver has none, the next one set 1, and the
     * current one set 0 and, through the push transform, set 1. */
    if (g_engine.has_pending) {g_engine.pending = *state;}
    else {
        if (g_engine.has_next) {g_engine.next = *state;render_invalidate_next_masks();}
        else {g_engine.current = *state;render_invalidate_masks();}
    }
    cr_profile_event(&g_profile, EV_REFRESH);
    g_engine.dirty = 1;
}

/* TCP progress belongs to the latest accepted maneuver. A third queued
 * maneuver must not overwrite the target of the route currently entering. */
static void engine_set_progress(int level,int state,double now) {
    if(g_engine.has_pending) {
        g_pending_progress_level=level;g_pending_progress_state=state;
    } else cr_progress_set(&g_arrow,level,state,now);
    g_engine.dirty=1;
}

static void engine_promote_pending(double now) {
    g_engine.next=g_engine.pending;
    g_engine.has_next=1;g_engine.has_pending=0;
    cr_progress_begin_handoff(&g_arrow,now);
    cr_progress_set(&g_arrow,g_pending_progress_level,g_pending_progress_state,now);
    maneuver_start_push();g_engine.phase=ENGINE_PUSHING;
    /* Set 0 is the current maneuver's: painted during the slide-in, or due after a commit. */
    render_invalidate_next_masks();g_engine.dirty=1;
    cr_profile_event(&g_profile,EV_PROMOTE);
    fprintf(stderr,"engine: promoting queued icon=%d\n",g_engine.next.icon);
}

static void engine_apply_presentation(void) {
    if (g_persp_deferred) {
        render_set_perspective(g_persp_deferred_value);
        g_persp_deferred = 0;
        g_engine.dirty = 1;
    }
}

/* Blank the popup without leaving the renderer in an undefined/no-current state.
 * FOLLOW_STREET is always prepared underneath at slide=1, but alpha=0 and the
 * cleared latch prevent it from earning FRAME_READY.  The next CMD_MANEUVER
 * fades the deterministic scene back in. */
static void clear_maneuver(void) {
    cr_lane_clear(&g_lane_decoder, &g_lane_guidance);
    cr_lane_panel_clear(g_lane_panel);
    render_reset_content_offset();
    memset(&g_engine.current, 0, sizeof(g_engine.current));
    g_engine.current.icon = ICON_APPROACH;
    g_engine.has_current = 1;
    g_engine.has_next    = 0;
    g_engine.has_pending = 0;
    g_engine.phase       = ENGINE_IDLE;
    maneuver_set_slide(1.0f);
    render_invalidate_masks();
    cr_profile_event(&g_profile, EV_CLEAR);
    cr_progress_reset(&g_arrow);
    g_persp_deferred = 0;
    g_fade_active    = 0;
    g_fade_alpha     = 0.0f;
    render_set_global_alpha(0.0f);
    g_cleared        = 1;
    g_engine.dirty   = 1;
}

/* Advance engine state machine */
static void engine_tick(double now) {
    switch (g_engine.phase) {
    case ENGINE_PUSHING:
        if (!maneuver_is_pushing()) {
            /* Push complete -- commit next as current */
            if (g_engine.has_next) {
                g_engine.current = g_engine.next;
                cr_scene_t *old=g_engine.current_scene;
                g_engine.current_scene=g_engine.next_scene;g_engine.next_scene=old;
                g_engine.has_next = 0;
            }
            maneuver_commit_pushed_state(&g_engine.current);
            cr_progress_finish_handoff(&g_arrow,now);
            render_invalidate_masks();
            cr_profile_event(&g_profile, EV_COMMIT);
            g_engine.dirty = 1;

            if (g_engine.has_pending) {
                /* Promote pending -> next, immediately start new push */
                engine_promote_pending(now);
            } else {
                g_engine.phase = ENGINE_SLIDING_IN;
                /* Crossfade during push already brought next roads to full alpha,
                 * no post-push fade needed. */
            }
        }
        break;
    case ENGINE_SLIDING_IN:
        if (!maneuver_is_animating()) {
            if (g_engine.has_pending) {
                /* Promote pending -> next, start push */
                engine_promote_pending(now);
            } else {
                g_engine.phase = ENGINE_IDLE;
            }
        }
        break;
    case ENGINE_IDLE:
        break;
    }
    if (g_engine.phase == ENGINE_IDLE) engine_apply_presentation();
}

/* ================================================================
 * Screenshot -- save framebuffer as PPM
 * ================================================================ */

static int g_snap_counter = 0;

/* keep the presentation rectangle in step with the platform window (a MOST
 * cluster's KOMO stream is larger than the rendered content). */
static void apply_output(void) {
    static unsigned applied_gen = 0;
    int ww, wh, x, y, w, h, opaque, resized, rw, rh;
    long free_before;
    unsigned gen = platform_get_output(&ww, &wh, &x, &y, &w, &h, &opaque);
    if (gen == applied_gen) return;
    applied_gen = gen;
    free_before = platform_free_memory_kb();
    if (opaque) {
        resized = render_set_output(ww, wh, x, y, w, h, 1);
        fprintf(stderr, "maneuver_render: output %dx%d, content at (%d,%d %dx%d), opaque\n",
                ww, wh, x, y, w, h);
    } else {
        resized = render_set_output(0, 0, 0, 0, 0, 0, 0);
    }
    if (resized) {
        render_get_render_size(&rw, &rh);
        fprintf(stderr, "maneuver_render: rendering %dx%d; free memory %ld KB before, %ld KB after\n",
                rw, rh, free_before, platform_free_memory_kb());
    }
    cr_profile_event(&g_profile, EV_OUTPUT);
    g_engine.dirty = 1;
}

/* Output diagnostics: while the carplay_verbose marker exists (the log collector sets it), keep
 * the latest settled maneuver frame, exactly as the MOST encoder gets it, for the collector.
 * Once per settled maneuver, and at most every CR_MOST_FRAME_INTERVAL_S seconds: the readback
 * stalls the GPU, so a maneuver held for a long stretch is not captured again.  Call after
 * render_end_frame() and before the swap. */
static void capture_most_frame(const struct timespec *now) {
    static struct timespec last = {0, 0};
    static int logged_ok = 0, logged_fail = 0;
    static maneuver_state_t captured;
    static unsigned captured_output = 0;
    int ww, wh, x, y, w, h, opaque;
    unsigned output;
    if (!render_output_is_opaque() || !g_engine.has_current || g_cleared
            || g_engine.current.icon == ICON_NONE || g_engine.phase != ENGINE_IDLE
            || render_is_animating()
            || !timespec_elapsed_at_least(now, &last, CR_MOST_FRAME_INTERVAL_S))
        return;
    /* The same maneuver in the same output window is already on file. */
    output = platform_get_output(&ww, &wh, &x, &y, &w, &h, &opaque);
    if (output == captured_output && memcmp(&captured, &g_engine.current, sizeof(captured)) == 0)
        return;
    last = *now;
    captured = g_engine.current;
    captured_output = output;
    if (!log_verbose_marker()) return;   /* only while logs are being collected */
    cr_profile_event(&g_profile, EV_CAPTURE);
    if (render_capture_output(CR_MOST_FRAME_PATH) == 0) {
        if (!logged_ok) fprintf(stderr, "maneuver_render: output frame saved to %s\n", CR_MOST_FRAME_PATH);
        logged_ok = 1;
    } else {
        if (!logged_fail) fprintf(stderr, "maneuver_render: output frame could not be saved to %s\n",
                                  CR_MOST_FRAME_PATH);
        logged_fail = 1;
    }
}

static void save_screenshot(const char *label) {
    char path[256];
    snprintf(path, sizeof(path), "snap_%03d_%s.ppm", g_snap_counter++, label);
    if (render_capture_output(path) == 0) fprintf(stderr, "engine: saved %s\n", path);
}

/* ================================================================
 * Render loop steps
 * ================================================================ */

/* What one loop iteration drained from Java: the latest maneuver and progress win. */
typedef struct {
    int maneuver, progress, screenshot, shutdown;
    maneuver_state_t pending;
    uint8_t flags, perspective;
    int level, mode, state;
    char label[17];
} cr_commands_t;

static int g_announced_first_frame;
static cr_frame_pacer_t g_pacer;

static void read_commands(cr_commands_t *c) {
    cr_cmd_t cmd;
    memset(c, 0, sizeof(*c));
    c->perspective = 1;
    c->state = CR_PROGRESS_OFF;
    while (cr_server_read_cmd(&cmd)) {
        if (cmd.cmd == CMD_LANES_BEGIN || cmd.cmd == CMD_LANES_LANE || cmd.cmd == CMD_LANES_COMMIT) {
            if (cr_lane_receive(&g_lane_decoder, &cmd, &g_lane_guidance)) {
                g_engine.dirty=1;
                fprintf(stderr, "lanes: event=%d showing=%d count=%d complete=%d\n",
                    (int)g_lane_guidance.event_index, g_lane_guidance.showing,
                    g_lane_guidance.count, g_lane_guidance.complete);
            }
            continue;
        }
        switch (cmd.cmd) {
        case CMD_MANEUVER: {
            cr_decode_maneuver(&cmd, &c->pending);
            c->flags = cr_merge_maneuver_flags(c->maneuver, c->flags, cmd.flags);
            c->perspective = cmd.payload[43];
            c->progress=1;
            c->level=(cmd.flags & MAN_FLAG_PROGRESS) ? cmd.payload[44] : 0;
            c->mode=(cmd.flags & MAN_FLAG_PROGRESS) ? cmd.payload[45] : 0;
            c->state=cr_progress_decode(cmd.flags,cmd.payload[42],c->mode);
            c->maneuver = 1;
            break;
        }
        case CMD_SCREENSHOT: {
            c->screenshot = 1;
            memcpy(c->label, cmd.payload, 16);
            c->label[16] = '\0';
            break;
        }
        case CMD_VISIBLE_AREA:
            if (render_set_visible_area((cmd.payload[0] << 8) | cmd.payload[1],
                    (cmd.payload[2] << 8) | cmd.payload[3],
                    (cmd.payload[4] << 8) | cmd.payload[5],
                    (cmd.payload[6] << 8) | cmd.payload[7]))
                g_engine.dirty = 1;
            break;
        case CMD_PERSPECTIVE: {
            int persp = cmd.payload[0] ? 1 : 0;
            render_set_perspective(persp);
            fprintf(stderr, "engine: perspective=%s\n", persp ? "ON" : "OFF");
            g_engine.dirty = 1;
            break;
        }
        case CMD_DEBUG:
            if (cmd.payload[0] == 2) {
                static int grid_on = 0;
                grid_on = !grid_on;
                render_set_debug_grid(grid_on);
                fprintf(stderr, "engine: grid=%s\n", grid_on ? "ON" : "OFF");
            } else if (cmd.payload[0] == 3) {
                /* 3D offset adjust up */
                extern float g_3d_offset_adjust;
                g_3d_offset_adjust -= 0.02f;
                fprintf(stderr, "engine: 3d_offset=%.3f\n", g_3d_offset_adjust);
            } else if (cmd.payload[0] == 4) {
                /* 3D offset adjust down */
                extern float g_3d_offset_adjust;
                g_3d_offset_adjust += 0.02f;
                fprintf(stderr, "engine: 3d_offset=%.3f\n", g_3d_offset_adjust);
            } else {
                maneuver_toggle_debug();
                fprintf(stderr, "engine: debug=%s\n", maneuver_is_debug() ? "ON" : "OFF");
            }
            g_engine.dirty = 1;
            break;
        case CMD_PROGRESS:
            c->progress=1;
            c->level=cmd.payload[0]; c->mode=cmd.payload[1];
            c->state=cr_progress_decode(cmd.flags,cmd.payload[2],c->mode);
            break;
        case CMD_CLEAR:
            /* CLEAR wins over an earlier MANEUVER drained in this same loop. */
            c->maneuver = 0;
            c->progress=0;
            clear_maneuver();
            g_announced_first_frame = 0;
            cr_server_clear_frame_ready();
            /* This barrier is sent before any later command in the same TCP
             * drain can draw and earn a new FRAME_READY.  Java therefore can
             * reject an old in-flight FRAME_READY without a timing guess. */
            cr_server_mark_frame_cleared();
            fprintf(stderr, "engine: clear (blank)\n");
            break;
        case CMD_SHUTDOWN:
            fprintf(stderr, "engine: shutdown command received\n");
            c->shutdown = 1;
            break;
        }
    }
}

/* Applies the latest maneuver (draining to latest) and progress. */
static void apply_commands(const cr_commands_t *c, double now) {
    if (c->maneuver) {
        int reveal_from_clear = g_cleared;
        if (reveal_from_clear) cr_progress_reset(&g_arrow);
        if (g_cleared) {
            g_cleared = 0;
            g_fade_alpha = 0.0f;
            g_fade_active = 1;
            render_set_global_alpha(0.0f);
        }
        if (c->flags & MAN_FLAG_SET_PERSP) {
            g_persp_deferred = 1;
            g_persp_deferred_value = c->perspective ? 1 : 0;
            fprintf(stderr, "engine: deferred perspective=%s\n",
                    g_persp_deferred_value ? "3D" : "2D");
        }
        if (reveal_from_clear) {
            /* The hidden FOLLOW_STREET is only a deterministic backing
             * state.  Replace it directly: the global alpha is the sole
             * CLEAR->visible transition, with no duplicate route push. */
            g_engine.current = c->pending;
            g_engine.has_current = 1;
            g_engine.has_next = 0;
            g_engine.has_pending = 0;
            g_engine.phase = ENGINE_IDLE;
            maneuver_set_slide(1.0f);
            render_invalidate_masks();
            cr_profile_event(&g_profile, EV_REVEAL);
            g_engine.dirty = 1;
            /* This direct reveal deliberately bypasses ENGINE_SLIDING_IN,
             * whose settle edge normally consumes the deferred presentation
             * fields.  Apply them here so the first real frame after CLEAR
             * carries the requested perspective as well. */
            engine_apply_presentation();
            fprintf(stderr, "engine: reveal from clear icon=%d\n",
                    c->pending.icon);
        } else if (c->flags & MAN_FLAG_REFRESH) {
            engine_refresh_maneuver(&c->pending,now);
        } else {
            engine_apply_maneuver(&c->pending,now);
        }
    }

    if(c->progress) {
        engine_set_progress(c->level,c->state,now);
        /* TCP order wins, including PROGRESS after MANEUVER in one drain.
         * Never let the maneuver's deferred snapshot rewind a newer tick. */
        g_engine.dirty=1;
    }
}

/* The loop's periodic duties, timed from the iteration's start. */
static void housekeeping(const struct timespec *now) {
    static struct timespec health_last, output_last, heartbeat_last;
    /* Window health-check + auto-recreate every 5 s.
     *
     * Two failure modes covered (see platform_qnx.c::platform_check_and_recover_window
     * for full RE-derived rationale):
     *   1. Our screen_window struct invalidated cross-process — the
     *      probe screen_get_window_property_iv returns ENOENT/EBADF/EINVAL.
     *   2. displaymanager disowned our managed window (m_surfaceSources[98]
     *      no longer holds it, e.g. the context switched away) — our
     *      SCREEN_PROPERTY_MANAGER_STRING no longer matches "All your base
     *      are belong to us!".  With our own id 98 there is no stock
     *      collision, so this is rare.
     *
     * On either signal, cluster_surface recreates the managed window (id 98,
     * 100 ms backoff inside) and recreate_output re-binds EGL. */
    if (timespec_elapsed_at_least(now, &health_last, 5)) {
        health_last = *now;
        watch_stage(WATCH_WINDOW_PROBE);
        platform_check_and_recover_window();
        watch_stage(WATCH_IDLE);
    }
    /* Java requests the MOST stream size at CarPlay connect; check every second
     * so the window has it before a route can bring the arrows view up. */
    if (timespec_elapsed_at_least(now, &output_last, 1)) {
        output_last = *now;
        watch_stage(WATCH_OUTPUT_CHECK);
        platform_check_output();
        apply_output();
        watch_stage(WATCH_IDLE);
    }
    /* Heartbeat: EVT_HEARTBEAT to Java once a second, from the loop itself.  Java's
     * RendererServer SO_TIMEOUT is 5 s.  send() does not block; the worst seen on the
     * car is ~5 ms, and a heartbeat thread did not remove the ~245 ms stalls. */
    if (timespec_elapsed_at_least(now, &heartbeat_last, 1)) {
        heartbeat_last = *now;
        watch_stage(WATCH_HEARTBEAT);
        cr_server_send_heartbeat();
        watch_stage(WATCH_IDLE);
    }
}

/* ACTIVE pacing, every platform: after a rendered frame sleep to the next PACED_FPS
 * deadline (frame_pacer.h).  eglSwapBuffers alone (QNX interval 2) did not hold the rate
 * in the car: frames came at up to 53/s and the rate wandered.  QNX 6.5 rounds a sleep
 * up to the kernel tick, but that does not accumulate: deadlines chain from the
 * previous deadline, not from the wake-up.
 *
 * Adaptive IDLE pacing:
 * when the loop produced NO frame this iteration (idle: no route / no maneuver) an
 * always-on renderer would busy-spin 100% CPU.  Throttle idle polling: 30 Hz for the
 * first second, then 10 Hz (1–5 s), then 3 Hz (>5 s); any activity snaps back to
 * 30 Hz.  Idle timing needs no precision. */
static void pace(int rendered, int64_t swap_ns, const struct timespec *start, unsigned masks_at_start) {
    static int idle_frames = 0;
    char pacing_line[160];
    struct timespec t_end;
    g_anim_prev_rendered = rendered;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    unsigned frame_masks = render_mask_paint_count() - masks_at_start;
    if (rendered) {
        idle_frames = 0;
        int64_t wait_ns = cr_pacer_frame_done(&g_pacer, timespec_ns(start), swap_ns,
                                              timespec_ns(&t_end));
        cr_profile_end(&g_profile, 1, frame_masks, wait_ns, timespec_ns(&t_end));
        if (frame_masks > g_frame_masks_peak) g_frame_masks_peak = frame_masks;
        if (cr_pacer_report(&g_pacer, timespec_ns(&t_end), pacing_line, sizeof(pacing_line)))
            log_pacing(pacing_line, timespec_ns(&t_end));
        if (wait_ns > 0) {
            watch_stage(WATCH_SLEEP);
            sleep_ns(wait_ns);
            watch_stage(WATCH_IDLE);
        }
    } else {
        cr_pacer_idle(&g_pacer);
        cr_profile_end(&g_profile, 0, frame_masks, 0, timespec_ns(&t_end));
        if (cr_pacer_report(&g_pacer, timespec_ns(&t_end), pacing_line, sizeof(pacing_line)))
            log_pacing(pacing_line, timespec_ns(&t_end));
        if (idle_frames < 10000) idle_frames++;
        long idle_ns = (idle_frames < TARGET_FPS)     ? FRAME_TIME_NS     /* <1 s: 30 Hz */
                     : (idle_frames < TARGET_FPS * 5) ? 100L * 1000000L  /* 1–5 s: 10 Hz */
                     :                                  333L * 1000000L; /* >5 s: 3 Hz  */
        watch_stage(WATCH_SLEEP);
        sleep_ns(idle_ns);
        watch_stage(WATCH_IDLE);
    }
}

/* ================================================================
 * Main
 * ================================================================ */

int main(int argc, char **argv) {
    int port=CR_TCP_PORT, arg;
    for(arg=1;arg<argc;arg++) {
        if(!strncmp(argv[arg],"--port=",7)) {
            char *end; long parsed=strtol(argv[arg]+7,&end,10);
            if(end==argv[arg]+7 || *end || parsed<1 || parsed>65535) {
                fprintf(stderr,"Invalid renderer port\n"); return 2;
            }
            port=(int)parsed;
        } else {
            fprintf(stderr,"Usage: %s [--port=1..65535]\n",argv[0]);
            return 2;
        }
    }
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    g_announced_first_frame = 0;   /* each run earns its own first FRAME_READY */

    fprintf(stderr, "%s maneuver_render: starting %dx%d\n", log_stamp(), WINDOW_W, WINDOW_H);

    /* Cover platform_init as well as runtime/teardown. A process blocked in
     * eglInitialize has a valid PID but no useful output; the supervisor's
     * PID-only maneuver health probe cannot detect it. Cold graphics gets a
     * generous 15 s, then the isolated process exits for a backed-off restart. */
    {
        pthread_t watchdog;
        if (pthread_create(&watchdog, NULL, renderer_watchdog_main, NULL) == 0)
            pthread_detach(watchdog);
        else
            fprintf(stderr, "maneuver_render: progress watchdog create failed\n");
    }

#ifdef PLATFORM_QNX
    /* Raise our scheduling priority to match native HMI graphics workers
     * (typically 13-18 on QNX 6.5).  Default user priority 10 lets busy
     * iAP2 / dio_manager / Java HMI threads preempt us for ~14-16 vsync
     * cycles at a time, which manifests as 245 ms render stalls every
     * ~530 ms.  At priority 15 we sit in the same league as the cluster
     * compositor's own workers and are not starved.
     *
     * Failure (EPERM if not root) is non-fatal — we'll still run, just
     * with stalls. */
    if (setprio(0, 15) < 0) {
        fprintf(stderr, "maneuver_render: setprio(15) failed: %s — running at default\n",
                strerror(errno));
    } else {
        fprintf(stderr, "maneuver_render: priority raised to 15\n");
    }
#endif

    /* Initialize TCP server FIRST so Java can connect while display inits.
     * If server fails, nothing works — exit immediately. */
    if (cr_server_init(port) < 0) {
        fprintf(stderr, "maneuver_render: server init failed\n");
        return 1;
    }

    if (platform_init(WINDOW_W, WINDOW_H) < 0) {
        fprintf(stderr, "maneuver_render: platform init failed\n");
        cr_server_shutdown();
        return 1;
    }

    int fb_w, fb_h;
    platform_get_framebuffer_size(&fb_w, &fb_h);
    fprintf(stderr, "maneuver_render: framebuffer %dx%d\n", fb_w, fb_h);

    maneuver_scene_provider_t provider={0};
    provider.handles=scene_handles;provider.build_route=scene_route;
    provider.paint_masks=scene_paint;provider.elevation=scene_elevation;
    cr_scene_configure_provider(&provider);maneuver_set_scene_provider(&provider);
    if (render_init(fb_w, fb_h) < 0) {
        fprintf(stderr, "maneuver_render: render init failed\n");
        platform_shutdown();
        cr_server_shutdown();
        return 1;
    }
    apply_output();
    cluster_video_start();               /* the phone's cluster stream, from the hook's frame ring */
    map_layer_start();                   /* window 99 and its pictures, on their own thread */

    /* Runtime has one colocated asset.  Keep one relative development/deploy
     * lookup and one explicit HU production fallback — no argv/CWD guessing. */
    if (render_load_flag_atlas(FLAG_ATLAS_LOCAL_PATH, 128, 128, 14) != 0)
        render_load_flag_atlas(FLAG_ATLAS_PRODUCTION_PATH, 128, 128, 14);

#ifdef CR_DEBUG_GRID
    render_set_debug_grid(1);
#endif
    /* Engine always has a deterministic hidden FOLLOW_STREET frame. */
    memset(&g_engine, 0, sizeof(g_engine));
    g_engine.current_scene=cr_scene_create();g_engine.next_scene=cr_scene_create();
    g_lane_panel=cr_lane_panel_create();
    if(!g_lane_panel)fprintf(stderr,"lanes: panel allocation failed\n");
    if(!g_engine.current_scene || !g_engine.next_scene) {
        fprintf(stderr,"scene: allocation failed; retaining ordinary renderer\n");
        cr_scene_destroy(g_engine.current_scene);cr_scene_destroy(g_engine.next_scene);
        g_engine.current_scene=g_engine.next_scene=NULL; maneuver_set_scene_provider(NULL);
    }
    { extern float g_3d_offset_adjust;g_3d_offset_adjust=-.16f; }
    clear_maneuver();

    fprintf(stderr, "maneuver_render: ready, waiting for commands on :%d\n", port);
    cr_server_mark_ready();

    int dirty = 1;
    int running = 1;
    cr_pacer_init(&g_pacer, 1000000000LL / PACED_FPS);
    fprintf(stderr, "maneuver_render: frames paced at %d fps\n", PACED_FPS);
    cr_profile_init(&g_profile);
    g_render_loop_started = 1;

    while (running && !platform_should_close()) {
        struct timespec t_start;
        cr_commands_t commands;
        watch_stage(WATCH_POLL);
        __sync_fetch_and_add(&g_loop_progress, 1);
        clock_gettime(CLOCK_MONOTONIC, &t_start);
        cr_profile_begin(&g_profile, WATCH_POLL, timespec_ns(&t_start));
        unsigned masks_at_start = render_mask_paint_count();

        platform_poll();

        watch_stage(WATCH_POLL);
        cr_server_poll();

        /* Java closed the bus socket (route/CarPlay off).  We are an always-on framework
         * service — do NOT exit.  Blank the popup so no stale frame lingers, clear the
         * peer-closed latch, and let cr_server_poll() reconnect when Java is back. */
        if (cr_server_peer_closed()) {
            fprintf(stderr, "engine: peer closed — clearing, staying alive for reconnect\n");
            clear_maneuver();
            g_announced_first_frame = 0;
            cr_server_clear_frame_ready();
            cr_server_clear_peer_closed();
        }

        double progress_now=(double)t_start.tv_sec+t_start.tv_nsec*1e-9;
        /* Animations advance by elapsed time, not by frames: at 20 fps a per-frame step
         * made every slide 1.5x slower.  Iterations that render are paced (frame_pacer.h), so
         * the gap since the previous rendering iteration is the frame time.  After idle
         * the first frame is one step; a stall longer than 4 frames slows, never jumps. */
        {
            static double anim_prev;
            float step = 1.0f;
            if (g_anim_prev_rendered) {
                step = (float)((progress_now - anim_prev) * TARGET_FPS);
                if (step < 0.0f) step = 0.0f;
                if (step > 4.0f) step = 4.0f;
            }
            anim_prev = progress_now;
            render_set_frame_step(step);
        }

        /* Drain all pending commands, keep only the last CMD_MANEUVER */
        watch_stage(WATCH_COMMANDS);
        read_commands(&commands);
        if (commands.shutdown) running = 0;
        watch_stage(WATCH_ENGINE);
        apply_commands(&commands, progress_now);
        if(cr_progress_tick(&g_arrow,progress_now)) g_engine.dirty=1;

        /* Tick engine state machine; prepare the scenes it leaves current and next */
        engine_tick(progress_now);
        watch_stage(WATCH_SCENE_PREPARE);
        prepare_engine_scenes();
        watch_stage(WATCH_ENGINE);

        /* Update framebuffer size (HiDPI) */
        int new_w, new_h;
        platform_get_framebuffer_size(&new_w, &new_h);
        if (new_w != fb_w || new_h != fb_h) {
            fb_w = new_w;
            fb_h = new_h;
            render_set_viewport(fb_w, fb_h);
            dirty = 1;
        }

        /* Fade-in animation */
        if (g_fade_active) {
            g_fade_alpha += FADE_SPEED * render_frame_step();
            if (g_fade_alpha >= 1.0f) {
                g_fade_alpha = 1.0f;
                g_fade_active = 0;
            }
            render_set_global_alpha(g_fade_alpha);
            g_engine.dirty = 1;
        }

        /* Render if needed */
        cr_rect_t panel_target;
        render_get_visible_area(NULL,&panel_target);
        watch_stage(WATCH_LANE_UPDATE);
        if(cr_lane_panel_update(g_lane_panel,&g_lane_guidance,panel_target.w,progress_now))
            g_engine.dirty=1;
        float content_frame[3];
        watch_stage(WATCH_LANE_FRAMING);
        cr_lane_panel_framing(g_lane_panel,g_engine.current_scene,
                             g_engine.has_next?g_engine.next_scene:NULL,content_frame);
        render_set_content_framing(content_frame[0],content_frame[1],content_frame[2]);
        watch_stage(WATCH_IDLE);
        if (g_engine.dirty || render_is_animating() || maneuver_needs_redraw() || commands.screenshot
            || cr_lane_panel_animating(g_lane_panel,progress_now)
#ifdef CR_DEBUG_GRID
            || 1  /* always render when grid is compiled in */
#endif
           )
            dirty = 1;
        g_engine.dirty = 0;

        int rendered_this_frame = 0;
        int64_t swap_ns = 0;
        if (dirty) {
            rendered_this_frame = 1;

            maneuver_state_t *next_ptr = g_engine.has_next ? &g_engine.next : NULL;
            watch_stage(WATCH_PREPARE);
            maneuver_prepare_frame(&g_engine.current, next_ptr);

            render_set_route_progress(g_arrow.fill,g_arrow.path_weight,g_arrow.glow);
            watch_stage(WATCH_BEGIN_FRAME);
            render_begin_frame();
            watch_stage(WATCH_SCENE_DRAW);
            maneuver_draw(&g_engine.current, next_ptr);
            cr_rect_t panel_visible;
            render_get_visible_area(&panel_visible,NULL);
            watch_stage(WATCH_LANE_DRAW);
            cr_lane_panel_draw(g_lane_panel,panel_visible,progress_now);
            watch_stage(WATCH_IDLE);
            render_debug_grid();
            watch_stage(WATCH_END_FRAME);
            render_end_frame();
            watch_stage(WATCH_IDLE);

            watch_stage(WATCH_SCREENSHOT);
            if (commands.screenshot)
                save_screenshot(commands.label);
            watch_stage(WATCH_CAPTURE);
            capture_most_frame(&t_start);

            struct timespec t_swap_start, t_swap_end;
            clock_gettime(CLOCK_MONOTONIC, &t_swap_start);
            watch_stage(WATCH_SWAP);
            int swap_ok = platform_swap();
            watch_stage(WATCH_IDLE);
            clock_gettime(CLOCK_MONOTONIC, &t_swap_end);
            swap_ns = timespec_ns(&t_swap_end) - timespec_ns(&t_swap_start);
            if (!swap_ok) {
                /* platform_swap recreated the QNX surface.  Repaint it on the
                 * next iteration even when the engine is otherwise idle.  Also
                 * classify this iteration as idle so the generic pacing below
                 * sleeps; a dead EGL surface does not provide vsync pacing. */
                g_engine.dirty = 1;
                rendered_this_frame = 0;
            }
            /* A transparent post-CLEAR FOLLOW_STREET frame is deliberately not
             * presentation-ready.  Only a later real command may re-arm Java's
             * ctx80 gate. */
            if (swap_ok && !g_announced_first_frame && g_engine.has_current && !g_cleared) {
                g_announced_first_frame = 1;
                cr_server_mark_frame_ready();
                fprintf(stderr, "engine: first frame ready\n");
            }

            /* Voluntarily yield to the compositor right after queuing a
             * new buffer.  Without this, the kernel eventually forces us
             * off-CPU when the buffer pool fills, but in long bursts
             * (~14-16 vsync cycles).  A cooperative yield keeps the
             * compositor's worker scheduled in time for the next frame
             * — much smoother on QNX displayable composition. */
            watch_stage(WATCH_YIELD);
            sched_yield();
            watch_stage(WATCH_IDLE);

            dirty = render_is_animating() || maneuver_needs_redraw() || g_fade_active
                 || cr_lane_panel_animating(g_lane_panel,progress_now)
                 || g_arrow.active || g_arrow.tint_active;
        }

        housekeeping(&t_start);
        pace(rendered_this_frame, swap_ns, &t_start, masks_at_start);
        watch_stage(WATCH_IDLE);
    }

    map_layer_stop();
    cluster_video_stop();
    cr_server_shutdown();
    maneuver_set_scene_provider(NULL);
    cr_scene_destroy(g_engine.current_scene);cr_scene_destroy(g_engine.next_scene);
    cr_lane_panel_destroy(g_lane_panel);
    render_shutdown();
    platform_shutdown();
    g_renderer_running = 0;

    fprintf(stderr, "maneuver_render: exit\n");
    return 0;
}
