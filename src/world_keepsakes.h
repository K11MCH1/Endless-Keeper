/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_keepsakes.h
 *     Endless world: keepsakes, loot for named creatures.
 * @par Purpose:
 *     Raid leaders (one in two) and the hearts of heroes' strongholds drop keepsakes. A keepsake lying in the world
 *     goes to the first keeper whose creature comes within reach of it (never an ally of the keeper who last had it)
 *     and is kept in that keeper's storage (world_storage.c). Picked up with the Hand and dropped on one of the
 *     keeper's named creatures that carries none, it is carried: one stat changes (health, strength, armour or speed,
 *     +5 to +15%). It shows after the creature's name and on its Rap Sheet. When its bearer dies it drops on the spot.
 *     The bearer is kept by its Misdeeds record (which survives a chunk freezing and a return from the grave); what a
 *     creature carries is copied into struct Game for joined players. Saved as the XTR2 record "KSK1".
 */
/******************************************************************************/
#ifndef DK_WORLD_KEEPSAKES_H
#define DK_WORLD_KEEPSAKES_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;
struct Coord3d;
struct Dungeon;

#define WORLD_KEEPSAKES_MAX 1024

/** Is this object a keepsake (one lying in the world or in storage)? */
TbBool world_keepsake_is_object(const struct Thing *thing);
/** A keepsake may drop here (a raid leader died, a stronghold's heart fell): `chance` in percent. */
void world_keepsakes_drop(const struct Coord3d *pos, int chance, uint32_t salt, PlayerNumber last_owner);
/** A held object was dropped by a keeper's Hand: a keepsake dropped on a named creature is given to it. */
void world_keepsakes_hand_dropped(struct Dungeon *dungeon, struct Thing *objtng);
/** A creature is about to die: its keepsake drops on the spot. */
void world_keepsakes_bearer_dying(struct Thing *creatng);
/** The stats a creature's keepsake changes. */
long world_keepsakes_speed(const struct Thing *creatng, long speed);
HitPoints world_keepsakes_max_health(const struct Thing *creatng, HitPoints health);
HitPoints world_keepsakes_adjust_damage(const struct Thing *victim, const struct Thing *attacker, HitPoints dmg);
/** What a creature carries, for its name label (" (a hare's foot)") and its Rap Sheet ("" when nothing). */
const char *world_keepsakes_label(const struct Thing *creatng);
const char *world_keepsakes_line(const struct Thing *creatng);
/** Each world turn (server or single player): keepsakes found, bearers followed, the test hook. */
void world_keepsakes_turn(void);

void world_keepsakes_reset(void);
size_t world_keepsakes_save_size(void);
void world_keepsakes_save(uint8_t *buf);
TbBool world_keepsakes_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
