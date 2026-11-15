/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_laws.c
 *     Endless world: Circle Laws (2026-11-13; see world_laws.h).
 * @par Purpose:
 *     Decisions on the page's open points (the user was away; the recommended choice each time):
 *     - One law per circle, from the second to the ninth; a circle's law holds in that circle only (they do not add
 *       up going outward), for every keeper there, human or computer, and under the caretaker too (it is the place's).
 *     - Where a law looks: healing, hunger and the fallen at the creature; digging and gold at the slab dug; spells
 *       at the spot cast on; rooms at the slab built on; portals at the portal.
 *     - Shown: a third line under the circle's banner when a keeper first breaks into it (on a joined player's
 *       machine too: the banner is drawn there).
 *     - "Portals bring nothing until that region's fortress falls" became "portals there bring nothing": a portal's
 *       fortress is not known where the portal stands (the fortress may be frozen far away).
 *     No state of its own: the laws come from the circles, so there is nothing to save.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_laws.h"
#include "world_circles.h"
#include "world_stream.h"
#include "world_daynight.h"
#include "world_attract.h"
#include <string.h>
#include "bflib_basics.h"
#include "config_magic.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "game_legacy.h"
#include "map_data.h"
#include "player_data.h"
#include "player_instances.h"
#include "room_data.h"
#include "slab_data.h"
#include "thing_data.h"
#include "post_inc.h"

static const char *const wl_texts[WLaw_COUNT] = {
    NULL,
    "Nothing heals outside a Lair",
    "Creatures eat twice as often",
    "Imps will not dig at night outside claimed land",
    "No spell is cast outside claimed land",
    "Gold mined yields half",
    "The fallen are not taken prisoner: they die",
    "Rooms cost twice as much to build",
    "Portals bring nothing",
};

static struct {
    TbBool read;
    unsigned forced;            /* KFX_WORLD_LAWS: laws held everywhere (tests) */
} wl_test;
static unsigned long wl_count[WLaw_COUNT];

static void wl_test_read(void)
{
    if (wl_test.read)
        return;
    wl_test.read = true;
    const char *v = getenv("KFX_WORLD_LAWS");
    if (v == NULL)
        return;
    for (const char *s = v; *s != '\0'; s++)
    {
        if ((*s >= '1') && (*s <= '8'))
            wl_test.forced |= 1u << (*s - '0');
    }
    SYNCLOG("World laws test: laws held everywhere: mask 0x%x", wl_test.forced);
}

int world_law_of_circle(int circle)
{
    if ((circle < 2) || (circle > 9))
        return WLaw_None;
    return circle - 1;
}

const char *world_law_text(int circle)
{
    return wl_texts[world_law_of_circle(circle)];
}

TbBool world_law_at(int law, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if ((law <= WLaw_None) || (law >= WLaw_COUNT) || !world_mode_active() || world_attract_active())
        return false;
    wl_test_read();
    if ((wl_test.forced & (1u << law)) != 0)
        return true;
    return (world_law_of_circle(world_circle_at_engine_slab(subtile_slab(stl_x), subtile_slab(stl_y))) == law);
}

static TbBool wl_keeper(PlayerNumber plyr_idx)
{
    return (plyr_idx >= 0) && (plyr_idx < PLAYERS_COUNT) && (plyr_idx != PLAYER_GOOD) && (plyr_idx != PLAYER_NEUTRAL);
}

static TbBool wl_holds(int law, PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!wl_keeper(plyr_idx) || !world_law_at(law, stl_x, stl_y))
        return false;
    wl_count[law]++;
    return true;
}

static TbBool wl_claimed_by(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    const struct SlabMap *slb = get_slabmap_for_subtile(stl_x, stl_y);
    return !slabmap_block_invalid(slb) && (slabmap_owner(slb) == plyr_idx);
}

TbBool world_law_blocks_heal(const struct Thing *creatng)
{
    if (!thing_is_creature(creatng))
        return false;
    const MapSubtlCoord x = creatng->mappos.x.stl.num, y = creatng->mappos.y.stl.num;
    if (!wl_keeper(creatng->owner) || !world_law_at(WLaw_NoHeal, x, y))
        return false;
    const struct Room *room = subtile_room_get(x, y);
    if (room_exists(room) && room_role_matches(room->kind, RoRoF_CrHealSleep))
        return false;
    wl_count[WLaw_NoHeal]++;
    return true;
}

TbBool world_law_double_hunger(const struct Thing *creatng)
{
    return wl_holds(WLaw_Hunger, creatng->owner, creatng->mappos.x.stl.num, creatng->mappos.y.stl.num);
}

TbBool world_law_blocks_dig(const struct Thing *creatng, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!wl_keeper(creatng->owner) || world_is_day() || !world_law_at(WLaw_NightDig, stl_x, stl_y))
        return false;
    if (wl_claimed_by(creatng->owner, creatng->mappos.x.stl.num, creatng->mappos.y.stl.num))
        return false;
    wl_count[WLaw_NightDig]++;
    return true;
}

TbBool world_law_blocks_spell(PlayerNumber plyr_idx, PowerKind pwkind, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    switch (pwkind)
    {
    case PwrK_HAND: case PwrK_SLAP: case PwrK_PICKUPCRTR: case PwrK_PICKUPGOLD: case PwrK_PICKUPFOOD:
        return false; /* the Hand is not a spell */
    default:
        break;
    }
    const struct PowerConfigStats *powerst = get_power_model_stats(pwkind);
    if ((powerst->can_cast_flags & PwCast_Anywhere) != 0)
        return false; /* cast without a place */
    if (!wl_keeper(plyr_idx) || !world_law_at(WLaw_Spells, stl_x, stl_y) || wl_claimed_by(plyr_idx, stl_x, stl_y))
        return false;
    wl_count[WLaw_Spells]++;
    return true;
}

long world_law_gold(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y, long gold)
{
    if ((gold <= 0) || !wl_holds(WLaw_HalfGold, plyr_idx, stl_x, stl_y))
        return gold;
    return gold / 2;
}

TbBool world_law_no_prisoners(const struct Thing *creatng)
{
    /* anyone who falls there: a raid's hero as much as a keeper's creature (the law is the place's) */
    if (!world_law_at(WLaw_NoPrisoners, creatng->mappos.x.stl.num, creatng->mappos.y.stl.num))
        return false;
    wl_count[WLaw_NoPrisoners]++;
    return true;
}

long world_law_room_cost(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y, long cost)
{
    if ((cost <= 0) || !wl_holds(WLaw_Rooms, plyr_idx, stl_x, stl_y))
        return cost;
    return cost * 2;
}

TbBool world_law_blocks_portal(const struct Room *room)
{
    return room_exists(room) && wl_holds(WLaw_Portals, room->owner, room->central_stl_x, room->central_stl_y);
}

void world_laws_turn(void)
{
    if (!world_mode_active() || ((game.play_gameturn % 3000) != 0))
        return;
    wl_test_read();
    if (wl_test.forced == 0)
        return;
    SYNCLOG("World laws: held so far: no heal %lu, hunger %lu, night dig %lu, spells %lu, half gold %lu, no prisoners %lu,"
        " rooms %lu, portals %lu", wl_count[1], wl_count[2], wl_count[3], wl_count[4], wl_count[5], wl_count[6],
        wl_count[7], wl_count[8]);
}
