/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_rivals.h
 *     Endless worlds: rival pairs, two enemy keepers locked in a war that never ends.
 * @par Purpose:
 *     See world_rivals.c. The generator places the pairs (world_gen.c, generator version 9 on); the keepers' brain
 *     (world_keeper_ai.c) fights their war; this keeps it going and decides what they do about humans.
 */
/******************************************************************************/
#ifndef DK_WORLD_RIVALS_H
#define DK_WORLD_RIVALS_H

#include "globals.h"
#include "bflib_basics.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Room;

/** What a rival keeper's brain needs for its war (engine positions). */
struct WorldRivalView {
    PlayerNumber rival;               /* its rival's player; PLAYER_NONE while there is no war (its rival asleep, gone) */
    TbBool truce;                     /* truced with its rival (against a human, or for the safe day) */
    TbBool rest;                      /* the truce is the safe day's: it goes for humans' creatures on sight */
    TbBool invaded;                   /* a human's creature (an Imp too) stands in its territory */
    struct Coord3d invader_pos;       /* the one nearest its heart */
    PlayerNumber invader;
    TbBool rival_invaded;             /* ... in its rival's territory */
    struct Coord3d rival_invader_pos;
    TbBool field_known;               /* the battlefield is loaded: the positions below are good */
    struct Coord3d field_mid;         /* the battlefield's middle */
    struct Coord3d field_own;         /* a third of the way from its own end of the battlefield */
    struct Coord3d field_far;         /* a third of the way from its rival's end */
    struct Coord3d rival_heart;
    int field_kind;                   /* WgField */
};

/** False when the player is no keeper of a rival pair. */
TbBool world_rivals_view(PlayerNumber plyr_idx, struct WorldRivalView *out);
/** Is the player a keeper of a rival pair (the stock attack plans never run for one)? */
TbBool world_rivals_is_rival(PlayerNumber plyr_idx);
/** May the player's Imps claim (or convert) this slab? A rival never takes its rival's home ground, nor its third of
 *  the battlefield. */
TbBool world_rivals_may_claim(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y);
/** Does the attacker leave this room alone (a rival never wrecks its rival's rooms)? */
TbBool world_rivals_spare_room(PlayerNumber attacker, const struct Room *room);
/** Does the attacker leave this keeper's Dungeon Heart alone (a rival never attacks its rival's heart)? */
TbBool world_rivals_spare_heart(PlayerNumber attacker, PlayerNumber heart_owner);
/** Is the player a rival pair's keeper truced with this owner, its rival (its Hand drops creatures on that ground)? */
TbBool world_rivals_truce_ground(PlayerNumber plyr_idx, PlayerNumber owner);
/** May its portal attract this kind (a rival keeper: at most two Dragons)? */
TbBool world_rivals_may_attract(PlayerNumber plyr_idx, ThingModel crmodel);
/** May the keeper build or dig here (a rival grows only away from its rival)? */
TbBool world_rivals_site_ok(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y);

void world_rivals_turn(void);
void world_rivals_reset(void);
size_t world_rivals_save_size(void);
void world_rivals_save(uint8_t *buf);
TbBool world_rivals_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
