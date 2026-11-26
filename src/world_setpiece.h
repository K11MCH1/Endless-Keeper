/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_setpiece.h
 *     Endless world: set pieces, whole maps placed at random in a generated world.
 * @par Purpose:
 *     A set piece is a normal KeeperFX map (made in Unearth or the original editor, any size up to 170x170 slabs) in
 *     its own folder levels/endless_cfgs/setpieces/<name>/ with a setpiece.cfg (biomes, weight, chance). Worlds of
 *     generator version 9 on place them by seed in fresh ground; they blend into the land round them. The creator's
 *     own pieces (setpieces/official/) are used only when the launcher's option is on (keeperfx.cfg
 *     OFFICIAL_SET_PIECES). See world_setpiece.c.
 */
/******************************************************************************/
#ifndef DK_WORLD_SETPIECE_H
#define DK_WORLD_SETPIECE_H

#include "bflib_basics.h"
#include "globals.h"
#include "world_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The first generator version whose worlds get set pieces (the user, 2026-11-25: "v9"). */
#define WORLD_SETPIECES_FROM_VERSION 9

#define SETPIECE_OVERLAY_THINGS 384
#define SETPIECE_CHUNK_KEEPERS 8
#define SETPIECE_NAME_LEN 32

/** Owners in a set piece's overlay. */
enum SetPieceOwner {
    SPO_Neutral = 0,
    SPO_Heroes = 1,
    SPO_Keeper0 = 2, /* + index in SetPieceOverlay.keepers */
};

struct SetPieceThing {
    uint8_t oclass, owner, level;   /* owner: SetPieceOwner; level: creature level 1..10 */
    uint16_t model;
    int32_t x, y;                   /* map coordinates relative to the chunk's first subtile */
    uint16_t extra;                 /* hero gate number, trap shots */
    uint32_t gold;                  /* a gold pile's gold (0: the model's own) */
};

/** A set piece's keeper standing in the chunk (world_stream.c registers it with the world's keepers). */
struct SetPieceKeeper {
    int64_t heart_x, heart_y;       /* world slabs */
    int64_t bbox_x1, bbox_y1, bbox_x2, bbox_y2;
    int32_t threat_radius;
    int32_t money;
};

/** What the set pieces decide in one world chunk. set[i]: 0 nothing (the land's own), 1 the piece's slab,
 *  2 the land's own but rock becomes earth (the fringe that blends a piece into the land round it). */
struct SetPieceOverlay {
    uint8_t set[WG_CHUNK_CELLS];
    uint8_t kind[WG_CHUNK_CELLS];
    uint8_t owner[WG_CHUNK_CELLS];
    uint8_t texture[WG_CHUNK_CELLS];
    uint8_t door[WG_CHUNK_CELLS];   /* door model on the slab (its kind is claimed floor) */
    uint8_t locked[WG_CHUNK_CELLS];
    int thing_count;
    struct SetPieceThing things[SETPIECE_OVERLAY_THINGS];
    int keeper_count;
    struct SetPieceKeeper keepers[SETPIECE_CHUNK_KEEPERS];
};

/** The set pieces in world chunk (cx, cy) (generated fresh); returns how many cells they decide (0: none, and always
 *  0 in worlds older than WORLD_SETPIECES_FROM_VERSION). */
int world_setpiece_overlay(int32_t cx, int32_t cy, struct SetPieceOverlay *ov);
/** Is a set piece (decided now if its region wasn't yet) in this box of world slabs? Always false in older worlds. */
TbBool world_setpiece_in_box(int64_t x1, int64_t y1, int64_t x2, int64_t y2);
/** After slabs were put on the engine map (chunks loaded): the pieces' own columns go back on those in this box of
 *  engine slabs that are still as the piece made them. */
void world_setpiece_columns_box(MapSlabCoord ex1, MapSlabCoord ey1, MapSlabCoord ex2, MapSlabCoord ey2);
/** Test hooks: KFX_WORLD_SPSURVEY=range, KFX_WORLD_TESTSETPIECE="T[,name]" (see world_setpiece.c). */
void world_setpiece_survey(int64_t range);
void world_setpiece_test_turn(void);
void world_setpiece_reveal_turn(void); /* test switch KFX_WORLD_SETPIECEREVEAL (a played session) */
void world_setpiece_want_chunks(void (*want)(int64_t wx, int64_t wy, int r)); /* (with that switch) */
/** The pieces' own map scripts (world_setpiece_script.c), on the machine that runs the world. */
void world_setpieces_turn(void);
/** XTR2 "SPS1": the scripts' flags, timers and what has run (after SPC2). */
size_t world_setpiece_scripts_save_size(void);
void world_setpiece_scripts_save(uint8_t *buf);
TbBool world_setpiece_scripts_load(const uint8_t *buf, size_t len);
void world_setpiece_scripts_reset(void);
/** A new world or a load: the pieces are read again and nothing is decided. */
void world_setpieces_reset(void);
size_t world_setpieces_save_size(void);
void world_setpieces_save(uint8_t *buf);
TbBool world_setpieces_load(const uint8_t *buf, size_t len);

/** keeperfx.cfg OFFICIAL_SET_PIECES (the launcher's "use the creator's set pieces"; off by default). */
extern TbBool world_official_setpieces;

#ifdef __cplusplus
}
#endif
#endif
