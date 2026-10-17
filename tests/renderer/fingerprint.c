/*
 * Geometry fingerprint of the maneuver renderer.
 *
 * Drives the real engine (main.c's engine_*, apply_commands, prepare_engine_scenes and the
 * render-loop body, with the same scene provider, scenes and lane panel as renderer_application_main)
 * and maneuver.c / render.c / route_path.c / scene sources against the recording GL fake
 * (gl_fake.c).  Everything the renderer would submit - clears, draws, program, pipeline
 * state, uniforms, vertex data - is written to FP_OUT in a deterministic text form, so the
 * file can be diffed before and after a refactor.
 *
 * Every unit (a maneuver / slide / progress / push / lane-panel case) runs in a forked
 * child from pristine process state, so units cannot influence each other and a failing
 * (asserting / UBSan) unit is attributed to its name.
 */
#define main renderer_application_main
#include "main.c"
#undef main
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/wait.h>

extern int fp_open(const char *path, int full);
extern void fp_close(void);
extern void fp_flush(void);
extern void fp_line(const char *fmt, ...);
extern void fp_pause(int p);
extern const char *fp_f(float x);
extern unsigned long fp_draws, fp_clears;
extern double fp_clock;
extern int fp_clock_on;

/* ------------------------------------------------------------------ unit table */

enum { K_SETTLED, K_NOSCENE, K_SLIDE, K_PROGRESS, K_ARROW, K_PUSH, K_PUSH3, K_PUSH_PROGRESS,
       K_LANES, K_LANES_PUSH, K_VISIBLE, K_REVEAL, K_DEBUG };

typedef struct {
    int kind;
    char name[112];
    maneuver_state_t a, b, c;
    int persp;
    float slide;
    float fill, pw, glow;
    int lane_case;
} unit_t;
static unit_t g_units[1024];
static int g_nunits;

static unit_t *add_unit(int kind, int persp, const char *fmt, ...) {
    unit_t *u = &g_units[g_nunits++];
    va_list ap;
    assert(g_nunits <= 1024);
    memset(u, 0, sizeof(*u));
    u->kind = kind;
    u->persp = persp;
    va_start(ap, fmt);
    vsnprintf(u->name, sizeof(u->name), fmt, ap);
    va_end(ap);
    return u;
}

static maneuver_state_t mk(int icon, int side, int dir, float exit_angle, int bap, int nj, const float *j) {
    maneuver_state_t s;
    memset(&s, 0, sizeof(s));
    s.icon = icon;
    s.driving_side = side;
    s.direction = dir;
    s.exit_angle = exit_angle;
    s.bap_geometry = bap;
    s.junction_angle_count = nj;
    for (int i = 0; i < nj; i++) s.junction_angles[i] = j[i];
    return s;
}
/* The parity test's icon matrix: every icon, both driving sides. */
static maneuver_state_t icon_side(int icon, int side) {
    return mk(icon, side, side ? 1 : -1, side ? 67.5f : -67.5f, 1, 0, NULL);
}

static const char *const g_icon_name[ICON_COUNT] = {
    "none", "approach", "turn", "uturn", "merge", "exit", "roundabout", "arrived", "lanechange", "rabexit"
};

static void build_units(void) {
    static const float j3[] = { -90, 0, 90 }, j4[] = { -90, 0, 90, 180 }, j2[] = { -90, 90 };
    static const float jr[] = { -135, -45, 45, 135 }, jr2[] = { -90, 0, 90 }, jr3[] = { 30, 120, -150 };
    static const float angles[] = { -180, -160, -135, -100, -90, -45, 0, 45, 90, 100, 101, 135, 160, 180 };
    int icon, side, i;

    /* Settled frame of every icon, both driving sides, 3D; 2D too. */
    for (icon = ICON_APPROACH; icon < ICON_COUNT; icon++)
        for (side = 0; side < 2; side++) {
            unit_t *u = add_unit(K_SETTLED, 1, "settled/%s/side%d/3d", g_icon_name[icon], side);
            u->a = icon_side(icon, side);
            u = add_unit(K_SETTLED, 0, "settled/%s/side%d/2d", g_icon_name[icon], side);
            u->a = icon_side(icon, side);
        }
    /* Turn angles: <= 100 degrees is a retained scene road, beyond it the native family. */
    for (i = 0; i < (int)(sizeof(angles) / sizeof(angles[0])); i++) {
        unit_t *u = add_unit(K_SETTLED, 1, "settled/turn%+.0f/3d", angles[i]);
        u->a = mk(ICON_TURN, 0, angles[i] < 0 ? -1 : 1, angles[i], 0, 0, NULL);
        if (i % 3 == 0) {
            u = add_unit(K_SETTLED, 0, "settled/turn%+.0f/2d", angles[i]);
            u->a = mk(ICON_TURN, 0, angles[i] < 0 ? -1 : 1, angles[i], 0, 0, NULL);
        }
    }
    /* Junction / ring road families, BAP and snapped. */
    add_unit(K_SETTLED, 1, "settled/turn90+side-streets3/3d")->a = mk(ICON_TURN, 0, 1, 90, 1, 3, j3);
    add_unit(K_SETTLED, 1, "settled/turn-90+side-streets4/3d")->a = mk(ICON_TURN, 0, -1, -90, 1, 4, j4);
    add_unit(K_SETTLED, 0, "settled/turn45+side-streets2/2d")->a = mk(ICON_TURN, 1, 1, 45, 1, 2, j2);
    add_unit(K_SETTLED, 1, "settled/approach+2/3d")->a = mk(ICON_APPROACH, 0, 0, 0, 1, 2, j2);
    add_unit(K_SETTLED, 1, "settled/approach+3/3d")->a = mk(ICON_APPROACH, 1, 0, 0, 1, 3, j3);
    add_unit(K_SETTLED, 0, "settled/approach+3/2d")->a = mk(ICON_APPROACH, 0, 0, 0, 1, 3, j3);
    for (side = 0; side < 2; side++) {
        add_unit(K_SETTLED, 1, "settled/rab+4/exit90/side%d/3d", side)->a = mk(ICON_ROUNDABOUT, side, 1, 90, 1, 4, jr);
        add_unit(K_SETTLED, 1, "settled/rab+4/exit-135/side%d/3d", side)->a = mk(ICON_ROUNDABOUT, side, -1, -135, 1, 4, jr);
        add_unit(K_SETTLED, 1, "settled/rab+3/exit180/side%d/3d", side)->a = mk(ICON_ROUNDABOUT, side, 1, 180, 1, 3, jr2);
        add_unit(K_SETTLED, 1, "settled/rab-snap+3/exit-67.5/side%d/3d", side)->a = mk(ICON_ROUNDABOUT, side, -1, -67.5f, 0, 3, jr3);
        add_unit(K_SETTLED, 0, "settled/rab+4/exit45/side%d/2d", side)->a = mk(ICON_ROUNDABOUT, side, 1, 45, 1, 4, jr);
    }
    add_unit(K_SETTLED, 1, "settled/rab-noroads/3d")->a = mk(ICON_ROUNDABOUT, 0, 1, 90, 1, 0, NULL);
    for (i = -1; i <= 1; i++) {
        add_unit(K_SETTLED, 1, "settled/arrived/dir%+d/3d", i)->a = mk(ICON_ARRIVED, 0, i, 0, 1, 0, NULL);
        add_unit(K_SETTLED, 1, "settled/uturn/dir%+d/3d", i)->a = mk(ICON_UTURN, 0, i, 180, 1, 0, NULL);
        add_unit(K_SETTLED, 1, "settled/exit/dir%+d/3d", i)->a = mk(ICON_EXIT, 0, i, 0, 1, 0, NULL);
        add_unit(K_SETTLED, 1, "settled/merge/dir%+d/3d", i)->a = mk(ICON_MERGE, 0, i, 0, 1, 0, NULL);
        add_unit(K_SETTLED, 1, "settled/lanechange/dir%+d/3d", i)->a = mk(ICON_LANE_CHANGE, 0, i, 0, 1, 0, NULL);
    }
    /* The built-in family without a scene provider (the allocation-failure fallback). */
    for (i = 0; i < 5; i++) {
        static const float a[] = { -90, -45, 45, 90, 160 };
        add_unit(K_NOSCENE, 1, "noscene/turn%+.0f/3d", a[i])->a = mk(ICON_TURN, 0, a[i] < 0 ? -1 : 1, a[i], 0, 0, NULL);
    }
    add_unit(K_NOSCENE, 1, "noscene/turn90+side-streets3/3d")->a = mk(ICON_TURN, 0, 1, 90, 1, 3, j3);
    add_unit(K_NOSCENE, 0, "noscene/turn90/2d")->a = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL);

    /* Slide positions of the route window. */
    {
        static const float slides[] = { 0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.25f, 1.5f, 2.0f };
        maneuver_state_t rep[7];
        const char *rep_name[7] = { "turn90", "turn-160", "rab", "exit", "arrived", "uturn", "approach" };
        rep[0] = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL);
        rep[1] = mk(ICON_TURN, 0, -1, -160, 1, 0, NULL);
        rep[2] = mk(ICON_ROUNDABOUT, 0, 1, 90, 1, 4, jr);
        rep[3] = mk(ICON_EXIT, 0, 1, 0, 1, 0, NULL);
        rep[4] = mk(ICON_ARRIVED, 0, 0, 0, 1, 0, NULL);
        rep[5] = mk(ICON_UTURN, 0, -1, 180, 1, 0, NULL);
        rep[6] = mk(ICON_APPROACH, 0, 0, 0, 1, 3, j3);
        for (i = 0; i < 7; i++)
            for (int k = 0; k < (int)(sizeof(slides) / sizeof(slides[0])); k++) {
                unit_t *u = add_unit(K_SLIDE, 1, "slide/%s/%.2f/3d", rep_name[i], slides[k]);
                u->a = rep[i];
                u->slide = slides[k];
                if (i == 0 && k % 2 == 0) {
                    u = add_unit(K_SLIDE, 0, "slide/%s/%.2f/2d", rep_name[i], slides[k]);
                    u->a = rep[i];
                    u->slide = slides[k];
                }
            }
    }
    /* Route progress (fill, path weight, glow), set directly. */
    {
        static const float pr[][3] = { { 0, 0, 0 }, { 0.25f, 1, 0 }, { 0.5f, 1, 0 }, { 0.75f, 1, 0 }, { 1, 1, 0 },
                                       { 0.5f, 0.5f, 0.5f }, { 0, 0, 1 }, { 0.3f, 0, 0 } };
        maneuver_state_t rep[4];
        const char *rep_name[4] = { "turn90", "turn160", "rab", "lanechange" };
        rep[0] = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL);
        rep[1] = mk(ICON_TURN, 0, 1, 160, 1, 0, NULL);
        rep[2] = mk(ICON_ROUNDABOUT, 0, 1, 90, 1, 4, jr);
        rep[3] = mk(ICON_LANE_CHANGE, 0, 1, 0, 1, 0, NULL);
        for (i = 0; i < 4; i++)
            for (int k = 0; k < (int)(sizeof(pr) / sizeof(pr[0])); k++) {
                unit_t *u = add_unit(K_PROGRESS, 1, "progress/%s/%.2f-%.2f-%.2f/3d", rep_name[i], pr[k][0], pr[k][1], pr[k][2]);
                u->a = rep[i];
                u->fill = pr[k][0]; u->pw = pr[k][1]; u->glow = pr[k][2];
            }
        add_unit(K_ARROW, 1, "progress/arrow-protocol/turn90/3d")->a = rep[0];
        add_unit(K_ARROW, 1, "progress/arrow-protocol/rab/3d")->a = rep[2];
    }
    /* Combined (current + next) transitions: each icon as current and as next, then extras. */
    for (icon = ICON_APPROACH; icon < ICON_COUNT; icon++) {
        int next = icon == ICON_COUNT - 1 ? ICON_APPROACH : icon + 1;
        unit_t *u = add_unit(K_PUSH, 1, "push/%s->%s/3d", g_icon_name[icon], g_icon_name[next]);
        u->a = icon_side(icon, icon & 1);
        u->b = icon_side(next, (next + 1) & 1);
        if (icon % 3 == 0) {
            u = add_unit(K_PUSH, 0, "push/%s->%s/2d", g_icon_name[icon], g_icon_name[next]);
            u->a = icon_side(icon, icon & 1);
            u->b = icon_side(next, (next + 1) & 1);
        }
    }
    {
        unit_t *u;
        u = add_unit(K_PUSH, 1, "push/turn90->turn-90/3d");
        u->a = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL); u->b = mk(ICON_TURN, 0, -1, -90, 0, 0, NULL);
        u = add_unit(K_PUSH, 1, "push/turn160->turn45/3d");
        u->a = mk(ICON_TURN, 0, 1, 160, 1, 0, NULL); u->b = mk(ICON_TURN, 0, 1, 45, 1, 0, NULL);
        u = add_unit(K_PUSH, 1, "push/turn-45->turn170/3d");
        u->a = mk(ICON_TURN, 0, -1, -45, 1, 0, NULL); u->b = mk(ICON_TURN, 0, 1, 170, 1, 0, NULL);
        u = add_unit(K_PUSH, 1, "push/rab->turn90/3d");
        u->a = mk(ICON_ROUNDABOUT, 0, 1, 90, 1, 4, jr); u->b = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL);
        u = add_unit(K_PUSH, 1, "push/turn90->rab/3d");
        u->a = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL); u->b = mk(ICON_ROUNDABOUT, 1, -1, -135, 1, 4, jr);
        u = add_unit(K_PUSH, 1, "push/approach3->turn-90+streets/3d");
        u->a = mk(ICON_APPROACH, 0, 0, 0, 1, 3, j3); u->b = mk(ICON_TURN, 0, -1, -90, 1, 4, j4);
        u = add_unit(K_PUSH, 1, "push/turn90->arrived/3d");
        u->a = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL); u->b = mk(ICON_ARRIVED, 0, 1, 0, 1, 0, NULL);
        u = add_unit(K_PUSH, 1, "push/arrived->turn90/3d");
        u->a = mk(ICON_ARRIVED, 0, -1, 0, 1, 0, NULL); u->b = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL);
        u = add_unit(K_PUSH, 0, "push/turn90->turn-90/2d");
        u->a = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL); u->b = mk(ICON_TURN, 0, -1, -90, 0, 0, NULL);
        u = add_unit(K_PUSH3, 1, "push3/turn90->turn-90->rab/3d");
        u->a = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL); u->b = mk(ICON_TURN, 0, -1, -90, 0, 0, NULL);
        u->c = mk(ICON_ROUNDABOUT, 0, 1, 90, 1, 4, jr);
        u = add_unit(K_PUSH_PROGRESS, 1, "push+progress/turn90->turn-90/3d");
        u->a = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL); u->b = mk(ICON_TURN, 0, -1, -90, 0, 0, NULL);
    }
    /* Lane panel under a settled maneuver, and across a push. */
    for (i = 0; i < 4; i++) {
        unit_t *u = add_unit(K_LANES, 1, "lanes/case%d/turn90/3d", i);
        u->a = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL);
        u->lane_case = i;
    }
    {
        unit_t *u = add_unit(K_LANES, 1, "lanes/case1/rab/3d");
        u->a = mk(ICON_ROUNDABOUT, 0, 1, 90, 1, 4, jr); u->lane_case = 1;
        u = add_unit(K_LANES, 0, "lanes/case2/turn-45/2d");
        u->a = mk(ICON_TURN, 0, -1, -45, 0, 0, NULL); u->lane_case = 2;
        u = add_unit(K_LANES_PUSH, 1, "lanes+push/case1/turn90->turn-90/3d");
        u->a = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL); u->b = mk(ICON_TURN, 0, -1, -90, 0, 0, NULL); u->lane_case = 1;
    }
    /* Visible-area (popup crop) switch, fade-in from a cleared state, debug overlays. */
    add_unit(K_VISIBLE, 1, "visible-area/turn90/3d")->a = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL);
    add_unit(K_VISIBLE, 1, "visible-area/rab/3d")->a = mk(ICON_ROUNDABOUT, 0, 1, 90, 1, 4, jr);
    add_unit(K_REVEAL, 1, "reveal/turn90/3d")->a = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL);
    add_unit(K_REVEAL, 0, "reveal/rab/2d")->a = mk(ICON_ROUNDABOUT, 0, 1, 90, 1, 4, jr);
    add_unit(K_DEBUG, 1, "debug/turn90/3d")->a = mk(ICON_TURN, 0, 1, 90, 0, 0, NULL);
}

/* ------------------------------------------------------------------ child side */

static double g_now = 1000.0;
static int g_frames_recorded;
static int g_force;
static float g_force_fill, g_force_pw, g_force_glow;
static const char *g_unit_name;
static int g_frame_no;

static void dump_scene(const char *tag, cr_scene_t *s) {
    const cr_scene_info_t *i;
    const route_path_t *r;
    if (!s) { fp_line("SCENE %s none", tag); return; }
    i = cr_scene_info(s);
    r = cr_scene_route(s);
    fp_line("SCENE %s kind=%d native=%d fallback=%d commands=%u elev=%s cam=%s,%s",
            tag, (int)i->kind, cr_scene_is_native(s), (int)i->fallback, i->command_count,
            fp_f(i->route_elevation), fp_f(i->camera_x), fp_f(i->camera_y));
    if (!r) { fp_line("SCENE %s route none", tag); return; }
    fp_line("SCENE %s route segs=%d pts=%d len=%s arrow=%s,%s,%s", tag, r->seg_count, r->pt_count,
            fp_f(r->total_length), fp_f(r->arrow_x), fp_f(r->arrow_y), fp_f(r->arrow_angle));
    for (int k = 0; k < r->seg_count; k++) {
        const route_seg_t *g = &r->segs[k];
        fp_line("SCENE %s seg%d type=%d %s %s %s %s c=%s,%s r=%s a=%s..%s", tag, k, g->type, fp_f(g->x0), fp_f(g->y0),
                fp_f(g->x1), fp_f(g->y1), fp_f(g->cx), fp_f(g->cy), fp_f(g->radius), fp_f(g->start_rad), fp_f(g->end_rad));
    }
}

static void dump_state(const char *tag, const maneuver_state_t *m) {
    char buf[320];
    int n = snprintf(buf, sizeof(buf), "icon=%d dir=%d side=%d bap=%d exit=%s junc=%d", m->icon, m->direction,
                     m->driving_side, m->bap_geometry, fp_f(m->exit_angle), m->junction_angle_count);
    for (int i = 0; i < m->junction_angle_count && n < (int)sizeof(buf) - 12; i++)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %s", fp_f(m->junction_angles[i]));
    fp_line("STATE %s %s", tag, buf);
}

/* One iteration of renderer_application_main's loop, drawing unconditionally. */
static void frame(int record) {
    cr_rect_t panel_target, panel_visible;
    float content_frame[3];
    maneuver_state_t *next_ptr;
    render_set_frame_step(1.0f);
    g_now += 1.0 / 30.0;
    fp_clock = g_now;
    if (cr_progress_tick(&g_arrow, g_now)) g_engine.dirty = 1;
    prepare_engine_scenes();
    engine_tick(g_now);
    prepare_engine_scenes();
    if (g_fade_active) {
        g_fade_alpha += FADE_SPEED * render_frame_step();
        if (g_fade_alpha >= 1.0f) { g_fade_alpha = 1.0f; g_fade_active = 0; }
        render_set_global_alpha(g_fade_alpha);
        g_engine.dirty = 1;
    }
    render_get_visible_area(NULL, &panel_target);
    cr_lane_panel_update(g_lane_panel, &g_lane_guidance, panel_target.w, g_now);
    cr_lane_panel_framing(g_lane_panel, g_engine.current_scene, g_engine.has_next ? g_engine.next_scene : NULL,
                          content_frame);
    render_set_content_framing(content_frame[0], content_frame[1], content_frame[2]);
    g_frame_no++;
    if (record) {
        g_frames_recorded++;
        fp_line("FRAME %s #%d phase=%d next=%d pending=%d fade=%s pushing=%d animating=%d render_anim=%d",
                g_unit_name, g_frame_no, (int)g_engine.phase, g_engine.has_next, g_engine.has_pending,
                fp_f(g_fade_alpha), maneuver_is_pushing(), maneuver_is_animating(), render_is_animating());
        dump_state("current", &g_engine.current);
        if (g_engine.has_next) dump_state("next", &g_engine.next);
        dump_scene("current", g_engine.current_scene);
        if (g_engine.has_next) dump_scene("next", g_engine.next_scene);
        fp_line("PANEL frame=%s,%s,%s visible-w=%s", fp_f(content_frame[0]), fp_f(content_frame[1]),
                fp_f(content_frame[2]), fp_f(panel_target.w));
    }
    fp_pause(!record);
    next_ptr = g_engine.has_next ? &g_engine.next : NULL;
    maneuver_prepare_frame(&g_engine.current, next_ptr);
    if (g_force) render_set_route_progress(g_force_fill, g_force_pw, g_force_glow);
    else render_set_route_progress(g_arrow.fill, g_arrow.path_weight, g_arrow.glow);
    render_begin_frame();
    maneuver_draw(&g_engine.current, next_ptr);
    render_get_visible_area(&panel_visible, NULL);
    cr_lane_panel_draw(g_lane_panel, panel_visible, g_now);
    render_debug_grid();
    render_end_frame();
    fp_pause(0);
}

static void settle(void) {
    int i;
    for (i = 0; i < 800; i++) {
        frame(0);
        if (i > 3 && !maneuver_is_animating() && !render_is_animating() && !g_fade_active &&
            g_engine.phase == ENGINE_IDLE && !g_arrow.active && !g_arrow.tint_active)
            break;
    }
    assert(i < 800 && "engine did not settle");
    frame(0);
    frame(0);
}

static void command(const maneuver_state_t *m, int persp) {
    cr_commands_t c;
    memset(&c, 0, sizeof(c));
    c.maneuver = 1;
    c.pending = *m;
    c.flags = MAN_FLAG_SET_PERSP;
    c.perspective = (uint8_t)persp;
    c.state = CR_PROGRESS_OFF;
    apply_commands(&c, g_now);
}

static void show(const maneuver_state_t *m, int persp) {
    command(m, persp);                 /* first command after CLEAR: the reveal path */
    settle();
}

static void write_atlas(const char *path) {
    FILE *f = fopen(path, "wb");
    unsigned char row[128 * 14 * 4];
    assert(f);
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 128 * 14; x++) {
            row[x * 4 + 0] = (unsigned char)(x * 3 + y);
            row[x * 4 + 1] = (unsigned char)((x >> 7) * 18);
            row[x * 4 + 2] = (unsigned char)(y * 2);
            row[x * 4 + 3] = (unsigned char)((x + y) % 7 ? 255 : 0);
        }
        fwrite(row, 1, sizeof(row), f);
    }
    fclose(f);
}

static void setup(int use_scene) {
    maneuver_scene_provider_t provider = { 0 };
    int fb_w, fb_h;
    fp_pause(1);
    fp_clock_on = 1; fp_clock = g_now;
    platform_get_framebuffer_size(&fb_w, &fb_h);
    assert(render_init(fb_w, fb_h) == 0);
    provider.handles = scene_handles; provider.build_route = scene_route;
    provider.paint_masks = scene_paint; provider.elevation = scene_elevation;
    cr_scene_configure_provider(&provider);
    maneuver_set_scene_provider(&provider);
    assert(render_load_flag_atlas("/tmp/parity_flag_atlas.rgba", 128, 128, 14) == 0);
    memset(&g_engine, 0, sizeof(g_engine));
    g_engine.current_scene = cr_scene_create();
    g_engine.next_scene = cr_scene_create();
    g_lane_panel = cr_lane_panel_create();
    assert(g_lane_panel && g_engine.current_scene && g_engine.next_scene);
    if (!use_scene) {
        cr_scene_destroy(g_engine.current_scene);
        cr_scene_destroy(g_engine.next_scene);
        g_engine.current_scene = g_engine.next_scene = NULL;
        maneuver_set_scene_provider(NULL);
    }
    { extern float g_3d_offset_adjust; g_3d_offset_adjust = -.16f; }
    clear_maneuver();
}

/* --- lane panel cases */
static void lane(cr_lane_guidance_t *g, int i, int status, int primary, int n, const int *angles) {
    cr_lane_record_t *l = &g->lanes[i];
    l->position = (uint16_t)i;
    l->status = (uint8_t)status;
    l->primary = (int16_t)primary;
    l->angle_count = (uint8_t)n;
    for (int k = 0; k < n; k++) l->angles[k] = (int16_t)angles[k];
}
static void lanes_show(int which, int showing) {
    cr_lane_guidance_t *g = &g_lane_guidance;
    static const int s[] = { 0 }, sr[] = { 0, 90 }, r[] = { 90 }, l[] = { -90 }, ls[] = { -90, 0 },
                     u[] = { 1000 }, uturn[] = { 180 }, sl[] = { -45 }, sh[] = { 135 }, srl[] = { 0, 90, -90 };
    memset(g, 0, sizeof(*g));
    g->event_index = 40 + which;
    g->showing = (uint8_t)showing;
    g->complete = (uint8_t)(which != 3);
    switch (which) {
    case 0:
        g->count = 3;
        lane(g, 0, 1, 0, 1, s); lane(g, 1, 2, 0, 2, sr); lane(g, 2, 1, 90, 1, r);
        break;
    case 1:
        g->count = 5;
        lane(g, 0, 1, -90, 1, l); lane(g, 1, 1, -90, 2, ls); lane(g, 2, 2, 0, 1, s);
        lane(g, 3, 2, 0, 1, s); lane(g, 4, 1, 90, 1, r);
        break;
    case 2:
        g->count = 8;
        lane(g, 0, 1, 180, 1, uturn); lane(g, 1, 1, -45, 1, sl); lane(g, 2, 1, 1000, 1, u);
        lane(g, 3, 2, 0, 3, srl); lane(g, 4, 2, 0, 1, s); lane(g, 5, 1, 135, 1, sh);
        lane(g, 6, 1, 1000, 0, NULL); lane(g, 7, 0, 90, 1, r);
        break;
    default:
        g->count = 2;
        lane(g, 0, 2, -90, 1, l); lane(g, 1, 1, 90, 1, r);
        break;
    }
}

static void run_unit(const unit_t *u) {
    int i;
    int use_scene = u->kind != K_NOSCENE;
    g_unit_name = u->name;
    setup(use_scene);
    fp_line("UNIT %s", u->name);
    switch (u->kind) {
    case K_SETTLED:
    case K_NOSCENE:
        show(&u->a, u->persp);
        frame(1);
        break;
    case K_SLIDE:
        show(&u->a, u->persp);
        maneuver_set_slide(u->slide);
        frame(1);
        frame(1);
        break;
    case K_PROGRESS:
        show(&u->a, u->persp);
        g_force = 1; g_force_fill = u->fill; g_force_pw = u->pw; g_force_glow = u->glow;
        frame(1);
        break;
    case K_ARROW: {
        /* The wire protocol: progress packets drive the arrow's own spring and glow. */
        static const int steps[][2] = { { 12, CR_PROGRESS_FILL }, { 6, CR_PROGRESS_FILL }, { 0, CR_PROGRESS_FILL },
                                        { 0, CR_PROGRESS_BLINK_HIGH }, { 0, CR_PROGRESS_BLINK_LOW }, { 0, CR_PROGRESS_OFF } };
        show(&u->a, u->persp);
        for (int k = 0; k < 6; k++) {
            engine_set_progress(steps[k][0], steps[k][1], g_now);
            for (i = 0; i < 9; i++) frame(i == 2 || i == 8);
        }
        break;
    }
    case K_PUSH:
    case K_PUSH3:
    case K_PUSH_PROGRESS: {
        int step = u->kind == K_PUSH3 ? 8 : 6;
        show(&u->a, u->persp);
        command(&u->b, u->persp);
        if (u->kind == K_PUSH3) { frame(1); frame(0); frame(0); command(&u->c, u->persp); }
        for (i = 0; i < 1500; i++) {
            int rec = i < 3 || i % step == 0 || (g_engine.phase == ENGINE_PUSHING && !maneuver_is_pushing());
            if (u->kind == K_PUSH_PROGRESS && (i == 3 || i == 20 || i == 60)) {
                engine_set_progress(i == 3 ? 8 : i == 20 ? 4 : 14, CR_PROGRESS_FILL, g_now);
            }
            frame(rec);
            if (i > 3 && g_engine.phase == ENGINE_IDLE && !maneuver_is_animating() && !g_arrow.active) break;
        }
        assert(i < 1500 && "transition did not end");
        frame(1);
        break;
    }
    case K_LANES:
    case K_LANES_PUSH: {
        show(&u->a, u->persp);
        lanes_show(u->lane_case, 1);
        for (i = 0; i < 30; i++) frame(i == 0 || i == 2 || i == 4 || i == 6 || i == 9 || i == 29);
        if (u->kind == K_LANES_PUSH) {
            command(&u->b, u->persp);
            for (i = 0; i < 1500; i++) {
                frame(i < 2 || i % 8 == 0 || (g_engine.phase == ENGINE_PUSHING && !maneuver_is_pushing()));
                if (i > 3 && g_engine.phase == ENGINE_IDLE && !maneuver_is_animating()) break;
            }
        }
        lanes_show(u->lane_case, 0);          /* fade out and reframe */
        for (i = 0; i < 24; i++) frame(i == 0 || i == 3 || i == 8 || i == 23);
        break;
    }
    case K_VISIBLE:
        show(&u->a, u->persp);
        render_set_visible_area(59, 27, 210, 153);
        for (i = 0; i < 24; i++) frame(i == 0 || i == 2 || i == 5 || i == 10 || i == 23);
        render_set_visible_area(0, 0, 328, 180);
        for (i = 0; i < 24; i++) frame(i == 1 || i == 6 || i == 23);
        break;
    case K_REVEAL:
        command(&u->a, u->persp);
        for (i = 0; i < 40; i++) frame(i < 2 || i == 3 || i == 5 || i == 8 || i == 12 || i == 39);
        break;
    case K_DEBUG:
        show(&u->a, u->persp);
        render_set_debug_grid(1);
        frame(1);
        render_set_debug_grid(0);
        maneuver_toggle_debug();
        frame(1);
        break;
    }
    fp_line("END %s frames=%d draws=%lu clears=%lu", u->name, g_frames_recorded, fp_draws, fp_clears);
}

/* ------------------------------------------------------------------ driver */

int main(void) {
    const char *out = getenv("FP_OUT");
    const char *verts = getenv("FP_VERTS");
    unsigned long *tot = mmap(NULL, 4 * sizeof(unsigned long), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int failed = 0;
    assert(tot != MAP_FAILED);
    setvbuf(stderr, NULL, _IOLBF, 0);
    write_atlas("/tmp/parity_flag_atlas.rgba");
    if (fp_open(out ? out : "fp.txt", verts && !strcmp(verts, "full")) != 0) {
        fprintf(stderr, "cannot open output\n");
        return 2;
    }
    build_units();
    fp_line("# maneuver_render geometry fingerprint, floats rounded to 1e-4, %d units", g_nunits);
    for (int i = 0; i < g_nunits; i++) {
        pid_t pid;
        int status;
        fp_flush();
        fflush(NULL);
        pid = fork();
        if (pid == 0) {
            run_unit(&g_units[i]);
            fp_flush();
            tot[0] += g_frames_recorded; tot[1] += fp_draws; tot[2] += fp_clears;
            _exit(0);
        }
        if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "UNIT FAILED: %s (status 0x%x)\n", g_units[i].name, status);
            failed = 1;
        }
    }
    fp_close();
    fprintf(stderr, "fingerprint: %d units, %lu frames, %lu draws, %lu clears\n", g_nunits, tot[0], tot[1], tot[2]);
    return failed;
}
