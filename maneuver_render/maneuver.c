/*
 * Maneuver icon rendering -- procedural GL icons for all CarPlay maneuver types.
 *
 * Single-FBO painter's algorithm:
 *   ROAD mask -- white outline drawn first, grey fill on top (same FBO)
 *   ROUTE -- real 3D extruded mesh (path-based, no mask)
 *
 * Outline and fill are rendered flat into the same FBO (no blending).
 * render_composite() applies materials and perspective (no subtraction).
 * Route is rendered as direct 3D geometry after composite.
 *
 * Copyright (c) 2026 LuKa (@LuKa_dev)
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "render.h"
#include "maneuver.h"
#include "route_path.h"
#include "visual_style.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ================================================================
 * Colors (used as flat mask colors -- lighting applied in composite)
 * ================================================================ */

#define AC_R 0.353f   /* #5AAAE6 */
#define AC_G 0.667f
#define AC_B 0.902f
#define AC_A 1.0f

#define SD_R CR_STYLE_ASPHALT_R
#define SD_G CR_STYLE_ASPHALT_G
#define SD_B CR_STYLE_ASPHALT_B
#define SD_A 1.0f

/* shorthand for passing color args */
#define SIDE    SD_R, SD_G, SD_B, SD_A

/* ================================================================
 * Arrow dimensions (NDC)
 * ================================================================ */

/* Road thickness layers */
#define SHAFT_T    CR_STYLE_ARROW_WIDTH
#define OL_W       CR_STYLE_SHOULDER_WIDTH
#define SIDE_T     CR_STYLE_ROAD_WIDTH
#define OL_T       (SIDE_T + OL_W * 2)
#define HEAD_SZ    (SHAFT_T * 1.3f)      /* arrowhead height */

/* Universal road lengths from junction center (y=0) */
#define ROAD_LEN   0.55f    /* solid road length from junction (all directions) */
#define BLUE_LEN   0.50f    /* blue active road length from junction */
#define FADE_LEN   0.30f    /* fade distance beyond solid road */

/* Derived positions */
#define SHAFT_BOT  (-ROAD_LEN)           /* entry solid bottom */
#define SHAFT_TOP  (BLUE_LEN)            /* straight arrow tip */
#define SIDE_TOP   (ROAD_LEN)            /* forward road solid end */

/* Joint helpers */
#define JOINT_R    (SHAFT_T * 0.5f)
#define JOINT_SEG  32
#define WHITE  CR_STYLE_SHOULDER_R, CR_STYLE_SHOULDER_G, CR_STYLE_SHOULDER_B, 1.0f

/* Roundabout */
#define RAB_RING_R   0.28f   /* roundabout ring radius */
#define RAB_RING_SEG 64      /* ring/arc segments */

/* U-turn */
#define UTURN_GAP    0.18f   /* half-distance between entry/exit shafts */
#define UTURN_TOP    0.30f   /* top of U arc */
#define UTURN_ARROW  (-0.10f) /* arrowhead Y on exit side */
#define UTURN_SEG    16      /* arc segments */

/* Angle thresholds */
#define ANGLE_DEDUP     5.0f    /* degrees: angles closer than this are "same direction" */
#define ANGLE_DEDUP_RAD 0.09f   /* radians: same threshold (~5deg) for roundabout internals */

/* Lane change / merge lateral offsets */
#define LANE_SHIFT   0.35f   /* lane change lateral offset */
#define MERGE_OFFSET 0.38f   /* merge ramp lateral offset */
#define BEND_LO     (-0.15f) /* lower S-bend point */
#define BEND_HI      0.15f   /* upper S-bend point */
#define MERGE_BEND_LO (-0.10f) /* merge lower S-bend point */

/* Arrived — concentric circle destination marker (matches stock HUD icon)
 *    ___
 *  (( . ))   outer = outline only (transparent inside)
 *    |X|     inner = filled (road end cap)
 *    |X|     road shaft
 */
#define ARRIVE_CENTER_Y   0.12f       /* shared center of both circles */
#define ARRIVE_INNER_R    0.18f       /* destination geometry independent of road stroke */
#define ARRIVE_OUTER_R    0.28f       /* outer circle: outline ring only */
#define ARRIVE_ROAD_TOP   ARRIVE_CENTER_Y  /* road extends into inner circle center */
#define ARRIVE_SEG        24          /* circle segment count */
#define SNAP_SENTINEL     999.0f      /* large value for angle snap comparison */

/* Destination flag sprite */
#define ARRIVE_FLAG_SZ    0.18f       /* half-extent of flag sprite */
#define FLAG_ANIM_SPEED   0.35f       /* frames per render frame */

/* Route extrusion heights */
#define ROUTE_BASE_Y  CR_STYLE_ROUTE_BASE_Y
#define ROUTE_TOP_Y   CR_STYLE_ROUTE_TOP_Y

/* Cached route mesh -- rebuilt only on maneuver state change */
static route_path_t g_route_path;
static route_mesh_t g_route_mesh;
static route_mesh_t g_route_tail_mesh;
static maneuver_scene_provider_t g_scene_provider;

void maneuver_set_scene_provider(const maneuver_scene_provider_t *provider) {
    if(provider && provider->build_route && provider->paint_masks)g_scene_provider=*provider;
    else memset(&g_scene_provider,0,sizeof(g_scene_provider));
    render_invalidate_masks();
}
static int scene_supplied(const maneuver_state_t *state) {
    return g_scene_provider.build_route && (!g_scene_provider.handles ||
        g_scene_provider.handles(g_scene_provider.context,state));
}
static void build_scene_route(const maneuver_state_t *state,route_path_t *path) {
    if(scene_supplied(state))g_scene_provider.build_route(g_scene_provider.context,state,path);
    else maneuver_build_route(state,path);
}
static float route_base_y(void) { return g_scene_provider.build_route?g_scene_provider.route_base_y:ROUTE_BASE_Y; }
static float route_top_y(void) { return g_scene_provider.build_route?g_scene_provider.route_top_y:ROUTE_TOP_Y; }

/* Route animation state -- sliding window */
static float g_route_slide = 1.0f;     /* slide parameter 0..2 (0=hidden, 1=in position, 2=pushed out) */
static int   g_route_animating = 0;    /* 1 while any slide animation running */
static float g_anim_start = 0.0f;     /* slide value at animation start */
static float g_anim_target = 1.0f;    /* slide target (1.0 = arrive, 2.0 = push out) */
static float g_route_pre_frac  = 0.0f; /* fraction of extended path before original start */
static float g_route_end_frac  = 1.0f; /* fraction of extended path at original end */
static float g_t_head = 1.0f;          /* computed head fraction for extrusion */
static int   g_route_debug = 0;        /* debug overlay toggle */
static float g_slug_override = -1.0f; /* >0: speed normalization length for combined path */
static float g_flag_frame = 0.0f;     /* destination flag animation frame */
static float g_tip_morph_t = 1.0f;   /* 0→1 independent timer for arrow→bulb morph */
static int   g_tip_morph_active = 0; /* 1 when morph animation is running */
#define TIP_MORPH_SPEED  0.07f       /* per frame (~0.5s at 30fps) */
static int   g_flag_active = 0;      /* 1 when showing arrived icon (flag animating) */
static float g_arrive_flag_dx = 0.0f; /* flag position on the outer circle */
static float g_arrive_flag_dy = 0.0f;
static float g_combined_flag_x = 0.0f; /* next maneuver's flag in combined path space */
static float g_combined_flag_y = 0.0f;
static int   g_masks_only_mode = 0;   /* append masks without composite/route draw */
static int   g_combined_window_active = 0;
static float g_combined_start_head_dist = 0.0f;
static float g_combined_end_head_dist = 0.0f;
static float g_next_cam_pan_x = 0.0f;
static float g_next_cam_pan_y = 0.0f;
static float g_next_cam_rot = 0.0f;
static float g_next_cam_intro_end_dist = 0.0f;
static float g_next_cam_follow_dist = 0.0f;
static float g_next_cam_release_end_dist = 0.0f;
static float g_xfade_mid_dist = 0.0f;  /* midpoint between maneuver zones on combined path */
static float g_man_cam_rot = 0.0f;
static float g_man_cam_x = 0.0f,g_man_cam_y = 0.0f;
static float g_cam_settle_start_x = 0.0f;
static float g_cam_settle_start_y = 0.0f;
static float g_cam_settle_start_rot = 0.0f;
static float g_cam_settle_t = 0.0f;
static int   g_cam_settle_active = 0;

static float g_light_settle_start_rot = 0.0f;
static float g_light_settle_t = 0.0f;
static int   g_light_settle_active = 0;
static float g_last_combined_rot = 0.0f;
#define ROUTE_SPEED_PEAK 0.045f         /* peak animation speed (NDC/frame on straight) */
#define ROUTE_SPEED_MIN  0.010f         /* minimum speed on sharpest turns */
#define ROUTE_EXTEND     RPATH_ANIMATION_EXTENSION           /* extension length beyond viewport */
#define ROUTE_TAIL_FADE_DIST 0.35f      /* only the pre-extension section that is normally off-screen */
#define ROUTE_TAIL_FADE_SLICES 24
#define TRANSITION_PAD   0.15f          /* minimum clear road between maneuver road zones */
#define XFADE_HALF       0.20f          /* spatial crossfade half-width in path distance */
#define CURV_WINDOW      3              /* points each side for curvature sampling */
#define CURV_SLOWDOWN    8.0f           /* curvature sensitivity (higher = more slowdown) */
#define CAMERA_SETTLE_SPEED 0.12f
#define CAMERA_SETTLE_MAX_ROT 0.45f
#define LIGHT_SETTLE_SPEED (1.0f / 50.0f)

static void clear_combined_transition(void) {
    g_slug_override = -1.0f;
    g_combined_window_active = 0;
    g_combined_start_head_dist = 0.0f;
    g_combined_end_head_dist = 0.0f;
    g_next_cam_pan_x = 0.0f;
    g_next_cam_pan_y = 0.0f;
    g_next_cam_rot = 0.0f;
    g_next_cam_intro_end_dist = 0.0f;
    g_next_cam_follow_dist = 0.0f;
    g_next_cam_release_end_dist = 0.0f;
    g_xfade_mid_dist = 0.0f;
    g_last_combined_rot = 0.0f;
}

static void clear_camera_settle(void) {
    g_cam_settle_start_x = 0.0f;
    g_cam_settle_start_y = 0.0f;
    g_cam_settle_start_rot = 0.0f;
    g_cam_settle_t = 0.0f;
    g_cam_settle_active = 0;
}

static void clear_light_settle(void) {
    g_light_settle_start_rot = 0.0f;
    g_light_settle_t = 0.0f;
    g_light_settle_active = 0;
    render_set_light_rotation(0.0f);
}

static void rpath_sample(const route_path_t *p, float t, float *out_x, float *out_y);

/* Spatial crossfade: 0.0 = fully current maneuver, 1.0 = fully next maneuver.
 * Based on arrow head distance along combined path relative to the midpoint
 * between maneuver road zones. Smoothstepped over ±XFADE_HALF. */
static float spatial_xfade(void) {
    float total, head_dist, rel;
    if (!g_combined_window_active || g_xfade_mid_dist <= 0.0f)
        return 0.0f;
    total = g_route_path.total_length;
    if (total < 1e-6f) return 0.0f;
    head_dist = g_t_head * total;
    rel = (head_dist - g_xfade_mid_dist) / XFADE_HALF;
    return rpath_smooth01((rel + 1.0f) * 0.5f);
}

int maneuver_is_animating(void) {
    /* Only route slide + camera settle gate engine transitions.
     * g_flag_active excluded — loops forever (ARRIVED flag animation).
     * g_light_settle_active excluded — purely cosmetic lighting blend
     * (~1.7s), must not delay queued icon promotion. Both still drive
     * continuous rendering via maneuver_needs_redraw(). */
    return g_route_animating || g_cam_settle_active;
}

int maneuver_needs_redraw(void) {
    return g_route_animating || g_flag_active || g_cam_settle_active
        || g_light_settle_active || (g_tip_morph_active && g_tip_morph_t < 1.0f);
}

void maneuver_set_slide(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 2.0f) t = 2.0f;
    g_route_slide = t;
    g_route_animating = 0;
    clear_combined_transition();
    clear_camera_settle();
    clear_light_settle();
}

void maneuver_start_push(void) {
    /* Seamless: keep current slide, just retarget to 2.0 */
    g_anim_start = g_route_slide;
    g_anim_target = 2.0f;
    g_route_animating = 1;
    clear_combined_transition();
    clear_camera_settle();
    clear_light_settle();
}

int maneuver_is_pushing(void) {
    return g_route_animating && g_anim_target > 1.5f;
}

void maneuver_toggle_debug(void) {
    g_route_debug = !g_route_debug;
}

int maneuver_is_debug(void) {
    return g_route_debug;
}

/* Point and unit direction at the start (at_end 0) or end (at_end 1) of a path segment. */
static void seg_dir(const route_seg_t *s, int at_end,
                    float *x, float *y, float *dx, float *dy) {
    if (s->type == RSEG_LINE) {
        *x = at_end ? s->x1 : s->x0;
        *y = at_end ? s->y1 : s->y0;
        float ddx = s->x1 - s->x0, ddy = s->y1 - s->y0;
        float len = sqrtf(ddx * ddx + ddy * ddy);
        if (len < 1e-6f) { *dx = 0; *dy = 1; }
        else { *dx = ddx / len; *dy = ddy / len; }
    } else {
        float a = at_end ? s->end_rad : s->start_rad;
        *x = s->cx + s->radius * cosf(a);
        *y = s->cy + s->radius * sinf(a);
        float sign = (s->end_rad - s->start_rad >= 0) ? 1.0f : -1.0f;
        *dx = -sign * sinf(a);
        *dy =  sign * cosf(a);
    }
}


/* Insert fillet arcs at segment junctions where tangent directions don't match.
 * Trims line segments linearly, arc segments by angular offset (stays on curve).
 * Must be called before rpath_extend/rpath_densify. */
static void rpath_fillet_junctions(route_path_t *p, float fillet_r) {
    if (p->seg_count < 2) return;

    route_seg_t new_segs[RPATH_MAX_SEGS];
    int new_count = 0;
    new_segs[new_count++] = p->segs[0];

    int i;
    for (i = 1; i < p->seg_count; i++) {
        if (new_count >= RPATH_MAX_SEGS - 2) {
            new_segs[new_count++] = p->segs[i];
            continue;
        }

        /* Junction: exit of prev, entry of next */
        float ex, ey, edx, edy;
        seg_dir(&new_segs[new_count - 1], 1, &ex, &ey, &edx, &edy);
        float sx, sy, sdx, sdy;
        seg_dir(&p->segs[i], 0, &sx, &sy, &sdx, &sdy);

        float dot = edx * sdx + edy * sdy;
        float alpha = acosf(dot < -1.0f ? -1.0f : (dot > 1.0f ? 1.0f : dot));

        if (alpha < 0.09f || alpha > (float)M_PI * 0.97f) {
            new_segs[new_count++] = p->segs[i];
            continue;
        }

        float vertex_half = ((float)M_PI - alpha) * 0.5f;
        float tan_vh = tanf(vertex_half);
        if (tan_vh < 1e-6f) tan_vh = 1e-6f;
        float t_dist = fillet_r / tan_vh;

        /* Segment arc-lengths for clamping */
        route_seg_t *prev = &new_segs[new_count - 1];
        route_seg_t *cur = &p->segs[i];
        float prev_len = (prev->type == RSEG_LINE)
            ? sqrtf((prev->x1-prev->x0)*(prev->x1-prev->x0)+(prev->y1-prev->y0)*(prev->y1-prev->y0))
            : fabsf(prev->end_rad - prev->start_rad) * prev->radius;
        float cur_len = (cur->type == RSEG_LINE)
            ? sqrtf((cur->x1-cur->x0)*(cur->x1-cur->x0)+(cur->y1-cur->y0)*(cur->y1-cur->y0))
            : fabsf(cur->end_rad - cur->start_rad) * cur->radius;
        float max_t = 0.45f * (prev_len < cur_len ? prev_len : cur_len);
        if (t_dist > max_t) t_dist = max_t;
        float actual_r = t_dist * tan_vh;

        /* --- Trim prev segment, get T1 and tangent at T1 --- */
        float t1x, t1y, t1dx, t1dy;
        if (prev->type == RSEG_LINE) {
            t1x = prev->x1 - edx * t_dist;
            t1y = prev->y1 - edy * t_dist;
            t1dx = edx; t1dy = edy;
            prev->x1 = t1x; prev->y1 = t1y;
        } else {
            float sweep = prev->end_rad - prev->start_rad;
            float da = t_dist / prev->radius;
            /* Trim end: move end_rad back towards start */
            float new_end = (sweep > 0) ? prev->end_rad - da : prev->end_rad + da;
            prev->end_rad = new_end;
            t1x = prev->cx + prev->radius * cosf(new_end);
            t1y = prev->cy + prev->radius * sinf(new_end);
            float s = (sweep > 0) ? 1.0f : -1.0f;
            t1dx = -s * sinf(new_end);
            t1dy =  s * cosf(new_end);
        }

        /* --- Trim next segment, get T2 and tangent at T2 --- */
        float t2x, t2y, t2dx, t2dy;
        route_seg_t trimmed = *cur;
        if (trimmed.type == RSEG_LINE) {
            t2x = trimmed.x0 + sdx * t_dist;
            t2y = trimmed.y0 + sdy * t_dist;
            t2dx = sdx; t2dy = sdy;
            trimmed.x0 = t2x; trimmed.y0 = t2y;
        } else {
            float sweep = trimmed.end_rad - trimmed.start_rad;
            float da = t_dist / trimmed.radius;
            /* Trim start: move start_rad forward towards end */
            float new_start = (sweep > 0) ? trimmed.start_rad + da : trimmed.start_rad - da;
            trimmed.start_rad = new_start;
            t2x = trimmed.cx + trimmed.radius * cosf(new_start);
            t2y = trimmed.cy + trimmed.radius * sinf(new_start);
            float s = (sweep > 0) ? 1.0f : -1.0f;
            t2dx = -s * sinf(new_start);
            t2dy =  s * cosf(new_start);
        }

        /* --- Compute fillet arc from actual on-curve T1/T2 and tangents --- */
        float cross2 = t1dx * t2dy - t1dy * t2dx;

        /* Perpendicular to T1 tangent, towards inside of turn */
        float perp_x, perp_y;
        if (cross2 > 0) {
            perp_x = -t1dy; perp_y = t1dx;
        } else {
            perp_x = t1dy; perp_y = -t1dx;
        }

        /* Solve for radius so circle through T1 also passes through T2.
         * center = T1 + perp * r.  Need |center - T2| = r.
         * => r = -(dx²+dy²) / (2*(dx*perp_x + dy*perp_y))
         * where dx = t1x-t2x, dy = t1y-t2y */
        {
            float dx = t1x - t2x, dy = t1y - t2y;
            float d_perp = dx * perp_x + dy * perp_y;
            if (fabsf(d_perp) > 1e-6f) {
                actual_r = -(dx * dx + dy * dy) / (2.0f * d_perp);
                if (actual_r < 0.0f) { perp_x = -perp_x; perp_y = -perp_y; actual_r = -actual_r; }
            }
        }
        float fcx = t1x + perp_x * actual_r;
        float fcy = t1y + perp_y * actual_r;

        float sa = atan2f(t1y - fcy, t1x - fcx);
        float ea = atan2f(t2y - fcy, t2x - fcx);

        if (cross2 > 0) {
            while (ea < sa) ea += 2.0f * (float)M_PI;
        } else {
            while (ea > sa) ea -= 2.0f * (float)M_PI;
        }

        /* Insert fillet arc segment */
        route_seg_t *f = &new_segs[new_count++];
        f->type = RSEG_ARC;
        f->cx = fcx; f->cy = fcy;
        f->radius = actual_r;
        f->start_rad = sa;
        f->end_rad = ea;
        f->x0 = f->x1 = 0; f->y0 = f->y1 = 0;

        new_segs[new_count++] = trimmed;
    }

    memcpy(p->segs, new_segs, new_count * sizeof(route_seg_t));
    p->seg_count = new_count;
}

/* Extend path with pre/post segments for sliding animation */
static void rpath_extend(route_path_t *p) {
    if (p->seg_count < 1 || p->seg_count + 2 > RPATH_MAX_SEGS) return;

    float sx, sy, sdx, sdy;
    seg_dir(&p->segs[0], 0, &sx, &sy, &sdx, &sdy);

    float ex, ey, edx, edy;
    seg_dir(&p->segs[p->seg_count - 1], 1, &ex, &ey, &edx, &edy);

    /* Shift segments up by 1 to make room for pre-segment */
    int i;
    for (i = p->seg_count - 1; i >= 0; i--)
        p->segs[i + 1] = p->segs[i];

    /* Pre-segment: extend backwards from first point */
    p->segs[0].type = RSEG_LINE;
    p->segs[0].x0 = sx - sdx * ROUTE_EXTEND;
    p->segs[0].y0 = sy - sdy * ROUTE_EXTEND;
    p->segs[0].x1 = sx;
    p->segs[0].y1 = sy;
    p->seg_count++;

    /* Post-segment: extend forward from last point */
    p->segs[p->seg_count].type = RSEG_LINE;
    p->segs[p->seg_count].x0 = ex;
    p->segs[p->seg_count].y0 = ey;
    p->segs[p->seg_count].x1 = ex + edx * ROUTE_EXTEND;
    p->segs[p->seg_count].y1 = ey + edy * ROUTE_EXTEND;
    p->seg_count++;
}

/* Derivative of smoothstep -- gives speed multiplier at position t.
 * Peak (1.5) at t=0.5, zero at t=0 and t=1. */
static float ease_inout_speed(float t) {
    if (t <= 0.0f || t >= 1.0f) return 0.0f;
    return 6.0f * t * (1.0f - t);
}

/* Compute curvature factor at fraction t along the densified path.
 * Returns 0..1: 1.0 = straight, 0.0 = sharpest turn.
 * Measures direction change over a small window of points. */
static float path_curvature_factor(const route_path_t *p, float t) {
    if (p->pt_count < 3 || p->total_length < 1e-6f) return 1.0f;

    float target_dist = t * p->total_length;
    int idx = 1;
    while (idx < p->pt_count - 1 && p->dist[idx] < target_dist) idx++;

    int lo = idx - CURV_WINDOW;
    int hi = idx + CURV_WINDOW;
    if (lo < 0) lo = 0;
    if (hi >= p->pt_count) hi = p->pt_count - 1;
    if (hi - lo < 2) return 1.0f;

    float d0x = p->px[lo + 1] - p->px[lo], d0y = p->py[lo + 1] - p->py[lo];
    float d1x = p->px[hi] - p->px[hi - 1], d1y = p->py[hi] - p->py[hi - 1];
    float len0 = sqrtf(d0x * d0x + d0y * d0y);
    float len1 = sqrtf(d1x * d1x + d1y * d1y);
    if (len0 < 1e-6f || len1 < 1e-6f) return 1.0f;

    float dot = (d0x * d1x + d0y * d1y) / (len0 * len1);
    if (dot > 1.0f) dot = 1.0f;
    if (dot < -1.0f) dot = -1.0f;

    float angle = acosf(dot);
    /* Gentle arcs (roundabout ring) get mild slowdown, sharp junctions get full */
    if (angle < 0.3f) return 0.7f;  /* mild slowdown on gentle arcs */
    float factor = 1.0f / (1.0f + CURV_SLOWDOWN * angle);
    return factor;
}

/* Sample position on densified path at fraction t (0..1). */
static void rpath_sample(const route_path_t *p, float t, float *out_x, float *out_y) {
    if (p->pt_count < 2) { *out_x = 0; *out_y = 0; return; }
    *out_x = p->px[p->pt_count - 1];
    *out_y = p->py[p->pt_count - 1];
    rpath_locate(p, t * p->total_length, out_x, out_y);
}

static float rpath_sample_heading(const route_path_t *p, float t) {
    float x, y;
    int i;

    if (p->pt_count < 2)
        return (float)(M_PI * 0.5);

    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    for (i = rpath_locate(p, t * p->total_length, &x, &y); i > 0 && i < p->pt_count; i++) {
        float dx = p->px[i] - p->px[i - 1];
        float dy = p->py[i] - p->py[i - 1];
        if (dx * dx + dy * dy > 1e-8f)
            return atan2f(dy, dx);
    }

    {
        float dx = p->px[p->pt_count - 1] - p->px[p->pt_count - 2];
        float dy = p->py[p->pt_count - 1] - p->py[p->pt_count - 2];
        if (dx * dx + dy * dy > 1e-8f)
            return atan2f(dy, dx);
    }

    return (float)(M_PI * 0.5);
}

static float wrap_angle(float a) {
    while (a > (float)M_PI) a -= 2.0f * (float)M_PI;
    while (a < -(float)M_PI) a += 2.0f * (float)M_PI;
    return a;
}

static float lerp_angle(float a, float b, float t) {
    return a + wrap_angle(b - a) * t;
}



static float light_settle_curve(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    /* Single smooth curve: critically damped spring feel.
     * Starts near zero velocity, gently accelerates, small overshoot, settle. */
    float u = rpath_smooth01(t);  /* smoothstep base */
    float overshoot = 4.2f * t * t * (1.0f - t) * (1.0f - t);  /* bell at ~0.5 */
    return u + overshoot * 0.18f;
}

static void apply_camera_pose(float pan_x, float pan_y, float rot) {
    g_man_cam_x=pan_x;g_man_cam_y=pan_y;
    g_man_cam_rot = rot;
    render_set_camera_pan(pan_x, pan_y);
    render_set_camera_rotation(rot);
    render_sync_camera();
}

static void set_default_camera(void) {
    apply_camera_pose(0.0f, 0.0f, 0.0f);
}

static void update_light_settle(void) {
    float blend;

    if (!g_light_settle_active) {
        render_set_light_rotation(0.0f);
        return;
    }

    blend = light_settle_curve(g_light_settle_t);
    render_set_light_rotation(lerp_angle(g_light_settle_start_rot, 0.0f, blend));

    g_light_settle_t += LIGHT_SETTLE_SPEED * render_frame_step();
    if (g_light_settle_t >= 1.0f)
        clear_light_settle();
}

static void update_camera_settle(void) {
    float blend;

    if (!g_cam_settle_active) {
        set_default_camera();
        return;
    }

    /* Ease-out (quadratic): starts at full velocity, decelerates to stop.
     * Avoids the velocity hiccup when transitioning from linear slide tracking. */
    blend = 1.0f - (1.0f - g_cam_settle_t) * (1.0f - g_cam_settle_t);
    apply_camera_pose(g_cam_settle_start_x * (1.0f - blend),
                      g_cam_settle_start_y * (1.0f - blend),
                      lerp_angle(g_cam_settle_start_rot, 0.0f, blend));

    g_cam_settle_t += CAMERA_SETTLE_SPEED * render_frame_step();
    if (g_cam_settle_t >= 1.0f) {
        clear_camera_settle();
        set_default_camera();
    }
}

static float g_release_cam_x = 0.0f, g_release_cam_y = 0.0f;

static void update_combined_camera(void) {
    float total = g_route_path.total_length;
    float head_dist;
    float intro_dist;
    float follow_t;
    float follow_x, follow_y;
    float follow_rot;

    if (total < 1e-6f) {
        set_default_camera();
        return;
    }

    head_dist = g_t_head * total;
    intro_dist = g_next_cam_intro_end_dist;
    follow_t = (g_next_cam_follow_dist > 0.0f) ? (g_next_cam_follow_dist / total) : 0.0f;
    if (follow_t < 0.0f) follow_t = 0.0f;
    if (follow_t > 1.0f) follow_t = 1.0f;

    if (head_dist <= intro_dist) {
        float denom = intro_dist - g_combined_start_head_dist;
        float blend = (denom > 1e-4f) ? (head_dist - g_combined_start_head_dist) / denom : 1.0f;

        rpath_sample(&g_route_path, g_t_head, &follow_x, &follow_y);
        follow_rot = rpath_sample_heading(&g_route_path, g_t_head) - (float)(M_PI * 0.5);
        blend = 2.0f * blend - blend * blend;
        apply_camera_pose(follow_x * blend, follow_y * blend,
                          lerp_angle(0.0f, follow_rot, blend));
        return;
    }

    if (head_dist <= g_next_cam_follow_dist) {
        rpath_sample(&g_route_path, g_t_head, &follow_x, &follow_y);
        follow_rot = rpath_sample_heading(&g_route_path, g_t_head) - (float)(M_PI * 0.5);
        g_release_cam_x = follow_x;
        g_release_cam_y = follow_y;
        apply_camera_pose(follow_x, follow_y, follow_rot);
        return;
    }

    /* Rotation: fixed anchor at follow_t (no spin on curved paths) */
    follow_rot = rpath_sample_heading(&g_route_path, follow_t) - (float)(M_PI * 0.5);

    /* Position: spring damper from last FOLLOW pos (held in g_release_cam)
     * toward target. Inherently velocity-continuous — no blend curve needed. */
    {
        float denom = g_next_cam_release_end_dist - g_next_cam_follow_dist;
        float blend = (denom > 1e-4f) ? (head_dist - g_next_cam_follow_dist) / denom : 1.0f;
        float cam_rot;
        float spring = 0.07f;

        if (blend > 1.0f) blend = 1.0f;

        g_release_cam_x += (g_next_cam_pan_x - g_release_cam_x) * spring;
        g_release_cam_y += (g_next_cam_pan_y - g_release_cam_y) * spring;

        /* Smoothstep: derivative=0 at both ends.
         * Avoids velocity spike at FOLLOW→RELEASE boundary. */
        cam_rot = lerp_angle(follow_rot, g_next_cam_rot, rpath_smooth01(blend));
        apply_camera_pose(g_release_cam_x, g_release_cam_y, cam_rot);
    }
}

/* Check if a maneuver needs elevation (path overlaps itself).
 * Returns lift in world units (0 = flat). Road thickness is 0.03,
 * so 0.06 lift clears the road below with a gap.
 * Roundabout: |exit_angle|>150 means exit stub near entry stub.
 * Turn: |exit_angle|>150 means near-U-turn. */
#define ELEV_LIFT 0.06f
float maneuver_builtin_elevation(const maneuver_state_t *s) {
    if (!s) return 0.0f;
    switch (s->icon) {
    case ICON_ROUNDABOUT:
    case ICON_TURN: {
        float a = s->exit_angle;
        if (a < 0) a = -a;
        if (a > 150) return ELEV_LIFT;
        return 0.0f;
    }
    default:
        return 0.0f;
    }
}

static float maneuver_elevation(const maneuver_state_t *s) {
    if (scene_supplied(s))
        return g_scene_provider.elevation ? g_scene_provider.elevation(g_scene_provider.context,s) : 0.0f;
    return maneuver_builtin_elevation(s);
}

void maneuver_prepare_frame(const maneuver_state_t *s, const maneuver_state_t *next_state) {
    int combined = (next_state != NULL && maneuver_is_pushing());

    if (s == NULL)
        return;

    if (!combined) {
        update_camera_settle();
        update_light_settle();
        /* Standalone: no ramp restart */
        rpath_set_ramp_restart(-1.0f);
        { float e = maneuver_elevation(s); rpath_set_elevation(e, e); }
    }
}

/* Compute the route window (g_route_pre_frac/end_frac, g_t_head) from g_route_slide.
 * slide 0..1 = slide-in (normal), slide 1..2 = push-out through exit. */
static void compute_slide_params(void) {
    if (g_route_path.total_length < 1e-6f) {
        g_route_pre_frac = 0.0f;
        g_route_end_frac = 1.0f;
        g_t_head = (g_route_slide < 1.0f) ? g_route_slide : 1.0f;
        return;
    }
    g_route_pre_frac = ROUTE_EXTEND / g_route_path.total_length;
    g_route_end_frac = (g_route_path.total_length - ROUTE_EXTEND) / g_route_path.total_length;

    if (g_combined_window_active) {
        float range = g_anim_target - g_anim_start;
        float progress = (range > 0.01f) ? (g_route_slide - g_anim_start) / range : 1.0f;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;

        float head_dist = g_combined_start_head_dist +
                          (g_combined_end_head_dist - g_combined_start_head_dist) * progress;

        if (head_dist < 0.0f) head_dist = 0.0f;
        if (head_dist > g_route_path.total_length) head_dist = g_route_path.total_length;

        g_t_head = head_dist / g_route_path.total_length;
        return;
    }

    /* slug_frac = fraction of path that the arrow covers.
     * For single maneuver: entire road = end_frac - pre_frac.
     * For combined path: one maneuver's worth (stored in g_slug_override). */
    float slug_frac;
    if (g_slug_override > 0.0f)
        slug_frac = g_slug_override / g_route_path.total_length;
    else
        slug_frac = g_route_end_frac - g_route_pre_frac;

    g_t_head = g_route_pre_frac + g_route_slide * slug_frac;
    /* Clamp to valid range -- push-out moves head forward */
    if (g_t_head > 1.0f) g_t_head = 1.0f;
}

/* Extrude the full route into g_route_mesh.  The tail stays at the path start;
 * route visibility is driven by the head only. */
static void route_extrude_body(void) {
    rpath_extrude(&g_route_path, &g_route_mesh, SHAFT_T,
                  route_base_y(), route_top_y(), 0.0f, g_t_head);
}

/* Draw the route mesh. */
static void route_draw_window(float t0, float t1, float alpha,
                              int cap_start, int cap_end, int tip_end) {
    if (alpha <= 0.001f || t1 - t0 <= 1e-5f)
        return;

    rpath_extrude_partial(&g_route_path, &g_route_tail_mesh, SHAFT_T,
                          route_base_y(), route_top_y(), t0, t1,
                          cap_start, cap_end, tip_end);
    rpath_draw(&g_route_tail_mesh, AC_R, AC_G, AC_B, AC_A * alpha);
}

static void route_draw_with_fade(void) {
    if (g_combined_window_active && g_route_path.total_length > 1e-6f) {
        float fade_t = ROUTE_TAIL_FADE_DIST / g_route_path.total_length;
        int i;

        if (fade_t > g_t_head)
            fade_t = g_t_head;

        if (fade_t > 1e-5f) {
            for (i = 0; i < ROUTE_TAIL_FADE_SLICES; i++) {
                float u0 = (float)i / (float)ROUTE_TAIL_FADE_SLICES;
                float u1 = (float)(i + 1) / (float)ROUTE_TAIL_FADE_SLICES;
                float seg_t0 = fade_t * u0;
                float seg_t1 = fade_t * u1;
                int final_tail_slice = (i == ROUTE_TAIL_FADE_SLICES - 1 && fade_t >= g_t_head - 1e-5f);

                route_draw_window(seg_t0, seg_t1, rpath_smooth01(u1),
                                  (i == 0), final_tail_slice, final_tail_slice);
            }
        }

        if (fade_t < g_t_head - 1e-5f) {
            route_draw_window(fade_t, g_t_head, 1.0f, 0, 1, 1);
        }
        return;
    }

    rpath_draw(&g_route_mesh, AC_R, AC_G, AC_B, AC_A);
}

/* ================================================================
 * Fading road helper
 *
 * Draws a fading extension from (x0,y0)->(x1,y1) in N segments with
 * alpha going from 1->0 via smoothstep.
 *
 * fill: 0 = white border strips only (outline pass)
 *       1 = grey center fill only (fill pass)
 * ================================================================ */

#define FADE_SEGS    64

static void draw_fading_road(float x0, float y0, float x1, float y1, int fill) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-6f) return;

    int i;
    for (i = 0; i < FADE_SEGS; i++) {
        float t0 = (float)i / FADE_SEGS;
        float t1 = (float)(i + 1) / FADE_SEGS;
        float sx = x0 + dx * t0, sy = y0 + dy * t0;
        float ex = x0 + dx * t1, ey = y0 + dy * t1;
        float alpha = rpath_smooth01(1.0f - t0);
        if (alpha < 0.01f) break;

        if (!fill) {
            /* Thin curbs only: translucent asphalt must never reveal a
             * full-width white backing during the fade. */
            float nx = -dy / len, ny = dx / len;
            float offset = (SIDE_T + OL_W) * .5f;
            for (int side = -1; side <= 1; side += 2) {
                float ox = nx * offset * side, oy = ny * offset * side;
                render_thick_line(sx + ox, sy + oy, ex + ox, ey + oy, OL_W,
                                  CR_STYLE_SHOULDER_R, CR_STYLE_SHOULDER_G, CR_STYLE_SHOULDER_B, alpha);
            }
        } else {
            /* Grey center fill -- overwrites outline interior (painter's algorithm) */
            render_thick_line(sx, sy, ex, ey,
                              SIDE_T, SD_R, SD_G, SD_B, alpha);
        }
    }
}

/* Polyline through n points with JOINT_R arc fillets at the interior vertices. */
static void build_filleted_path(route_path_t *p, const float *xs, const float *ys, int n) {
    int i;
    rpath_clear(p);
    for (i = 1; i < n; i++)
        rpath_add_line(p, xs[i - 1], ys[i - 1], xs[i], ys[i]);
    rpath_fillet_junctions(p, JOINT_R);
}

/* ================================================================
 * Roundabout shared helpers
 * ================================================================ */

typedef struct {
    float snapped_deg;  /* snapped exit angle in degrees */
    float exit_rad;     /* math radians: (90 - snapped_deg) * PI / 180 */
    int   snapped_idx;  /* index into junction_angles, or -1 if no snap */
} rab_snap_t;

static rab_snap_t snap_roundabout_exit(float exit_angle_deg,
                                       const float *junction_angles,
                                       int junction_angle_count, int bap_geometry) {
    rab_snap_t r;
    r.snapped_deg = exit_angle_deg;
    r.snapped_idx = -1;
    if (!bap_geometry && junction_angle_count > 0) {
        int best = 0;
        float best_diff = SNAP_SENTINEL;
        int i;
        for (i = 0; i < junction_angle_count && i < MAX_JUNCTION_ANGLES; i++) {
            float diff = fabsf((float)junction_angles[i] - exit_angle_deg);
            if (diff > 180.0f) diff = 360.0f - diff;
            if (diff < best_diff) { best_diff = diff; best = i; }
        }
        if (best_diff < 30.0f) {
            r.snapped_deg = (float)junction_angles[best];
            r.snapped_idx = best;
        }
    }
    r.exit_rad = (90.0f - r.snapped_deg) * (float)M_PI / 180.0f;
    return r;
}

/* Compute arc start/end for roundabout traversal.
 * driving_side=0 → RHT (CCW), driving_side=1 → LHT (CW). */
static void roundabout_arc_range(float entry_rad, float exit_rad, int driving_side,
                                 float *arc_s, float *arc_e) {
    *arc_s = entry_rad;
    *arc_e = exit_rad;
    if (driving_side == 0) {
        while (*arc_e <= *arc_s) *arc_e += 2.0f * (float)M_PI;
        if (*arc_e - *arc_s < 0.01f) *arc_e += 2.0f * (float)M_PI;
    } else {
        while (*arc_e >= *arc_s) *arc_e -= 2.0f * (float)M_PI;
        if (*arc_s - *arc_e < 0.01f) *arc_e -= 2.0f * (float)M_PI;
    }
}

/* ================================================================
 * Route path building (standalone, no mask rendering)
 * ================================================================ */

typedef struct {
    float cx, cy, r, start, end, heading, x0, y0, tip_x, tip_y;
} roundabout_departure_t;

static roundabout_departure_t roundabout_departure(int driving_side) {
    roundabout_departure_t d;
    float sign = driving_side == 1 ? -1.0f : 1.0f;
    d.r = RAB_RING_R;
    d.cx = -sign * d.r;
    d.cy = -0.12f;
    d.start = driving_side == 1 ? (float)M_PI : 0.0f;
    d.end = driving_side == 1 ? (float)M_PI * 0.75f : (float)M_PI * 0.25f;
    d.heading = d.end;
    d.x0 = d.cx + d.r * cosf(d.end);
    d.y0 = d.cy + d.r * sinf(d.end);
    d.tip_x = d.x0 + 0.38f * cosf(d.heading);
    d.tip_y = d.y0 + 0.38f * sinf(d.heading);
    return d;
}

static void build_roundabout_departure(int side, route_path_t *path) {
    roundabout_departure_t d = roundabout_departure(side);
    rpath_clear(path);
    rpath_add_line(path, 0, SHAFT_BOT, 0, d.cy);
    rpath_add_arc(path, d.cx, d.cy, d.r, d.start, d.end);
    rpath_add_line(path, d.x0, d.y0, d.tip_x, d.tip_y);
    rpath_fillet_junctions(path, JOINT_R);
    rpath_set_arrow(path, d.tip_x, d.tip_y, d.heading);
}

static int uturn_goes_left(const maneuver_state_t *s) {
    if (s->direction != 0) return s->direction < 0;
    return s->driving_side == 0;
}

/* Lateral offset of the lane change / exit lane and start offset of a merge ramp. */
static float lane_shift(const maneuver_state_t *s) {
    return ((s->direction < 0) ? -1.0f : 1.0f) * LANE_SHIFT;
}

static float merge_start_x(const maneuver_state_t *s) {
    return ((s->direction > 0) ? 1.0f : -1.0f) * -MERGE_OFFSET;
}

/* Build route path for a single maneuver -- raw segments only.
 * No rpath_extend, no rpath_densify, no rpath_extrude.
 * Sets arrow position. Used by draw_scene and for path chaining. */
void maneuver_build_route(const maneuver_state_t *state, route_path_t *path) {
    switch (state->icon) {
    case ICON_NONE:
        rpath_clear(path);
        break;
    case ICON_ROUNDABOUT_EXIT:
        build_roundabout_departure(state->driving_side, path);
        break;
    case ICON_APPROACH: {
        float straight_end = ARRIVE_ROAD_TOP - HEAD_SZ;
        rpath_clear(path);
        rpath_add_line(path, 0, SHAFT_BOT, 0, straight_end);
        rpath_set_arrow(path, 0, straight_end, (float)(M_PI * 0.5));
        break;
    }
    case ICON_TURN: {
        float angle_rad = (float)state->exit_angle * (float)M_PI / 180.0f;
        float end_x = BLUE_LEN * sinf(angle_rad);
        float end_y = BLUE_LEN * cosf(angle_rad);
        float pts_x[] = {0, 0, end_x};
        float pts_y[] = {SHAFT_BOT, 0, end_y};
        build_filleted_path(path, pts_x, pts_y, 3);
        float head_angle = (float)(M_PI * 0.5) - angle_rad;
        rpath_set_arrow(path, end_x, end_y, head_angle);
        break;
    }
    case ICON_UTURN: {
        int go_left = uturn_goes_left(state);
        float enter_x = go_left ?  UTURN_GAP : -UTURN_GAP;
        float exit_x  = go_left ? -UTURN_GAP :  UTURN_GAP;
        float top_y   = UTURN_TOP;
        rpath_clear(path);
        rpath_add_line(path, enter_x, SHAFT_BOT, enter_x, top_y);
        /* Arc from entry side to exit side over the top.
         * Arc center at (0, top_y), radius = UTURN_GAP.
         * go_left: enter is right (+x), exit is left (-x) -> arc 0->PI (CCW over top)
         * go_right: enter is left (-x), exit is right (+x) -> arc PI->0 (CW over top) */
        if (go_left) {
            rpath_add_arc(path, 0, top_y, UTURN_GAP, 0, (float)M_PI);
        } else {
            rpath_add_arc(path, 0, top_y, UTURN_GAP, (float)M_PI, 0);
        }
        rpath_add_line(path, exit_x, top_y, exit_x, UTURN_ARROW);
        rpath_fillet_junctions(path, JOINT_R);
        rpath_set_arrow(path, exit_x, UTURN_ARROW, (float)(-M_PI * 0.5));
        break;
    }
    case ICON_ROUNDABOUT: {
        float ring_r = RAB_RING_R;
        float entry_rad = (float)(-M_PI * 0.5);
        rab_snap_t snap = snap_roundabout_exit((float)state->exit_angle,
            state->junction_angles, state->junction_angle_count, state->bap_geometry);
        float ext = BLUE_LEN - ring_r;
        float entry_pt_x = ring_r * cosf(entry_rad);
        float entry_pt_y = ring_r * sinf(entry_rad);
        float ex0_x = ring_r * cosf(snap.exit_rad);
        float ex0_y = ring_r * sinf(snap.exit_rad);
        float ex_tip_x = (ring_r + ext) * cosf(snap.exit_rad);
        float ex_tip_y = (ring_r + ext) * sinf(snap.exit_rad);
        float arc_s, arc_e;
        roundabout_arc_range(entry_rad, snap.exit_rad, state->driving_side, &arc_s, &arc_e);
        rpath_clear(path);
        rpath_add_line(path, 0, SHAFT_BOT, entry_pt_x, entry_pt_y);
        rpath_add_arc(path, 0, 0, ring_r, arc_s, arc_e);
        rpath_add_line(path, ex0_x, ex0_y, ex_tip_x, ex_tip_y);
        rpath_fillet_junctions(path, JOINT_R);
        rpath_set_arrow(path, ex_tip_x, ex_tip_y, snap.exit_rad);
        break;
    }
    case ICON_MERGE: {
        float start_x = merge_start_x(state);
        float pts_x[] = {start_x, start_x, 0, 0};
        float pts_y[] = {SHAFT_BOT, MERGE_BEND_LO, BEND_HI, BLUE_LEN};
        build_filleted_path(path, pts_x, pts_y, 4);
        rpath_set_arrow(path, 0, BLUE_LEN, (float)(M_PI * 0.5));
        break;
    }
    case ICON_LANE_CHANGE:
    case ICON_EXIT: {
        float shift = lane_shift(state);
        float pts_x[] = {0, 0, shift, shift};
        float pts_y[] = {SHAFT_BOT, BEND_LO, BEND_HI, BLUE_LEN};
        build_filleted_path(path, pts_x, pts_y, 4);
        rpath_set_arrow(path, shift, BLUE_LEN, (float)(M_PI * 0.5));
        break;
    }
    case ICON_ARRIVED:
    default: {
        rpath_clear(path);
        rpath_add_line(path, 0, SHAFT_BOT, 0, ARRIVE_CENTER_Y);
        rpath_set_arrow(path, 0, ARRIVE_CENTER_Y, (float)(M_PI * 0.5));
        break;
    }
    }
}

typedef struct {
    float x, y;       /* point in local maneuver coords */
    float heading;    /* radians (math convention) */
} route_pose_t;

static route_pose_t route_path_get_start_pose(const route_path_t *path) {
    route_pose_t st = {0, SHAFT_BOT, (float)(M_PI * 0.5)};
    if (path->seg_count > 0) {
        float sx, sy, sdx, sdy;
        seg_dir(&path->segs[0], 0, &sx, &sy, &sdx, &sdy);
        st.x = sx;
        st.y = sy;
        st.heading = atan2f(sdy, sdx);
    }
    return st;
}

static route_pose_t route_path_get_end_pose(const route_path_t *path) {
    route_pose_t ex = {0, ARRIVE_ROAD_TOP - HEAD_SZ, (float)(M_PI * 0.5)};
    if (path->seg_count > 0) {
        float exx, exy, edx, edy;
        seg_dir(&path->segs[path->seg_count - 1], 1, &exx, &exy, &edx, &edy);
        ex.x = exx;
        ex.y = exy;
        ex.heading = atan2f(edy, edx);
    }
    return ex;
}

static void compute_combined_transform(const maneuver_state_t *cur_state,
                                       const maneuver_state_t *next_state,
                                       float *out_tx, float *out_ty,
                                       float *out_cos_r, float *out_sin_r,
                                       float *out_rot);

static void bounds_include_pt(float x, float y, float *max_abs_x, float *max_abs_y) {
    float ax = fabsf(x);
    float ay = fabsf(y);

    if (ax > *max_abs_x) *max_abs_x = ax;
    if (ay > *max_abs_y) *max_abs_y = ay;
}

static void bounds_include_box(float tx, float ty, float cos_r, float sin_r,
                               float half_w, float half_h,
                               float *max_abs_x, float *max_abs_y) {
    static const float k_corners[4][2] = {
        { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f }
    };
    int i;

    for (i = 0; i < 4; i++) {
        float lx = k_corners[i][0] * half_w;
        float ly = k_corners[i][1] * half_h;
        float x = cos_r * lx - sin_r * ly + tx;
        float y = sin_r * lx + cos_r * ly + ty;
        bounds_include_pt(x, y, max_abs_x, max_abs_y);
    }
}

static void compute_transition_mask_bounds(float *out_abs_x, float *out_abs_y) {
    /* This conservative allocation envelope is computed from built-in sample
     * states, not the provider's current/next snapshot identities. A viewport
     * resize must never ask a provider to resolve these synthetic states. */
    maneuver_scene_provider_t saved_provider=g_scene_provider;
    memset(&g_scene_provider,0,sizeof(g_scene_provider));
    const float local_half_w = ROAD_LEN + FADE_LEN + OL_T * 0.5f;
    const float local_half_h = ROAD_LEN + FADE_LEN + OL_T * 0.5f;
    const float safety = 1.05f;
    const maneuver_state_t samples[] = {
        { .icon = ICON_APPROACH },
        { .icon = ICON_TURN, .exit_angle = 30 },
        { .icon = ICON_TURN, .exit_angle = 90 },
        { .icon = ICON_TURN, .exit_angle = 135 },
        { .icon = ICON_TURN, .exit_angle = -30 },
        { .icon = ICON_TURN, .exit_angle = -90 },
        { .icon = ICON_TURN, .exit_angle = -135 },
        { .icon = ICON_UTURN, .driving_side = 0 },
        { .icon = ICON_UTURN, .driving_side = 1 },
        { .icon = ICON_MERGE, .direction = -1 },
        { .icon = ICON_MERGE, .direction = 1 },
        { .icon = ICON_EXIT, .direction = -1 },
        { .icon = ICON_EXIT, .direction = 1 },
        { .icon = ICON_ROUNDABOUT, .exit_angle = 90, .driving_side = 0 },
        { .icon = ICON_ROUNDABOUT, .exit_angle = 0, .driving_side = 0 },
        { .icon = ICON_ROUNDABOUT, .exit_angle = -90, .driving_side = 0 },
        { .icon = ICON_ROUNDABOUT, .exit_angle = 180, .driving_side = 0 },
        { .icon = ICON_ROUNDABOUT, .exit_angle = 90, .driving_side = 1 },
        { .icon = ICON_ROUNDABOUT, .exit_angle = 0, .driving_side = 1 },
        { .icon = ICON_ROUNDABOUT, .exit_angle = -90, .driving_side = 1 },
        { .icon = ICON_ROUNDABOUT, .exit_angle = 180, .driving_side = 1 },
        { .icon = ICON_LANE_CHANGE, .direction = -1 },
        { .icon = ICON_LANE_CHANGE, .direction = 1 },
        { .icon = ICON_ROUNDABOUT_EXIT, .driving_side = 0 },
        { .icon = ICON_ROUNDABOUT_EXIT, .driving_side = 1 },
        { .icon = ICON_ARRIVED },
    };
    float max_abs_x = local_half_w;
    float max_abs_y = local_half_h;
    int i, j;

    for (i = 0; i < (int)(sizeof(samples) / sizeof(samples[0])); i++) {
        for (j = 0; j < (int)(sizeof(samples) / sizeof(samples[0])); j++) {
            float tx, ty, cos_r, sin_r, rot;

            compute_combined_transform(&samples[i], &samples[j], &tx, &ty, &cos_r, &sin_r, &rot);
            bounds_include_box(0.0f, 0.0f, 1.0f, 0.0f,
                               local_half_w, local_half_h,
                               &max_abs_x, &max_abs_y);
            bounds_include_box(tx, ty, cos_r, sin_r,
                               local_half_w, local_half_h,
                               &max_abs_x, &max_abs_y);
        }
    }

    *out_abs_x = max_abs_x * safety;
    *out_abs_y = max_abs_y * safety;
    g_scene_provider=saved_provider;
}

/* The envelope depends only on the built-in sample states, so it is computed once. */
void maneuver_get_transition_mask_bounds(float *out_abs_x, float *out_abs_y) {
    static float abs_x, abs_y;
    static int computed;

    if (!computed) {
        compute_transition_mask_bounds(&abs_x, &abs_y);
        computed = 1;
    }
    if (out_abs_x != NULL) *out_abs_x = abs_x;
    if (out_abs_y != NULL) *out_abs_y = abs_y;
}

/* Road reach past exit/before entry along the connecting heading.
 * Exit: how far road mask extends past the route arrow point.
 * Entry: how far road mask extends below SHAFT_BOT. */
static float maneuver_exit_road_reach(const maneuver_state_t *state) {
    if(scene_supplied(state))return g_scene_provider.exit_road_reach;
    return (ROAD_LEN - BLUE_LEN) + FADE_LEN;  /* 0.05 + 0.30 = 0.35 */
}

static float maneuver_entry_road_reach(const maneuver_state_t *state) {
    if(scene_supplied(state))return g_scene_provider.entry_road_reach;
    return FADE_LEN;  /* 0.30 */
}

static void compute_combined_transform(const maneuver_state_t *cur_state,
                                       const maneuver_state_t *next_state,
                                       float *out_tx, float *out_ty,
                                       float *out_cos_r, float *out_sin_r,
                                       float *out_rot) {
    route_path_t cur_path, next_path;
    route_pose_t cur_end, next_start;
    float rot, cos_r, sin_r;
    float ns_rx, ns_ry;
    float tx, ty;

    build_scene_route(cur_state, &cur_path);
    rpath_extend(&cur_path);
    build_scene_route(next_state, &next_path);
    rpath_extend(&next_path);

    cur_end = route_path_get_end_pose(&cur_path);
    next_start = route_path_get_start_pose(&next_path);
    rot = cur_end.heading - next_start.heading;
    cos_r = cosf(rot);
    sin_r = sinf(rot);
    ns_rx = cos_r * next_start.x - sin_r * next_start.y;
    ns_ry = sin_r * next_start.x + cos_r * next_start.y;

    tx = cur_end.x - ns_rx;
    ty = cur_end.y - ns_ry;

    /* Part A: enforce minimum separation between junction centers.
     * Project center-to-center onto exit heading; if too small, push out. */
    {
        /* Built-in maneuvers leave along their arrow, supplied scenes along the path end. */
        float exit_heading = (!scene_supplied(cur_state) && cur_path.seg_count > 0)
                           ? cur_path.arrow_angle : cur_end.heading;
        float hdx = cosf(exit_heading);
        float hdy = sinf(exit_heading);
        float proj = tx * hdx + ty * hdy;
        float min_sep = maneuver_exit_road_reach(cur_state)
                       + maneuver_entry_road_reach(next_state)
                       + TRANSITION_PAD;
        if (proj < min_sep) {
            float deficit = min_sep - proj;
            tx += deficit * hdx;
            ty += deficit * hdy;
        }
    }

    *out_tx = tx;
    *out_ty = ty;
    *out_cos_r = cos_r;
    *out_sin_r = sin_r;
    *out_rot = rot;
}


/* ================================================================
 * Icon mask painting -- every icon paints its road twice into the mask:
 * pass 0 the white outline, pass 1 the grey fill over it (painter's order).
 * ================================================================ */

/* Angular distance between two angles in degrees (0..180). */
static float angle_dist(float a, float b) {
    float d = fabsf(a - b);
    if (d > 180.0f) d = 360.0f - d;
    return d;
}

/* Helper: check if angle (degrees) overlaps forward (0deg) or entry (180deg). */
static int angle_near_fixed(float deg, float eps) {
    float ad = fabsf(deg);
    return (ad < eps || fabsf(ad - 180.0f) < eps);
}

/* Helper: check if angle overlaps forward, entry, or any angle in a list. */
static int angle_near_any(float deg, const float *angles, int count, float eps) {
    if (angle_near_fixed(deg, eps)) return 1;
    int i;
    for (i = 0; i < count; i++) {
        if (angle_dist(deg, (float)angles[i]) < eps) return 1;
    }
    return 0;
}

/* Check if junction angles include a forward road (~0° ± 30°) */
static int has_forward_road(float exit_angle, const float *side_angles, int side_count) {
    int i;
    /* The active straight exit is itself a road, even when iOS does not list
     * it among junction branches. Match the active-stub deduplication below. */
    if (fabsf(exit_angle) < ANGLE_DEDUP) return 1;
    for (i = 0; i < side_count; i++) {
        if (side_angles[i] > -30 && side_angles[i] < 30) return 1;
    }
    return 0;
}

/* Per pass (fill 0 = outline, 1 = fill): road width and colour. */
static const float k_pass_w[2] = { OL_T, SIDE_T };
static const float k_pass_c[2][4] = { { WHITE }, { SIDE } };
#define PASS_C(f) k_pass_c[f][0], k_pass_c[f][1], k_pass_c[f][2], k_pass_c[f][3]

static void pass_line(int fill, float x0, float y0, float x1, float y1) {
    render_thick_line(x0, y0, x1, y1, k_pass_w[fill], PASS_C(fill));
}

static void pass_disc(int fill, float x, float y) {
    render_disc(x, y, k_pass_w[fill] * 0.5f, JOINT_SEG, PASS_C(fill));
}

static void pass_arc(int fill, float cx, float cy, float r, float a0, float a1, int segs) {
    render_arc(cx, cy, r, k_pass_w[fill], a0, a1, segs, PASS_C(fill));
}

/* Road fading out below the entry / past the forward end of lane x. */
static void pass_fade_entry(int fill, float x) {
    draw_fading_road(x, SHAFT_BOT, x, SHAFT_BOT - FADE_LEN, fill);
}

static void pass_fade_forward(int fill, float x) {
    draw_fading_road(x, SIDE_TOP, x, SIDE_TOP + FADE_LEN, fill);
}

/* Road leaving the junction at deg (clockwise from forward): its fade-out
 * beyond ROAD_LEN, or the solid road from the centre. */
static void pass_stub(int fill, float deg, int fade) {
    float rad = deg * (float)M_PI / 180.0f;
    float sx = ROAD_LEN * sinf(rad), sy = ROAD_LEN * cosf(rad);
    if (fade)
        draw_fading_road(sx, sy, (ROAD_LEN + FADE_LEN) * sinf(rad),
                         (ROAD_LEN + FADE_LEN) * cosf(rad), fill);
    else
        pass_line(fill, 0, 0, sx, sy);
}

static void pass_side_stubs(int fill, int fade, const float *angles, int count) {
    int i;
    for (i = 0; i < count && i < MAX_JUNCTION_ANGLES; i++) {
        if (angle_near_fixed(angles[i], ANGLE_DEDUP)) continue;
        pass_stub(fill, angles[i], fade);
    }
}

static void paint_approach(int fill, const maneuver_state_t *s) {
    pass_fade_entry(fill, 0);
    pass_fade_forward(fill, 0);
    pass_side_stubs(fill, 1, s->junction_angles, s->junction_angle_count);
    pass_line(fill, 0, SHAFT_BOT, 0, SIDE_TOP);
    pass_side_stubs(fill, 0, s->junction_angles, s->junction_angle_count);
    if (s->junction_angle_count > 0)
        pass_disc(fill, 0, 0);
}

static void paint_turn(int fill, const maneuver_state_t *s) {
    int fwd = has_forward_road(s->exit_angle, s->junction_angles, s->junction_angle_count);
    int own_stub = !angle_near_any(s->exit_angle, s->junction_angles, s->junction_angle_count, ANGLE_DEDUP);
    if (fwd)
        pass_fade_forward(fill, 0);
    pass_fade_entry(fill, 0);
    if (own_stub)
        pass_stub(fill, s->exit_angle, 1);
    pass_side_stubs(fill, 1, s->junction_angles, s->junction_angle_count);
    if (fwd)
        pass_line(fill, 0, 0, 0, SIDE_TOP);
    pass_line(fill, 0, SHAFT_BOT, 0, 0);
    if (own_stub)
        pass_stub(fill, s->exit_angle, 0);
    pass_side_stubs(fill, 0, s->junction_angles, s->junction_angle_count);
    pass_disc(fill, 0, 0);
}

static void paint_uturn(int fill, const maneuver_state_t *s) {
    int go_left = uturn_goes_left(s);
    float enter_x = go_left ?  UTURN_GAP : -UTURN_GAP;
    float exit_x  = go_left ? -UTURN_GAP :  UTURN_GAP;
    pass_fade_entry(fill, enter_x);
    pass_fade_entry(fill, exit_x);
    pass_line(fill, enter_x, SHAFT_BOT, enter_x, UTURN_TOP);
    pass_arc(fill, 0, UTURN_TOP, UTURN_GAP, 0, (float)M_PI, UTURN_SEG);
    pass_line(fill, exit_x, UTURN_TOP, exit_x, SHAFT_BOT);
    if (fill) {
        pass_disc(fill, exit_x, UTURN_TOP);
        pass_disc(fill, enter_x, UTURN_TOP);
    }
}

static void paint_roundabout(int fill, const maneuver_state_t *s) {
    float ring_r = RAB_RING_R;
    float stub = ROAD_LEN - ring_r;
    float entry_rad = (float)(-M_PI * 0.5);
    float entry_top = -ring_r;
    int n_sides = s->junction_angle_count < MAX_JUNCTION_ANGLES ? s->junction_angle_count : MAX_JUNCTION_ANGLES;
    rab_snap_t snap = snap_roundabout_exit(s->exit_angle, s->junction_angles,
                                           s->junction_angle_count, s->bap_geometry);
    int i, j;

    /* Stub angles (math radians): the entry, the active exit, then every other
     * junction angle, skipping any within ANGLE_DEDUP_RAD of one already drawn. */
    float drawn_rads[MAX_JUNCTION_ANGLES + 2];
    int n_drawn = 0;
    drawn_rads[n_drawn++] = entry_rad;
    for (i = -1; i < n_sides; i++) {
        float sr = i < 0 ? snap.exit_rad
                         : (90.0f - s->junction_angles[i]) * (float)M_PI / 180.0f;
        int dup = 0;
        if (i >= 0 && i == snap.snapped_idx) continue;
        for (j = 0; j < n_drawn; j++) {
            float d = fabsf(sr - drawn_rads[j]);
            if (d > (float)M_PI) d = 2.0f * (float)M_PI - d;
            if (d < ANGLE_DEDUP_RAD) { dup = 1; break; }
        }
        if (!dup) drawn_rads[n_drawn++] = sr;
    }

    pass_fade_entry(fill, 0);
    for (i = 1; i < n_drawn; i++)
        draw_fading_road((ring_r + stub) * cosf(drawn_rads[i]), (ring_r + stub) * sinf(drawn_rads[i]),
                         (ring_r + stub + FADE_LEN) * cosf(drawn_rads[i]),
                         (ring_r + stub + FADE_LEN) * sinf(drawn_rads[i]), fill);
    render_rect(-k_pass_w[fill] * 0.5f, SHAFT_BOT, k_pass_w[fill], entry_top - SHAFT_BOT, PASS_C(fill));
    if (fill)
        pass_disc(fill, 0, entry_top);
    for (i = 1; i < n_drawn; i++) {
        float sx0 = ring_r * cosf(drawn_rads[i]), sy0 = ring_r * sinf(drawn_rads[i]);
        pass_line(fill, sx0, sy0, (ring_r + stub) * cosf(drawn_rads[i]), (ring_r + stub) * sinf(drawn_rads[i]));
        if (fill)
            pass_disc(fill, sx0, sy0);
    }
    render_circle(0, 0, ring_r, k_pass_w[fill], RAB_RING_SEG, PASS_C(fill));
}

/* Destination marker (matches stock HUD icon): the outer circle is an outline
 * ring only, the inner circle a filled road end cap. */
static void paint_arrived(int fill) {
    pass_fade_entry(fill, 0);
    pass_line(fill, 0, SHAFT_BOT, 0, ARRIVE_ROAD_TOP);
    if (!fill) {
        render_circle(0, ARRIVE_CENTER_Y, ARRIVE_OUTER_R, OL_W, ARRIVE_SEG, WHITE);
        render_disc(0, ARRIVE_CENTER_Y, ARRIVE_INNER_R, ARRIVE_SEG, WHITE);
    } else {
        /* Grey leaves an OL_W white border on the inner circle. */
        render_disc(0, ARRIVE_ROAD_TOP, SIDE_T * 0.5f, ARRIVE_SEG, SIDE);
        render_disc(0, ARRIVE_CENTER_Y, ARRIVE_INNER_R - OL_W, ARRIVE_SEG, SIDE);
    }
}

/* Lane change / merge S-bend: shaft x_in up to bend_lo, diagonal to (x_out, bend_hi), shaft to top_y. */
static void pass_sbend(int fill, float x_in, float x_out, float bend_lo, float bend_hi, float top_y) {
    pass_line(fill, x_in, SHAFT_BOT, x_in, bend_lo);
    pass_line(fill, x_in, bend_lo, x_out, bend_hi);
    pass_disc(fill, x_in, bend_lo);
    pass_disc(fill, x_out, bend_hi);
    pass_line(fill, x_out, bend_hi, x_out, top_y);
}

static void paint_exit(int fill, const maneuver_state_t *s, int through_road) {
    float shift = lane_shift(s);
    pass_fade_entry(fill, 0);
    if (through_road) pass_fade_forward(fill, 0);
    pass_fade_forward(fill, shift);
    if (through_road) pass_line(fill, 0, SHAFT_BOT, 0, SIDE_TOP);
    pass_sbend(fill, 0, shift, BEND_LO, BEND_HI, SIDE_TOP);
}

static void paint_merge(int fill, const maneuver_state_t *s) {
    float start_x = merge_start_x(s);
    pass_fade_entry(fill, 0);
    pass_fade_forward(fill, 0);
    pass_fade_entry(fill, start_x);
    pass_line(fill, 0, SHAFT_BOT, 0, SIDE_TOP);
    pass_sbend(fill, start_x, 0, MERGE_BEND_LO, BEND_HI, SHAFT_TOP);
}

/* Separate exit-from-roundabout stage: only the local departure arc is shown. */
static void paint_roundabout_departure(int fill, const maneuver_state_t *s) {
    roundabout_departure_t d = roundabout_departure(s->driving_side);
    pass_line(fill, 0, SHAFT_BOT, 0, d.cy);
    pass_arc(fill, d.cx, d.cy, d.r, 0, (float)M_PI, RAB_RING_SEG);
    pass_line(fill, d.x0, d.y0, d.tip_x + 0.05f * cosf(d.heading), d.tip_y + 0.05f * sinf(d.heading));
}

static void paint_builtin_masks(const maneuver_state_t *s) {
    int fill;
    render_set_raised(0);
    render_begin_outline_mask();
    for (fill = 0; fill < 2; fill++) {
        switch (s->icon) {
        case ICON_APPROACH: paint_approach(fill, s); break;
        case ICON_TURN: paint_turn(fill, s); break;
        case ICON_UTURN: paint_uturn(fill, s); break;
        case ICON_MERGE: paint_merge(fill, s); break;
        case ICON_EXIT: paint_exit(fill, s, 1); break;
        case ICON_LANE_CHANGE: paint_exit(fill, s, 0); break;
        case ICON_ROUNDABOUT_EXIT: paint_roundabout_departure(fill, s); break;
        case ICON_ROUNDABOUT: paint_roundabout(fill, s); break;
        case ICON_ARRIVED: paint_arrived(fill); break;
        default: break;
        }
    }
    render_end_outline_mask();
}

/* ================================================================
 * Main dispatch
 * ================================================================ */

/* Paint a maneuver's road masks, through the transform when it is the second
 * maneuver of a push. Returns 0 when the state has no masks to paint.
 * Providers may use the camera for local layout calculations: restore the
 * engine's pose before compositing; road masks never steer the handoff. */
static int paint_scene_masks(const maneuver_state_t *s, int shifted,
                             float tx, float ty, float cos_r, float sin_r) {
    if (scene_supplied(s)) {
        float cam_x = g_man_cam_x, cam_y = g_man_cam_y, cam_rot = g_man_cam_rot;
        g_scene_provider.paint_masks(g_scene_provider.context, s, tx, ty, cos_r, sin_r);
        apply_camera_pose(cam_x, cam_y, cam_rot);
        return 1;
    }
    if (s->icon <= ICON_NONE || s->icon >= ICON_COUNT)
        return 0;
    if (shifted) render_push_mask_transform(tx, ty, cos_r, sin_r);
    paint_builtin_masks(s);
    if (shifted) render_pop_mask_transform();
    return 1;
}

/* Destination flag position on the outer circle: left 9h, centre 12h, right 3h. */
static void arrive_flag_pos(int dir, float *x, float *y) {
    *x = (dir < 0) ? -ARRIVE_OUTER_R : (dir > 0) ? ARRIVE_OUTER_R : 0.0f;
    *y = dir ? ARRIVE_CENTER_Y : ARRIVE_CENTER_Y + ARRIVE_OUTER_R;
}

/* Paint a maneuver's masks, then (unless only masks are wanted) build its
 * route, set the slide window and composite. */
static void draw_scene(const maneuver_state_t *s) {
    int arrived = !scene_supplied(s) && s->icon == ICON_ARRIVED;

    if (!paint_scene_masks(s, 0, 0, 0, 1, 0) || g_masks_only_mode)
        return;

    build_scene_route(s, &g_route_path);
    rpath_extend(&g_route_path);
    rpath_densify(&g_route_path);
    if (arrived) {
        /* Morph arrow->bulb via independent timer (not tied to slide).
         * Restarted by maneuver_commit_pushed_state. */
        g_route_path.tip_blend = g_tip_morph_t;
        g_route_path.bulb_radius = ARRIVE_INNER_R - OL_W;
        arrive_flag_pos(s->direction, &g_arrive_flag_dx, &g_arrive_flag_dy);
    }
    compute_slide_params();
    route_extrude_body();
    render_composite();
    if (arrived)
        render_sprite_flag(g_arrive_flag_dx, g_arrive_flag_dy, ARRIVE_FLAG_SZ, (int)g_flag_frame);
    route_draw_with_fade();
}

void maneuver_draw(const maneuver_state_t *s, const maneuver_state_t *next_state) {
    render_select_mask_set(0);                /* every path starts on the current maneuver's set */
    if (s->icon == ICON_NONE && next_state == NULL) {
        g_flag_active = 0;
        render_begin_outline_mask();
        render_end_outline_mask();
        render_composite();
        return;
    }
    /* Track whether flag animation is active — also during push TO arrived */
    g_flag_active = (s->icon == ICON_ARRIVED)
                 || (next_state != NULL && next_state->icon == ICON_ARRIVED);
    if (!g_flag_active) {
        g_combined_flag_x = 0.0f;
        g_combined_flag_y = 0.0f;
    }

    /* Advance flag animation */
    if (g_flag_active) {
        int fc = render_get_flag_frame_count();
        g_flag_frame += FLAG_ANIM_SPEED * render_frame_step();
        if (fc > 0 && g_flag_frame >= (float)fc)
            g_flag_frame = fmodf(g_flag_frame, (float)fc);
    }

    /* Combined path: when pushing with a known next maneuver, build joined route
     * after the draw_* pass so arrow slides continuously between maneuvers.  The current
     * maneuver's masks are set 0 and the next one's set 1; each is painted once after an
     * invalidation and then only composited (a new next maneuver invalidates set 1 only).
     * Push frames still run the whole draw below, whose painting replays without GPU work
     * once clean.  A frame paints at most one set while the next maneuver is invisible. */
    int combined = (next_state != NULL && maneuver_is_pushing());
    int current_painted = combined && render_masks_dirty();
    if (!combined)
        g_combined_window_active = 0;

    /* Compute length-normalized slide delta.
     * Speed uses the slug length (one maneuver's road) for consistent animation pace,
     * even when the path is a combined two-maneuver path. */
    float orig_len = (g_slug_override > 0.0f) ? g_slug_override
                   : (g_route_path.total_length - 2.0f * ROUTE_EXTEND);
    if (orig_len < 0.1f) orig_len = 0.1f;

    /* Unified animation: slide from g_anim_start -> g_anim_target.
     * Progress normalized to [0,1] within that range.
     * Ease in/out + curvature slowdown on turns. */
    if (g_route_animating) {
        float range = g_anim_target - g_anim_start;
        if (range < 0.01f) range = 0.01f;
        float progress = (g_route_slide - g_anim_start) / range;
        float e_speed = ease_inout_speed(progress);
        if (e_speed < 0.15f) e_speed = 0.15f;
        float curv = path_curvature_factor(&g_route_path, g_t_head);
        float speed = ROUTE_SPEED_MIN + (ROUTE_SPEED_PEAK - ROUTE_SPEED_MIN) * e_speed * curv;
        g_route_slide += speed * render_frame_step() / orig_len;
        if (g_route_slide >= g_anim_target) {
            g_route_slide = g_anim_target;
            g_route_animating = 0;
        }
    }

    /* Advance tip morph timer (independent of slide).
     * Starts when ARRIVED slide reaches 60%, or immediately after push commit. */
    if (s != NULL && s->icon == ICON_ARRIVED && !g_tip_morph_active
            && (g_route_slide >= 0.5f || g_anim_start >= 1.0f)) {
        g_tip_morph_active = 1;
    }
    if (g_tip_morph_active && g_tip_morph_t < 1.0f) {
        g_tip_morph_t += TIP_MORPH_SPEED * render_frame_step();
        if (g_tip_morph_t > 1.0f) g_tip_morph_t = 1.0f;
    }

    /* If masks are cached and still valid, just re-composite + rebuild mesh at current slide
     * (handles perspective animation and route animation without re-rendering masks).
     * Not for a push: its second pass and joined route are built below. */
    if (!combined && !render_masks_dirty()) {
        compute_slide_params();
        /* Settled ARRIVED: forward tip morph via g_tip_morph_t */
        if (s->icon == ICON_ARRIVED)
            g_route_path.tip_blend = g_tip_morph_t;
        if (g_route_animating || g_route_slide != 1.0f
                || (g_tip_morph_active && g_tip_morph_t < 1.0f)) {
            /* Rebuild mesh at current slide (path segments still cached in g_route_path) */
            route_extrude_body();
        }
        render_composite();
        if (s->icon == ICON_ARRIVED)
            render_sprite_flag(g_arrive_flag_dx, g_arrive_flag_dy, ARRIVE_FLAG_SZ, (int)g_flag_frame);
        route_draw_with_fade();
        if (g_route_debug)
            rpath_draw_debug(&g_route_path, 0.0f, g_t_head);
        return;
    }

    /* Render current maneuver masks (+ composite if not combined) */
    g_masks_only_mode = combined;
    draw_scene(s);
    g_masks_only_mode = 0;

    /* When combined, rebuild joined route path and draw it */
    if (combined) {
        /* Build the combined path from the current maneuver's route.
         * Each maneuver keeps its own pre/post extension so the handoff has
         * spacing, and the FBO uses the same transform. */
        route_path_t next_path;
        float rot, cos_r, sin_r;
        float tx, ty;
        build_scene_route(s, &g_route_path);

        /* Measure first maneuver's road length BEFORE extending --
         * use it for speed normalization during the combined handoff. */
        rpath_densify(&g_route_path);
        g_slug_override = g_route_path.total_length;
        rpath_extend(&g_route_path);

        build_scene_route(next_state, &next_path);
        rpath_densify(&next_path);
        float next_road_len = next_path.total_length;
        rpath_extend(&next_path);
        compute_combined_transform(s, next_state, &tx, &ty, &cos_r, &sin_r, &rot);
        g_last_combined_rot = rot;

        /* Pre-compute flag position for ARRIVED in combined space.
         * Full rotation: flag stays anchored to the rotated circle. */
        if (next_state->icon == ICON_ARRIVED) {
            float nfx, nfy;
            arrive_flag_pos(next_state->direction, &nfx, &nfy);
            g_combined_flag_x = cos_r * nfx - sin_r * nfy + tx;
            g_combined_flag_y = sin_r * nfx + cos_r * nfy + ty;
        }

        /* Set ramp restart at boundary where next maneuver's path begins.
         * Current extended path length = slug + 2*ROUTE_EXTEND.
         * Elevation = max of current and next (either may self-overlap). */
        rpath_set_ramp_restart(g_slug_override + 2.0f * ROUTE_EXTEND);
        rpath_set_elevation(maneuver_elevation(s), maneuver_elevation(next_state));
        rpath_xform_append(&g_route_path, &next_path, tx, ty, cos_r, sin_r, rot);
        float ax = cos_r * next_path.arrow_x - sin_r * next_path.arrow_y + tx;
        float ay = sin_r * next_path.arrow_x + cos_r * next_path.arrow_y + ty;
        rpath_set_arrow(&g_route_path, ax, ay, next_path.arrow_angle + rot);
        rpath_densify(&g_route_path);

        /* Part B: spatial crossfade midpoint -- halfway between road zones */
        {
            float cur_zone_end = ROUTE_EXTEND + g_slug_override
                               + maneuver_exit_road_reach(s);
            float next_zone_start = g_route_path.total_length - ROUTE_EXTEND
                                  - next_road_len
                                  - maneuver_entry_road_reach(next_state);
            if (next_zone_start < cur_zone_end)
                next_zone_start = cur_zone_end;
            g_xfade_mid_dist = 0.5f * (cur_zone_end + next_zone_start);
        }

        /* Morph the visible blue body from the current maneuver window to the
         * committed next-maneuver window so the handoff lands on the exact
         * standalone next-state pose. */
        {
            float current_total = g_slug_override + 2.0f * ROUTE_EXTEND;
            float start_head = ROUTE_EXTEND + g_anim_start * g_slug_override;
            float start_tail = start_head - g_slug_override;
            float end_head = g_route_path.total_length - ROUTE_EXTEND;
            float end_tail = end_head - next_road_len;
            float start_mid, end_mid, transition_len;

            if (start_head < 0.0f) start_head = 0.0f;
            if (start_head > current_total) start_head = current_total;
            if (start_tail < 0.0f) start_tail = 0.0f;
            if (start_tail > current_total) start_tail = current_total;
            if (end_tail < 0.0f) end_tail = 0.0f;
            if (end_head < 0.0f) end_head = 0.0f;
            if (end_head > g_route_path.total_length)
                end_head = g_route_path.total_length;

            g_combined_window_active = 1;
            g_combined_start_head_dist = start_head;
            g_combined_end_head_dist = end_head;
            g_next_cam_pan_x = tx;
            g_next_cam_pan_y = ty;
            g_next_cam_rot = rot;
            g_next_cam_follow_dist = current_total + ROUTE_EXTEND;
            g_next_cam_intro_end_dist = start_head + 0.30f * g_slug_override;
            if (g_next_cam_intro_end_dist < start_head)
                g_next_cam_intro_end_dist = start_head;
            if (g_next_cam_intro_end_dist > g_next_cam_follow_dist)
                g_next_cam_intro_end_dist = g_next_cam_follow_dist;
            g_next_cam_release_end_dist = end_head;

            start_mid = 0.5f * (start_tail + start_head);
            end_mid = 0.5f * (end_tail + end_head);
            transition_len = end_mid - start_mid;
            if (transition_len < 0.01f) transition_len = 0.01f;

            if (g_slug_override > 0.01f)
                g_anim_target = g_anim_start + transition_len / g_slug_override;
        }

        compute_slide_params();
        update_combined_camera();

        /* Reverse morph bulb→arrow in first 40% of push (synced with morph-in) */
        if (s->icon == ICON_ARRIVED) {
            float range = g_anim_target - g_anim_start;
            float progress = (range > 0.01f) ? (g_route_slide - g_anim_start) / range : 1.0f;
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;
            float morph_out = (progress < 0.4f) ? (1.0f - progress / 0.4f) : 0.0f;
            g_route_path.tip_blend = morph_out;
            g_route_path.bulb_radius = ARRIVE_INNER_R - OL_W;
        }

        /* Crossfade: current roads fade out, next roads fade in (spatial) */
        {
            float pp = spatial_xfade();
            float base_alpha = render_get_global_alpha();

            /* Pass 1: composite current maneuver roads (already in FBO) fading out */
            render_set_global_alpha(base_alpha * (1.0f - pp));
            render_composite();

            /* Reset depth so second composite doesn't fight first */
            render_reset_depth();

            /* Pass 2: draw next maneuver masks into set 1, composite fading in. */
            render_select_mask_set(1);
            /* This frame painted set 0 and the next maneuver is not yet visible: set 1 waits
             * for the next frame, so the two sets' mask work is spread over two frames. */
            if (current_painted && pp <= 0.0f) render_hold_mask_set();
            paint_scene_masks(next_state, 1, tx, ty, cos_r, sin_r);

            render_set_global_alpha(base_alpha * pp);
            render_composite();
            render_select_mask_set(0);

            render_set_global_alpha(base_alpha);
        }

        /* Crossfade flags: current fades out, next fades in (spatial) */
        {
            float pp = spatial_xfade();
            float ba = render_get_global_alpha();
            if (s->icon == ICON_ARRIVED) {
                render_set_global_alpha(ba * (1.0f - pp));
                render_sprite_flag(g_arrive_flag_dx, g_arrive_flag_dy, ARRIVE_FLAG_SZ, (int)g_flag_frame);
            }
            if (next_state->icon == ICON_ARRIVED) {
                render_set_global_alpha(ba * pp);
                render_sprite_flag(g_combined_flag_x, g_combined_flag_y, ARRIVE_FLAG_SZ,
                                   (int)g_flag_frame);
            }
            render_set_global_alpha(ba);
        }

        route_extrude_body();
        route_draw_with_fade();
    }

    if (g_route_debug)
        rpath_draw_debug(&g_route_path, 0.0f, g_t_head);
}

void maneuver_commit_pushed_state(const maneuver_state_t *state) {
    float settle_rot;
    float light_rot;

    (void)state;
    g_route_slide = 1.0f;
    g_anim_start = 1.0f;
    g_anim_target = 1.0f;
    g_route_animating = 0;
    /* Carry spring's remaining offset into settle (transformed to new local space).
     * Without this, short-road maneuvers (ARRIVED) jump at commit. */
    {
        float dx = g_release_cam_x - g_next_cam_pan_x;
        float dy = g_release_cam_y - g_next_cam_pan_y;
        float cr = cosf(-g_next_cam_rot);
        float sr = sinf(-g_next_cam_rot);
        g_cam_settle_start_x = cr * dx - sr * dy;
        g_cam_settle_start_y = sr * dx + cr * dy;
    }
    /* Reset tip morph — ARRIVED will start it on next frame */
    g_tip_morph_t = 0.0f;
    g_tip_morph_active = 0;
    settle_rot = wrap_angle(g_man_cam_rot - g_next_cam_rot);
    if (settle_rot > CAMERA_SETTLE_MAX_ROT) settle_rot = CAMERA_SETTLE_MAX_ROT;
    if (settle_rot < -CAMERA_SETTLE_MAX_ROT) settle_rot = -CAMERA_SETTLE_MAX_ROT;
    g_cam_settle_start_rot = settle_rot;
    g_cam_settle_t = 0.0f;
    g_cam_settle_active = 1;
    light_rot = wrap_angle(g_last_combined_rot);
    g_light_settle_start_rot = light_rot;
    g_light_settle_t = 0.0f;
    g_light_settle_active = (fabsf(light_rot) > 1e-4f);

    clear_combined_transition();
}
