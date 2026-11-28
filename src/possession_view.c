/******************************************************************************/
// Free implementation of the Dungeon Keeper strategy game.
/******************************************************************************/
/** @file possession_view.c
 *     Endless Keeper: the crosshair of third-person Possession, and its test switch.
 * @par Purpose:
 *     Draws where the possessed creature's attacks and spells go while the camera sits behind it (the user,
 *     2026-11-28: "Camera behind the creature. Crosshair for aiming."): the point its aim (yaw and pitch from its shot
 *     origin, as thing_fire_shot fires without a target) first meets rock, and the creature the game would pick as the
 *     target (find_possession_target, by yaw, as the packet's target and the server pick it) with corner marks.
 *     Drawing only: nothing here changes the game.
 * @par Comment:
 *     None.
 * @author   Endless Keeper
 * @date     28 Nov 2026
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "possession_view.h"
#include "bflib_planar.h"
#include "bflib_vidraw.h"
#include "config_creature.h"
#include "creature_control.h"
#include "creature_instances.h"
#include "engine_camera.h"
#include "engine_render.h"
#include "game_legacy.h"
#include "local_camera.h"
#include "local_possession.h"
#include "player_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "vidfade.h"
#include "vidmode.h"
#include "bflib_keybrd.h"
#include "config.h"
#include "dungeon_data.h"
#include "net_game.h"
#include "scrcapt.h"
#include "keeperfx.hpp"
#include "player_instances.h"
#include "thing_navigate.h"
#include <stdlib.h>
#include <string.h>
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
/* the crosshair last drawn, for the test switch's log */
static int32_t last_cross_x;
static int32_t last_cross_y;
static ThingIndex last_cross_target;

/* how far the aim is followed to rock: past this the crosshair is the aim's far point */
#define CROSSHAIR_AIM_STEP 32
#define CROSSHAIR_AIM_STEPS 96

/** The shot origin of thing_fire_shot for a creature (its shot_shift, grown with its level), at the drawn place. */
static void crosshair_shot_origin(const struct Thing *creatng, const struct CreatureControl *cctrl, const struct Coord3d *drawn,
    int32_t yaw, struct Coord3d *pos)
{
    const int32_t grow = game.conf.crtr_conf.exp.size_increase_on_exp * creature_size_exp_level(cctrl->exp_level);
    const int32_t shift_x = cctrl->shot_shift_x + (cctrl->shot_shift_x * grow) / 100;
    const int32_t shift_y = cctrl->shot_shift_y + (cctrl->shot_shift_y * grow) / 100;
    const int32_t shift_z = cctrl->shot_shift_z + (cctrl->shot_shift_z * grow) / 100;
    (void)creatng;
    *pos = *drawn;
    pos->x.val += distance_with_angle_to_coord_x(shift_x, yaw + DEGREES_90);
    pos->y.val += distance_with_angle_to_coord_y(shift_x, yaw + DEGREES_90);
    pos->x.val += distance_with_angle_to_coord_x(shift_y, yaw);
    pos->y.val += distance_with_angle_to_coord_y(shift_y, yaw);
    pos->z.val += shift_z;
}

static void crosshair_box(long x, long y, long w, long h, TbPixel colour)
{
    if ((w > 0) && (h > 0)) {
        LbDrawBox(x / pixel_size, y / pixel_size, (w + pixel_size - 1) / pixel_size, (h + pixel_size - 1) / pixel_size, colour);
    }
}

/** A bar with a dark edge one unit round it, so it shows on bright and dark ground alike. */
static void crosshair_bar(long x, long y, long w, long h, long edge, TbPixel colour)
{
    crosshair_box(x - edge, y - edge, w + 2 * edge, h + 2 * edge, colours[0][0][0]);
    crosshair_box(x, y, w, h, colour);
}

static void draw_crosshair_at(long cx, long cy, long unit, TbPixel colour)
{
    const long gap = 3 * unit;
    const long len = 5 * unit;
    const long thick = unit;
    const long edge = max(1, unit / 2);
    crosshair_bar(cx - gap - len, cy - thick / 2, len, thick, edge, colour);
    crosshair_bar(cx + gap, cy - thick / 2, len, thick, edge, colour);
    crosshair_bar(cx - thick / 2, cy - gap - len, thick, len, edge, colour);
    crosshair_bar(cx - thick / 2, cy + gap, thick, len, edge, colour);
    crosshair_bar(cx - thick / 2, cy - thick / 2, thick, thick, edge, colour);
}

/** Corner marks round the creature the aim picks. */
static void draw_target_marks(long cx, long cy, long half, long unit, TbPixel colour)
{
    const long len = 4 * unit;
    const long thick = unit;
    const long edge = max(1, unit / 2);
    for (int sx = -1; sx <= 1; sx += 2)
    {
        for (int sy = -1; sy <= 1; sy += 2)
        {
            const long corner_x = cx + sx * half;
            const long corner_y = cy + sy * half;
            crosshair_bar((sx < 0) ? corner_x : corner_x - len, (sy < 0) ? corner_y : corner_y - thick, len, thick, edge, colour);
            crosshair_bar((sx < 0) ? corner_x : corner_x - thick, (sy < 0) ? corner_y : corner_y - len, thick, len, edge, colour);
        }
    }
}

/** The target the creature's active attack or spell would take: none for a spell on itself. */
static ThingIndex crosshair_target(struct Thing *viewtng, const struct CreatureControl *cctrl, int32_t yaw)
{
    const CrInstance inst_id = cctrl->active_instance_id;
    if (inst_id == CrInst_NULL) {
        return 0;
    }
    const struct InstanceInfo *inst_inf = creature_instance_info_get(inst_id);
    if (flag_is_set(inst_inf->instance_property_flags, InstPF_SelfBuff) && !flag_is_set(inst_inf->instance_property_flags, InstPF_RangedBuff)) {
        return 0;
    }
    return find_possession_target(viewtng, inst_id, yaw);
}

void draw_possession_crosshair(void)
{
    if (!local_possession_third_person() || (get_local_view_type() != PVT_CreatureContrl)) {
        return;
    }
    struct Thing *thing = thing_get(get_local_user_state()->controlled_thing_idx);
    if (!thing_is_creature(thing)) {
        return;
    }
    const struct CreatureControl *cctrl = get_local_creature_control(thing);
    struct Thing *viewtng = get_local_possession_view(thing);
    const struct Camera *cam = get_local_active_camera();
    /* the camera keeps the look the creature turns to: its yaw and pitch are the aim */
    const int32_t yaw = cam->rotation_angle_x;
    const int32_t pitch = cam->rotation_angle_y;
    const struct ThingInterpolateResult drawn = interpolate_thing(thing);
    struct Coord3d origin;
    crosshair_shot_origin(thing, cctrl, &drawn.mappos, yaw, &origin);
    struct Coord3d hit;
    project_point_to_wall_on_angle(&origin, &hit, yaw, pitch, CROSSHAIR_AIM_STEP, CROSSHAIR_AIM_STEPS);

    const long unit = max(1, (long)MyScreenHeight / 360);
    int32_t cx = local_state.engine_window_x + local_state.engine_window_width / 2;
    int32_t cy = local_state.engine_window_y + local_state.engine_window_height / 2;
    int32_t sx;
    int32_t sy;
    if (map_to_screen_in_front(&hit, &sx, &sy)) {
        cx = sx;
        cy = sy;
    }
    TbPixel colour = colours[15][15][15];
    const ThingIndex target_idx = crosshair_target(viewtng, cctrl, yaw);
    struct Thing *target = thing_get(target_idx);
    if ((target_idx != 0) && thing_exists(target))
    {
        /* the game aims at it, not along the line: marks round it and the crosshair turns red */
        colour = colours[15][3][3];
        const struct ThingInterpolateResult tdrawn = interpolate_thing(target);
        struct Coord3d mid = tdrawn.mappos;
        mid.z.val += target->clipbox_size_z / 2;
        struct Coord3d top = tdrawn.mappos;
        top.z.val += target->clipbox_size_z;
        int32_t tx;
        int32_t ty;
        int32_t top_x;
        int32_t top_y;
        if (map_to_screen_in_front(&mid, &tx, &ty) && map_to_screen_in_front(&top, &top_x, &top_y))
        {
            const long half = max(6 * unit, (long)abs(ty - top_y) + 2 * unit);
            draw_target_marks(tx, ty, half, unit, colour);
        }
    }
    draw_crosshair_at(cx, cy, unit, colour);
    last_cross_x = cx;
    last_cross_y = cy;
    last_cross_target = target_idx;
}

static void possess_view_test_log(const char *what, const struct Thing *thing)
{
    const struct Camera *cam = get_local_active_camera();
    SYNCLOG("POSSESS_VIEW: %s: third=%d creature (%d,%d,%d) camera (%d,%d,%d) back %d (reach %d/1024) yaw %d pitch %d; crosshair (%d,%d) of window (%d,%d %dx%d), target %d",
        what, (int)local_possession_view(), (int)thing->mappos.x.val, (int)thing->mappos.y.val, (int)thing->mappos.z.val,
        (int)cam->mappos.x.val, (int)cam->mappos.y.val, (int)cam->mappos.z.val,
        (int)get_2d_distance(&thing->mappos, &cam->mappos), (int)local_state.camera.third_person_reach, (int)cam->rotation_angle_x, (int)cam->rotation_angle_y,
        (int)last_cross_x, (int)last_cross_y, (int)local_state.engine_window_x, (int)local_state.engine_window_y,
        (int)local_state.engine_window_width, (int)local_state.engine_window_height, (int)last_cross_target);
}

static void possess_view_test_shot(const char *name)
{
    char fname[64];
    snprintf(fname, sizeof(fname), "scrshots/%s.%s", name, (screenshot_format == 1) ? "png" : "bmp");
    SYNCLOG("POSSESS_VIEW: screenshot %s: %d", fname, (int)take_screenshot(fname));
}

/* Test switch KFX_POSSESS_VIEW_TEST=T[,MODEL] (single player only): at T player 0 gets a MODEL (default TROLL) 6 subtiles from its
   Heart and takes it over in third person, with a hostile Archer 5 subtiles ahead of it; T+10 screenshot tp_target (the
   Archer marked if the aim picks it), T+60 its first attack and T+62 screenshot tp_swipe, T+80 screenshot
   tp_third (crosshair, the Archer marked if the aim picks it), T+90 C (farther behind), T+102 screenshot tp_far, T+105 C
   (first person), T+115 screenshot tp_first, T+120 C (behind), S (walking backwards) held T+120..T+320 so the camera meets a wall behind, T+330 screenshot tp_wall, T+340
   C again (farther behind), T+380 the game quits. "POSSESS_VIEW:" lines give the camera and crosshair at each step. */
void possession_view_test_turn(void)
{
    static long at = -2;
    static ThingIndex idx = 0;
    static char model_name[32] = "TROLL";
    if (at == -2)
    {
        const char *v = getenv("KFX_POSSESS_VIEW_TEST");
        at = (v != NULL) ? atol(v) : -1;
        const char *comma = (v != NULL) ? strchr(v, ',') : NULL;
        if (comma != NULL) {
            snprintf(model_name, sizeof(model_name), "%s", comma + 1);
        }
    }
    const long t = (long)get_gameturn();
    if ((at < 0) || (t < at) || (t > at + 380) || network_is_active())
        return;
    const long k = t - at;
    struct Thing *thing = thing_get(idx);
    if (k == 0)
    {
        struct Thing *heart = get_player_soul_container(PLAYER0);
        const ThingModel model = get_id(creature_desc, model_name);
        if (!thing_exists(heart) || (model <= 0))
        {
            SYNCLOG("POSSESS_VIEW: no heart or no creature %s", model_name);
            at = -1;
            return;
        }
        struct Coord3d pos = heart->mappos;
        pos.x.val += 6 * COORD_PER_STL;
        pos.z.val = 0;
        thing = create_creature(&pos, model, PLAYER0);
        if (!thing_exists(thing))
        {
            at = -1;
            return;
        }
        move_thing_in_map(thing, &pos);
        thing->move_angle_xy = ANGLE_EAST;
        idx = thing->index;
        local_state.camera.third_person = PossV_Behind;
        local_state.camera.third_person_chosen = true;
        SYNCLOG("POSSESS_VIEW: taking over %s index %d: %d", thing_model_name(thing), (int)thing->index,
            (int)control_creature_as_controller(get_my_user(), thing));
        struct Coord3d epos = pos;
        epos.x.val += 5 * COORD_PER_STL;
        const ThingModel archer = get_id(creature_desc, "ARCHER");
        struct Thing *enemy = (archer > 0) ? create_creature(&epos, archer, PLAYER_GOOD) : INVALID_THING;
        if (thing_exists(enemy)) {
            move_thing_in_map(enemy, &epos);
            SYNCLOG("POSSESS_VIEW: archer index %d at (%d,%d)", (int)enemy->index, (int)epos.x.val, (int)epos.y.val);
        }
        return;
    }
    if (!thing_exists(thing))
        return;
    if ((k % 20) == 0)
        possess_view_test_log("turn", thing);
    if (k == 10)
        possess_view_test_shot("tp_target");
    if (k == 60)
    {
        /* the creature's first attack, for the swipe drawn behind it (smaller than in first person) */
        const CrInstance inst = creature_instance_get_available_id_for_pos(thing, 0);
        if (inst > 0)
            set_creature_instance(thing, inst, 0, 0);
    }
    if (k == 62)
        possess_view_test_shot("tp_swipe");
    if (k == 80)
        possess_view_test_shot("tp_third");
    /* C goes round: behind -> farther behind (T+90) -> first person (T+105) -> behind (T+120) -> farther (T+340) */
    if ((k == 90) || (k == 105) || (k == 120) || (k == 340))
        toggle_local_possession_view();
    if (k == 102)
        possess_view_test_shot("tp_far");
    if (k == 115)
        possess_view_test_shot("tp_first");
    if ((k >= 120) && (k < 320))
        lbKeyOn[KC_S] = 1;
    else if (k == 320)
        lbKeyOn[KC_S] = 0;
    if (k == 330)
        possess_view_test_shot("tp_wall");
    if (k == 380)
    {
        SYNCLOG("POSSESS_VIEW: done");
        quit_game = 1;
        exit_keeper = 1;
    }
}
/******************************************************************************/
#ifdef __cplusplus
}
#endif
