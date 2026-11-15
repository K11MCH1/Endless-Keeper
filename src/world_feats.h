/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_feats.h
 *     Endless world: the Keeper's Rap Sheet.
 * @par Purpose:
 *     Each keeper's tracked feats, written in the game's voice, each in five tiers; each tier reached pays player
 *     XP. The counts live in struct Game (game.world_feat_count, world_feat_tier), so a joined thin client has them;
 *     the simulating machine counts them and saves them (XTR2 record "FTS1"). The Rap Sheet window (!feats or
 *     /feats in the chat line) shows the keeper's own feats and every listed keeper's top three.
 *     Endless worlds only: every entry point does nothing when world_mode_active() is false.
 */
/******************************************************************************/
#ifndef DK_WORLD_FEATS_H
#define DK_WORLD_FEATS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;
struct GuiMenu;
struct GuiButton;

/* WORLD_FEAT_SLOTS (game_legacy.h): room for later feats */
enum WorldFeat {
    WFt_Poultry = 0,   /* heroes turned into chickens */
    WFt_Flyer,         /* creatures dropped into battle */
    WFt_NightShift,    /* outposts cleared between dusk and dawn */
    WFt_Heartbreaker,  /* keepers' hearts destroyed */
    WFt_SlaveDriver,   /* creatures slapped */
    WFt_BoneIdle,      /* Skeletons raised in the Prison */
    WFt_PortalHunter,  /* portals claimed */
    WFt_CleanHands,    /* raids beaten without casting a spell */
    WFt_OldBones,      /* days the Heart has beaten */
    WFt_COUNT
};
#define WORLD_FEAT_TIERS 5

/** Count one (or n) of a feat for a keeper (the simulating machine; human keepers in endless worlds only). */
void world_feats_add(PlayerNumber plyr_idx, int feat, uint32_t n);
/** Hooks. */
void world_feats_chickened(struct Thing *victim, PlayerNumber caster);
void world_feats_dropped(struct Thing *creatng, PlayerNumber plyr_idx);
void world_feats_slapped(struct Thing *creatng, PlayerNumber plyr_idx);
void world_feats_power_used(PlayerNumber plyr_idx, int pwkind);
/** A raid on this keeper is over: beaten (its heroes died or fled) and the day it landed. */
/** A raid has just landed on this keeper (Clean hands counts spells from this turn). */
void world_feats_raid_landed(PlayerNumber plyr_idx);
void world_feats_raid_over(PlayerNumber plyr_idx, TbBool beaten, long landed_day);
/** Each world turn (server or single player): the days feat, the test hook. */
void world_feats_turn(void);
/** Each machine, each turn: the tiers this machine's keeper reached, as messages. */
void world_feats_present(void);
/** A seat starts again: its feats from nothing. */
void world_feats_seat_reset(PlayerNumber plyr_idx);
/** A keeper's tier in a feat (0-5), the feat's name and line. */
int world_feats_tier(PlayerNumber plyr_idx, int feat);
const char *world_feats_name(int feat);
/** A keeper's top three feats (by tier, then by how near the next tier): feat numbers, -1 when fewer. */
void world_feats_top3(PlayerNumber plyr_idx, int out[3]);
/** " (feat tier, feat tier, feat tier)" for a keeper's name in the player panel ("": no feats yet). */
const char *world_feats_top3_text(PlayerNumber plyr_idx);

/** The chat line "!feats" or "/feats" (also "!rapsheet"): opens or closes the Rap Sheet; true when it was one. */
TbBool world_feats_command(const char *text);
extern struct GuiMenu world_feats_menu;
/** Each frame where the menus are kept (world_respawn_prompt_turn): the test hook that opens the window. */
void world_feats_menu_turn(void);

/** The world save's extra ("FTS1"). */
void world_feats_reset(void);
size_t world_feats_save_size(void);
void world_feats_save(uint8_t *buf);
TbBool world_feats_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
