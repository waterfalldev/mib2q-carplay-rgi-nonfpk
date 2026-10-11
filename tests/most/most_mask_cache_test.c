/*
 * Transition-mask cache: host test of the real render.c + maneuver.c against a GLES2 fake
 * that records, per frame, the clears and draws that reach each mask framebuffer and the
 * texture and depth bias of every draw into the scene framebuffer.
 *
 * Checks that a push paints the current (set 0) and next (set 1) masks once and then only
 * composites them, that a cached push frame composites exactly what the painted frame did
 * (same textures, same depth bias, no extra draws), that a new or refreshed next maneuver
 * repaints set 1 only, that the commit repaints set 0, that set 1 composites set 0's route
 * when the next maneuver painted none (as the single shared route texture did), and that a
 * frame paints one set, not two, while the next maneuver is invisible.
 */
#define PLATFORM_QNX 1

#include "render.c"                 /* -I <renderer tree>/maneuver_render */
#include "maneuver.h"

#include <stdarg.h>

/* ---------------------------------------------------------------- GL model */

static GLuint g_bound_fbo, g_bound_tex[8], g_active_unit, g_next_name = 1;
static float g_last_zbias, g_last_alpha;
#define NAMES_MAX 64
static const char *g_uniform_names[NAMES_MAX];
static int g_uniform_count;

typedef struct {
    int clears[FBO_COUNT];           /* per mask framebuffer */
    int mask_draws;                  /* draws into any mask framebuffer */
    int scene_draws;                 /* draws into the scene (SSAA) framebuffer */
    int window_draws;                /* draws into the window (framebuffer 0): pass 2 */
    int depth_resets;                /* render_reset_depth: a push's second composite follows */
    float next_alpha;                /* global alpha of that composite's first draw, -1 none */
    char scene[8192];                /* "tex:zbias " per scene draw */
} frame_rec_t;
static frame_rec_t R;

static int mask_index(GLuint fbo) {
    for (int i = 0; i < FBO_COUNT; i++) if (g_fbos[i] && g_fbos[i] == fbo) return i;
    return -1;
}

static const char *tex_name(GLuint tex) {
    static const char *names[FBO_COUNT] = { "road0", "route0", "road1", "route1" };
    for (int i = 0; i < FBO_COUNT; i++) if (g_fbo_texs[i] == tex) return names[i];
    return "other";
}

void glBindFramebuffer(GLenum target, GLuint fb) { (void)target; g_bound_fbo = fb; }
void glClear(GLbitfield mask) {
    int i = mask_index(g_bound_fbo);
    if (i >= 0) R.clears[i]++;
    else if (g_bound_fbo == g_ss_fbo && mask == GL_DEPTH_BUFFER_BIT) R.depth_resets++;
}
void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    (void)mode; (void)first; (void)count;
    if (mask_index(g_bound_fbo) >= 0) { R.mask_draws++; return; }
    if (g_bound_fbo == 0) { R.window_draws++; return; }
    if (g_bound_fbo != g_ss_fbo) return;
    R.scene_draws++;
    if (R.depth_resets == 1 && R.next_alpha < 0.0f) R.next_alpha = g_last_alpha;
    size_t n = strlen(R.scene);
    if (n + 48 < sizeof(R.scene))
        snprintf(R.scene + n, sizeof(R.scene) - n, "%s:%.6g ", tex_name(g_bound_tex[0]), g_last_zbias);
}
void glActiveTexture(GLenum unit) { g_active_unit = unit - GL_TEXTURE0; }
void glBindTexture(GLenum target, GLuint tex) { (void)target; if (g_active_unit < 8) g_bound_tex[g_active_unit] = tex; }

/* Uniform locations by name, so the depth bias can be told apart from other uniforms. */
GLint glGetUniformLocation(GLuint p, const char *n) {
    (void)p;
    for (int i = 0; i < g_uniform_count; i++) if (!strcmp(g_uniform_names[i], n)) return i;
    if (g_uniform_count == NAMES_MAX) return NAMES_MAX;
    g_uniform_names[g_uniform_count] = n;
    return g_uniform_count++;
}
void glUniform1f(GLint l, GLfloat a) {
    if (l >= 0 && l < g_uniform_count && !strcmp(g_uniform_names[l], "u_z_bias")) g_last_zbias = a;
    if (l >= 0 && l < g_uniform_count && !strcmp(g_uniform_names[l], "u_global_alpha")) g_last_alpha = a;
}

static void gen(GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = g_next_name++; }
void glGenTextures(GLsizei n, GLuint *t) { gen(n, t); }
void glGenFramebuffers(GLsizei n, GLuint *t) { gen(n, t); }
void glGenRenderbuffers(GLsizei n, GLuint *t) { gen(n, t); }
GLuint glCreateShader(GLenum type) { (void)type; return g_next_name++; }
GLuint glCreateProgram(void) { return g_next_name++; }
void glGetShaderiv(GLuint s, GLenum p, GLint *v) { (void)s; *v = p == GL_COMPILE_STATUS ? GL_TRUE : 0; }
void glGetProgramiv(GLuint s, GLenum p, GLint *v) { (void)s; *v = p == GL_LINK_STATUS ? GL_TRUE : 0; }
GLenum glCheckFramebufferStatus(GLenum t) { (void)t; return GL_FRAMEBUFFER_COMPLETE; }
void glGetIntegerv(GLenum p, GLint *v) { *v = (p == GL_MAX_TEXTURE_SIZE || p == GL_MAX_RENDERBUFFER_SIZE) ? 8192 : 0; }
GLenum glGetError(void) { return GL_NO_ERROR; }
#ifdef GL_PROGRAM_CACHE_H
const GLubyte *glGetString(GLenum name) { (void)name; return (const GLubyte *)"fake"; }
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) { (void)name; return NULL; }
#endif
GLint glGetAttribLocation(GLuint p, const char *n) { (void)p; (void)n; return 1; }
void glGetShaderInfoLog(GLuint s, GLsizei n, GLsizei *l, char *log) { (void)s; (void)n; if (l) *l = 0; if (log && n) log[0] = 0; }
void glGetProgramInfoLog(GLuint s, GLsizei n, GLsizei *l, char *log) { (void)s; (void)n; if (l) *l = 0; if (log && n) log[0] = 0; }
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *out) {
    (void)x; (void)y; (void)f; (void)t; memset(out, 0, (size_t)w * (size_t)h * 4);
}

/* State the mask cache does not depend on. */
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { (void)x; (void)y; (void)w; (void)h; }
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { (void)r; (void)g; (void)b; (void)a; }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { (void)r; (void)g; (void)b; (void)a; }
void glEnable(GLenum cap) { (void)cap; }
void glDisable(GLenum cap) { (void)cap; }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { (void)x; (void)y; (void)w; (void)h; }
void glAttachShader(GLuint p, GLuint s) { (void)p; (void)s; }
void glDetachShader(GLuint p, GLuint s) { (void)p; (void)s; }
void glBindAttribLocation(GLuint p, GLuint i, const char *n) { (void)p; (void)i; (void)n; }
void glBindRenderbuffer(GLenum t, GLuint r) { (void)t; (void)r; }
void glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { (void)a; (void)b; (void)c; (void)d; }
void glCompileShader(GLuint s) { (void)s; }
void glDeleteFramebuffers(GLsizei n, const GLuint *f) { (void)n; (void)f; }
void glDeleteProgram(GLuint p) { (void)p; }
void glDeleteRenderbuffers(GLsizei n, const GLuint *r) { (void)n; (void)r; }
void glDeleteShader(GLuint s) { (void)s; }
void glDeleteTextures(GLsizei n, const GLuint *t) { (void)n; (void)t; }
void glDepthFunc(GLenum f) { (void)f; }
void glDepthMask(GLboolean f) { (void)f; }
void glDisableVertexAttribArray(GLuint i) { (void)i; }
void glEnableVertexAttribArray(GLuint i) { (void)i; }
void glFramebufferRenderbuffer(GLenum a, GLenum b, GLenum c, GLuint d) { (void)a; (void)b; (void)c; (void)d; }
void glFramebufferTexture2D(GLenum a, GLenum b, GLenum c, GLuint d, GLint e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void glLinkProgram(GLuint p) { (void)p; }
void glRenderbufferStorage(GLenum a, GLenum b, GLsizei c, GLsizei d) { (void)a; (void)b; (void)c; (void)d; }
void glShaderSource(GLuint s, GLsizei c, const char **str, const GLint *l) { (void)s; (void)c; (void)str; (void)l; }
void glTexImage2D(GLenum a, GLint b, GLenum c, GLsizei d, GLsizei e, GLint f, GLenum g, GLenum h, const void *i) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i;
}
void glTexParameteri(GLenum a, GLenum b, GLint c) { (void)a; (void)b; (void)c; }
void glUniform1i(GLint l, GLint a) { (void)l; (void)a; }
void glUniform2f(GLint l, GLfloat a, GLfloat b) { (void)l; (void)a; (void)b; }
void glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { (void)l; (void)a; (void)b; (void)c; }
void glUniform3fv(GLint l, GLsizei n, const GLfloat *v) { (void)l; (void)n; (void)v; }
void glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { (void)l; (void)a; (void)b; (void)c; (void)d; }
void glUniform4fv(GLint l, GLsizei n, const GLfloat *v) { (void)l; (void)n; (void)v; }
void glUniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { (void)l; (void)n; (void)t; (void)v; }
void glUseProgram(GLuint p) { (void)p; }
void glVertexAttrib1f(GLuint i, GLfloat a) { (void)i; (void)a; }
void glVertexAttribPointer(GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st, const void *p) {
    (void)i; (void)s; (void)t; (void)n; (void)st; (void)p;
}

/* ---------------------------------------------------------------- checks */

static int g_checks, g_failures;

static void check(int ok, const char *fmt, ...) {
    va_list ap;
    g_checks++;
    if (ok) return;
    g_failures++;
    printf("FAIL: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* One loop iteration as main.c renders it. */
static void frame(const maneuver_state_t *cur, const maneuver_state_t *next) {
    memset(&R, 0, sizeof(R));
    R.next_alpha = -1.0f;
    maneuver_prepare_frame(cur, next);
    render_begin_frame();
    maneuver_draw(cur, next);
    render_end_frame();
}

static int clears(void) { int n = 0; for (int i = 0; i < FBO_COUNT; i++) n += R.clears[i]; return n; }

/* The composite draws of a frame: the scene draws that sample a mask texture. */
static void composites(char *out, size_t size) {
    const char *p = R.scene;
    out[0] = '\0';
    while (*p) {
        const char *end = strchr(p, ' ');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (strncmp(p, "other", 5) != 0 && strlen(out) + len + 2 < size) {
            strncat(out, p, len);
            strcat(out, " ");
        }
        p += len + (end ? 1 : 0);
    }
}

/* A push from one built-in maneuver to another: both mask sets painted once. */
static void push_paints_each_set_once(void) {
    maneuver_state_t cur, next;
    char first[1024], cached[1024], now[1024];
    int frames = 0;
    memset(&cur, 0, sizeof(cur));
    memset(&next, 0, sizeof(next));
    cur.icon = ICON_TURN;  cur.exit_angle = 90.0f;
    next.icon = ICON_TURN; next.exit_angle = -90.0f;

    maneuver_set_slide(1.0f);
    render_invalidate_masks();
    frame(&cur, NULL);
    check(R.clears[FBO_ROAD] == 1 && clears() == 1 && R.mask_draws > 0,
          "settled: set 0's road painted once (clears %d/%d/%d/%d, mask draws %d)",
          R.clears[0], R.clears[1], R.clears[2], R.clears[3], R.mask_draws);
    frame(&cur, NULL);
    check(clears() == 0 && R.mask_draws == 0, "settled, cached: nothing painted (%d clears, %d draws)",
          clears(), R.mask_draws);

    maneuver_start_push();
    render_invalidate_next_masks();                /* engine_apply_maneuver */
    frame(&cur, &next);
    check(R.clears[FBO_ROAD_NEXT] == 1 && clears() == 1 && R.mask_draws > 0,
          "push start: the next road painted once, set 0 kept (clears %d/%d/%d/%d)",
          R.clears[0], R.clears[1], R.clears[2], R.clears[3]);
    composites(first, sizeof(first));
    check(strstr(first, "road0:") && strstr(first, "road1:") && strstr(first, "road0:") < strstr(first, "road1:"),
          "push start: current fades out from set 0, next fades in from set 1: %s", first);
    int painted_scene_draws = R.scene_draws;

    frame(&cur, &next);
    composites(cached, sizeof(cached));
    check(clears() == 0 && R.mask_draws == 0, "push, cached: nothing painted (%d clears, %d draws)",
          clears(), R.mask_draws);
    check(!strcmp(first, cached), "push, cached: same composites and depth bias as when painted\n  painted %s\n  cached  %s",
          first, cached);
    check(R.scene_draws == painted_scene_draws, "push, cached: no extra scene draws (%d, painted %d)",
          R.scene_draws, painted_scene_draws);

    render_invalidate_next_masks();                /* engine_refresh_maneuver of the next */
    frame(&cur, &next);
    check(R.clears[FBO_ROAD_NEXT] == 1 && clears() == 1,
          "refresh of the next mid-push: set 1 repainted (clears %d/%d/%d/%d)",
          R.clears[0], R.clears[1], R.clears[2], R.clears[3]);

    int repaints = 0;
    while (maneuver_is_pushing() && frames < 2000) {
        frame(&cur, &next);
        repaints += clears() + R.mask_draws;
        frames++;
    }
    check(!maneuver_is_pushing() && frames > 10, "the push ran to its end (%d frames)", frames);
    check(repaints == 0, "no mask work in the rest of the push (%d clears + draws)", repaints);
    composites(now, sizeof(now));
    check(strstr(now, "road1:") != NULL, "the last push frame still composites set 1: %s", now);

    maneuver_commit_pushed_state(&next);           /* engine_tick */
    render_invalidate_masks();
    frame(&next, NULL);
    composites(now, sizeof(now));
    check(R.clears[FBO_ROAD] == 1 && clears() == 1, "commit: set 0 repainted with the new current (clears %d/%d/%d/%d)",
          R.clears[0], R.clears[1], R.clears[2], R.clears[3]);
    check(strstr(now, "road0:") && !strstr(now, "road1:") && !strstr(now, "route1:"),
          "settled after the push: set 0 only: %s", now);
}

/* A supplied scene, as on the car: road and route layers under the push transform, like
 * cr_scene_paint.  A state with direction 1 paints no route layer. */
static int g_scene_paints;
static void scene_route(void *ctx, const maneuver_state_t *st, route_path_t *path) {
    (void)ctx;
    maneuver_build_route(st, path);
}
static void scene_paint(void *ctx, const maneuver_state_t *st, float tx, float ty, float c, float sn) {
    (void)ctx;
    g_scene_paints++;
    render_push_mask_transform(tx, ty, c, sn);
    render_begin_outline_mask(); render_triangle(0, 0, 1, 0, 0, 1, 1, 1, 1, 1); render_end_outline_mask();
    if (st->direction != 1) {
        render_begin_route_mask(); render_triangle(0, 0, 1, 0, 0, 1, 0, 0, 1, 1); render_end_outline_mask();
    }
    render_pop_mask_transform();
}
static int scene_handles(void *ctx, const maneuver_state_t *st) { (void)ctx; (void)st; return 1; }

/* A push between supplied scenes paints each set's road and route once; the next maneuver's
 * route goes to set 1, or set 1 composites set 0's route when the next paints none. */
static void scene_push_paints_route_layers_once(int next_has_route) {
    maneuver_scene_provider_t provider;
    maneuver_state_t cur, next;
    char first[1024], cached[1024];
    const char *what = next_has_route ? "scene push" : "scene push, next without route";
    int frames = 0, repaints = 0;
    memset(&provider, 0, sizeof(provider));
    provider.build_route = scene_route;
    provider.paint_masks = scene_paint;
    provider.handles = scene_handles;
    provider.route_base_y = 0.0f;
    provider.route_top_y = 0.02f;
    provider.entry_road_reach = provider.exit_road_reach = 0.35f;
    maneuver_set_scene_provider(&provider);
    memset(&cur, 0, sizeof(cur));
    memset(&next, 0, sizeof(next));
    cur.icon = ICON_TURN;  cur.exit_angle = 90.0f;
    next.icon = ICON_TURN; next.exit_angle = -90.0f; next.direction = next_has_route ? 0 : 1;

    maneuver_set_slide(1.0f);
    render_invalidate_masks();
    frame(&cur, NULL);
    check(R.clears[FBO_ROAD] == 1 && R.clears[FBO_ROUTE] == 1 && clears() == 2,
          "%s, settled: set 0's road and route painted (clears %d/%d/%d/%d)",
          what, R.clears[0], R.clears[1], R.clears[2], R.clears[3]);

    maneuver_start_push();
    render_invalidate_next_masks();
    frame(&cur, &next);
    check(R.clears[FBO_ROAD] == 0 && R.clears[FBO_ROUTE] == 0 && R.clears[FBO_ROAD_NEXT] == 1
          && R.clears[FBO_ROUTE_NEXT] == (next_has_route ? 1 : 0),
          "%s start: the next scene's layers painted once into set 1 (clears %d/%d/%d/%d)",
          what, R.clears[0], R.clears[1], R.clears[2], R.clears[3]);
    composites(first, sizeof(first));
    {
        const char *road1 = strstr(first, "road1:");
        check(road1 && strstr(first, "route0:") && strstr(first, "route0:") < road1
              && (next_has_route ? strstr(road1, "route1:") != NULL
                                 : strstr(road1, "route0:") != NULL && !strstr(first, "route1:")),
              "%s start: set 0 road+route, then set 1 road and %s route: %s",
              what, next_has_route ? "its own" : "set 0's", first);
    }

    g_scene_paints = 0;
    frame(&cur, &next);
    composites(cached, sizeof(cached));
    check(clears() == 0 && R.mask_draws == 0 && g_scene_paints == 2,
          "%s, cached: both scenes replayed (%d paints) with no mask work (%d clears, %d draws)",
          what, g_scene_paints, clears(), R.mask_draws);
    check(!strcmp(first, cached), "%s, cached: same composites and depth bias\n  painted %s\n  cached  %s",
          what, first, cached);

    render_invalidate_next_masks();                /* the next scene rebuilt mid-push */
    frame(&cur, &next);
    check(R.clears[FBO_ROAD_NEXT] == 1 && R.clears[FBO_ROUTE_NEXT] == (next_has_route ? 1 : 0)
          && R.clears[FBO_ROAD] == 0 && R.clears[FBO_ROUTE] == 0,
          "%s, next rebuilt: set 1 repainted (clears %d/%d/%d/%d)",
          what, R.clears[0], R.clears[1], R.clears[2], R.clears[3]);

    while (maneuver_is_pushing() && frames < 2000) {
        frame(&cur, &next);
        repaints += clears() + R.mask_draws;
        frames++;
    }
    check(!maneuver_is_pushing() && repaints == 0, "%s: no mask work for the rest (%d frames, %d)",
          what, frames, repaints);
    maneuver_commit_pushed_state(&next);
    maneuver_set_scene_provider(NULL);             /* invalidates, back to built-ins */
}

/* With both sets due while the next maneuver is invisible (a commit and a promotion in one
 * tick, or the current maneuver refreshed mid-push), set 0 is painted first and set 1 in the
 * next frame.  Meanwhile the push's second composite shows set 0's layers at alpha 0, at the
 * same depth bias.  Once the next maneuver is visible, a frame paints both sets it needs. */
static void push_spreads_due_sets_over_frames(void) {
    maneuver_state_t cur, next;
    char held[1024], painted[1024], *p;
    int frames = 0, visible = 0, held_ok = 1;
    memset(&cur, 0, sizeof(cur));
    memset(&next, 0, sizeof(next));
    cur.icon = ICON_TURN;  cur.exit_angle = 90.0f;
    next.icon = ICON_TURN; next.exit_angle = -90.0f;

    maneuver_set_slide(1.0f);
    render_invalidate_masks();
    frame(&cur, NULL);
    maneuver_start_push();
    render_invalidate_masks();                     /* engine_tick: commit, then promote */
    frame(&cur, &next);
    composites(held, sizeof(held));
    check(R.next_alpha == 0.0f && R.clears[FBO_ROAD] == 1 && R.clears[FBO_ROAD_NEXT] == 0 && clears() == 1,
          "both due, next invisible: set 0 painted, set 1 held (clears %d/%d/%d/%d, next alpha %g)",
          R.clears[0], R.clears[1], R.clears[2], R.clears[3], R.next_alpha);
    check(strstr(held, "road0:") && !strstr(held, "road1:"), "held: set 0's road in set 1's place: %s", held);
    frame(&cur, &next);
    composites(painted, sizeof(painted));
    check(R.clears[FBO_ROAD_NEXT] == 1 && clears() == 1, "the next frame paints set 1 (clears %d/%d/%d/%d)",
          R.clears[0], R.clears[1], R.clears[2], R.clears[3]);
    while ((p = strstr(painted, "road1:")) != NULL) p[4] = '0';
    check(!strcmp(held, painted), "held: same composites and depth bias as once painted, set 0's texture\n"
          "  held    %s\n  painted %s", held, painted);
    frame(&cur, &next);
    check(clears() == 0 && R.mask_draws == 0, "then no mask work (%d clears, %d draws)", clears(), R.mask_draws);

    while (maneuver_is_pushing() && frames < 2000 && !visible) {
        render_invalidate_masks();                 /* the current maneuver refreshed mid-push */
        frame(&cur, &next);
        frames++;
        if (R.next_alpha > 0.0f) {
            visible = 1;
            check(R.clears[FBO_ROAD] == 1 && R.clears[FBO_ROAD_NEXT] == 1,
                  "refresh with the next visible (alpha %g): both sets painted (clears %d/%d/%d/%d)",
                  R.next_alpha, R.clears[0], R.clears[1], R.clears[2], R.clears[3]);
        } else if (R.clears[FBO_ROAD] != 1 || R.clears[FBO_ROAD_NEXT] != 0) {
            held_ok = 0;
        }
    }
    check(held_ok, "every refresh while the next was invisible painted set 0 only");
    check(visible && frames > 1, "the next maneuver became visible during the push (%d frames)", frames);
    while (maneuver_is_pushing() && frames < 4000) { frame(&cur, &next); frames++; }
    maneuver_commit_pushed_state(&next);
}

/* A hold lasts until the next selection and never past the frame. */
static void hold_ends_with_selection_and_frame(void) {
    render_invalidate_masks();
    render_begin_frame();
    memset(&R, 0, sizeof(R));
    render_select_mask_set(1);
    render_hold_mask_set();
    render_begin_outline_mask(); render_triangle(0, 0, 1, 0, 0, 1, 1, 1, 1, 1); render_end_outline_mask();
    render_composite();
    render_select_mask_set(0);
    render_begin_outline_mask(); render_triangle(0, 0, 1, 0, 0, 1, 1, 1, 1, 1); render_end_outline_mask();
    render_composite();
    check(R.clears[FBO_ROAD_NEXT] == 0 && R.clears[FBO_ROAD] == 1,
          "a hold ends with its selection: set 1 held, set 0 then painted (clears %d/%d/%d/%d)",
          R.clears[0], R.clears[1], R.clears[2], R.clears[3]);
    render_select_mask_set(1);
    render_hold_mask_set();
    render_end_frame();
    render_begin_frame();
    memset(&R, 0, sizeof(R));
    render_begin_outline_mask(); render_triangle(0, 0, 1, 0, 0, 1, 1, 1, 1, 1); render_end_outline_mask();
    render_composite();
    render_select_mask_set(0);
    check(R.clears[FBO_ROAD_NEXT] == 1, "a hold ends with the frame: the held set is painted in the next (clears %d/%d/%d/%d)",
          R.clears[0], R.clears[1], R.clears[2], R.clears[3]);
    render_end_frame();
}

/* Set 1 without a route layer composites set 0's route, as the one shared texture did. */
static void next_without_route_uses_current_route(void) {
    char c[1024];
    render_invalidate_masks();
    render_select_mask_set(0);
    render_begin_frame();
    memset(&R, 0, sizeof(R));
    render_begin_outline_mask(); render_triangle(0, 0, 1, 0, 0, 1, 1, 1, 1, 1); render_end_outline_mask();
    render_begin_route_mask();   render_triangle(0, 0, 1, 0, 0, 1, 0, 0, 1, 1); render_end_outline_mask();
    render_composite();
    render_select_mask_set(1);
    render_begin_outline_mask(); render_triangle(0, 0, 1, 0, 0, 1, 1, 1, 1, 1); render_end_outline_mask();
    render_composite();
    render_select_mask_set(0);
    composites(c, sizeof(c));
    check(R.clears[FBO_ROAD] == 1 && R.clears[FBO_ROUTE] == 1 && R.clears[FBO_ROAD_NEXT] == 1
          && R.clears[FBO_ROUTE_NEXT] == 0, "painted: road+route in set 0, road in set 1");
    check(strstr(c, "road0:") && strstr(c, "route0:") && strstr(c, "road1:")
          && strstr(strstr(c, "road1:"), "route0:") && !strstr(c, "route1:"),
          "set 1 composites its road and set 0's route: %s", c);
    render_end_frame();
}

/* A clean-set painting that never reaches its end_mask must not swallow later flat draws:
 * the next frame's overlays (the lane panel) still reach the scene. */
static void unfinished_painting_does_not_hide_overlays(void) {
    static const float tri[] = { 0, 0, 10, 0, 0, 10 };
    cr_rect_t clip = { 0, 0, 100, 50 };
    render_invalidate_masks();
    render_select_mask_set(0);
    render_begin_frame();
    render_begin_outline_mask(); render_triangle(0, 0, 1, 0, 0, 1, 1, 1, 1, 1); render_end_outline_mask();
    render_composite();                            /* set 0 is clean now */
    render_end_frame();
    render_begin_frame();
    render_begin_outline_mask();                   /* replay on the clean set, no end_mask */
    render_triangle(0, 0, 1, 0, 0, 1, 1, 1, 1, 1);
    memset(&R, 0, sizeof(R));
    render_begin_overlay(clip);
    render_overlay_mesh(tri, 3, 0, 0, 1, 1, 1, 1);
    render_end_overlay();
    check(R.scene_draws == 1, "an overlay after an unfinished clean-set painting is drawn (%d draws)", R.scene_draws);
    memset(&R, 0, sizeof(R));
    render_begin_outline_mask();                   /* again unfinished, then the frame ends */
    render_end_frame();
    check(R.window_draws == 1, "pass 2 still copies the frame to the window (%d draws)", R.window_draws);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (render_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) != 0) {
        printf("FAIL: render_init\n");
        return 1;
    }
    check(g_fbos[FBO_ROAD_NEXT] != 0 && g_fbo_texs[FBO_ROUTE_NEXT] != 0, "init: four mask framebuffers");
    push_paints_each_set_once();
    scene_push_paints_route_layers_once(1);
    scene_push_paints_route_layers_once(0);
    push_spreads_due_sets_over_frames();
    hold_ends_with_selection_and_frame();
    next_without_route_uses_current_route();
    unfinished_painting_does_not_hide_overlays();
    render_shutdown();
    printf("most_mask_cache_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
