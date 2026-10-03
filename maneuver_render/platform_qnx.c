/*
 * QNX platform implementation -- cluster_surface (raw screen) + EGL + GLES2
 *
 * Creates managed displayables via the shared cluster_surface primitive (raw
 * screen_create_window + screen_manage_window, no libdisplayinit) and sets up
 * EGL/GLES2 for rendering: the scene window 98 and, while a MOST cluster asks for
 * it, the MAP view window 99.  Context routing is Java-driven (no dmdt).
 *
 * Copyright (c) 2026 LuKa (@LuKa_dev)
 */

#ifdef PLATFORM_QNX

#include "log_stamp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "platform.h"
#include "protocol.h"
#include "cluster_surface.h"

/* EGL state shared by every window */
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLConfig  g_egl_config  = 0;          /* saved for window recreate */
static int g_width = 0, g_height = 0;          /* the scene's content size */
static volatile int g_should_close = 0;

/* One managed cluster window and its MOST handshake (protocol.h): the request Java writes,
 * the report of the window presenting it, and the window + EGL surface themselves.  The
 * scene window 98 always exists (content size, or the KOMO size a MOST cluster asks for);
 * the MAP window 99 exists only while its request does.  Both use every function below.
 * The scene is an RGBA GL window; the map an NV12 video window the CPU writes, no EGL (map14:
 * the MOST capture takes it). */
typedef struct {
    const char *name;            /* log label */
    int id;                      /* displayable = ID_STRING */
    const char *request_path, *ready_path;
    int on_demand;               /* no request, no window (the MAP view) */
    int swap_interval;
    cluster_surface_t *cs;       /* created / probed / recreated by cluster_surface */
    EGLSurface surface;
    EGLContext context;          /* the scene's: made with its first EGL surface */
    int out_w, out_h;            /* presented window size */
    unsigned gen;                /* changes whenever the presented size does */
    int requested;               /* the last read found a valid request: a MOST cluster */
    int ready_maybe;             /* a report may exist (unknown at start: a previous process) */
    unsigned ready_serial;       /* one per published window, so Java sees every recreation */
    unsigned swap_failures;      /* consecutive failed swaps, for the log */
    int shown;                   /* map: a frame was posted on this window (it is then reported) */
    int stale;                   /* map: its recreate (new size, lost window) is still to come */
    unsigned created;            /* windows made so far (the map draws a new one at once) */
} cr_output_t;

static cr_output_t g_arrows = { "scene", CR_DISPLAYABLE_ID, CR_MOST_OUTPUT_PATH,
    CR_MOST_OUTPUT_READY_PATH, 0, 2, NULL, EGL_NO_SURFACE, EGL_NO_CONTEXT, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0 };
static cr_output_t g_map = { "map", CR_MAP_DISPLAYABLE_ID, CR_MOST_MAP_OUTPUT_PATH,
    CR_MOST_MAP_OUTPUT_READY_PATH, 1, 1, NULL, EGL_NO_SURFACE, EGL_NO_CONTEXT, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0 };

/* Set to 1 once platform_init has successfully created the initial window, so
 * the health check knows to keep recovering (vs "renderer not yet up, ignore"). */
static int g_window_expected = 0;

/* Overridable IDs (env: CR_CONTEXT_ID, CR_DISPLAY_ID) */
static int g_displayable_id;
static int g_context_id;
static int g_display_id;

static int read_env_int(const char *name, int def) {
    const char *v = getenv(name);
    return (v && v[0]) ? atoi(v) : def;
}

/* The request Java writes in place (this unit's /tmp is procnto shared memory: no rename),
 * so a read can race the write.  Only a complete "<width> <height>\n" in range counts:
 *   OUTPUT_ABSENT  no file (Virtual Cockpit, before any MOST session, or no MAP view):
 *                  the scene's content size / no map window;
 *   OUTPUT_UNREADY empty, partial or invalid: keep the current window;
 *   OUTPUT_SIZE    *w x *h. */
enum { OUTPUT_ABSENT, OUTPUT_UNREADY, OUTPUT_SIZE };

static int parse_output_number(const char *s, int *pos, int *value) {
    int start = *pos, v = 0;
    while (s[*pos] >= '0' && s[*pos] <= '9' && v <= CR_OUTPUT_MAX)
        v = v * 10 + (s[(*pos)++] - '0');
    *value = v;
    return *pos > start;
}

static int read_output_request(const cr_output_t *o, int *w, int *h) {
    char buf[32];
    size_t n;
    int pos = 0, rw = 0, rh = 0;
    FILE *f = fopen(o->request_path, "r");
    if (!f) return errno == ENOENT ? OUTPUT_ABSENT : OUTPUT_UNREADY;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (!parse_output_number(buf, &pos, &rw) || buf[pos++] != ' ') return OUTPUT_UNREADY;
    if (!parse_output_number(buf, &pos, &rh) || buf[pos] != '\n' || buf[pos + 1] != '\0')
        return OUTPUT_UNREADY;
    if (rw < CR_OUTPUT_MIN || rh < CR_OUTPUT_MIN || rw > CR_OUTPUT_MAX || rh > CR_OUTPUT_MAX)
        return OUTPUT_UNREADY;
    *w = rw;
    *h = rh;
    return OUTPUT_SIZE;
}

/* The scene at its content size is a transparent overlay (Virtual Cockpit); anything a MOST
 * cluster streams, and the map window always, is an opaque frame streamed as-is. */
static int output_is_content(const cr_output_t *o) {
    return !o->on_demand && o->out_w == g_width && o->out_h == g_height;
}

/* Withdraw the report before any window change or release: a stale one (the window about to
 * be replaced, or an earlier renderer process) must never let Java compose over a window
 * that is not there.  If unlink is refused, overwrite it with a line Java can never match. */
static void withdraw_output_ready(cr_output_t *o) {
    FILE *f;
    int e;
    if (!o->ready_maybe) return;             /* nothing published: a VC never has a report */
    if (unlink(o->ready_path) == 0 || errno == ENOENT) {
        o->ready_maybe = 0;
        return;
    }
    e = errno;
    f = fopen(o->ready_path, "w");
    if (f) {
        int bad = fputs("XXXX XXXX\n", f) < 0;
        if (fclose(f) == 0 && !bad) return;
    }
    fprintf(stderr, "platform_qnx: cannot invalidate %s output ready file errno=%d\n", o->name, e);
}

/* Tell Java which window is now presented (protocol.h *_READY_PATH), only once a MOST cluster
 * has asked for a size.  The token (pid.serial, fixed width) is new for every window, so
 * Java re-points the encoder after any recreation, not just a size change. */
static void publish_output_ready(cr_output_t *o) {
    FILE *f;
    int bad;
    if (!o->requested) return;
    f = fopen(o->ready_path, "w");
    o->ready_maybe = 1;
    if (!f) {
        fprintf(stderr, "platform_qnx: cannot open %s output ready file errno=%d\n", o->name, errno);
        return;
    }
    bad = fprintf(f, "%04d %04d %010d.%010u\n", o->out_w, o->out_h, (int)getpid(), ++o->ready_serial)
          != CR_OUTPUT_READY_LENGTH;
    if (fclose(f) != 0 || bad)
        fprintf(stderr, "platform_qnx: cannot publish %s output ready file\n", o->name);
}

/* The scene's context current on its window (or nothing current while that surface is down). */
static void make_scene_current(void) {
    if (g_egl_display == EGL_NO_DISPLAY) return;
    if (g_arrows.surface != EGL_NO_SURFACE && g_arrows.context != EGL_NO_CONTEXT)
        eglMakeCurrent(g_egl_display, g_arrows.surface, g_arrows.surface, g_arrows.context);
    else
        eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

/* Destroy a window's EGL surface, unbound first, then make the scene's context current again
 * (nothing, when the scene's surface was the one destroyed). */
static void drop_surface(cr_output_t *o) {
    if (g_egl_display == EGL_NO_DISPLAY || o->surface == EGL_NO_SURFACE) return;
    eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(g_egl_display, o->surface);
    o->surface = EGL_NO_SURFACE;
    make_scene_current();
}

/*
 * Create a managed cluster window via the shared cluster_surface primitive.
 * cluster_surface_create() internally does screen_create_context() — which is exactly
 * why the scene window MUST exist BEFORE eglGetDisplay: the Qualcomm (Adreno/GSL) libEGL
 * requires a screen context to already exist in the process at eglGetDisplay time.  When
 * maneuver_render ran as a JVM child it inherited the HMI's screen connection so this
 * was masked; standalone / as a framework service there is none → eglGetDisplay SIGSEGVs.
 * Idempotent (cs guard) so create_window_and_egl_surface() can call it too.
 * FORMAT=RGBA8888 (8) matches our EGL config; USAGE=OPENGL_ES2 (0x20) lets the GPU render
 * into it; transparent=1 → the cluster compositor blends us over the KDK bg / stock map.
 */
static int ensure_cluster_window(cr_output_t *o) {
    if (o->cs) return 0;
    cluster_surface_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.id          = o->id;              /* 98 / 99 */
    cfg.width       = o->out_w;           /* content size, or the MOST KOMO stream size */
    cfg.height      = o->out_h;
    cfg.format      = o->on_demand ? 12 : 8;      /* SCREEN_FORMAT_NV12 : SCREEN_FORMAT_RGBA8888 */
    cfg.usage       = o->on_demand ? 0x06 : 0x20; /* SCREEN_USAGE_READ|WRITE (CPU) : SCREEN_USAGE_OPENGL_ES2 */
    cfg.nbuffers    = 2;                  /* double-buffered */
    cfg.transparent = output_is_content(o) ? 1 : 0;   /* MOST: opaque frame, streamed as-is */
    o->cs = cluster_surface_create(&cfg);
    if (!o->cs) {
        fprintf(stderr, "platform_qnx: %s cluster_surface_create failed\n", o->name);
        return -1;
    }
    o->created++;
    o->stale = 0;
    return 0;
}

/* The window can be drawn: the scene with its surface; the map once it is the window
 * configured (a suppressed resize leaves the old, smaller one behind until the recreate). */
static int output_drawable(cr_output_t *o) {
    return o->on_demand ? o->cs && cluster_surface_window(o->cs) && !o->stale : o->surface != EGL_NO_SURFACE;
}

static int create_window_and_egl_surface(cr_output_t *o);

/* A new window, ready to draw: the scene gets its EGL surface; the map needs nothing. */
static int open_output(cr_output_t *o) {
    return o->on_demand ? (o->cs && cluster_surface_window(o->cs) ? 0 : -1) : create_window_and_egl_surface(o);
}

/* Java hears of a new scene window at once; of a map window when its first frame is posted,
 * so the MOST stream never shows an unwritten buffer. */
static void window_ready(cr_output_t *o) {
    o->shown = 0;
    if (!o->on_demand) publish_output_ready(o);
}

/*
 * Create a managed cluster window (once, via cluster_surface) + an EGL surface bound to it.
 * Called from platform_init and (to rebind EGL) from the recreate path after we lose
 * displaymanager's m_surfaceSources binding.  Each window's GL context is made on its first
 * surface; the scene's context is current again afterwards.
 */
static int create_window_and_egl_surface(cr_output_t *o) {
    if (g_egl_display == EGL_NO_DISPLAY || g_egl_config == 0) {
        fprintf(stderr, "platform_qnx: create_window: missing EGL prerequisites\n");
        return -1;
    }

    if (ensure_cluster_window(o) != 0) return -1;

    EGLNativeWindowType native_window = (EGLNativeWindowType)cluster_surface_window(o->cs);
    if (!native_window) {
        fprintf(stderr, "platform_qnx: %s cluster_surface has no window\n", o->name);
        return -1;
    }

    /* MU1316 eglsub-screen has EGL and native Screen interval state: set the
     * native state before EGL snapshots the new window. */
    {
        int interval = o->swap_interval, actual = -1;
        if (screen_set_window_property_iv(cluster_surface_window(o->cs),
                SCREEN_PROPERTY_SWAP_INTERVAL, &interval) != 0)
            fprintf(stderr, "platform_qnx: native swap interval request failed errno=%d\n", errno);
        else {
            screen_get_window_property_iv(cluster_surface_window(o->cs), SCREEN_PROPERTY_SWAP_INTERVAL, &actual);
            fprintf(stderr, "platform_qnx: %s native swap interval requested=%d readback=%d\n",
                    o->name, interval, actual);
        }
    }
    o->surface = eglCreateWindowSurface(g_egl_display, g_egl_config, native_window, NULL);
    if (o->surface == EGL_NO_SURFACE) {
        fprintf(stderr, "platform_qnx: eglCreateWindowSurface FAILED err=0x%x\n", eglGetError());
        return -1;
    }

    if (o->context == EGL_NO_CONTEXT) {
        /* A window nobody can draw must not be reported ready. */
        EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        o->context = eglCreateContext(g_egl_display, g_egl_config, EGL_NO_CONTEXT, ctx_attrs);
        if (o->context == EGL_NO_CONTEXT) {
            fprintf(stderr, "platform_qnx: %s eglCreateContext FAILED err=0x%x\n", o->name, eglGetError());
            goto fail;
        }
    }
    if (!eglMakeCurrent(g_egl_display, o->surface, o->surface, o->context)) {
        fprintf(stderr, "platform_qnx: eglMakeCurrent FAILED err=0x%x\n", eglGetError());
        goto fail;
    }
    /* Swap interval belongs to the current EGL draw surface. Apply it on
     * initial creation AND loss/recovery; a recreated surface starts at its
     * driver default. This is output pacing, not a MOST capture phase lock. */
    if (!eglSwapInterval(g_egl_display, o->swap_interval)) {
        fprintf(stderr, "platform_qnx: eglSwapInterval(%d) FAILED err=0x%x\n",
                o->swap_interval, eglGetError());
        goto fail;
    }
    make_scene_current();

    /* Context routing is Java's job: DisplayManagerMIB2High.defineContexts() declares
     * dc[80]={98,101,102,33}, dc[81]={98}, dc[82]={99} and DisplayManager.switchContext()
     * points the cluster at them → setActiveDisplayable(4, id) → MOST encoder reads the
     * window.  NO dmdt. */
    return 0;

fail:
    drop_surface(o);
    return -1;
}

/*
 * Rebuild the EGL surface on a fresh managed window after loss/disown.
 * cluster_surface owns the window teardown + recreate (100 ms backoff inside);
 * we only re-bind EGL.  With our own ids (98/99) there is no stock collision, so
 * this now fires rarely — only on a genuine DM disown (context switched away).
 */
static void recreate_output(cr_output_t *o, const char *reason) {
    fprintf(stderr, "platform_qnx: recreating %s window (reason=%s)\n", o->name, reason ? reason : "?");
    withdraw_output_ready(o);

    /* Detach + destroy the EGL surface bound to the dying window. */
    drop_surface(o);

    /* Recreate the managed window (backoff-throttled inside cluster_surface).  On
     * suppression/failure the surface stays EGL_NO_SURFACE; the next check tick
     * retries via the "window missing" path. */
    o->shown = 0;
    if (!o->cs || cluster_surface_recreate(o->cs) != 0) {
        fprintf(stderr, "platform_qnx: window recreate suppressed/failed — will retry\n");
        o->stale = 1;
        return;
    }
    o->created++;
    o->stale = 0;

    /* The same path restores the swap interval and checks context ownership. */
    if (open_output(o) != 0) return;

    fprintf(stderr, "platform_qnx: %s window recreated OK\n", o->name);
    window_ready(o);                        /* also completes a suppressed resize */
}

/* Destroy a window so displaymanager's m_surfaceSources slot clears promptly (instead of
 * at process-exit cleanup).  The slot then stays empty; Java has already switched the
 * cluster back to the stock context. */
static void release_output(cr_output_t *o) {
    withdraw_output_ready(o);
    drop_surface(o);
    o->shown = 0;
    if (o->cs) {
        cluster_surface_destroy(o->cs);
        o->cs = NULL;
        fprintf(stderr, "platform_qnx: managed window destroyed — displayable %d released\n", o->id);
    }
}

/* Follow Java's request for one window (~every 1 s).  The scene resizes to the MOST KOMO
 * stream size; the map window appears for a request and goes when it does.  A suppressed
 * resize leaves the EGL surface torn down with the new size configured;
 * platform_check_and_recover_window's "window missing" path completes it. */
static void check_output(cr_output_t *o) {
    int want_w = g_width, want_h = g_height, request, was_most = o->requested;
    if (!g_window_expected || (!o->on_demand && !o->cs)) return;
    request = read_output_request(o, &want_w, &want_h);
    if (request == OUTPUT_UNREADY) return;
    o->requested = request == OUTPUT_SIZE;
    if (o->on_demand && !o->requested) {
        if (o->cs) fprintf(stderr, "platform_qnx: %s window no longer requested\n", o->name);
        release_output(o);
        return;
    }
    if (o->cs && want_w == o->out_w && want_h == o->out_h) {
        /* A first request for the size already presented: report the live window. */
        if (o->requested && !was_most && output_drawable(o) && (!o->on_demand || o->shown))
            publish_output_ready(o);
        return;
    }

    fprintf(stderr, "platform_qnx: %s output window %dx%d -> %dx%d (%s%s)\n", o->name, o->out_w, o->out_h,
            want_w, want_h, (want_w == g_width && want_h == g_height && !o->on_demand)
            ? "content size" : "MOST KOMO stream", o->on_demand ? ", NV12" : "");
    withdraw_output_ready(o);
    drop_surface(o);
    /* Configure first: whichever recreate succeeds (here or the loss-recovery retry)
     * opens the window at the requested size. */
    o->out_w = want_w;
    o->out_h = want_h;
    o->gen++;
    o->shown = 0;
    if (!o->cs) {
        if (ensure_cluster_window(o) != 0) return;
    } else if (cluster_surface_resize(o->cs, o->out_w, o->out_h, output_is_content(o) ? 1 : 0) != 0) {
        fprintf(stderr, "platform_qnx: output resize suppressed/failed — will retry\n");
        o->stale = 1;
        return;
    } else {
        o->created++;
        o->stale = 0;
    }
    if (open_output(o) != 0) return;
    fprintf(stderr, "platform_qnx: %s output window %dx%d OK\n", o->name, o->out_w, o->out_h);
    window_ready(o);
}

void platform_check_output(void) {
    check_output(&g_arrows);
    check_output(&g_map);
}

#ifndef CR_FREE_MEMORY_PATH
#define CR_FREE_MEMORY_PATH "/proc"     /* procnto reports free system memory as its size */
#endif
long platform_free_memory_kb(void) {
    struct stat st;
    if (stat(CR_FREE_MEMORY_PATH, &st) != 0) return -1;
    return (long)(st.st_size / 1024);
}

unsigned platform_get_output(int *win_w, int *win_h, int *x, int *y, int *w, int *h, int *opaque) {
    int dw = g_arrows.out_w, dh = g_arrows.out_h;
    *win_w = g_arrows.out_w;
    *win_h = g_arrows.out_h;
    *opaque = output_is_content(&g_arrows) ? 0 : 1;
    if (!output_is_content(&g_arrows) && g_width > 0 && g_height > 0) {
        /* Aspect-fit the whole 328x181 frame, centred (rounded to the nearest pixel): the
         * empty ECC row stays in, as it does in the VC's window, so 800x252 gives 457x252
         * at x=171 (not the 328x180-based 459x252 at x=170). */
        if ((long)g_arrows.out_w * g_height <= (long)g_arrows.out_h * g_width) {
            dw = g_arrows.out_w;
            dh = (int)(((long)g_arrows.out_w * g_height + g_width / 2) / g_width);
        } else {
            dh = g_arrows.out_h;
            dw = (int)(((long)g_arrows.out_h * g_width + g_height / 2) / g_height);
        }
    }
    *w = dw;
    *h = dh;
    *x = (g_arrows.out_w - dw) / 2;
    *y = (g_arrows.out_h - dh) / 2;
    return g_arrows.gen;
}

/* The MAP view window, for its own draw (map_layer.c). */
int platform_map_window(int *w, int *h, unsigned *window) {
    if (!output_drawable(&g_map)) return 0;
    *w = g_map.out_w;
    *h = g_map.out_h;
    *window = g_map.created;
    return 1;
}

/* The NV12 map window's next buffer, handed out by platform_map_nv12_buffer until posted. */
static screen_buffer_t g_map_buffer;
static int g_map_buffer_failures;

int platform_map_nv12_buffer(unsigned char **y, int *y_stride, unsigned char **uv, int *uv_stride) {
    screen_buffer_t buffers[2] = { NULL, NULL };
    void *pointer = NULL;
    int stride = 0, size[2] = { 0, 0 }, offsets[3] = { 0, 0, 0 };
    g_map_buffer = NULL;
    if (!output_drawable(&g_map)) return 0;
    /* Only a buffer at least the window's size is ever written. */
    if (screen_get_window_property_pv(cluster_surface_window(g_map.cs), SCREEN_PROPERTY_RENDER_BUFFERS,
                                      (void **)buffers) != 0 || !buffers[0]
            || screen_get_buffer_property_pv(buffers[0], SCREEN_PROPERTY_POINTER, &pointer) != 0 || !pointer
            || screen_get_buffer_property_iv(buffers[0], SCREEN_PROPERTY_BUFFER_SIZE, size) != 0
            || size[0] < g_map.out_w || size[1] < g_map.out_h
            || screen_get_buffer_property_iv(buffers[0], SCREEN_PROPERTY_STRIDE, &stride) != 0
            || stride < g_map.out_w
            || screen_get_buffer_property_iv(buffers[0], SCREEN_PROPERTY_PLANAR_OFFSETS, offsets) != 0) {
        if (g_map_buffer_failures++ % 30 == 0)
            fprintf(stderr, "platform_qnx: map NV12 buffer unavailable (pointer=%p size=%dx%d stride=%d "
                    "for %dx%d, errno=%d)\n", pointer, size[0], size[1], stride, g_map.out_w, g_map.out_h, errno);
        return 0;
    }
    g_map_buffer_failures = 0;
    /* Screen reports where each plane starts; a chroma offset of 0 means right after luma. */
    *y = (unsigned char *)pointer + offsets[0];
    *y_stride = stride;
    *uv = (unsigned char *)pointer + (offsets[1] > offsets[0] ? offsets[1] : offsets[0] + stride * g_map.out_h);
    *uv_stride = stride;
    g_map_buffer = buffers[0];
    return 1;
}

int platform_map_nv12_post(void) {
    int dirty[4] = { 0, 0, g_map.out_w, g_map.out_h };
    screen_buffer_t buffer = g_map_buffer;
    g_map_buffer = NULL;
    if (!buffer || !g_map.cs || !cluster_surface_window(g_map.cs)) return 0;
    if (screen_post_window(cluster_surface_window(g_map.cs), buffer, 1, dirty, 0) != 0) {
        fprintf(stderr, "platform_qnx: map NV12 post failed errno=%d → recreate\n", errno);
        recreate_output(&g_map, "post-failed");
        return 0;
    }
    if (!g_map.shown) {
        g_map.shown = 1;
        publish_output_ready(&g_map);
    }
    return 1;
}

/* Present a window.  Common swap failures when displaymanager yanks it (native nav collision,
 * KOMO context exit): EGL_BAD_SURFACE, EGL_BAD_NATIVE_WINDOW, EGL_CONTEXT_LOST.  Without an
 * explicit recreate the renderer would spin on a dead surface forever, since the scene's swap
 * drives the loop's frame pacing. */
static int swap_output(cr_output_t *o) {
    EGLint err;
    if (eglSwapBuffers(g_egl_display, o->surface) == EGL_TRUE) {
        if (o->swap_failures > 0)
            fprintf(stderr, "platform_qnx: %s eglSwapBuffers recovered after %u failures\n",
                    o->name, o->swap_failures);
        o->swap_failures = 0;
        return 1;
    }
    err = eglGetError();
    o->swap_failures++;
    if (o->swap_failures == 1 || (o->swap_failures % 30u) == 0)
        fprintf(stderr, "platform_qnx: %s eglSwapBuffers failed err=0x%x streak=%u → recreate\n",
                o->name, err, o->swap_failures);
    recreate_output(o, "swap-failed");
    return 0;
}

/*
 * Health check (cheap — call ~every 5 s).  cluster_surface_lost() does the
 * VISIBLE + MANAGER_STRING probes (disown detection with the seen-once handshake).
 * We also recover the case where a prior recreate was backoff-suppressed and left
 * the EGL surface torn down.
 */
static void check_and_recover(cr_output_t *o) {
    if (!o->cs) return;

    /* A prior recreate was suppressed → window (or its EGL surface) still down → retry. */
    if (!output_drawable(o)) {
        recreate_output(o, "window missing");
        return;
    }
    if (cluster_surface_lost(o->cs)) {
        recreate_output(o, "window lost/disowned");
    }
}

void platform_check_and_recover_window(void) {
    if (!g_window_expected) return;
    check_and_recover(&g_arrows);
    check_and_recover(&g_map);
}

/* Counterpart to platform_check_and_recover_window (renderer atexit / shutdown). */
void platform_release_displayable(void) {
    release_output(&g_map);
    release_output(&g_arrows);
}

/* Context focus is Java-driven now (DisplayManager.switchContext); this
 * renderer runs no dmdt.  Kept as a no-op so main.c / the macOS stub still
 * link against the same symbol. */
void platform_ensure_focus(void) {
}

static void signal_handler(int sig) {
    /* Only use async-signal-safe functions (write, signal, raise). */
    static const char msg_segv[] = "platform_qnx: caught SIGSEGV\n";
    static const char msg_abrt[] = "platform_qnx: caught SIGABRT\n";
    static const char msg_term[] = "platform_qnx: caught SIGTERM\n";
    static const char msg_unk[]  = "platform_qnx: caught signal\n";

    if (sig == SIGSEGV)      write(STDERR_FILENO, msg_segv, sizeof(msg_segv) - 1);
    else if (sig == SIGABRT) write(STDERR_FILENO, msg_abrt, sizeof(msg_abrt) - 1);
    else if (sig == SIGTERM) write(STDERR_FILENO, msg_term, sizeof(msg_term) - 1);
    else                     write(STDERR_FILENO, msg_unk,  sizeof(msg_unk)  - 1);

    g_should_close = 1;
    if (sig == SIGTERM) {
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

int platform_init(int width, int height) {
    g_width = width;
    g_height = height;
    g_arrows.out_w = width;
    g_arrows.out_h = height;
    /* A MOST session earlier in this boot. */
    g_arrows.requested = read_output_request(&g_arrows, &g_arrows.out_w, &g_arrows.out_h) == OUTPUT_SIZE;

    /* Our own displayable id (98), NOT the stock route-guidance slot (20).
     * No env override — pointing the renderer at a different id would silently
     * leave us out of the cluster MOST encoder path. */
    g_displayable_id = CR_DISPLAYABLE_ID;
    g_context_id     = read_env_int("CR_CONTEXT_ID", CR_CONTEXT_ID);
    g_display_id     = read_env_int("CR_DISPLAY_ID", CR_DISPLAY_ID);

    if (!getenv("IPL_CONFIG_DIR"))
        putenv("IPL_CONFIG_DIR=/etc/eso/production");

    signal(SIGTERM, signal_handler);
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
    atexit(platform_release_displayable);

    /* Force line-buffered stderr so output isn't lost on crash */
    setvbuf(stderr, NULL, _IOLBF, 0);

    fprintf(stderr, "platform_qnx: displayable=%d context=%d display=%d\n",
            g_displayable_id, g_context_id, g_display_id);
    if (!output_is_content(&g_arrows))
        fprintf(stderr, "platform_qnx: output window %dx%d (MOST KOMO stream) for %dx%d content\n",
                g_arrows.out_w, g_arrows.out_h, g_width, g_height);
    fprintf(stderr, "platform_qnx: LD_LIBRARY_PATH=%s\n",
            getenv("LD_LIBRARY_PATH") ? getenv("LD_LIBRARY_PATH") : "(unset)");
    fprintf(stderr, "platform_qnx: IPL_CONFIG_DIR=%s\n",
            getenv("IPL_CONFIG_DIR") ? getenv("IPL_CONFIG_DIR") : "(unset)");
    {
        char cwd[256];
        fprintf(stderr, "platform_qnx: pid=%d ppid=%d cwd=%s\n",
                getpid(), getppid(),
                getcwd(cwd, sizeof(cwd)) ? cwd : "(unknown)");
    }

    /* Create the managed cluster window (→ screen_create_context) BEFORE eglGetDisplay.
     * The Qualcomm Adreno/GSL libEGL derefs a screen context that must already exist in
     * the process; without it eglGetDisplay SIGSEGVs.  This was masked when the HMI JVM
     * spawned us (inherited screen connection) but not standalone / as a framework service. */
    withdraw_output_ready(&g_arrows);        /* reports left by an earlier renderer */
    withdraw_output_ready(&g_map);
    fprintf(stderr, "platform_qnx: pre-EGL cluster window (screen_create_context)...\n");
    if (ensure_cluster_window(&g_arrows) != 0) return -1;

    fprintf(stderr, "platform_qnx: eglGetDisplay...\n");
    g_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "platform_qnx: FAIL eglGetDisplay\n");
        return -1;
    }

    EGLint major, minor;
    if (!eglInitialize(g_egl_display, &major, &minor)) {
        fprintf(stderr, "platform_qnx: FAIL eglInitialize (err=0x%x)\n", eglGetError());
        return -1;
    }
    fprintf(stderr, "%s platform_qnx: EGL %d.%d\n", log_stamp(), major, minor);

    /* No MSAA — FXAA post-process handles edge smoothing instead */
    EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };

    EGLint num_configs = 0;
    fprintf(stderr, "platform_qnx: eglChooseConfig...\n");
    if (!eglChooseConfig(g_egl_display, config_attrs, &g_egl_config, 1, &num_configs) ||
        num_configs == 0) {
            fprintf(stderr, "platform_qnx: FAIL eglChooseConfig (err=0x%x)\n", eglGetError());
            return -1;
    }
    fprintf(stderr, "platform_qnx: got %d config(s)\n", num_configs);

    /* screen_* are linked directly (-lscreen) via cluster_surface — no dlsym.  The scene's
     * GL context is made with its first surface. */
    eglBindAPI(EGL_OPENGL_ES_API);

    fprintf(stderr, "platform_qnx: display_create_window(%dx%d, disp=%d)...\n",
            width, height, g_displayable_id);
    if (create_window_and_egl_surface(&g_arrows) != 0) {
        return -1;
    }
    g_window_expected = 1;
    publish_output_ready(&g_arrows);

    /* Keep the live BSP capability strings in the renderer log.  They are the
     * authoritative answer for cross-process Screen/EGL buffer import support;
     * SDK headers alone only describe APIs that may not be implemented by this
     * APQ8064 image. */
    fprintf(stderr, "platform_qnx: EGL vendor=%s version=%s extensions=%s\n",
            eglQueryString(g_egl_display, EGL_VENDOR),
            eglQueryString(g_egl_display, EGL_VERSION),
            eglQueryString(g_egl_display, EGL_EXTENSIONS));
    fprintf(stderr, "platform_qnx: GL vendor=%s renderer=%s version=%s extensions=%s\n",
            (const char *)glGetString(GL_VENDOR),
            (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VERSION),
            (const char *)glGetString(GL_EXTENSIONS));

    /* Do NOT switch focus here.  Java waits for EVT_FRAME_READY and then
     * forces a real away->74 context transition so DisplayManager's
     * preContextSwitchHook updates the MOST encoder after our first frame
     * is already queued. */

    fprintf(stderr, "platform_qnx: OK %dx%d (swap interval=2 requested; capture cadence independent)\n", width, height);
    return 0;
}

int platform_swap(void) {
    return swap_output(&g_arrows);
}

void platform_poll(void) {
    /* QNX: no event loop, just check signal flag */
}

int platform_should_close(void) {
    return g_should_close;
}

void platform_shutdown(void) {
    int hidden = 0;
    struct timespec release_wait;
    cr_output_t *outputs[2];
    int i;
    /* Clean shutdown sequence:
     *   1. Release GL contexts and EGL surfaces while keeping EGLDisplay /
     *      displayinit globals alive (full teardown of shared display
     *      resources can collide with native components).
     *   2. Explicitly destroy our screen_windows via screen_destroy_window
     *      so displaymanager's m_surfaceSources slots vacate promptly.
     * Context restore (switching the cluster back to the stock context) is
     * Java's job now — this renderer runs no dmdt. */
    outputs[0] = &g_arrows;
    outputs[1] = &g_map;
    if (g_egl_display != EGL_NO_DISPLAY) {
        for (i = 0; i < 2; ++i)
            if (outputs[i]->cs && cluster_surface_window(outputs[i]->cs))
                screen_set_window_property_iv(cluster_surface_window(outputs[i]->cs),
                                              SCREEN_PROPERTY_VISIBLE, &hidden);
        if (g_arrows.context != EGL_NO_CONTEXT) glFinish();
        release_wait.tv_sec = 0;
        release_wait.tv_nsec = 50L * 1000L * 1000L;
        while (nanosleep(&release_wait, &release_wait) != 0 && errno == EINTR) {}
        for (i = 0; i < 2; ++i) drop_surface(outputs[i]);   /* then nothing is current */
        for (i = 0; i < 2; ++i) {
            if (outputs[i]->context != EGL_NO_CONTEXT) {
                /* Release the GL context so the driver frees its internal
                 * resources (shader cache, framebuffer attachments, command
                 * buffers).  Skip eglTerminate — EGLDisplay is shared with
                 * native cluster components and terminating it would tear
                 * down their surfaces too. */
                eglDestroyContext(g_egl_display, outputs[i]->context);
                outputs[i]->context = EGL_NO_CONTEXT;
            }
        }
    }
    platform_release_displayable();
}

void platform_get_framebuffer_size(int *width, int *height) {
    *width = g_width;
    *height = g_height;
}

void platform_get_routing_ids(int *display_id, int *context_id, int *displayable_id) {
    if (display_id) *display_id = g_display_id;
    if (context_id) *context_id = g_context_id;
    if (displayable_id) *displayable_id = g_displayable_id;
}

int platform_key_tap(int key) {
    (void)key;
    return 0;
}

#endif /* PLATFORM_QNX */
