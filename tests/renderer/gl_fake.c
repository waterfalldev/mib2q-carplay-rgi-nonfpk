/*
 * GLES2 fake for the parity harness.  Replaces the driver for every renderer test.
 *
 * Models just enough state for render.c to initialise and run (object names, shader/program
 * bookkeeping, uniform locations by name, bound framebuffer/texture/program, enabled
 * client-side vertex arrays) and, when fp_open() has been called, writes a deterministic
 * record of every glDrawArrays / glClear: the framebuffer, program (named by a hash of its
 * shader sources), pipeline state, every uniform set on that program, and the vertex data
 * read from the client-side arrays.  Floats are printed rounded to 1e-4.
 *
 * Without fp_open() nothing is recorded and the fake is silent.
 */
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ recorder */

static FILE *g_fp;
static int g_fp_full;               /* 1: every vertex of every draw */
unsigned long fp_draws, fp_clears;

#define FULL_LIMIT 6                /* verts per draw dumped in the default mode (mask triangles, quads) */
#define CHUNK 64

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = p;
    while (n--) h = (h ^ *b++) * 1099511628211ull;
    return h;
}
#define FNV0 14695981039346656037ull

static int32_t quant(float x) {
    double q;
    if (x != x) return INT32_MIN;
    if (x > 1e5f) return INT32_MAX;
    if (x < -1e5f) return INT32_MIN + 1;
    q = floor((double)x * 1e4 + 0.5);
    return (int32_t)q;
}
/* "%.4f" of the quantised value, never "-0.0000". */
const char *fp_f(float x) {
    static char buf[8][24];
    static int n;
    char *b = buf[n++ & 7];
    int32_t q;
    if (x != x) return "nan";
    if (x > 1e5f) return "inf";
    if (x < -1e5f) return "-inf";
    q = quant(x);
    snprintf(b, 24, "%s%d.%04d", q < 0 ? "-" : "", abs(q) / 10000, abs(q) % 10000);
    return b;
}

int fp_open(const char *path, int full) {
    g_fp = path ? fopen(path, "w") : NULL;
    g_fp_full = full;
    fp_draws = fp_clears = 0;
    return path && !g_fp ? -1 : 0;
}
void fp_close(void) { if (g_fp) fclose(g_fp); g_fp = NULL; }
void fp_flush(void) { if (g_fp) fflush(g_fp); }
int fp_on(void) { return g_fp != NULL; }
void fp_line(const char *fmt, ...) {
    va_list ap;
    if (!g_fp) return;
    va_start(ap, fmt);
    vfprintf(g_fp, fmt, ap);
    va_end(ap);
    fputc('\n', g_fp);
}
/* Recording can be paused around frames that are only there to advance the animation. */
static int g_paused;
void fp_pause(int p) { g_paused = p; }

/* ------------------------------------------------------------------ object model */

#define MAXOBJ 512
static GLuint g_next = 1;

typedef struct { GLuint id; GLenum type; uint64_t src; } shader_t;
static shader_t g_shaders[MAXOBJ];
static int g_nshaders;

#define MAXU 96
typedef struct {
    char name[48];
    char kind[8];
    int n;
    float v[32];
    int set;
} uniform_t;
typedef struct {
    GLuint id;
    uint64_t hash;
    int nattached;
    char attr_name[16][32];
    int attr_bound[16];
    int next_attr;
    uniform_t u[MAXU];
    int nu;
} program_t;
static program_t g_programs[64];
static int g_nprograms;

typedef struct { GLuint id; int w, h, ordinal; uint64_t data; } texture_t;
static texture_t g_textures[MAXOBJ];
static int g_ntextures;

typedef struct { GLuint id; GLuint color; } fbo_t;
static fbo_t g_fbos_[64];
static int g_nfbos;

static GLuint g_bound_fbo, g_bound_prog, g_active_unit;
static GLuint g_bound_tex[8];
static unsigned g_caps;             /* bit per tracked capability */
static GLenum g_blend[4], g_depth_func = GL_LESS;
static GLboolean g_depth_mask = GL_TRUE, g_cmask[4] = { 1, 1, 1, 1 };
static GLint g_vp[4], g_sc[4];
static GLfloat g_clear_color[4];

typedef struct { int enabled, size, stride; GLenum type; const void *ptr; float generic[4]; } attrib_t;
static attrib_t g_attr[16];

static program_t *find_program(GLuint id) {
    int i;
    for (i = 0; i < g_nprograms; i++) if (g_programs[i].id == id) return &g_programs[i];
    return NULL;
}
static shader_t *find_shader(GLuint id) {
    int i;
    for (i = 0; i < g_nshaders; i++) if (g_shaders[i].id == id) return &g_shaders[i];
    return NULL;
}
static texture_t *find_texture(GLuint id) {
    int i;
    for (i = 0; i < g_ntextures; i++) if (g_textures[i].id == id) return &g_textures[i];
    return NULL;
}
static fbo_t *find_fbo(GLuint id) {
    int i;
    for (i = 0; i < g_nfbos; i++) if (g_fbos_[i].id == id) return &g_fbos_[i];
    return NULL;
}
static const char *tex_label(GLuint id) {
    static char buf[8][40];
    static int n;
    char *b = buf[n++ & 7];
    texture_t *t;
    if (!id) return "-";
    t = find_texture(id);
    if (!t || !t->w) { snprintf(b, 40, "T?"); return b; }
    if (t->data) snprintf(b, 40, "T%dx%d#%d[%08x]", t->w, t->h, t->ordinal, (unsigned)t->data);
    else snprintf(b, 40, "T%dx%d#%d", t->w, t->h, t->ordinal);
    return b;
}
static const char *fbo_label(GLuint id) {
    static char buf[8][48];
    static int n;
    char *b = buf[n++ & 7];
    fbo_t *f;
    if (!id) return "window";
    f = find_fbo(id);
    if (!f || !f->color) { snprintf(b, 48, "F?"); return b; }
    snprintf(b, 48, "F:%s", tex_label(f->color));
    return b;
}

/* ------------------------------------------------------------------ GL entry points */

static void gen(GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = g_next++; }
void glGenTextures(GLsizei n, GLuint *t) {
    gen(n, t);
    for (GLsizei i = 0; i < n && g_ntextures < MAXOBJ; i++) {
        memset(&g_textures[g_ntextures], 0, sizeof(texture_t));
        g_textures[g_ntextures++].id = t[i];
    }
}
void glGenFramebuffers(GLsizei n, GLuint *t) {
    gen(n, t);
    for (GLsizei i = 0; i < n && g_nfbos < 64; i++) {
        g_fbos_[g_nfbos].id = t[i];
        g_fbos_[g_nfbos++].color = 0;
    }
}
void glGenRenderbuffers(GLsizei n, GLuint *t) { gen(n, t); }
GLuint glCreateShader(GLenum type) {
    GLuint id = g_next++;
    if (g_nshaders < MAXOBJ) { g_shaders[g_nshaders].id = id; g_shaders[g_nshaders].type = type; g_shaders[g_nshaders++].src = 0; }
    return id;
}
GLuint glCreateProgram(void) {
    GLuint id = g_next++;
    if (g_nprograms < 64) {
        memset(&g_programs[g_nprograms], 0, sizeof(program_t));
        g_programs[g_nprograms++].id = id;
    }
    return id;
}
void glShaderSource(GLuint s, GLsizei c, const char **str, const GLint *l) {
    shader_t *sh = find_shader(s);
    uint64_t h = FNV0;
    if (!sh) return;
    for (GLsizei i = 0; i < c; i++) {
        size_t n = (l && l[i] >= 0) ? (size_t)l[i] : strlen(str[i]);
        h = fnv(h, str[i], n);
    }
    sh->src = h;
}
void glAttachShader(GLuint p, GLuint s) {
    program_t *pr = find_program(p);
    shader_t *sh = find_shader(s);
    if (!pr || !sh) return;
    pr->hash += (sh->src ^ (uint64_t)sh->type * 0x9e3779b97f4a7c15ull) * 0x2545f4914f6cdd1dull;
    pr->nattached++;
}
void glDetachShader(GLuint p, GLuint s) { (void)p; (void)s; }
void glBindAttribLocation(GLuint p, GLuint i, const char *n) {
    program_t *pr = find_program(p);
    if (!pr || i >= 16) return;
    snprintf(pr->attr_name[i], 32, "%s", n);
    pr->attr_bound[i] = 1;
}
GLint glGetAttribLocation(GLuint p, const char *n) {
    program_t *pr = find_program(p);
    if (!pr) return 0;
    for (int i = 0; i < 16; i++) if (pr->attr_bound[i] && !strcmp(pr->attr_name[i], n)) return i;
    for (int i = 0; i < 16; i++) {
        if (!pr->attr_bound[i]) {
            snprintf(pr->attr_name[i], 32, "%s", n);
            pr->attr_bound[i] = 1;
            return i;
        }
    }
    return -1;
}
GLint glGetUniformLocation(GLuint p, const char *n) {
    program_t *pr = find_program(p);
    if (!pr) return -1;
    for (int i = 0; i < pr->nu; i++) if (!strcmp(pr->u[i].name, n)) return i;
    if (pr->nu == MAXU) return -1;
    snprintf(pr->u[pr->nu].name, sizeof(pr->u[0].name), "%s", n);
    return pr->nu++;
}
static void put_uniform(GLint l, const char *kind, int n, const float *v) {
    program_t *pr = find_program(g_bound_prog);
    uniform_t *u;
    if (!pr || l < 0 || l >= pr->nu) return;
    u = &pr->u[l];
    snprintf(u->kind, sizeof(u->kind), "%s", kind);
    if (n > 32) n = 32;
    u->n = n;
    memcpy(u->v, v, (size_t)n * sizeof(float));
    u->set = 1;
}
void glUniform1f(GLint l, GLfloat a) { float v[1] = { a }; put_uniform(l, "1f", 1, v); }
void glUniform2f(GLint l, GLfloat a, GLfloat b) { float v[2] = { a, b }; put_uniform(l, "2f", 2, v); }
void glUniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { float v[3] = { a, b, c }; put_uniform(l, "3f", 3, v); }
void glUniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { float v[4] = { a, b, c, d }; put_uniform(l, "4f", 4, v); }
void glUniform1i(GLint l, GLint a) { float v[1] = { (float)a }; put_uniform(l, "1i", 1, v); }
void glUniform3fv(GLint l, GLsizei n, const GLfloat *v) { put_uniform(l, "3fv", 3 * n, v); }
void glUniform4fv(GLint l, GLsizei n, const GLfloat *v) { put_uniform(l, "4fv", 4 * n, v); }
void glUniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v) { (void)t; put_uniform(l, "m4", 16 * n, v); }

void glUseProgram(GLuint p) { g_bound_prog = p; }
void glDeleteProgram(GLuint p) { (void)p; }
void glDeleteShader(GLuint s) { (void)s; }
void glCompileShader(GLuint s) { (void)s; }
void glLinkProgram(GLuint p) { (void)p; }
void glGetShaderiv(GLuint s, GLenum p, GLint *v) { (void)s; *v = p == GL_COMPILE_STATUS ? GL_TRUE : 0; }
void glGetProgramiv(GLuint s, GLenum p, GLint *v) { (void)s; *v = p == GL_LINK_STATUS ? GL_TRUE : 0; }
void glGetShaderInfoLog(GLuint s, GLsizei n, GLsizei *l, char *log) { (void)s; if (l) *l = 0; if (log && n) log[0] = 0; }
void glGetProgramInfoLog(GLuint s, GLsizei n, GLsizei *l, char *log) { (void)s; if (l) *l = 0; if (log && n) log[0] = 0; }

void glBindFramebuffer(GLenum target, GLuint fb) { (void)target; g_bound_fbo = fb; }
void glBindRenderbuffer(GLenum t, GLuint r) { (void)t; (void)r; }
void glActiveTexture(GLenum unit) { g_active_unit = unit - GL_TEXTURE0; }
void glBindTexture(GLenum target, GLuint tex) { (void)target; if (g_active_unit < 8) g_bound_tex[g_active_unit] = tex; }
void glFramebufferTexture2D(GLenum a, GLenum att, GLenum c, GLuint tex, GLint e) {
    fbo_t *f = find_fbo(g_bound_fbo);
    (void)a; (void)c; (void)e;
    if (f && att == GL_COLOR_ATTACHMENT0) f->color = tex;
}
void glFramebufferRenderbuffer(GLenum a, GLenum b, GLenum c, GLuint d) { (void)a; (void)b; (void)c; (void)d; }
void glRenderbufferStorage(GLenum a, GLenum b, GLsizei c, GLsizei d) { (void)a; (void)b; (void)c; (void)d; }
GLenum glCheckFramebufferStatus(GLenum t) { (void)t; return GL_FRAMEBUFFER_COMPLETE; }
void glDeleteFramebuffers(GLsizei n, const GLuint *f) { (void)n; (void)f; }
void glDeleteRenderbuffers(GLsizei n, const GLuint *r) { (void)n; (void)r; }
void glDeleteTextures(GLsizei n, const GLuint *t) { (void)n; (void)t; }

static int bytes_per_pixel(GLenum fmt, GLenum type) {
    int comps = fmt == GL_RGBA ? 4 : fmt == GL_RGB ? 3 : fmt == GL_LUMINANCE_ALPHA ? 2 : 1;
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) return 2;
    return comps;
}
void glTexImage2D(GLenum a, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint f, GLenum fmt, GLenum type, const void *data) {
    texture_t *t = g_active_unit < 8 ? find_texture(g_bound_tex[g_active_unit]) : NULL;
    (void)a; (void)ifmt; (void)f;
    if (!t || level != 0) return;
    if (!t->w) {
        int n = 0;
        for (int i = 0; i < g_ntextures; i++) if (g_textures[i].w == w && g_textures[i].h == h) n++;
        t->w = w; t->h = h; t->ordinal = n;
    }
    t->data = data ? (uint32_t)fnv(FNV0, data, (size_t)w * (size_t)h * (size_t)bytes_per_pixel(fmt, type)) : 0;
}
void glTexParameteri(GLenum a, GLenum b, GLint c) { (void)a; (void)b; (void)c; }

static void cap(GLenum c, int on) {
    int bit;
    switch (c) {
    case GL_BLEND: bit = 0; break;
    case GL_DEPTH_TEST: bit = 1; break;
    case GL_CULL_FACE: bit = 2; break;
    case GL_SCISSOR_TEST: bit = 3; break;
    case GL_STENCIL_TEST: bit = 4; break;
    case GL_DITHER: bit = 5; break;
    case GL_POLYGON_OFFSET_FILL: bit = 6; break;
    default: return;
    }
    if (on) g_caps |= 1u << bit; else g_caps &= ~(1u << bit);
}
void glEnable(GLenum c) { cap(c, 1); }
void glDisable(GLenum c) { cap(c, 0); }
void glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { g_blend[0] = a; g_blend[1] = b; g_blend[2] = c; g_blend[3] = d; }
void glDepthFunc(GLenum f) { g_depth_func = f; }
void glDepthMask(GLboolean f) { g_depth_mask = f; }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { g_cmask[0] = r; g_cmask[1] = g; g_cmask[2] = b; g_cmask[3] = a; }
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { g_vp[0] = x; g_vp[1] = y; g_vp[2] = w; g_vp[3] = h; }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { g_sc[0] = x; g_sc[1] = y; g_sc[2] = w; g_sc[3] = h; }
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { g_clear_color[0] = r; g_clear_color[1] = g; g_clear_color[2] = b; g_clear_color[3] = a; }

void glGetIntegerv(GLenum p, GLint *v) {
    switch (p) {
    case GL_MAX_TEXTURE_SIZE:
    case GL_MAX_RENDERBUFFER_SIZE: *v = 8192; break;
    case GL_FRAMEBUFFER_BINDING: *v = (GLint)g_bound_fbo; break;
    case GL_VIEWPORT: memcpy(v, g_vp, sizeof(g_vp)); break;
    default: *v = 0;
    }
}
GLenum glGetError(void) { return GL_NO_ERROR; }
const GLubyte *glGetString(GLenum name) { (void)name; return (const GLubyte *)"fake"; }
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) { (void)name; return NULL; }
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *out) {
    (void)x; (void)y; (void)f; (void)t; memset(out, 0, (size_t)w * (size_t)h * 4);
}

void glEnableVertexAttribArray(GLuint i) { if (i < 16) g_attr[i].enabled = 1; }
void glDisableVertexAttribArray(GLuint i) { if (i < 16) g_attr[i].enabled = 0; }
void glVertexAttribPointer(GLuint i, GLint s, GLenum t, GLboolean n, GLsizei st, const void *p) {
    (void)n;
    if (i >= 16) return;
    g_attr[i].size = s; g_attr[i].type = t; g_attr[i].stride = st; g_attr[i].ptr = p;
}
void glVertexAttrib1f(GLuint i, GLfloat a) {
    if (i < 16) { g_attr[i].generic[0] = a; g_attr[i].generic[1] = g_attr[i].generic[2] = 0; g_attr[i].generic[3] = 1; }
}

/* ------------------------------------------------------------------ recording */

static int cmp_u(const void *a, const void *b) {
    return strcmp((*(uniform_t *const *)a)->name, (*(uniform_t *const *)b)->name);
}

static void dump_state(const program_t *pr) {
    uniform_t *sorted[MAXU];
    int n = 0, i;
    fprintf(g_fp, " st vp=%d,%d,%d,%d caps=%02x sc=%d,%d,%d,%d blend=%x,%x,%x,%x depth=%x/%d cmask=%d%d%d%d\n",
            g_vp[0], g_vp[1], g_vp[2], g_vp[3], g_caps, g_sc[0], g_sc[1], g_sc[2], g_sc[3],
            g_blend[0], g_blend[1], g_blend[2], g_blend[3], g_depth_func, g_depth_mask,
            g_cmask[0], g_cmask[1], g_cmask[2], g_cmask[3]);
    fprintf(g_fp, " tex");
    for (i = 0; i < 4; i++) if (g_bound_tex[i]) fprintf(g_fp, " %d=%s", i, tex_label(g_bound_tex[i]));
    fputc('\n', g_fp);
    if (!pr) return;
    for (i = 0; i < pr->nu; i++) if (pr->u[i].set) sorted[n++] = (uniform_t *)&pr->u[i];
    qsort(sorted, (size_t)n, sizeof(sorted[0]), cmp_u);
    for (i = 0; i < n; i++) {
        const uniform_t *u = sorted[i];
        fprintf(g_fp, " u %s %s", u->name, u->kind);
        for (int k = 0; k < u->n; k++) fprintf(g_fp, " %s", fp_f(u->v[k]));
        fputc('\n', g_fp);
    }
}

void glClear(GLbitfield mask) {
    if (!g_fp || g_paused) return;
    fp_clears++;
    fprintf(g_fp, "CLEAR %s mask=%x color=%s,%s,%s,%s\n", fbo_label(g_bound_fbo), (unsigned)mask,
            fp_f(g_clear_color[0]), fp_f(g_clear_color[1]), fp_f(g_clear_color[2]), fp_f(g_clear_color[3]));
}

/* Pipeline state and uniforms are printed only when they differ from what the previous draw
 * printed (per program for uniforms); the draw hash covers the full state either way. */
#define DCACHE 2048
static struct { char key[96]; char text[200]; } g_dcache[DCACHE];
static int g_ndcache;
static void emit_delta(FILE *out, unsigned prog, const char *full) {
    const char *p = full;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[200], key[96];
        int i;
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len); line[len] = 0;
        if (!strncmp(line, " st", 3) || !strncmp(line, " tex", 4)) {
            snprintf(key, sizeof(key), "%.4s", line + 1);
        } else {
            char name[64];
            sscanf(line, " u %63s", name);
            snprintf(key, sizeof(key), "%08x:%s", prog, name);
        }
        for (i = 0; i < g_ndcache; i++) if (!strcmp(g_dcache[i].key, key)) break;
        if (i == g_ndcache && g_ndcache < DCACHE) { snprintf(g_dcache[i].key, 96, "%s", key); g_dcache[i].text[0] = 0; g_ndcache++; }
        if (i < g_ndcache && strcmp(g_dcache[i].text, line)) {
            snprintf(g_dcache[i].text, 200, "%s", line);
            fprintf(out, "%s\n", line);
        }
        if (!e) break;
        p = e + 1;
    }
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    program_t *pr;
    int act[16], na = 0, i;
    uint64_t vh = FNV0, all;
    FILE *real;
    char *rec = NULL;
    size_t reclen = 0, fulllen = 0;
    char *stext = NULL;
    if (!g_fp || g_paused) return;
    pr = find_program(g_bound_prog);
    fp_draws++;
    real = g_fp;
    g_fp = open_memstream(&rec, &reclen);       /* the record is hashed as a whole, then written */
    fprintf(g_fp, "DRAW fbo=%s prog=P%08x mode=%x first=%d count=%d\n", fbo_label(g_bound_fbo),
            pr ? (unsigned)(pr->hash ^ (pr->hash >> 32)) : 0u, mode, first, count);
    {
        FILE *rk = g_fp;
        g_fp = open_memstream(&stext, &fulllen);
        dump_state(pr);
        fclose(g_fp);
        g_fp = rk;
    }
    for (i = 0; i < 16; i++) if (g_attr[i].enabled && g_attr[i].ptr) act[na++] = i;
    for (i = 0; i < na; i++) {
        const attrib_t *a = &g_attr[act[i]];
        fprintf(g_fp, " a %d:%s size=%d stride=%d type=%x\n", act[i],
                pr && pr->attr_bound[act[i]] ? pr->attr_name[act[i]] : "?", a->size, a->stride, a->type);
    }
    /* Vertex data, quantised.  One line per vertex when small; otherwise per-component
     * range/sum plus a hash of every CHUNK vertices. */
    {
        int full = g_fp_full || count <= FULL_LIMIT;
        double mn[16][4], mx[16][4], sm[16][4];
        uint64_t ch = FNV0;
        for (i = 0; i < na; i++) for (int k = 0; k < 4; k++) { mn[i][k] = 1e30; mx[i][k] = -1e30; sm[i][k] = 0; }
        for (int v = 0; v < count; v++) {
            if (full) fprintf(g_fp, " v %d:", v);
            for (i = 0; i < na; i++) {
                const attrib_t *a = &g_attr[act[i]];
                int stride = a->stride ? a->stride : a->size * 4;
                const unsigned char *p = (const unsigned char *)a->ptr + (size_t)(first + v) * (size_t)stride;
                for (int k = 0; k < a->size && k < 4; k++) {
                    float x;
                    int32_t q;
                    memcpy(&x, p + 4 * k, 4);
                    q = quant(x);
                    vh = fnv(vh, &q, 4);
                    ch = fnv(ch, &q, 4);
                    if (x < mn[i][k]) mn[i][k] = x;
                    if (x > mx[i][k]) mx[i][k] = x;
                    sm[i][k] += (double)q * 1e-4;
                    if (full) fprintf(g_fp, " %s", fp_f(x));
                }
                if (full && i + 1 < na) fputs(" |", g_fp);
            }
            if (full) fputc('\n', g_fp);
            else if ((v + 1) % CHUNK == 0 || v + 1 == count) {
                fprintf(g_fp, " vc %d..%d %08x\n", v - v % CHUNK, v, (unsigned)(ch ^ (ch >> 32)));
                ch = FNV0;
            }
        }
        if (!full) {
            for (i = 0; i < na; i++) {
                const attrib_t *a = &g_attr[act[i]];
                fprintf(g_fp, " vs %d", act[i]);
                for (int k = 0; k < a->size && k < 4; k++)
                    fprintf(g_fp, " [%s %s sum %s]", fp_f((float)mn[i][k]), fp_f((float)mx[i][k]), fp_f((float)sm[i][k]));
                fputc('\n', g_fp);
            }
        }
    }
    fclose(g_fp);
    g_fp = real;
    all = fnv(fnv(FNV0, stext, fulllen), rec, reclen);   /* full state, header, attributes, vertices */
    fprintf(g_fp, "#%lu ", fp_draws);
    {
        const char *nl = strchr(rec, '\n');
        size_t first_line = nl ? (size_t)(nl - rec) + 1 : reclen;
        fwrite(rec, 1, first_line, g_fp);
        emit_delta(g_fp, pr ? (unsigned)(pr->hash ^ (pr->hash >> 32)) : 0u, stext);
        fwrite(rec + first_line, 1, reclen - first_line, g_fp);
    }
    free(rec);
    free(stext);
    fprintf(g_fp, " hash v=%08x all=%08x\n", (unsigned)(vh ^ (vh >> 32)), (unsigned)(all ^ (all >> 32)));
}

/* ------------------------------------------------------------------ virtual clock */
/* render.c animates the popup crop and the lane-panel reframing from CLOCK_MONOTONIC.  The
 * fingerprint harness sets fp_clock (seconds) and switches this on, so those animations
 * advance with the frame count instead of the wall clock.  Off: the real clock. */
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
double fp_clock;
int fp_clock_on;
int clock_gettime(clockid_t id, struct timespec *ts) {
    if (fp_clock_on) {
        ts->tv_sec = (time_t)fp_clock;
        ts->tv_nsec = (long)((fp_clock - (double)ts->tv_sec) * 1e9);
        return 0;
    }
    return (int)syscall(SYS_clock_gettime, id, ts);
}
