/*
 * MOST output window: host test of the real platform_qnx.c + cluster_surface.c
 * against a recording fake of the QNX screen / EGL APIs.
 *
 * Checks what maneuver_render does with CR_MOST_OUTPUT_PATH ("<width> <height>", written
 * by MostPresentation): the managed window 98's SIZE/BUFFER_SIZE/TRANSPARENCY, the EGL
 * surface rebind, the presentation rectangle platform_get_output() reports, and that
 * nothing changes without the request (Virtual Cockpit).  Built and run inside the QNX
 * toolchain image with the host gcc (run-native-tests.sh).
 */
#define PLATFORM_QNX 1

/* platform_free_memory_kb() reads procnto's /proc size on the car; here a file of known size. */
#define CR_FREE_MEMORY_PATH "/tmp/cr_free_memory_probe"
#include <unistd.h>
static int test_unlink(const char *path);
#define unlink test_unlink
#include "platform_qnx.c"          /* -I <renderer tree>/maneuver_render */
#undef unlink
#include "cluster_surface.c"       /* -I <renderer tree>/common */

#include <stdarg.h>

/* ---------------------------------------------------------------- fake screen */

struct fake_window {
    int id;
    int size[2], buffer_size[2];
    int transparency;          /* -1 = never set */
    int managed, buffers, destroyed;
};

#define MAX_WINDOWS 64
static struct fake_window g_windows[MAX_WINDOWS];
static int g_window_count;
static int g_contexts_created, g_contexts_destroyed;
static int g_fail_create_window;
static int g_fail_egl_create_surface;
static int g_create_with_ready;
static int g_fail_ready_unlink;

static int test_unlink(const char *path) {
    if (g_fail_ready_unlink && strcmp(path, CR_MOST_OUTPUT_READY_PATH) == 0) {
        errno = EACCES;
        return -1;
    }
    return unlink(path);
}

static struct fake_window *fw(screen_window_t w) { return (struct fake_window *)w; }

int screen_create_context(screen_context_t *pctx, int flags) {
    static int ctx;
    (void)flags;
    g_contexts_created++;
    *pctx = (screen_context_t)&ctx;
    return 0;
}
int screen_destroy_context(screen_context_t ctx) { (void)ctx; g_contexts_destroyed++; return 0; }
int screen_create_window(screen_window_t *pwin, screen_context_t ctx) {
    (void)ctx;
    FILE *ready = fopen(CR_MOST_OUTPUT_READY_PATH, "r");
    if (ready) {
        char line[64] = "";
        size_t n = fread(line, 1, sizeof(line) - 1, ready);
        fclose(ready);
        /* Any report Java could accept: a bare size (earlier renderers) or size + token. */
        int sized = line[0] >= '0' && line[0] <= '9' && line[4] == ' ' &&
                    line[5] >= '0' && line[5] <= '9';
        if (sized && ((n == 10 && line[9] == '\n') ||
                      (n == CR_OUTPUT_READY_LENGTH && line[9] == ' ' && line[CR_OUTPUT_READY_LENGTH - 1] == '\n')))
            g_create_with_ready++;
    }
    if (g_fail_create_window || g_window_count >= MAX_WINDOWS) { errno = ENOMEM; return -1; }
    struct fake_window *w = &g_windows[g_window_count];
    memset(w, 0, sizeof(*w));
    w->id = ++g_window_count;
    w->transparency = -1;
    *pwin = (screen_window_t)w;
    return 0;
}
int screen_destroy_window(screen_window_t win) { fw(win)->destroyed = 1; return 0; }
int screen_set_window_property_cv(screen_window_t win, int pname, int len, const char *param) {
    (void)win; (void)pname; (void)len; (void)param;
    return 0;
}
int screen_set_window_property_iv(screen_window_t win, int pname, const int *param) {
    struct fake_window *w = fw(win);
    if (w->destroyed) { errno = EBADF; return -1; }
    if (pname == SCREEN_PROPERTY_SIZE) { w->size[0] = param[0]; w->size[1] = param[1]; }
    if (pname == SCREEN_PROPERTY_BUFFER_SIZE) { w->buffer_size[0] = param[0]; w->buffer_size[1] = param[1]; }
    if (pname == SCREEN_PROPERTY_TRANSPARENCY) w->transparency = param[0];
    return 0;
}
int screen_get_window_property_iv(screen_window_t win, int pname, int *param) {
    if (fw(win)->destroyed) { errno = EBADF; return -1; }
    *param = pname == SCREEN_PROPERTY_SWAP_INTERVAL ? 2 : 1;
    return 0;
}
int screen_get_window_property_cv(screen_window_t win, int pname, int len, char *param) {
    (void)win; (void)pname;
    snprintf(param, (size_t)len, "%s", CS_MANAGER_STRING);
    return 0;
}
int screen_manage_window(screen_window_t win, const char *group) {
    (void)group;
    fw(win)->managed = 1;
    return 0;
}
int screen_create_window_buffers(screen_window_t win, int count) { fw(win)->buffers = count; return 0; }

/* ---------------------------------------------------------------- fake EGL / GL */

static int g_surfaces_created, g_surfaces_destroyed, g_live_surface;
static void *g_surface_window;          /* native window of the live surface */
static void *g_current_surface;
static int dummy_display, dummy_config, dummy_context;

EGLDisplay eglGetDisplay(EGLNativeDisplayType id) { (void)id; return (EGLDisplay)&dummy_display; }
EGLBoolean eglInitialize(EGLDisplay d, EGLint *maj, EGLint *min) { (void)d; *maj = 1; *min = 4; return EGL_TRUE; }
EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *a, EGLConfig *c, EGLint n, EGLint *num) {
    (void)d; (void)a; (void)n;
    *c = (EGLConfig)&dummy_config;
    *num = 1;
    return EGL_TRUE;
}
EGLBoolean eglBindAPI(EGLenum api) { (void)api; return EGL_TRUE; }
EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext s, const EGLint *a) {
    (void)d; (void)c; (void)s; (void)a;
    return (EGLContext)&dummy_context;
}
EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint *a) {
    (void)d; (void)c; (void)a;
    if (g_fail_egl_create_surface || fw((screen_window_t)w)->destroyed) return EGL_NO_SURFACE;
    g_surfaces_created++;
    g_live_surface++;
    g_surface_window = w;
    return (EGLSurface)(size_t)(0x1000 + g_surfaces_created);
}
EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s) {
    (void)d; (void)s;
    g_surfaces_destroyed++;
    g_live_surface--;
    return EGL_TRUE;
}
EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c) {
    (void)d; (void)rd; (void)c;
    g_current_surface = dr;
    return EGL_TRUE;
}
EGLBoolean eglSwapInterval(EGLDisplay d, EGLint i) { (void)d; (void)i; return EGL_TRUE; }
EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s) { (void)d; (void)s; return EGL_TRUE; }
EGLint eglGetError(void) { return 0x3000; }
const char *eglQueryString(EGLDisplay d, EGLint n) { (void)d; (void)n; return "fake"; }
EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c) { (void)d; (void)c; return EGL_TRUE; }
EGLBoolean eglTerminate(EGLDisplay d) { (void)d; return EGL_TRUE; }
const GLubyte *glGetString(GLenum n) { (void)n; return (const GLubyte *)"fake"; }
void glFinish(void) { }

/* ---------------------------------------------------------------- checks */

static int g_checks, g_failures;

static void check(int ok, const char *fmt, ...) {
    va_list ap;
    g_checks++;
    if (ok) return;
    g_failures++;
    fprintf(stdout, "FAIL: ");
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fprintf(stdout, "\n");
}

static struct fake_window *live_window(void) {
    return g_cs ? fw(cluster_surface_window(g_cs)) : NULL;
}

static void write_request(const char *text) {
    FILE *f = fopen(CR_MOST_OUTPUT_PATH, "w");
    if (!f) { perror("request"); exit(2); }
    fputs(text, f);
    fclose(f);
}

static void write_ready(const char *text) {
    FILE *f = fopen(CR_MOST_OUTPUT_READY_PATH, "w");
    if (!f) { perror("ready"); exit(2); }
    fputs(text, f);
    fclose(f);
}

static void expect_no_ready(const char *what) {
    FILE *f = fopen(CR_MOST_OUTPUT_READY_PATH, "r");
    check(f == NULL && errno == ENOENT, "%s: no stale renderer ready file", what);
    if (f) fclose(f);
}

static void expect_output(const char *what, int ww, int wh, int x, int y, int w, int h, int opaque) {
    int a[7];
    platform_get_output(&a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]);
    check(a[0] == ww && a[1] == wh && a[2] == x && a[3] == y && a[4] == w && a[5] == h && a[6] == opaque,
          "%s: output %dx%d at (%d,%d %dx%d) opaque=%d, expected %dx%d at (%d,%d %dx%d) opaque=%d",
          what, a[0], a[1], a[2], a[3], a[4], a[5], a[6], ww, wh, x, y, w, h, opaque);
}

static void expect_window(const char *what, int w, int h, int transparency) {
    struct fake_window *win = live_window();
    check(win != NULL && !win->destroyed, "%s: live window", what);
    if (!win) return;
    check(win->size[0] == w && win->size[1] == h, "%s: SIZE %dx%d, expected %dx%d",
          what, win->size[0], win->size[1], w, h);
    check(win->buffer_size[0] == w && win->buffer_size[1] == h, "%s: BUFFER_SIZE %dx%d, expected %dx%d",
          what, win->buffer_size[0], win->buffer_size[1], w, h);
    check(win->transparency == transparency, "%s: TRANSPARENCY %d, expected %d",
          what, win->transparency, transparency);
    check(win->managed && win->buffers == 2, "%s: managed and double-buffered", what);
    check(g_live_surface == 1 && g_surface_window == (void *)win && g_current_surface == g_egl_surface,
          "%s: one current EGL surface on the live window", what);
    check(g_create_with_ready == 0, "%s: ready removed before window creation", what);
    /* Java composes ctx 81 only once the renderer reports this exact size, and re-composes
     * when the token (this process, this window's serial) changes.  No MOST request: no
     * report at all. */
    char want[64], got[64] = "";
    FILE *f = fopen(CR_MOST_OUTPUT_READY_PATH, "r");
    if (f) { size_t n = fread(got, 1, sizeof(got) - 1, f); got[n] = '\0'; fclose(f); }
    if (g_most_request)
        snprintf(want, sizeof(want), "%04d %04d %010d.%010u\n", w, h, (int)getpid(), g_ready_serial);
    else
        want[0] = '\0';
    check(strcmp(got, want) == 0, "%s: renderer reports '%s', expected '%s'", what, got, want);
}

static void reset_platform(void);
static void wait_backoff(void);

static void read_ready(char *out, size_t size) {
    FILE *f = fopen(CR_MOST_OUTPUT_READY_PATH, "r");
    size_t n = 0;
    if (f) { n = fread(out, 1, size - 1, f); fclose(f); }
    out[n] = '\0';
}

/* Every window the renderer creates gets a new token - resize, swap-failure recreate, loss
 * recovery - so Java can re-point the encoder after a recreation it did not ask for; and a
 * first request for the size already presented is reported at once. */
static void every_window_new_token(void) {
    char a[64], b[64], c[64], d[64];
    reset_platform();
    write_request("0328 0181\n");                 /* a MOST request that equals the content */
    check(platform_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) == 0, "token: platform_init");
    expect_window("token: init", 328, 181, SCREEN_TRANSPARENCY_SOURCE_OVER);
    read_ready(a, sizeof(a));
    write_request("0800 0252\n");
    wait_backoff();
    platform_check_output();
    expect_window("token: resize", 800, 252, -1);
    read_ready(b, sizeof(b));
    wait_backoff();
    platform_recreate_window("swap-failed");
    expect_window("token: swap-failure recreate", 800, 252, -1);
    read_ready(c, sizeof(c));
    check(strcmp(a, b) != 0 && strcmp(b, c) != 0 && strncmp(b, c, 10) == 0,
          "token: new token for every window, same size after a recreate ('%s' '%s' '%s')", a, b, c);

    reset_platform();                             /* a VC renderer, then a first request */
    check(platform_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) == 0, "first: platform_init");
    expect_no_ready("first: no report before any request");
    write_request("0328 0181\n");
    wait_backoff();
    platform_check_output();
    read_ready(d, sizeof(d));
    check(strncmp(d, "0328 0181 ", 10) == 0 && strlen(d) == CR_OUTPUT_READY_LENGTH,
          "first: a request for the presented size is reported at once ('%s')", d);
}

static void reset_platform(void) {
    if (g_cs) { cluster_surface_destroy(g_cs); g_cs = NULL; }
    g_egl_display = EGL_NO_DISPLAY;
    g_egl_surface = EGL_NO_SURFACE;
    g_egl_context = EGL_NO_CONTEXT;
    g_egl_config = 0;
    g_window_expected = 0;
    g_live_surface = 0;
    g_surface_window = NULL;
    g_current_surface = NULL;
    g_fail_create_window = 0;
    g_fail_egl_create_surface = 0;
    g_create_with_ready = 0;
    g_fail_ready_unlink = 0;
    g_most_request = 0;                           /* a fresh renderer process */
    g_ready_maybe = 1;
    unlink(CR_MOST_OUTPUT_PATH);
    unlink(CR_MOST_OUTPUT_READY_PATH);
}

static void wait_backoff(void) {
    struct timespec t = {0, 150 * 1000000L};
    nanosleep(&t, NULL);
}

/* A Virtual Cockpit never writes the request: the window stays the content, transparent. */
static void no_request_is_unchanged(void) {
    reset_platform();
    check(platform_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) == 0, "vc: platform_init");
    expect_window("vc: init", 328, 181, SCREEN_TRANSPARENCY_SOURCE_OVER);
    expect_output("vc: init", 328, 181, 0, 0, 328, 181, 0);
    unsigned gen = platform_get_output(&(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0});
    int windows = g_window_count, surfaces = g_surfaces_created;
    wait_backoff();
    platform_check_output();
    platform_check_output();
    check(g_window_count == windows && g_surfaces_created == surfaces, "vc: no window/surface churn");
    check(platform_get_output(&(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}) == gen,
          "vc: output generation unchanged");
    expect_window("vc: after checks", 328, 181, SCREEN_TRANSPARENCY_SOURCE_OVER);
    expect_no_ready("vc: no report on a Virtual Cockpit");
}

/* MOST: the request arrives while the renderer runs (CarPlay connect). */
static void request_resizes_running_window(void) {
    reset_platform();
    check(platform_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) == 0, "most: platform_init");
    int ctx_before = g_contexts_created, ctx_destroyed_before = g_contexts_destroyed;
    unsigned gen0 = platform_get_output(&(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0});
    struct fake_window *old = live_window();
    int destroyed = g_surfaces_destroyed;
    write_request("800 252\n");
    wait_backoff();
    platform_check_output();
    check(old->destroyed, "most: old 328x181 window destroyed");
    check(g_surfaces_destroyed == destroyed + 1, "most: old EGL surface destroyed once");
    check(g_contexts_created == ctx_before && g_contexts_destroyed == ctx_destroyed_before,
          "most: screen context kept (EGL display was created with it)");
    expect_window("most: resized", 800, 252, -1);
    expect_output("most: resized", 800, 252, 171, 0, 457, 252, 1);
    unsigned gen1 = platform_get_output(&(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0});
    check(gen1 != gen0, "most: output generation bumped");
    int windows = g_window_count;
    wait_backoff();
    platform_check_output();
    platform_check_and_recover_window();
    check(g_window_count == windows, "most: unchanged request does not recreate");
    check(platform_get_output(&(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}) == gen1,
          "most: generation stable");
}

/* A renderer restarted later in the same boot reads the request before its first window. */
static void request_at_init(void) {
    reset_platform();
    write_request("800 252\n");
    write_ready("0328 0181\n");                 /* left by an earlier renderer process */
    int windows = g_window_count;
    check(platform_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) == 0, "init: platform_init");
    check(g_window_count == windows + 1, "init: exactly one window created");
    expect_window("init", 800, 252, -1);
    expect_output("init", 800, 252, 171, 0, 457, 252, 1);
    wait_backoff();
    platform_check_output();
    check(g_window_count == windows + 1, "init: no resize afterwards");
}

static void failed_init_does_not_report_ready(void) {
    reset_platform();
    write_request("0800 0252\n");
    write_ready("0328 0181\n");
    g_fail_egl_create_surface = 1;
    check(platform_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) != 0,
          "init failure: EGL surface creation fails");
    expect_no_ready("init failure");
    check(g_create_with_ready == 0, "init failure: stale ready removed before window creation");
}

/* Incomplete, malformed or out-of-range requests are ignored: at start-up the content size
 * is used. */
static void invalid_requests(void) {
    static const char *bad[] = {
        "", "abc\n", "800\n", "800 252", "800 25", "800  252\n", " 800 252\n", "800 252 \n",
        "800 252\n\n", "800x252\n", "10 10\n", "63 252\n", "800 2049\n", "5000 252\n",
        "-800 252\n", "99999999999999999999 252\n"
    };
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char what[64];
        reset_platform();
        write_request(bad[i]);
        snprintf(what, sizeof(what), "invalid '%s'", bad[i]);
        check(platform_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) == 0, "%s: platform_init", what);
        expect_window(what, 328, 181, SCREEN_TRANSPARENCY_SOURCE_OVER);
        expect_output(what, 328, 181, 0, 0, 328, 181, 0);
    }
    /* Removing the request returns to the content size. */
    reset_platform();
    write_request("800 252\n");
    check(platform_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) == 0, "revert: platform_init");
    unlink(CR_MOST_OUTPUT_PATH);
    wait_backoff();
    platform_check_output();
    expect_window("revert", 328, 181, SCREEN_TRANSPARENCY_SOURCE_OVER);
    expect_output("revert", 328, 181, 0, 0, 328, 181, 0);
}

/* Java writes the request fixed-width ("%04d %04d\n"), so a rewrite is whole even if the
 * car's shared-memory /tmp did not truncate; variable width would leave a stale tail. */
static void fixed_width_rewrite(void) {
    FILE *f;
    reset_platform();
    write_request("0800 0252\n");
    check(platform_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) == 0, "fixed: platform_init");
    expect_window("fixed: leading zeros", 800, 252, -1);
    f = fopen(CR_MOST_OUTPUT_PATH, "r+");                 /* rewrite without truncation */
    fputs("0656 0360\n", f);
    fclose(f);
    wait_backoff();
    platform_check_output();
    expect_window("fixed: untruncated rewrite", 656, 360, -1);
    write_request("2048 1024\n");
    wait_backoff();
    platform_check_output();
    f = fopen(CR_MOST_OUTPUT_PATH, "r+");                 /* variable width, untruncated */
    fputs("800 252\n", f);
    fclose(f);
    wait_backoff();
    platform_check_output();
    expect_window("fixed: stale tail refused, window kept", 2048, 1024, -1);
}

/* Java writes the request in place (the car's /tmp cannot rename), so a read can see the
 * truncated or half-written file.  Anything short of a complete line keeps the window. */
static void torn_reads_keep_window(void) {
    static const char *torn[] = { "", "6", "656", "656 ", "656 3", "656 36", "656 360" };
    size_t i;
    reset_platform();
    write_request("800 252\n");
    check(platform_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) == 0, "torn: platform_init");
    unsigned gen = platform_get_output(&(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0});
    int windows = g_window_count, surfaces = g_surfaces_created;
    for (i = 0; i < sizeof(torn) / sizeof(torn[0]); i++) {
        char what[48];
        snprintf(what, sizeof(what), "torn '%s'", torn[i]);
        write_request(torn[i]);
        wait_backoff();
        platform_check_output();
        check(g_window_count == windows && g_surfaces_created == surfaces, "%s: window kept", what);
        expect_window(what, 800, 252, -1);
        check(platform_get_output(&(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}, &(int){0}) == gen,
              "%s: output unchanged", what);
    }
    write_request("656 360\n");                   /* the write completes */
    wait_backoff();
    platform_check_output();
    expect_window("torn: completed", 656, 360, -1);
    expect_output("torn: completed", 656, 360, 2, 0, 652, 360, 1);
}

/* Aspect fit, centred, for other stream sizes stock could report. */
static void fit_geometry(void) {
    static const int cases[][6] = {
        /* window w, h -> x, y, w, h */
        {800, 252, 171, 0, 457, 252},
        {656, 362, 0, 0, 656, 362},
        {400, 220, 0, 0, 399, 220},
        {328, 400, 0, 109, 328, 181},
        {64, 64, 0, 14, 64, 35},
        {2048, 2048, 0, 459, 2048, 1130},
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char req[32], what[48];
        reset_platform();
        snprintf(req, sizeof(req), "%d %d\n", cases[i][0], cases[i][1]);
        write_request(req);
        snprintf(what, sizeof(what), "fit %dx%d", cases[i][0], cases[i][1]);
        check(platform_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) == 0, "%s: platform_init", what);
        expect_output(what, cases[i][0], cases[i][1], cases[i][2], cases[i][3], cases[i][4], cases[i][5], 1);
        int a[7];
        platform_get_output(&a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]);
        check(a[2] >= 0 && a[3] >= 0 && a[2] + a[4] <= a[0] && a[3] + a[5] <= a[1], "%s: inside window", what);
    }
}

/* A resize inside cluster_surface's 100 ms backoff leaves the EGL surface down with the
 * new size configured; the next health check completes it at that size. */
static void suppressed_resize_completes(void) {
    reset_platform();
    check(platform_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) == 0, "backoff: platform_init");
    write_request("800 252\n");
    wait_backoff();
    platform_check_output();
    expect_window("backoff: first", 800, 252, -1);
    write_request("656 362\n");
    platform_check_output();                      /* within 100 ms of the first */
    check(g_egl_surface == EGL_NO_SURFACE && g_live_surface == 0,
          "backoff: surface torn down while suppressed");
    expect_no_ready("backoff: suppressed resize");
    expect_output("backoff: configured", 656, 362, 0, 0, 656, 362, 1);
    platform_check_output();
    check(g_egl_surface == EGL_NO_SURFACE, "backoff: unchanged request does not retry by itself");
    wait_backoff();
    platform_check_and_recover_window();          /* "egl surface missing" */
    expect_window("backoff: recovered", 656, 362, -1);
    /* platform_swap's swap-failure recreate reuses the configured size too. */
    write_request("800 252\n");
    platform_check_output();                      /* suppressed again */
    check(g_egl_surface == EGL_NO_SURFACE, "backoff: suppressed again");
    expect_no_ready("backoff: suppressed again");
    wait_backoff();
    platform_recreate_window("swap-failed");
    expect_window("backoff: swap recreate", 800, 252, -1);
}

/* Window creation failing outright keeps retrying at the requested size. */
static void failed_create_retries(void) {
    reset_platform();
    check(platform_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) == 0, "fail: platform_init");
    write_request("800 252\n");
    wait_backoff();
    g_fail_create_window = 1;
    platform_check_output();
    check(g_egl_surface == EGL_NO_SURFACE, "fail: no surface while creation fails");
    expect_no_ready("fail: window creation");
    g_fail_create_window = 0;
    wait_backoff();
    platform_check_and_recover_window();
    expect_window("fail: recovered", 800, 252, -1);
}

static void failed_egl_resize_retries(void) {
    reset_platform();
    check(platform_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) == 0, "EGL fail: platform_init");
    write_request("0800 0252\n");
    wait_backoff();
    g_fail_egl_create_surface = 1;
    platform_check_output();
    check(g_egl_surface == EGL_NO_SURFACE, "EGL fail: no EGL surface after resize");
    expect_no_ready("EGL fail: resize");
    g_fail_egl_create_surface = 0;
    wait_backoff();
    platform_check_and_recover_window();
    expect_window("EGL fail: recovered", 800, 252, -1);
    platform_release_displayable();
    expect_no_ready("EGL fail: released");
}

static void unlink_failure_invalidates_old_ready(void) {
    reset_platform();
    write_request("0656 0360\n");                 /* a MOST window with a live report */
    check(platform_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) == 0, "unlink fail: platform_init");
    expect_window("unlink fail: reported", 656, 360, -1);
    write_request("0800 0252\n");
    wait_backoff();
    g_fail_ready_unlink = 1;
    g_fail_create_window = 1;
    platform_check_output();
    check(g_egl_surface == EGL_NO_SURFACE, "unlink fail: resize failed after invalidation");
    FILE *f = fopen(CR_MOST_OUTPUT_READY_PATH, "r");
    int first = f ? fgetc(f) : EOF;
    if (f) fclose(f);
    check(first == 'X', "unlink fail: old ready size poisoned when unlink is unavailable");
    g_fail_ready_unlink = 0;
    g_fail_create_window = 0;
    wait_backoff();
    platform_check_and_recover_window();
    expect_window("unlink fail: recovered", 800, 252, -1);
}

/* The renderer logs free memory around its render-target resize: stat size in KB, or -1. */
static void free_memory_reported(void) {
    FILE *f = fopen(CR_FREE_MEMORY_PATH, "wb");
    check(f != NULL, "free memory: probe file created");
    if (!f) return;
    check(ftruncate(fileno(f), 5L * 1024 * 1024 + 700) == 0, "free memory: probe sized");
    fclose(f);
    check(platform_free_memory_kb() == 5120, "free memory: %ld KB, expected 5120", platform_free_memory_kb());
    unlink(CR_FREE_MEMORY_PATH);
    check(platform_free_memory_kb() == -1, "free memory: -1 when unreadable, got %ld", platform_free_memory_kb());
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    free_memory_reported();
    no_request_is_unchanged();
    request_resizes_running_window();
    request_at_init();
    failed_init_does_not_report_ready();
    invalid_requests();
    fit_geometry();
    torn_reads_keep_window();
    fixed_width_rewrite();
    suppressed_resize_completes();
    failed_create_retries();
    failed_egl_resize_retries();
    unlink_failure_invalidates_old_ready();
    every_window_new_token();
    reset_platform();
    printf("most_output_platform_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
