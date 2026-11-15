/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_consecrate.c
 *     Endless world: Consecrated Ground (2026-11-13; see world_consecrate.h).
 * @par Purpose:
 *     Decisions on the page's open points (the user was away; the recommended choice each time):
 *     - How large: a Monk's ring reaches 4 slabs round it (it moves with the Monk), a chapel's 6 slabs round the
 *       Temple's middle.
 *     - Which raids: every Monk sheds one: a raid's Monks (the raid tiers bring them from the keeper's 13th level), an
 *       outpost's or a fortress's. A Monk that is unconscious, held or dying sheds none.
 *     - The Hand does nothing inside it: every power of the keeper's is refused there (can_cast_spell: the Hand's
 *       pick-up, slaps, every spell) and nothing is dropped there (can_drop_thing_here). Only human keepers: the
 *       computer keepers play as they do.
 *     - The rings are in the game state (game.world_rings): a joined player's machine marks them too, with white
 *       sparkles round each ring every second.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_consecrate.h"
#include "world_stream.h"
#include <math.h>
#include <string.h>
#include "bflib_basics.h"
#include "config_creature.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "creature_states.h"
#include "creature_states_prisn.h"
#include "game_legacy.h"
#include "globals.h"
#include "player_instances.h"
#include "room_data.h"
#include "thing_data.h"
#include "thing_effects.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "thing_creature.h"
#include "magic_powers.h"
#include "power_hand.h"
#include "player_utils.h"
#include "config_magic.h"
#include "post_inc.h"

#define CS_MONK_RING 4
#define CS_CHAPEL_RING 6

TbBool world_consecrated_at(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!world_mode_active() || (plyr_idx >= PLAYERS_MAX) || !world_player_is_human(plyr_idx))
        return false;
    for (int i = 0; i < WORLD_RINGS_MAX; i++)
    {
        const struct WorldRing *rg = &game.world_rings[i];
        if (rg->r == 0)
            continue;
        const long dx = (long)stl_x - rg->stl_x, dy = (long)stl_y - rg->stl_y;
        const long rr = (long)rg->r * STL_PER_SLB;
        if (dx * dx + dy * dy <= rr * rr)
            return true;
    }
    return false;
}

/** Test hook KFX_WORLD_MONKTEST=T: at T a hero Monk 6 slabs east of player 0's heart (frozen in place); at T+40 the
 *  log says whether player 0 may slap, drop and cast at the Monk and 12 slabs from it (the real checks). */
static void cs_test(void)
{
    static long at = -1;
    static ThingIndex monk_idx = 0;
    if (at < 0)
    {
        const char *v = getenv("KFX_WORLD_MONKTEST");
        at = (v != NULL) ? atol(v) : 0;
    }
    if (at <= 0)
        return;
    struct Thing *heart = find_players_dungeon_heart(PLAYER0);
    if (!thing_exists(heart))
        return;
    if (game.play_gameturn == (GameTurn)at)
    {
        struct Coord3d pos = heart->mappos;
        pos.x.val += 6 * STL_PER_SLB * COORD_PER_STL;
        pos.z.val = 0;
        struct Thing *monk = create_creature(&pos, (ThingModel)get_id(creature_desc, "MONK"), PLAYER_GOOD);
        if (thing_exists(monk))
        {
            move_thing_in_map(monk, &pos);
            monk_idx = monk->index;
        }
        SYNCLOG("World test: consecrate: a Monk by player 0's heart (%s)", thing_exists(monk) ? "made" : "failed");
    }
    if ((game.play_gameturn == (GameTurn)(at + 40)) && (monk_idx > 0))
    {
        struct Thing *monk = thing_get(monk_idx);
        if (!thing_exists(monk))
            return;
        const MapSubtlCoord mx = monk->mappos.x.stl.num, my = monk->mappos.y.stl.num;
        const MapSubtlCoord fx = heart->mappos.x.stl.num - 3 * STL_PER_SLB, fy = heart->mappos.y.stl.num; /* own floor */
        SYNCLOG("World test: consecrate: at the Monk: slap %d, drop %d, lightning %d; on its own floor 3 slabs west of the heart: slap %d, drop %d,"
            " lightning %d", (int)can_cast_spell(PLAYER0, PwrK_SLAP, mx, my, monk, CastChk_SkipAvailiabilty),
            (int)can_drop_thing_here(mx, my, PLAYER0, 1), (int)can_cast_spell(PLAYER0, PwrK_LIGHTNING, mx, my, INVALID_THING,
            CastChk_SkipAvailiabilty), (int)can_cast_spell(PLAYER0, PwrK_SLAP, fx, fy, INVALID_THING, CastChk_SkipAvailiabilty),
            (int)can_drop_thing_here(fx, fy, PLAYER0, 1), (int)can_cast_spell(PLAYER0, PwrK_LIGHTNING, fx, fy, INVALID_THING,
            CastChk_SkipAvailiabilty));
    }
}

void world_consecrate_turn(void)
{
    if (!world_mode_active())
        return;
    cs_test();
    if ((game.play_gameturn % 8) != 5)
        return;
    struct WorldRing rings[WORLD_RINGS_MAX];
    memset(rings, 0, sizeof(rings));
    int n = 0;
    /* the Monks */
    const long monk = get_id(creature_desc, "MONK");
    const struct StructureList *clist = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (ThingIndex i = clist->index; (i > 0) && (guard++ <= (long)THINGS_COUNT) && (n < WORLD_RINGS_MAX);)
    {
        struct Thing *c = thing_get(i);
        if (thing_is_invalid(c))
            break;
        i = c->next_of_class;
        if ((c->model != monk) || (c->owner != PLAYER_GOOD) || creature_is_dying(c) || creature_is_being_unconscious(c) ||
            creature_is_kept_in_custody(c) || thing_is_picked_up(c))
            continue;
        rings[n].stl_x = c->mappos.x.stl.num;
        rings[n].stl_y = c->mappos.y.stl.num;
        rings[n].r = CS_MONK_RING;
        n++;
    }
    /* the chapels: the heroes' Temples */
    for (int i = 1; (i < ROOMS_COUNT) && (n < WORLD_RINGS_MAX); i++)
    {
        const struct Room *room = room_get(i);
        if (!room_exists(room) || (room->owner != PLAYER_GOOD) || !room_role_matches(room->kind, RoRoF_CrHappyPray))
            continue;
        rings[n].stl_x = room->central_stl_x;
        rings[n].stl_y = room->central_stl_y;
        rings[n].r = CS_CHAPEL_RING;
        n++;
    }
    if (memcmp(rings, game.world_rings, sizeof(rings)) != 0)
        memcpy(game.world_rings, rings, sizeof(rings));
}

void world_consecrate_local_turn(void)
{
    if (!world_mode_active() || ((game.play_gameturn % 20) != 0))
        return;
    for (int i = 0; i < WORLD_RINGS_MAX; i++)
    {
        const struct WorldRing *rg = &game.world_rings[i];
        if (rg->r == 0)
            continue;
        const int points = 8 + 2 * rg->r;
        const double rad = (double)rg->r * STL_PER_SLB * COORD_PER_STL;
        for (int k = 0; k < points; k++)
        {
            const double a = 2.0 * M_PI * k / points + (double)(game.play_gameturn / 20) * 0.15;
            struct Coord3d pos;
            pos.x.val = (MapCoord)(subtile_coord_center(rg->stl_x) + (long)(cos(a) * rad));
            pos.y.val = (MapCoord)(subtile_coord_center(rg->stl_y) + (long)(sin(a) * rad));
            if ((pos.x.val <= 0) || (pos.y.val <= 0))
                continue;
            pos.z.val = get_floor_height_at(&pos) + 16;
            create_effect_element(&pos, TngEffElm_WhiteSparklesSmall, PLAYER_GOOD);
        }
    }
}
