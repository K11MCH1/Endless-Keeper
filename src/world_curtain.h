/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_curtain.h
 *     Endless world: the loading screen as a curtain over a running game (2026-11-14).
 * @par Purpose:
 *     When this machine's keeper is taken to another place of the world (a World Boss's arena in the world apart,
 *     and home again), the game's own loading picture covers the screen for the time the journey takes: nothing of
 *     the world or the panel is drawn under it, and nothing can be clicked. The game itself runs on behind it, on
 *     this machine and on a server alike: it is a picture on one machine's screen, not a state of the game.
 */
/******************************************************************************/
#ifndef DK_WORLD_CURTAIN_H
#define DK_WORLD_CURTAIN_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The curtain goes up now and stays up for at least `min_turns` game turns, and for as long after that as it is
 *  held (world_curtain_hold). Raising one that is up only lengthens it. */
void world_curtain_raise(long min_turns);
/** While this is called with true every turn, the curtain stays up past its least time (what it hides isn't there
 *  yet); with false, or not called, it comes down when that time is over. */
void world_curtain_hold(TbBool hold);
/** Is the curtain drawn this frame? It always comes down by itself after WORLD_CURTAIN_MAX_TURNS, or after
 *  WORLD_CURTAIN_MAX_MS by the clock (a paused game's turns stand still), whatever holds it: nothing may leave a
 *  player behind it. */
TbBool world_curtain_up(void);
/** Draws it: the whole frame (instead of the view and the panel). Inside an open frame. */
void world_curtain_draw(void);
/** Down at once, and forgotten (a world ends, a save is loaded). */
void world_curtain_reset(void);

#define WORLD_CURTAIN_MAX_TURNS (20 * 20)
#define WORLD_CURTAIN_MAX_MS 30000

#ifdef __cplusplus
}
#endif
#endif
