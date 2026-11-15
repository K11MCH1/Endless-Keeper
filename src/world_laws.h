/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_laws.h
 *     Endless world: Circle Laws (2026-11-13).
 * @par Purpose:
 *     From the second circle outward each circle has one law of its own, which holds in that place for every keeper
 *     digging there, and is shown under the circle's banner when a keeper first breaks into it.
 */
/******************************************************************************/
#ifndef DK_WORLD_LAWS_H
#define DK_WORLD_LAWS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;
struct Room;

enum WorldLaw {
    WLaw_None = 0,
    WLaw_NoHeal,       /* nothing heals outside a Lair */
    WLaw_Hunger,       /* creatures eat twice as often */
    WLaw_NightDig,     /* imps will not dig at night outside claimed land */
    WLaw_Spells,       /* no spells outside claimed land */
    WLaw_HalfGold,     /* gold mined there yields half */
    WLaw_NoPrisoners,  /* the fallen are not taken prisoner: they die */
    WLaw_Rooms,        /* rooms cost twice as much to build */
    WLaw_Portals,      /* portals there bring nothing */
    WLaw_COUNT,
};

/** The law of a circle (1..9); WLaw_None for the first. */
int world_law_of_circle(int circle);
/** The law's text, for the banner (NULL: none). */
const char *world_law_text(int circle);
/** Does this law hold at this subtile? */
TbBool world_law_at(int law, MapSubtlCoord stl_x, MapSubtlCoord stl_y);

/* the hooks: each is false / unchanged outside an endless world, for heroes and neutral things */
TbBool world_law_blocks_heal(const struct Thing *creatng);
TbBool world_law_double_hunger(const struct Thing *creatng);
TbBool world_law_blocks_dig(const struct Thing *creatng, MapSubtlCoord stl_x, MapSubtlCoord stl_y);
TbBool world_law_blocks_spell(PlayerNumber plyr_idx, PowerKind pwkind, MapSubtlCoord stl_x, MapSubtlCoord stl_y);
long world_law_gold(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y, long gold);
TbBool world_law_no_prisoners(const struct Thing *creatng);
long world_law_room_cost(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y, long cost);
TbBool world_law_blocks_portal(const struct Room *room);
/** Every few thousand turns, a log line of how often each law held (tests). */
void world_laws_turn(void);

#ifdef __cplusplus
}
#endif
#endif
