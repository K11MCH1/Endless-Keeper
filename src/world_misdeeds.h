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
void world_misdeeds_on_damage(struct Thing *victim, HitPoints dmg, PlayerNumber dealer);
/** A hit of attacker (may be invalid) on victim: the damage after the vices (Do-Gooder's Bane, Territorial, the dark). */
HitPoints world_misdeeds_adjust_damage(struct Thing *victim, struct Thing *attacker, HitPoints dmg);
/** A creature's top speed after its vices. */
long world_misdeeds_speed(const struct Thing *thing, long speed);
/** Afraid of Nothing in the Dark: it never loses its nerve at night (world_watchfire.c). */
TbBool world_misdeeds_fearless_in_dark(const struct Thing *thing);
/** Asbestos Hide: lava doesn't hurt it. */
TbBool world_misdeeds_lava_proof(const struct Thing *thing);
/** Bad Influence: the percentage added to a training creature's experience. */
/** Virtues: a creature resting after a cure on the rack (it is not trained meanwhile). */
TbBool world_misdeeds_resting(const struct Thing *thing);
int world_misdeeds_train_bonus(const struct Thing *thing);
/** Contracts: a creature's wage (0 on a contract without one). */
GoldAmount world_misdeeds_pay(const struct Thing *thing, GoldAmount pay);
/** Contracts: a creature on a contract without a wage: it is owed no payday and wants no salary. */
TbBool world_misdeeds_no_wage(const struct Thing *thing);
/** Contracts: how many meals fill a creature (doubled on Bed and Board). */
long world_misdeeds_hunger(const struct Thing *thing, long hunger);
/** Seeing Through Their Eyes: a possessed Imp digs harder. */
long world_misdeeds_dig_damage(const struct Thing *digger, long damage);
/** Marching Order: an ability's reach for this creature (the Rear reaches further). */
long world_misdeeds_range(const struct Thing *thing, long range);
/** Regimens: a creature's stat (1 strength, 2 defence, 3 dexterity, 4 health) with its regimens. */
long world_misdeeds_regimen_stat(const struct Thing *thing, int stat, long value);
/** Lessons: the farthest a creature uses an ability from (farther when mastered). */
long world_misdeeds_inst_range(const struct Thing *thing, CrInstance inst, long range);
/** Lessons: the level a creature learns its kind's ability in this slot at (natural: its kind's level). */
int world_misdeeds_learn_level(const struct Thing *thing, int slot, int natural);
/** Lessons: a mastered ability's reset time; true when changed. */
TbBool world_misdeeds_reset_time(const struct Thing *thing, CrInstance inst, int32_t *ritime);
/** Lessons: a shot's damage from a mastered ability. */
HitPoints world_misdeeds_shot_damage(const struct Thing *shotng, const struct Thing *shooter, const struct Thing *target,
    HitPoints dmg);
/** Virtues: the share of its work a creature does now, in percent (0 while it rests after a correction). */
int world_misdeeds_work_percent(const struct Thing *thing);
/** Virtues: the fear check's threshold (tenths of a percent of its health), raised by Prudence. */
long world_misdeeds_fear(const struct Thing *thing, long fear);
/** Virtues: an enemy is knocked out by this killer. */
void world_misdeeds_knocked_out(struct Thing *victim, struct Thing *killertng);
/** Virtues: a keeper's power was cast on a creature (a correction on the rack). */
void world_misdeeds_power_on(PlayerNumber plyr_idx, PowerKind pwkind, struct Thing *thing);
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
/** Test hook (a joined client's turn). */
void world_misdeeds_client_turn(void);
/** Strays: a keeper's chicken dropped on a wild creature at night, or on their stray: 1 when it was eaten (the chicken
 *  is gone), -1 when the stray refused it (it stays in the hand), 0 when it is no matter for Strays. */
int world_misdeeds_stray_feed(PlayerNumber plyr_idx, struct Thing *food, struct Thing *creatng);
/** Strays: a neutral creature that is someone's stray: no keeper's creature claims it. */
TbBool world_misdeeds_stray_kept(const struct Thing *thing);
/** Strays: a wild creature that ate a keeper's chicken tonight. It is wild until dawn and does not fade meanwhile. */
TbBool world_misdeeds_stray_fed(const struct Thing *thing);
/** Strays: dawn found a fed wild creature out of a fight: it stays where it is, as a neutral creature. */
void world_misdeeds_stray_dawn(struct Thing *thing);
/** Alumni: the turns between a keeper's portal arrivals, shortened by renown. */
long world_misdeeds_entrance_wait(PlayerNumber plyr_idx, long wait);
/** Past Lives: a creature is sacrificed in a Temple of this owner. */
void world_misdeeds_sacrificed(struct Thing *thing, PlayerNumber temple_owner);
/** Contracts: the size a creature's lair totem is drawn at (Bed and Board: bigger). */
int world_misdeeds_lair_size(const struct Thing *creatng, int size);
/** Contracts: the places a creature's lair takes in a Lair (Bed and Board: twice its kind's). Every place the stock
 *  code books, frees, counts or asks for a lair's room goes through this. */
int world_misdeeds_lair_places(const struct Thing *creatng, int places);
/** The chunk manager: the chunks the waiting bodies lie in stay loaded. */
void world_misdeeds_want_chunks(void (*want)(int64_t wx, int64_t wy, int r));
/** Packets: Rename (the creature and the name packed in par1 and par2) and the pick of a vice. */
void world_misdeeds_packet_rename(PlayerNumber plyr_idx, uint32_t p1, uint32_t p2);
void world_misdeeds_packet_pick(PlayerNumber plyr_idx, ThingIndex idx, int choice);

/** The world save's extra ("MSD1"). */
/** A creature's vices, for other windows (the World Boss picker's tooltip): their names and what each does, at most
 *  `max`. Returns how many, or -1 when this machine can't know (a joined machine knows only the creature its Rap Sheet
 *  shows). */
int world_misdeeds_vices_of(const struct Thing *thing, const char **names, const char **does, int max);
void world_misdeeds_reset(void);
/** A seat gets a new dungeon: the strays its old keeper fed go wild again. */
void world_misdeeds_strays_seat_reset(PlayerNumber plyr_idx);
size_t world_misdeeds_save_size(void);
void world_misdeeds_save(uint8_t *buf);
TbBool world_misdeeds_load(const uint8_t *buf, size_t len);

/** The Rap Sheet tab and the pencil. */
void world_misdeeds_rap_background(struct GuiMenu *gmnu);
/** The Rap Sheet for the creature window (world_creature_window.c): the same record in the window's lettering, in the
 *  box (x, y, w, h) in pixels; upp is the window's scale (16: one pixel a unit). Its clickable rows and picks are the
 *  buttons of the panel's page (the same callbacks), placed where this draws them. */
void world_misdeeds_window_draw(int x, int y, int w, int h, int upp);
/** The bar the window's sheet scrolls with, where it is higher than its room (and the mouse wheel). */
void world_misdeeds_scroll_maintain(struct GuiButton *gbtn);
void world_misdeeds_scroll_draw(struct GuiButton *gbtn);
void world_misdeeds_scroll_click(struct GuiButton *gbtn);
void world_misdeeds_pencil_maintain(struct GuiButton *gbtn);
void world_misdeeds_pencil_draw(struct GuiButton *gbtn);
void world_misdeeds_rename_done(struct GuiButton *gbtn);
void world_misdeeds_pick_maintain(struct GuiButton *gbtn);
void world_misdeeds_pick_draw(struct GuiButton *gbtn);
void world_misdeeds_pick_click(struct GuiButton *gbtn);
void world_misdeeds_mark_maintain(struct GuiButton *gbtn);
void world_misdeeds_mark_draw(struct GuiButton *gbtn);
void world_misdeeds_mark_click(struct GuiButton *gbtn);
void world_misdeeds_regimen_maintain(struct GuiButton *gbtn);
void world_misdeeds_regimen_draw(struct GuiButton *gbtn);
void world_misdeeds_regimen_click(struct GuiButton *gbtn);
/** A thin client: the server's coded word of a sin revealed; true when the text was one. */
TbBool world_misdeeds_net_notice(const char *text);
/** The name a creature shows to players: the one its keeper gave it with Rename, from level 10 on. NULL: it shows no
 *  name (the names the game makes up are not shown in an endless world). */
const char *world_misdeeds_shown_name(const struct Thing *thing);
/** The creature's name above its health flower while the hand hovers over it. top_y: the flower's top. */
/** Once per frame after the world is drawn: the label kept by world_misdeeds_draw_label. */
void world_misdeeds_draw_overlay(void);
void world_misdeeds_draw_label(const struct Thing *thing, long scr_x, long top_y, int flower_h, TbBool under_hand);

#ifdef __cplusplus
}
#endif
#endif
