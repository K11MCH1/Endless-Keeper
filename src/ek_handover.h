/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file ek_handover.h
 *     Endless Keeper: the game hands over to the launcher once, after the programs were renamed (ek_handover.c).
 */
/******************************************************************************/
#ifndef EK_HANDOVER_H
#define EK_HANDOVER_H

#ifdef __cplusplus
extern "C" {
#endif

/** Windows: EndlessKeeper.exe started with no arguments while the bridge keeperfx.exe is still beside it starts
 *  ek-launcher.exe instead (which moves the shortcuts and removes the bridge). 1: it did, the game ends; 0: play. */
int ek_handover_to_launcher(int argc);

#ifdef __cplusplus
}
#endif

#endif
