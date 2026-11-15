/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_loyalist.h
 *     Endless world: the last loyalist.
 * @par Purpose:
 *     When a keeper's dungeon falls, the "Start again" box offers one survivor (renamed ones first, then by level) or
 *     the body of the greatest veteran to walk with the keeper. It arrives at the new Heart with its kind, name and
 *     Misdeeds record, at half its level, and does not count toward the strength of the raids (the keeper's best
 *     creature level) until its first fight. The resting mark is saved (XTR2 record "LOY1").
 */
/******************************************************************************/
#ifndef DK_WORLD_LOYALIST_H
#define DK_WORLD_LOYALIST_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;
struct GuiButton;

/** The body of the greatest veteran (the dungeon's dead list), as a choice. */
#define WORLD_LOYALIST_VETERAN 0xFFFFu

/** The "Start again" box: its loyalist button (cycles the choices), and the choice to send (par1, par2). */
void world_loyalist_button_click(struct GuiButton *gbtn);
extern char world_loyalist_label[64];
void world_loyalist_box_shown(void);
void world_loyalist_choice(unsigned long *par1, unsigned long *par2);
/** The respawn packet's choice (simulating machine): kept for world_loyalist_depart. */
void world_loyalist_choose(PlayerNumber plyr_idx, unsigned long par1, unsigned long par2);
/** Start again, before the old dungeon goes: the chosen creature leaves with its record (it is taken off the map). */
void world_loyalist_depart(PlayerNumber plyr_idx);
/** Start again, after the new Heart is placed: the loyalist arrives there. */
void world_loyalist_arrive(PlayerNumber plyr_idx);
/** Whether a creature is a loyalist that has not fought yet (it doesn't count toward raids). */
TbBool world_loyalist_resting(const struct Thing *thing);
/** Each world turn (server or single player): a resting loyalist that fights stops resting; the test hook. */
void world_loyalist_turn(void);

/** A seat freed without Start again: the records kept for its defeat's notes go. */
void world_loyalist_seat_freed(PlayerNumber p);
void world_loyalist_reset(void);
size_t world_loyalist_save_size(void);
void world_loyalist_save(uint8_t *buf);
TbBool world_loyalist_load(const uint8_t *buf, size_t len);

/** A creature dies: a defeated keeper's noted creature keeps its Misdeeds record for the loyalist choice. */
void world_loyalist_dying(struct Thing *thing);

#ifdef __cplusplus
}
#endif
#endif
