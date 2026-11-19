/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_threat.h
 *     Endless world: how strong a keeper's army is, for sizing raids and wild packs (2026-11-18).
 * @par Purpose:
 *     Raids and wild packs followed the keeper's single strongest creature, so one creature that outgrew the army
 *     brought raids of three to five parties far above the rest of it (the user, 2026-11-18: "Raiders are
 *     overperforming ... We need to move away from the based-on-highest-creature-level rule"). This measures the
 *     whole army instead: each creature's fighting power, the levels of its middle and its upper quarter.
 */
/******************************************************************************/
#ifndef DK_WORLD_THREAT_H
#define DK_WORLD_THREAT_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A keeper's army (imps not counted). */
struct WorldArmy {
    double power;   /* fighting power: its 10 strongest creatures in full, the rest at a quarter */
    int count;      /* creatures counted */
    int median;     /* the middle creature's level (1-based; 1 for none) */
    int upper;      /* the level a quarter of the army is at or above (1-based; 1 for none) */
    int best;       /* the strongest creature's level (for the log) */
};

/** The fighting power of one creature of a kind at a level (1-based): its health times its strength, both grown by the
 *  game's per-level increases, in thousands. Heroes are priced the same way, so a raid and an army compare. */
double world_threat_power(ThingModel model, int level);
/** A keeper's army now. */
void world_threat_army(PlayerNumber plyr_idx, struct WorldArmy *army);
/** The level wild packs and raid followers are measured against: the army's upper quarter (1 for none). */
int world_threat_upper_level(PlayerNumber plyr_idx);

#ifdef __cplusplus
}
#endif
#endif
