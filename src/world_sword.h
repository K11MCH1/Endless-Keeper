/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_sword.h
 *     Endless world: Take Up the Sword (2026-11-13).
 * @par Purpose:
 *     A keeper invites another player to lead the raids on their own dungeon: the guest is put into the raid's
 *     leader in first person and may move from hero to hero while the raid lasts.
 */
/******************************************************************************/
#ifndef DK_WORLD_SWORD_H
#define DK_WORLD_SWORD_H

#include "bflib_basics.h"
#include "globals.h"
#include "net_main.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

/** May this user take control of this creature (a hero of a raid they were invited to lead)? */
TbBool world_sword_may_drive(NetUserId user, const struct Thing *thing);
/** Is a guest inside this hero (its keeper's creatures get twice the experience for hitting it)? */
TbBool world_sword_driven(const struct Thing *thing);
/** A seated player's "!sword ..." line on a dedicated server: 1 (handled). */
TbBool world_sword_command(int user, PlayerNumber plyr_idx, const char *args);
void world_sword_turn(void);
/** The server: is this player a guest leading a raid now? (The stream stays fogged for it: what the hero sees is
 *  revealed to the guest's keeper as it goes, which is all the guest is sent.) */
TbBool world_sword_unfogged(PlayerNumber plyr_idx);
/** A thin client: a server notice that puts this player into a hero or takes them out (not chat). */
TbBool world_sword_net_notice(const char *text);
/** A thin client: goes into the hero once it is on this machine. */
void world_sword_local_turn(void);

/** A seat gets a new dungeon (starts again, or a new player): a guest's invitations and drive end. */
void world_sword_player_reset(PlayerNumber plyr_idx);
void world_sword_reset(void);
size_t world_sword_save_size(void);
void world_sword_save(uint8_t *buf);
TbBool world_sword_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
