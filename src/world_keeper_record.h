/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_keeper_record.h
 *     Endless worlds: a keeper's lifetime record, its Heart's migration and the Fill room's marks, as kept in struct Game.
 * @par Purpose:
 *     Plain data only (struct Game includes it): the record the Keeper panel's Profile shows (world_keeper_panel.c)
 *     and the state of Move The Heart (world_migrate.c). Both live in struct Game so that a thin client gets them with
 *     the rest of the game (the player levels' XP does the same).
 */
/******************************************************************************/
#ifndef DK_WORLD_KEEPER_RECORD_H
#define DK_WORLD_KEEPER_RECORD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(1)

/** A player's record kept with the world (save sections PST1 and the extra REC2). The first four are the dedicated
 *  server console's record (PST1); the rest came with the Keeper panel (2026-11-19). Kept through Reset Dungeon and
 *  Start again (a lifetime record: the user, 2026-11-19), cleared when another player takes the seat. */
struct WorldPlayerStats {
    uint32_t hearts;           /* keeper hearts destroyed (Keepers Defeated) */
    uint32_t outposts;         /* heroes outposts destroyed */
    uint32_t portals;          /* portals captured */
    uint32_t play_turns;       /* game turns played (Time Played) */
    uint32_t hearts_lost;      /* its own Heart destroyed */
    uint32_t heroes_killed;    /* heroes its creatures (and traps) killed */
    uint32_t creatures_killed; /* other keepers' and wild creatures its creatures killed */
    uint32_t dug_farthest;     /* the farthest slab its imps dug or mined out, in slabs from its Heart */
    uint16_t portrait;         /* the creature kind whose portrait the Profile shows (0: none chosen) */
    uint16_t reserved;
};

/** Move The Heart (world_migrate.c): one per player. Places are world slabs (the engine window moves). */
struct WorldMigration {
    uint8_t state;     /* WMig_* (world_migrate.h) */
    uint8_t carriers;  /* how many of the four corners have their Imp now */
    uint8_t carried;   /* the Heart is up on its carriers (else it lies on the floor) */
    uint8_t moved;     /* the Heart has moved since the game began: creatures use rooms near it only */
    int32_t to_x, to_y;     /* the new home's middle slab */
    int32_t from_x, from_y; /* the old home's middle slab */
    int32_t from_z;         /* the Heart's height over its old pedestal */
    uint16_t imp[4];        /* the Imp at each corner (thing index, 0: none) */
    int32_t imp_made[4];    /* and its creation turn (an index can be used again) */
};
/** The Fill room (world_fill.c): one floor slab marked to be filled with earth. Kept in struct Game (a thin client
 *  draws the marks' wireframes) and in the world save (FIL1). Places are world slabs. */
#define WORLD_FILL_MAX 2048
struct WorldFillMark {
    int32_t x, y;         /* the slab */
    uint8_t owner;        /* the keeper who placed it */
    uint8_t hits;         /* the Imp's dig strokes so far */
    uint8_t blocked;      /* filling it now would shut a creature (or the Imp) in: it waits */
    uint8_t used;         /* 1: the entry is a mark (0: free; struct Game starts zeroed) */
    uint16_t worker[3];   /* the Imps coming or filling, up to three as at a dig (thing index, 0: none) */
    uint16_t wait;        /* turns the Imps have waited at it for the slab to be clear */
    uint16_t walk_from[3]; /* when each of them set off (the turn's low 16 bits) */
    uint16_t reserved2;
    int32_t check_turn;   /* when "blocked" was last worked out (while an Imp walks to it: when it set off) */
};

#pragma pack()

#ifdef __cplusplus
}
#endif
#endif
