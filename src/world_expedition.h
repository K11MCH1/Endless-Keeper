/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_expedition.h
 *     Endless worlds: the Expedition spell (three Imps scout towards a place without digging).
 * @par Purpose:
 *     The user, 2026-11-21: a keeper spell, Expedition, cast anywhere on the map (the 3D view or the full map). It
 *     costs 10,000 gold and sends three of the keeper's Imps (held away meanwhile: counted in the creature list, but
 *     they can't be picked up, as Imps an enemy keeps in prison). On the minimap and the full map they show as three
 *     red dots, like raiders: from the dungeon's outermost wall on the line towards the place, then in a straight line
 *     to it; the place itself pings in red (the Alarm Bells' Call to Arms ring). Within 15 slabs of their way they find
 *     hero structures, portals, other keepers, spellbooks, specials and neutral creatures, and mark each with the
 *     query mode's question mark on both maps, nothing else: no fog is lifted, nothing is shown. At the place the
 *     expedition ends and the Imps are back at once. Only its keeper sees the dots, the ping and the marks. See
 *     world_expedition.c.
 */
/******************************************************************************/
#ifndef DK_WORLD_EXPEDITION_H
#define DK_WORLD_EXPEDITION_H

#include "globals.h"
#include "bflib_basics.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

/** The POWER_EXPEDITION power of the loaded config, or 0 when it has none. */
PowerKind world_expedition_power_kind(void);
/** Starts an expedition of the keeper's towards a world slab, paying for it unless free. Game state: the server, or
 *  every machine in step. Lb_SUCCESS when it set off. */
TbResult world_expedition_cast(PlayerNumber plyr_idx, int64_t wx, int64_t wy, TbBool free);
/** magic.cfg UseFunction magic_use_power_expedition (cast in the 3D view, at an engine subtile). */
TbResult world_expedition_cast_at_subtile(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y, TbBool free);
/** The full map: the local player clicks a place with the spell chosen. True when the click was taken (a packet sent). */
TbBool world_expedition_map_click(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** engine_redraw.c, the full map: with the spell chosen its pointer plays there too. True when it was set. */
TbBool world_expedition_map_pointer(void);
/** packets.c: PckA_WorldExpedition (world slab in par1, par2). */
void world_expedition_packet(PlayerNumber plyr_idx, int32_t wx, int32_t wy);

/** An Imp away on an expedition (the creature list counts it, unpickable, as an Imp kept in an enemy's prison). */
TbBool world_expedition_imp(const struct Thing *thing);
/** The creature state of the Imps away (crstates.cfg ImpOnExpedition). */
short imp_on_expedition(struct Thing *thing);
/** crstates.cfg without the state (an older file): it is made here. */
void world_expedition_states_fallback(void);

/** custom_sprites.c, after the fxdata zips: ENDLESS_EXPEDITION_BIG, _BUTTON, _CURSOR (8 frames), _QMARK. */
void world_expedition_register_icons(void);
/** vidmode.c: the spell pointer's hot spot (its middle: the frames are bigger than the stock 32 pixels). True when
 *  the sprite is the Expedition's pointer. */
TbBool world_expedition_pointer_hotspot(long spridx, long *hot_x, long *hot_y);

/** What the maps draw for a keeper (map coordinates of the engine, which may lie off the loaded map). */
#define WEX_VIEW_MAX 256
struct WorldExpeditionView {
    int dots, pings, marks;
    int32_t dot_x[WEX_VIEW_MAX], dot_y[WEX_VIEW_MAX];
    int32_t ping_x[WEX_VIEW_MAX], ping_y[WEX_VIEW_MAX];
    int32_t mark_x[WEX_VIEW_MAX], mark_y[WEX_VIEW_MAX];
};
void world_expedition_view(PlayerNumber plyr_idx, struct WorldExpeditionView *view);
/** The question mark: a panel sprite for the full map, and palette indexes for the minimap's buffer. */
long world_expedition_qmark_icon(void);
void world_expedition_qmark_bitmap(const unsigned char **index, const unsigned char **mask, int *w, int *h);
/** The ping's colour and the dots' (red). */
unsigned char world_expedition_colour(void);

/** world_stream.c's chunk keeper: the ground round each expedition is loaded, ahead of it too. */
void world_expedition_want_chunks(void (*want)(int64_t wx, int64_t wy, int r));

void world_expedition_turn(void);
void world_expedition_seat_reset(PlayerNumber plyr_idx);
size_t world_expedition_save_size(void);
void world_expedition_save(uint8_t *buf);
TbBool world_expedition_load(const uint8_t *buf, size_t len);
void world_expedition_reset(void);

#ifdef __cplusplus
}
#endif
#endif
