/******************************************************************************/
// Endless Keeper: Misdeeds.
/******************************************************************************/
/** @file world_misdeeds.h
 *     Misdeeds: what each creature of a keeper did in its life (counters), the Vices it earns from them, Rename,
 *     the Rap Sheet tab of the creature query panel, Bad Influence. Endless worlds only: every entry point does
 *     nothing when world_mode_active() is false.
 */
/******************************************************************************/
#ifndef WORLD_MISDEEDS_H
#define WORLD_MISDEEDS_H

#include <stddef.h>
#include <stdint.h>
#include "globals.h"
#include "bflib_basics.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;
struct CreatureControl;
struct GuiButton;
struct GuiMenu;

/** The longest name Rename takes (what fits a packet, the name box and the flower's label). */
#define WORLD_MISDEED_NAME_MAX 9

/** Once per game turn (from world_process_turn): the test hook. */
void world_misdeeds_turn(void);
/** Once per creature per turn (from process_landscape_affecting_creature); samples one turn in 32. */
void world_misdeeds_creature_tick(struct Thing *thing);
/** A creature was killed (or knocked out) by killertng (may be invalid). */
void world_misdeeds_on_kill(struct Thing *victim, struct Thing *killertng);
/** A creature took this much damage. */
void world_misdeeds_on_damage(struct Thing *victim, HitPoints dmg);
/** A hit of attacker (may be invalid) on victim: the damage after the vices (Do-Gooder's Bane, Territorial, the dark). */
HitPoints world_misdeeds_adjust_damage(struct Thing *victim, struct Thing *attacker, HitPoints dmg);
/** A creature's top speed after its vices. */
long world_misdeeds_speed(const struct Thing *thing, long speed);
/** Asbestos Hide: lava doesn't hurt it. */
TbBool world_misdeeds_lava_proof(const struct Thing *thing);
/** Bad Influence: the percentage added to a training creature's experience. */
int world_misdeeds_train_bonus(const struct Thing *thing);
/** A dungeon heart was destroyed: the keepers' creatures near it helped. */
void world_misdeeds_heart_destroyed(const struct Thing *heartng);
/** A creature's control structure is going: its record goes too, unless it was frozen with its chunk. */
void world_misdeeds_on_delete(struct CreatureControl *cctrl);
/** A creature is frozen with its chunk: the record number to keep in the chunk's record (0: none). */
uint16_t world_misdeeds_freeze(const struct Thing *thing);
/** A creature is back from a chunk's record. */
void world_misdeeds_thaw(struct Thing *thing, uint16_t id);
/** Unfinished Business. A creature is about to die for real (cause_creature_death); its thing becomes a corpse
 *  (begin: before the creature is deleted, gives the record to hand to made; made: the corpse, maybe invalid). */
void world_misdeeds_dying(struct Thing *thing);
uint16_t world_misdeeds_corpse_begin(struct Thing *thing);
void world_misdeeds_corpse_made(struct Thing *deadtng, uint16_t id);
/** A body that waits for its rescue does not vanish (for a day). */
TbBool world_misdeeds_body_keep(const struct Thing *deadtng);
/** A waiting body is only its own keeper's to carry: true when that player's imps must leave it alone. */
TbBool world_misdeeds_body_not_for(const struct Thing *deadtng, PlayerNumber plyr_idx);
/** An imp drops this corpse in that Graveyard: true when the corpse is gone (the creature is back, or the body was
 *  nobody's to bury). */
struct Room;
TbBool world_misdeeds_body_home(struct Thing *deadtng, struct Room *room);
/** Bad Blood: a creature came through a portal. */
void world_misdeeds_attracted(struct Thing *thing);
/** The chunk manager: the chunks the waiting bodies lie in stay loaded. */
void world_misdeeds_want_chunks(void (*want)(int64_t wx, int64_t wy, int r));
/** Packets: Rename (the creature and the name packed in par1 and par2) and the pick of a vice. */
void world_misdeeds_packet_rename(PlayerNumber plyr_idx, uint32_t p1, uint32_t p2);
void world_misdeeds_packet_pick(PlayerNumber plyr_idx, ThingIndex idx, int choice);

/** The world save's extra ("MSD1"). */
void world_misdeeds_reset(void);
size_t world_misdeeds_save_size(void);
void world_misdeeds_save(uint8_t *buf);
TbBool world_misdeeds_load(const uint8_t *buf, size_t len);

/** The Rap Sheet tab and the pencil. */
void world_misdeeds_rap_background(struct GuiMenu *gmnu);
void world_misdeeds_pencil_maintain(struct GuiButton *gbtn);
void world_misdeeds_pencil_draw(struct GuiButton *gbtn);
void world_misdeeds_rename_done(struct GuiButton *gbtn);
void world_misdeeds_pick_maintain(struct GuiButton *gbtn);
void world_misdeeds_pick_draw(struct GuiButton *gbtn);
void world_misdeeds_pick_click(struct GuiButton *gbtn);
/** The creature's name above its health flower while the hand hovers over it. top_y: the flower's top. */
/** Once per frame after the world is drawn: the label kept by world_misdeeds_draw_label. */
void world_misdeeds_draw_overlay(void);
void world_misdeeds_draw_label(const struct Thing *thing, long scr_x, long top_y, int flower_h, TbBool under_hand);

#ifdef __cplusplus
}
#endif
#endif
