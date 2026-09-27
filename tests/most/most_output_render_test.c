/*
 * MOST output window: host test of the real render.c final pass (Pass 2) against a
 * GLES2 fake that models the state Pass 2 depends on (bound framebuffer, viewport, clear
 * colour, colour mask, scissor, blend, bound texture) and the window's pixels.
 *
 * Checks that render_set_output() presents the frame into the fitted rectangle of a larger
 * window with every other pixel opaque black and every pixel's alpha 1 (the MOST encoder
 * streams the buffer as-is), that the GL state left for the next frame is unchanged, and
 * that without render_set_output() (Virtual Cockpit) Pass 2 is the upstream sequence.
 */
#define PLATFORM_QNX 1

#include "render.c"                 /* -I <renderer tree>/maneuver_render */

#include <stdarg.h>
#include <unistd.h>

/* ---------------------------------------------------------------- GL model */

#define WIN_MAX_W 2048
#define WIN_MAX_H 2048

typedef struct { float r, g, b, a; } px_t;

static px_t *g_px;                  /* window (framebuffer 0) pixels, bottom-left origin */
static int g_px_w, g_px_h;
static GLuint g_bound_fbo, g_bound_tex[8], g_active_unit;
static GLint g_vp[4];
static GLfloat g_cc[4];
static GLboolean g_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
static int g_scissor_on, g_blend_on, g_depth_on;
static GLint g_scissor[4];
static int g_draws_to_window, g_clears_to_window;
static GLuint g_next_name = 1;
/* Allocation sizes by object name, and injected failures for the render-target fallback. */
#define NAMES_MAX 4096
static int g_tex_w[NAMES_MAX], g_tex_h[NAMES_MAX], g_rb_w[NAMES_MAX], g_rb_h[NAMES_MAX];
static GLuint g_bound_rb;
static int g_fail_alloc_wider_than;    /* allocations wider than this: GL_OUT_OF_MEMORY; 0 = none */
static int g_incomplete_at_ss_w;       /* framebuffers incomplete while g_ss_w == this; 0 = none */
static GLint g_gl_limit = 4096;        /* GL_MAX_TEXTURE_SIZE and GL_MAX_RENDERBUFFER_SIZE */
static GLenum g_pending_error = GL_NO_ERROR;
static int g_allocs, g_widest_alloc;   /* since the test last cleared them */
static int g_draw_tex_w, g_draw_tex_h; /* size of the texture Pass 2 last drew to the window */
static char g_log[16384];

static void logf_(const char *fmt, ...) {
    va_list ap;
    size_t n = strlen(g_log);
    if (n + 128 >= sizeof(g_log)) return;
    va_start(ap, fmt);
    vsnprintf(g_log + n, sizeof(g_log) - n, fmt, ap);
    va_end(ap);
}

/* A texture's "content" is a colour derived from its name: premultiplied, part transparent. */
static px_t texture_colour(GLuint tex) {
    px_t c = { 0.1f * (float)(tex % 7), 0.2f, 0.3f + 0.05f * (float)(tex % 5), 0.5f };
    return c;
}

static void window_alloc(int w, int h) {
    free(g_px);
    g_px = (px_t *)calloc((size_t)w * (size_t)h, sizeof(px_t));
    g_px_w = w;
    g_px_h = h;
    /* Garbage from a previous buffer: Pass 2 must overwrite every pixel it streams. */
    for (int i = 0; i < w * h; i++) { g_px[i].r = 0.9f; g_px[i].g = 0.1f; g_px[i].b = 0.9f; g_px[i].a = 0.3f; }
}

static void write_px(int x, int y, px_t c) {
    px_t *p = &g_px[y * g_px_w + x];
    if (g_mask[0]) p->r = c.r;
    if (g_mask[1]) p->g = c.g;
    if (g_mask[2]) p->b = c.b;
    if (g_mask[3]) p->a = c.a;
}

static int clip(int *x0, int *y0, int *x1, int *y1) {
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > g_px_w) *x1 = g_px_w;
    if (*y1 > g_px_h) *y1 = g_px_h;
    if (g_scissor_on) {
        if (*x0 < g_scissor[0]) *x0 = g_scissor[0];
        if (*y0 < g_scissor[1]) *y0 = g_scissor[1];
        if (*x1 > g_scissor[0] + g_scissor[2]) *x1 = g_scissor[0] + g_scissor[2];
        if (*y1 > g_scissor[1] + g_scissor[3]) *y1 = g_scissor[1] + g_scissor[3];
    }
    return *x0 < *x1 && *y0 < *y1;
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    g_vp[0] = x; g_vp[1] = y; g_vp[2] = w; g_vp[3] = h;
    if (g_bound_fbo == 0) logf_("Viewport(%d,%d,%d,%d) ", x, y, w, h);
}
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
    g_cc[0] = r; g_cc[1] = g; g_cc[2] = b; g_cc[3] = a;
    if (g_bound_fbo == 0) logf_("ClearColor(%g,%g,%g,%g) ", r, g, b, a);
}
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    g_mask[0] = r; g_mask[1] = g; g_mask[2] = b; g_mask[3] = a;
    logf_("ColorMask(%d,%d,%d,%d) ", r, g, b, a);
}
void glClear(GLbitfield mask) {
    if (g_bound_fbo != 0) return;
    logf_("Clear(%s%s) ", (mask & GL_COLOR_BUFFER_BIT) ? "C" : "", (mask & GL_DEPTH_BUFFER_BIT) ? "D" : "");
    g_clears_to_window++;
    if (!(mask & GL_COLOR_BUFFER_BIT)) return;
    /* glClear ignores the viewport; only the scissor and colour mask apply. */
    int x0 = 0, y0 = 0, x1 = g_px_w, y1 = g_px_h;
    if (!clip(&x0, &y0, &x1, &y1)) return;
    px_t c = { g_cc[0], g_cc[1], g_cc[2], g_cc[3] };
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) write_px(x, y, c);
}
void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (g_bound_fbo != 0) return;
    logf_("DrawArrays(%d,%d,%d tex=%u) ", (int)mode, first, count, g_bound_tex[0]);
    g_draws_to_window++;
    if (g_bound_tex[0] < NAMES_MAX) { g_draw_tex_w = g_tex_w[g_bound_tex[0]]; g_draw_tex_h = g_tex_h[g_bound_tex[0]]; }
    /* Pass 2 draws one full-NDC quad: it covers exactly the viewport. */
    int x0 = g_vp[0], y0 = g_vp[1], x1 = g_vp[0] + g_vp[2], y1 = g_vp[1] + g_vp[3];
    if (!clip(&x0, &y0, &x1, &y1)) return;
    px_t c = texture_colour(g_bound_tex[0]);
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) write_px(x, y, c);
}
void glBindFramebuffer(GLenum target, GLuint fb) {
    (void)target;
    g_bound_fbo = fb;
    if (fb == 0) logf_("| BindFramebuffer(0) ");
}
void glEnable(GLenum cap) {
    if (cap == GL_SCISSOR_TEST) g_scissor_on = 1;
    if (cap == GL_BLEND) g_blend_on = 1;
    if (cap == GL_DEPTH_TEST) g_depth_on = 1;
}
void glDisable(GLenum cap) {
    if (cap == GL_SCISSOR_TEST) g_scissor_on = 0;
    if (cap == GL_BLEND) g_blend_on = 0;
    if (cap == GL_DEPTH_TEST) g_depth_on = 0;
}
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { g_scissor[0] = x; g_scissor[1] = y; g_scissor[2] = w; g_scissor[3] = h; }
void glActiveTexture(GLenum unit) { g_active_unit = unit - GL_TEXTURE0; }
void glBindTexture(GLenum target, GLuint tex) { (void)target; if (g_active_unit < 8) g_bound_tex[g_active_unit] = tex; }

/* Object creation / queries: every object is valid, every shader compiles. */
static void gen(GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = g_next_name++; }
void glGenTextures(GLsizei n, GLuint *t) { gen(n, t); }
void glGenFramebuffers(GLsizei n, GLuint *t) { gen(n, t); }
void glGenRenderbuffers(GLsizei n, GLuint *t) { gen(n, t); }
GLuint glCreateShader(GLenum type) { (void)type; return g_next_name++; }
GLuint glCreateProgram(void) { return g_next_name++; }
void glGetShaderiv(GLuint s, GLenum p, GLint *v) { (void)s; *v = p == GL_COMPILE_STATUS ? GL_TRUE : 0; }
void glGetProgramiv(GLuint s, GLenum p, GLint *v) { (void)s; *v = p == GL_LINK_STATUS ? GL_TRUE : 0; }
GLenum glCheckFramebufferStatus(GLenum t) {
    (void)t;
    return g_incomplete_at_ss_w && g_ss_w == g_incomplete_at_ss_w ? GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT
                                                                : GL_FRAMEBUFFER_COMPLETE;
}
void glGetIntegerv(GLenum p, GLint *v) {
    *v = (p == GL_MAX_TEXTURE_SIZE || p == GL_MAX_RENDERBUFFER_SIZE) ? g_gl_limit : 0;
}
GLenum glGetError(void) { GLenum e = g_pending_error; g_pending_error = GL_NO_ERROR; return e; }
/* An allocation too wide fails as a driver out of memory does: error, storage unchanged. */
static int alloc_ok(GLsizei w, GLsizei h) {
    (void)h;
    g_allocs++;
    if (w > g_widest_alloc) g_widest_alloc = w;
    if (g_fail_alloc_wider_than && w > g_fail_alloc_wider_than) { g_pending_error = GL_OUT_OF_MEMORY; return 0; }
    return 1;
}
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *out) {
    unsigned char *o = (unsigned char *)out;
    (void)f; (void)t;
    for (int r = 0; r < h; r++) for (int c = 0; c < w; c++) {
        px_t p = g_px[(y + r) * g_px_w + x + c];
        float ch[4] = { p.r, p.g, p.b, p.a };
        for (int k = 0; k < 4; k++) o[((size_t)r * w + c) * 4 + k] = (unsigned char)lrintf(fminf(1.0f, fmaxf(0.0f, ch[k])) * 255.0f);
    }
}
#ifdef GL_PROGRAM_CACHE_H
const GLubyte *glGetString(GLenum name) { (void)name; return (const GLubyte *)"fake"; }
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) {
    (void)name;
    return NULL;                      /* this test does not exercise the shader cache */
}
#endif
GLint glGetAttribLocation(GLuint p, const char *n) { (void)p; (void)n; return 1; }
GLint glGetUniformLocation(GLuint p, const char *n) { (void)p; (void)n; return 1; }
void glGetShaderInfoLog(GLuint s, GLsizei n, GLsizei *l, char *log) { (void)s; (void)n; if (l) *l = 0; if (log && n) log[0] = 0; }
void glGetProgramInfoLog(GLuint s, GLsizei n, GLsizei *l, char *log) { (void)s; (void)n; if (l) *l = 0; if (log && n) log[0] = 0; }

/* State Pass 2 does not depend on. */
void glAttachShader(GLuint p, GLuint s) { (void)p; (void)s; }
void glDetachShader(GLuint p, GLuint s) { (void)p; (void)s; }
void glBindAttribLocation(GLuint p, GLuint i, const char *n) { (void)p; (void)i; (void)n; }
void glBindRenderbuffer(GLenum t, GLuint r) { (void)t; g_bound_rb = r; }
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
void glRenderbufferStorage(GLenum a, GLenum b, GLsizei c, GLsizei d) {
    (void)a; (void)b;
    if (alloc_ok(c, d) && g_bound_rb < NAMES_MAX) { g_rb_w[g_bound_rb] = c; g_rb_h[g_bound_rb] = d; }
}
void glShaderSource(GLuint s, GLsizei c, const char **str, const GLint *l) { (void)s; (void)c; (void)str; (void)l; }
void glTexImage2D(GLenum a, GLint b, GLenum c, GLsizei d, GLsizei e, GLint f, GLenum g, GLenum h, const void *i) {
    GLuint tex = g_active_unit < 8 ? g_bound_tex[g_active_unit] : 0;
    (void)a; (void)b; (void)c; (void)f; (void)g; (void)h; (void)i;
    if (alloc_ok(d, e) && tex < NAMES_MAX) { g_tex_w[tex] = d; g_tex_h[tex] = e; }
}
void glTexParameteri(GLenum a, GLenum b, GLint c) { (void)a; (void)b; (void)c; }
void glUniform1f(GLint l, GLfloat a) { (void)l; (void)a; }
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

/* maneuver.c: render_init sizes its masks from this envelope; any positive one will do. */
void maneuver_get_transition_mask_bounds(float *x, float *y) { *x = 2.0f; *y = 2.0f; }

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

static int same(float a, float b) { return fabsf(a - b) < 1e-6f; }
static int px_is(px_t p, px_t e) { return same(p.r, e.r) && same(p.g, e.g) && same(p.b, e.b) && same(p.a, e.a); }

static void frame(void) {
    g_log[0] = '\0';
    g_draws_to_window = g_clears_to_window = 0;
    render_begin_frame();
    render_end_frame();
}

/* Pass 2's GL calls: the log from the last BindFramebuffer(0). */
static const char *pass2(void) {
    const char *p = strstr(g_log, "| BindFramebuffer(0) ");
    return p ? p : "";
}

static void expect_state_for_next_frame(const char *what) {
    check(g_mask[0] && g_mask[1] && g_mask[2] && g_mask[3], "%s: colour mask restored", what);
    check(g_blend_on && g_depth_on, "%s: blend and depth test re-enabled", what);
    check(!g_scissor_on, "%s: scissor off", what);
    /* The next frame renders into the SSAA FBO at its own viewport. */
    g_log[0] = '\0';
    render_begin_frame();
    check(g_bound_fbo == g_ss_fbo && g_vp[0] == 0 && g_vp[1] == 0 && g_vp[2] == g_ss_w && g_vp[3] == g_ss_h,
          "%s: next frame viewport %d,%d %dx%d on fbo %u", what, g_vp[0], g_vp[1], g_vp[2], g_vp[3], g_bound_fbo);
    render_end_frame();
}

/* Mask content size on a MOST output: MASK_MIN_SCALE (1.6) times the content. */
static int most_mask(int n) { return (int)(n * 1.6f + 0.5f); }

/* Every render target for content w x h: the frame (colour, depth and FXAA) at sw x sh, and
 * the transition masks at their envelope around an mw x mh content. */
static void expect_render_size(const char *what, int w, int h, int sw, int sh, int mw, int mh) {
    int rw = 0, rh = 0, i;
    render_get_render_size(&rw, &rh);
    check(rw == w && rh == h, "%s: renders %dx%d, expected %dx%d", what, rw, rh, w, h);
    check(g_tex_w[g_ss_tex] == sw && g_tex_h[g_ss_tex] == sh, "%s: SSAA colour %dx%d, expected %dx%d",
          what, g_tex_w[g_ss_tex], g_tex_h[g_ss_tex], sw, sh);
    check(g_rb_w[g_ss_depth] == sw && g_rb_h[g_ss_depth] == sh, "%s: SSAA depth %dx%d",
          what, g_rb_w[g_ss_depth], g_rb_h[g_ss_depth]);
    check(g_tex_w[g_fxaa_tex] == sw && g_tex_h[g_fxaa_tex] == sh, "%s: FXAA %dx%d",
          what, g_tex_w[g_fxaa_tex], g_tex_h[g_fxaa_tex]);
    for (i = 0; i < FBO_COUNT; i++) {
        check(g_tex_w[g_fbo_texs[i]] == mask_extent(mw) && g_tex_h[g_fbo_texs[i]] == mask_extent(mh),
              "%s: mask %d %dx%d, expected %dx%d", what, i, g_tex_w[g_fbo_texs[i]], g_tex_h[g_fbo_texs[i]],
              mask_extent(mw), mask_extent(mh));
    }
}

/* Virtual Cockpit: no render_set_output -> Pass 2 is the upstream sequence, window-filling,
 * transparent where the content is. */
static void vc_pass2_is_upstream(void) {
    window_alloc(328, 181);
    frame();
    char expected[512];
    snprintf(expected, sizeof(expected),
             "| BindFramebuffer(0) Viewport(0,0,328,181) ClearColor(0,0,0,0) Clear(CD) DrawArrays(4,0,6 tex=%u) ",
             g_fxaa_tex);
    check(strcmp(pass2(), expected) == 0, "vc: pass 2\n  got      %s\n  expected %s", pass2(), expected);
    px_t content = texture_colour(g_fxaa_tex);
    int bad = 0;
    for (int i = 0; i < 328 * 181; i++) bad += !px_is(g_px[i], content);
    check(bad == 0, "vc: %d window pixels are not the content (alpha kept for the VC compositor)", bad);
    expect_render_size("vc", 328, 181, 525, 290, 525, 290);
    check(g_draw_tex_w == 525 && g_draw_tex_h == 290, "vc: pass 2 resolves a %dx%d frame, expected 525x290",
          g_draw_tex_w, g_draw_tex_h);
    expect_state_for_next_frame("vc");
}

static void most_pass2(int ww, int wh, int x, int y, int w, int h) {
    char what[64];
    snprintf(what, sizeof(what), "most %dx%d", ww, wh);
    window_alloc(ww, wh);
    render_set_output(ww, wh, x, y, w, h, 1);
    /* The scene renders at the content size itself, not supersampled: Pass 2 is a 1:1 copy.
     * The masks keep 1.6x density from the content size. */
    expect_render_size(what, w, h, w, h, most_mask(w), most_mask(h));
    check(render_set_output(ww, wh, x, y, w, h, 1) == 0, "%s: an unchanged output keeps the targets", what);
    frame();
    check(g_draw_tex_w == w && g_draw_tex_h == h, "%s: pass 2 copies a %dx%d frame into %dx%d",
          what, g_draw_tex_w, g_draw_tex_h, w, h);
    char expected[512];
    snprintf(expected, sizeof(expected),
             "| BindFramebuffer(0) Viewport(0,0,%d,%d) ClearColor(0,0,0,1) Clear(CD) Viewport(%d,%d,%d,%d) "
             "DrawArrays(4,0,6 tex=%u) Viewport(0,0,%d,%d) ColorMask(0,0,0,1) ClearColor(0,0,0,1) Clear(C) "
             "ColorMask(1,1,1,1) ",
             ww, wh, x, y, w, h, g_fxaa_tex, ww, wh);
    check(strcmp(pass2(), expected) == 0, "%s: pass 2\n  got      %s\n  expected %s", what, pass2(), expected);
    check(g_draws_to_window == 1, "%s: one blit to the window", what);

    px_t content = texture_colour(g_fxaa_tex);
    px_t inside = content;
    inside.a = 1.0f;
    px_t black = {0, 0, 0, 1};
    int bad_inside = 0, bad_outside = 0, not_opaque = 0;
    for (int py = 0; py < wh; py++) {
        for (int px = 0; px < ww; px++) {
            px_t p = g_px[py * ww + px];
            int in = px >= x && px < x + w && py >= y && py < y + h;
            if (in) bad_inside += !px_is(p, inside);
            else bad_outside += !px_is(p, black);
            not_opaque += !same(p.a, 1.0f);
        }
    }
    check(bad_inside == 0, "%s: %d pixels inside (%d,%d %dx%d) are not the content", what, bad_inside, x, y, w, h);
    check(bad_outside == 0, "%s: %d pixels outside the content are not opaque black", what, bad_outside);
    check(not_opaque == 0, "%s: %d pixels with alpha != 1", what, not_opaque);
    expect_state_for_next_frame(what);
}

/* A scissor left enabled by an overlay pass would clip Pass 2's clears. */
static void overlay_scissor_is_closed(void) {
    window_alloc(800, 252);
    render_set_output(800, 252, 171, 0, 457, 252, 1);
    g_log[0] = '\0';
    render_begin_frame();
    cr_rect_t clip = {0, 0, 100, 50};
    render_begin_overlay(clip);
    render_end_overlay();
    render_end_frame();
    int not_opaque = 0;
    for (int i = 0; i < 800 * 252; i++) not_opaque += !same(g_px[i].a, 1.0f);
    check(not_opaque == 0, "overlay: %d pixels with alpha != 1 after an overlay pass", not_opaque);
}

static void reset_refusals(void) {
    g_refused_w = g_refused_h = 0;
}

/* Pass 2 into (171,0 457x252) of an 800x252 window: content where it should be, black and
 * opaque elsewhere, whatever size the frame was rendered at. */
static void expect_kvs_most_picture(const char *what) {
    px_t inside = texture_colour(g_fxaa_tex), black = {0, 0, 0, 1};
    int bad = 0;
    inside.a = 1.0f;
    window_alloc(800, 252);
    frame();
    for (int py = 0; py < 252; py++)
        for (int px = 0; px < 800; px++)
            bad += !px_is(g_px[py * 800 + px], px >= 171 && px < 171 + 457 ? inside : black);
    check(bad == 0, "%s: %d window pixels wrong", what, bad);
}

/* A native size whose masks cannot be allocated falls back to today's 328x181 frame,
 * resampled into the output as before, and is not tried again. */
static void fallback_on_out_of_memory(void) {
    const char *what = "out of memory";
    render_set_output(0, 0, 0, 0, 0, 0, 0);
    reset_refusals();
    g_fail_alloc_wider_than = 1200;           /* 457x252's masks (1462 wide) fail; 328x181's (1050) fit */
    check(render_set_output(800, 252, 171, 0, 457, 252, 1) == 1, "%s: targets reallocated", what);
    expect_render_size(what, 328, 181, 328, 181, 525, 290);
    check(g_refused_w == 457 && g_refused_h == 252, "%s: 457x252 refused", what);
    expect_kvs_most_picture(what);
    check(g_draw_tex_w == 328 && g_draw_tex_h == 181, "%s: pass 2 resamples the 328x181 frame", what);
    g_allocs = 0;
    check(render_set_output(800, 252, 171, 0, 457, 252, 1) == 0 && g_allocs == 0,
          "%s: a refused size is not retried (%d allocations)", what, g_allocs);
    /* A different output size is a new attempt. */
    g_allocs = 0;
    render_set_output(820, 300, 138, 0, 544, 300, 1);
    check(g_allocs > 0, "%s: a new size is attempted", what);
    expect_render_size(what, 328, 181, 328, 181, 525, 290);
    g_fail_alloc_wider_than = 0;
    render_set_output(640, 240, 102, 0, 435, 240, 1);
    expect_render_size("after out of memory", 435, 240, 435, 240, most_mask(435), most_mask(240));
    render_set_output(0, 0, 0, 0, 0, 0, 0);
    reset_refusals();
}

/* A framebuffer that is incomplete at the native size is a failure as well. */
static void fallback_on_incomplete_framebuffer(void) {
    const char *what = "incomplete framebuffer";
    render_set_output(0, 0, 0, 0, 0, 0, 0);
    reset_refusals();
    g_incomplete_at_ss_w = 457;
    render_set_output(800, 252, 171, 0, 457, 252, 1);
    expect_render_size(what, 328, 181, 328, 181, 525, 290);
    check(g_refused_w == 457, "%s: 457x252 refused", what);
    expect_kvs_most_picture(what);
    g_incomplete_at_ss_w = 0;
    render_set_output(0, 0, 0, 0, 0, 0, 0);
    reset_refusals();
}

/* A MOST content size equal to the platform framebuffer has no smaller size to fall back to:
 * a failure at 1x falls back to the platform scale and is not tried again. */
static void fallback_same_size_to_platform_scale(void) {
    const char *what = "same-size fallback";
    render_set_output(0, 0, 0, 0, 0, 0, 0);
    reset_refusals();
    g_incomplete_at_ss_w = 328;
    render_set_output(328, 181, 0, 0, 328, 181, 1);
    expect_render_size(what, 328, 181, 525, 290, 525, 290);
    check(g_refused_w == 328 && g_refused_h == 181, "%s: 328x181 at 1x refused", what);
    g_allocs = 0;
    check(render_set_output(328, 181, 0, 0, 328, 181, 1) == 0 && g_allocs == 0,
          "%s: the platform-scale fallback is kept (%d allocations)", what, g_allocs);
    g_incomplete_at_ss_w = 0;
    render_set_output(0, 0, 0, 0, 0, 0, 0);
    reset_refusals();
}

/* A size over the GL's texture/renderbuffer limit is refused before anything is allocated;
 * an unreported limit (0) refuses nothing. */
static void gl_limit(void) {
    const char *what = "GL limit";
    render_set_output(0, 0, 0, 0, 0, 0, 0);
    reset_refusals();
    g_gl_limit = 1200;
    g_widest_alloc = 0;
    render_set_output(800, 252, 171, 0, 457, 252, 1);
    check(g_widest_alloc <= 1200, "%s: widest allocation %d, over the limit", what, g_widest_alloc);
    expect_render_size(what, 328, 181, 328, 181, 525, 290);
    check(g_refused_w == 457, "%s: 457x252 refused", what);
    render_set_output(0, 0, 0, 0, 0, 0, 0);
    reset_refusals();
    g_gl_limit = 0;
    render_set_output(800, 252, 171, 0, 457, 252, 1);
    expect_render_size("unreported GL limit", 457, 252, 457, 252, most_mask(457), most_mask(252));
    g_gl_limit = 4096;
    render_set_output(0, 0, 0, 0, 0, 0, 0);
    reset_refusals();
}

/* The collector's frame: the whole window, top row first, as binary PPM. */
static void capture_writes_window(void) {
    const char *what = "capture", *path = "/tmp/most_output_render_test.ppm";
    char header[32];
    size_t header_len, size;
    unsigned char *data;
    FILE *f;
    px_t content = texture_colour(g_fxaa_tex), marker = {1, 0, 0, 1};
    window_alloc(800, 252);
    render_set_output(800, 252, 171, 0, 457, 252, 1);
    frame();
    for (int px = 0; px < 800; px++) g_px[px] = marker;      /* GL row 0: the bottom row */
    check(render_capture_output(path) == 0, "%s: written", what);
    header_len = (size_t)snprintf(header, sizeof(header), "P6\n800 252\n255\n");
    f = fopen(path, "rb");
    check(f != NULL, "%s: readable", what);
    if (!f) return;
    fseek(f, 0, SEEK_END);
    size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    check(size == header_len + 800 * 252 * 3, "%s: %zu bytes", what, size);
    data = (unsigned char *)malloc(size);
    if (data && fread(data, 1, size, f) == size) {
        const unsigned char *px = data + header_len;
        const unsigned char *top_left = px, *centre = px + (126 * 800 + 400) * 3;
        const unsigned char *bottom_left = px + (251 * 800) * 3;
        check(memcmp(data, header, header_len) == 0, "%s: PPM header", what);
        check(top_left[0] == 0 && top_left[1] == 0 && top_left[2] == 0, "%s: top-left is the black border", what);
        check(bottom_left[0] == 255 && bottom_left[1] == 0 && bottom_left[2] == 0,
              "%s: GL's bottom row is the file's last row", what);
        check(centre[0] == (unsigned char)lrintf(content.r * 255) && centre[1] == (unsigned char)lrintf(content.g * 255)
              && centre[2] == (unsigned char)lrintf(content.b * 255), "%s: centre is the content", what);
    } else {
        check(0, "%s: read back", what);
    }
    free(data);
    fclose(f);
    unlink(path);
    check(render_capture_output("/nonexistent-directory/frame.ppm") == -1, "%s: unwritable path fails", what);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (render_init(CR_DEFAULT_WIDTH, CR_DEFAULT_HEIGHT) != 0) {
        printf("FAIL: render_init\n");
        return 1;
    }
    check(g_fxaa_prog != 0 && g_fxaa_tex != 0, "init: FXAA path (as on QNX)");
    vc_pass2_is_upstream();
    most_pass2(800, 252, 171, 0, 457, 252);     /* stock KDK / KVS_Most on */
    most_pass2(328, 181, 0, 0, 328, 181); /* same dimensions, different SSAA scale */
    most_pass2(656, 362, 0, 0, 656, 362);       /* exact 2x: no bars */
    most_pass2(328, 400, 0, 109, 328, 181);     /* bars above and below */
    overlay_scissor_is_closed();
    fallback_on_out_of_memory();
    fallback_on_incomplete_framebuffer();
    fallback_same_size_to_platform_scale();
    gl_limit();
    capture_writes_window();
    /* win_w <= 0 restores the upstream window-filling pass. */
    render_set_output(0, 0, 0, 0, 0, 0, 0);
    vc_pass2_is_upstream();
    printf("most_output_render_test: %d checks, %d failures\n", g_checks, g_failures);
    free(g_px);
    return g_failures ? 1 : 0;
}
