/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_setpiece_script.h
 *     Endless world: a set piece's own map script (world_setpiece_script.c), for world_setpiece.c.
 * @par Purpose:
 *     Each placed set piece runs its map's script (.txt) by itself: its own flags, timers and parties, its action
 *     points and hero gates where the piece lies, while its ground is loaded. Commands that change the whole game
 *     (winning, losing, what is available, rules) are left out.
 */
/******************************************************************************/
#ifndef DK_WORLD_SETPIECE_SCRIPT_H
#define DK_WORLD_SETPIECE_SCRIPT_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPS_PLAYERS 8
#define SPS_FLAGS 8
#define SPS_TIMERS 8

struct SpScript;

/** An action point or hero gate of the piece's map. */
struct SpScriptPoint {
    uint16_t num;
    int32_t x, y;               /* map coordinates from the map's top left */
    int32_t range;              /* map coordinates (action points) */
};

/** Where a placed piece is and who its players are now. */
struct SpScriptPlace {
    const char *name;
    int64_t x0, y0;             /* world slab of the map's top left */
    int w, h;
    const struct SpScriptPoint *aps;
    int naps;
    const struct SpScriptPoint *gates;
    int ngates;
    PlayerNumber keeper_of[SPS_PLAYERS]; /* the map's player 1-7 -> the engine player its keeper is now (-1: none) */
    int64_t heart_x[SPS_PLAYERS], heart_y[SPS_PLAYERS]; /* world slabs of the keepers' hearts */
};

/** A placed piece's script state (saved). */
struct SpScriptRun {
    uint32_t since;             /* the game turn it first ran; 0: not yet */
    int32_t flags[SPS_PLAYERS][SPS_FLAGS];
    uint32_t timers[SPS_PLAYERS][SPS_TIMERS]; /* the turn each was set */
    uint16_t nbits;
    uint8_t done[64];           /* a bit per statement run once (up to 512) */
};

/** Reads a map script; NULL when nothing in it can run. kept / dropped: how many commands it keeps and leaves out. */
struct SpScript *sp_script_parse(const char *text, const char *where, int *kept, int *dropped);
void sp_script_free(struct SpScript *s);
/** One pass of a placed piece's script (the caller runs it while the piece's ground is loaded). */
void sp_script_run(const struct SpScript *s, const struct SpScriptPlace *pl, struct SpScriptRun *run);

#ifdef __cplusplus
}
#endif
#endif
