/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_outpost.h
 *     Endless world: heroes outposts.
 * @par Purpose:
 *     Small hero strongholds (21x21 slabs) found by exploring: one iron entrance, a Heroes Heart at the far end,
 *     rooms with hero portals, heavy traps. Breaking the entrance door wakes the portals; coming near the heart's
 *     portal wakes it; the heart drops a heroes' spellbook (world_progress).
 */
/******************************************************************************/
#ifndef DK_WORLD_OUTPOST_H
#define DK_WORLD_OUTPOST_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

#define OUTPOST_SIZE 21
#define OUTPOST_OVERLAY_THINGS 64

/** What the outposts put into one generated chunk (16x16 slabs, index ly*16+lx). */
struct OutpostOverlayThing {
    uint8_t oclass;          /* TCls_Object, TCls_Trap */
    uint8_t heroes;          /* owned by the heroes (else neutral) */
    uint16_t model;
    int32_t x, y;            /* map coordinates relative to the chunk's first subtile */
    uint16_t extra;          /* hero gate number, trap shots */
};
struct OutpostOverlay {
    uint8_t set[256];        /* 1: the outpost decides this cell */
    uint8_t kind[256];       /* engine slab kind */
    uint8_t heroes[256];     /* owned by the heroes */
    uint8_t wall[256];       /* an automatic wall */
    uint8_t door[256];       /* door model, 0 none */
    int thing_count;
    struct OutpostOverlayThing things[OUTPOST_OVERLAY_THINGS];
};

/** The outposts' cells and things in world chunk (cx, cy); returns how many cells they decide. */
int world_outpost_overlay(int32_t cx, int32_t cy, struct OutpostOverlay *ov);
/** Is there an outpost (or its way in) in this box of world slabs? */
TbBool world_outpost_in_box(int64_t x1, int64_t y1, int64_t x2, int64_t y2);
/** Test hooks: the outpost nearest a world slab (within 3 regions): its box corner, entrance and outward step, heart
 *  centre and the heart's portal, in world slabs. */
TbBool world_outpost_nearest(int64_t wx, int64_t wy, int64_t *x0, int64_t *y0, int64_t *ex, int64_t *ey, int *odx,
    int *ody, int64_t *hx, int64_t *hy, int64_t *gx, int64_t *gy);
/** The simulating machine's turn: entrances broken, hearts approached. */
void world_outposts_turn(void);
/** A heroes heart is gone: an outpost's drops its spellbook. */
void world_outpost_heart_destroyed(const struct Thing *heartng);
/** A new world, a load, the end: outposts on (a world made before them turns them off while it loads). */
void world_outposts_reset(void);
void world_outposts_set_enabled(TbBool on);
size_t world_outposts_save_size(void);
void world_outposts_save(uint8_t *buf);
TbBool world_outposts_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
