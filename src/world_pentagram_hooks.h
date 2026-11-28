/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_pentagram_hooks.h
 *     Endless worlds: the Pentagram's hooks, called from the game's systems (world_pentagram_fx.c). Each returns its
 *     argument unchanged outside endless worlds and for keepers without the skill.
 */
/******************************************************************************/
#ifndef DK_WORLD_PENTAGRAM_HOOKS_H
#define DK_WORLD_PENTAGRAM_HOOKS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;
struct Room;

/* combat (thing_shots.c) */
HitPoints world_pentagram_adjust_damage(struct Thing *victim, struct Thing *attacker, HitPoints dmg);
HitPoints world_pentagram_heart_damage(struct Thing *heartng, struct Thing *shotng, HitPoints dmg);
HitPoints world_pentagram_door_damage(struct Thing *doortng, struct Thing *shotng, HitPoints dmg);
/** A deployed trap's damage, in percent more (Artifice's minors, Killing Floor, Minefield). */
long world_pentagram_trap_damage_pct(const struct Thing *traptng);

/* creatures' figures (thing_stats.c) */
HitPoints world_pentagram_max_health(const struct Thing *thing, HitPoints max_health);
long world_pentagram_speed(const struct Thing *thing, long speed);
GoldAmount world_pentagram_pay(const struct Thing *thing, GoldAmount pay);
GoldAmount world_pentagram_training_cost(const struct Thing *thing, GoldAmount cost);
HitPoints world_pentagram_heart_max(const struct Thing *heartng, HitPoints base);
/** A door's most health: Bolted Doors and Artifice's minors. */
HitPoints world_pentagram_door_max(const struct Thing *doortng, HitPoints base);
/** Named and level 10 and up (Renown, Cult of the Keeper). */
TbBool world_pentagram_named(const struct Thing *thing);
/** War Horn: the creature answered the Call To Arms in the last 20 seconds. */
TbBool world_pentagram_cta_fresh(const struct Thing *thing);

/** Healing a creature receives (thing_stats.c apply_health_to_thing): Second Wind and the minors on its own ground. */
HitPoints world_pentagram_heal(const struct Thing *thing, HitPoints amount);

/* keeper spells (magic_powers.c, thing_creature.c, world_progress.c) */
GoldAmount world_pentagram_power_price(PlayerNumber plyr_idx, PowerKind pwkind, KeepPwrLevel power_level, GoldAmount price);
KeepPwrLevel world_pentagram_price_level(PlayerNumber plyr_idx, KeepPwrLevel power_level);
GoldAmount world_pentagram_spell_pay(PlayerNumber plyr_idx, PowerKind pwkind, GoldAmount price);
/** The rest of the price could not be paid: the Soul Harvest credit world_pentagram_spell_pay used goes back. */
void world_pentagram_spell_unpay(PlayerNumber plyr_idx, GoldAmount credit);
void world_pentagram_soul_harvest(PlayerNumber plyr_idx);
KeepPwrLevel world_pentagram_cast_begin(PlayerNumber plyr_idx, PowerKind pwkind, KeepPwrLevel power_level);
void world_pentagram_cast_end(PlayerNumber plyr_idx, PowerKind pwkind, KeepPwrLevel power_level, MapSubtlCoord stl_x,
    MapSubtlCoord stl_y, struct Thing *thing, unsigned long mod_flags, TbResult ret);
GameTurnDelta world_pentagram_spell_duration(PlayerNumber caster, SpellKind spell_idx, GameTurnDelta duration);
int world_pentagram_power_bonus(PlayerNumber plyr_idx, int what, int bonus);
long world_pentagram_heal_amount(PlayerNumber plyr_idx, long amount);

/* experience and kills (thing_shots.c, creature_states_train.c, thing_creature.c) */
long world_pentagram_combat_xp(const struct Thing *shooter, const struct Thing *target, long exp_factor);
long world_pentagram_training_xp(const struct Thing *thing, long value);
void world_pentagram_on_kill(struct Thing *victim, struct Thing *killer, PlayerNumber killer_plyr);

/* Imps and digging (creature_instances.c, creature_states_spdig.c, map_data.c, world_hand.c, world_progress.c) */
long world_pentagram_dig_damage(const struct Thing *digger, MapSubtlCoord stl_x, MapSubtlCoord stl_y, TbBool gold, long dmg);
long world_pentagram_mined_gold(const struct Thing *digger, long gold);
GoldAmount world_pentagram_gold_hold(const struct Thing *digger, GoldAmount hold);
int world_pentagram_claim_extra(const struct Thing *digger);
int world_pentagram_reinforce_hits(const struct Thing *digger, int hits);
TbBool world_pentagram_digs_enemy_walls(PlayerNumber plyr_idx);
int world_pentagram_imp_cap(PlayerNumber plyr_idx, int cap);
int world_pentagram_hand_cap(PlayerNumber plyr_idx, int cap);
/* gold */
GoldAmount world_pentagram_gold_per_hoard(PlayerNumber plyr_idx, GoldAmount per_hoard);
GoldAmount world_pentagram_room_cost(PlayerNumber plyr_idx, RoomKind rkind, GoldAmount cost);
GoldAmount world_pentagram_room_sale(PlayerNumber plyr_idx, RoomKind rkind, GoldAmount cost, long percent);
GoldAmount world_pentagram_fill_cost(PlayerNumber plyr_idx, GoldAmount cost);
/** Cave-In on unclaimed dug floor: how many slabs beyond the keeper's claimed ground (Rockfall 3, Sappers 3 more). */
int world_pentagram_fill_reach(PlayerNumber plyr_idx);
/** Night Watch: a raid on this keeper landed at a world slab. */
void world_pentagram_raid_landed(PlayerNumber plyr_idx, int64_t wx, int64_t wy);
/** Dark Sight, Night Sentinels: how many subtiles further this creature sees (its visual range). */
long world_pentagram_sight_bonus(const struct Thing *creatng);
/** Lantern: an Imp dug out a slab (more ground round it is revealed). */
void world_pentagram_dug(const struct Thing *creatng, MapSlabCoord slb_x, MapSlabCoord slb_y);
/** The simulating machine, each turn: Night Sentinels' raiders and Siege Maps' strongholds. */
void world_pentagram_marks_turn(void);
/* gold and XP from the world's events (the simulating machine) */
GoldAmount world_pentagram_gold_more(PlayerNumber plyr_idx, GoldAmount gold); /* Blood Money (Pentagram gold, mining) */
void world_pentagram_room_taken(struct Room *room, const struct Thing *claimer);    /* Plunder: before it changes hands */
void world_pentagram_heart_destroyed(PlayerNumber by, const struct Thing *heartng); /* Pillager */
void world_pentagram_portal_captured(PlayerNumber plyr_idx, int biome);            /* first in a biome: XP, Scout's Reward */
void world_pentagram_fortress_razed(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y); /* XP, Crusade's End */
void world_pentagram_raid_over(PlayerNumber plyr_idx, int made, int killed, double raid_power, double army_power);
void world_pentagram_boss_won(PlayerNumber plyr_idx, int grade);                    /* XP, Boss Slayer's book */
void world_pentagram_level_gained(PlayerNumber plyr_idx);                           /* a room, spell or trap level */
TbBool world_pentagram_crusade_spared(PlayerNumber plyr_idx);                       /* Crusade's End */

/* creatures */
GameTurnDelta world_pentagram_portal_wait(PlayerNumber plyr_idx, GameTurnDelta wait);
void world_pentagram_portal_arrival(struct Thing *creatng);
int world_pentagram_level_cap(PlayerNumber plyr_idx, int cap);
long world_pentagram_anger(const struct Thing *creatng, long anger, int reason);
TbBool world_pentagram_never_leaves(const struct Thing *creatng);
int world_pentagram_vice_level(const struct Thing *thing, int level);
GameTurnDelta world_pentagram_slap(struct Thing *thing, PlayerNumber slapper, GameTurnDelta turns);
int world_pentagram_slap_reach(PlayerNumber plyr_idx, int reach);
GameTurnDelta world_pentagram_slap_hurry(PlayerNumber plyr_idx, GameTurnDelta turns);
int world_pentagram_scared(const struct Thing *creatng);

/* rooms' work */
long world_pentagram_work(const struct Thing *creatng, unsigned long rrole, long value);
int world_pentagram_level_speed(PlayerNumber plyr_idx, int where, int speed);
TbBool world_pentagram_academy(PlayerNumber plyr_idx);
long world_pentagram_torture(PlayerNumber torturer, long turn_val);
long world_pentagram_prison_hunger(PlayerNumber jailer, long every);
GameTurnDelta world_pentagram_trap_delay(PlayerNumber plyr_idx, GameTurnDelta delay);

/** Each world turn, per keeper: the skills that keep time. */
void world_pentagram_keep_time(PlayerNumber plyr_idx);

#ifdef __cplusplus
}
#endif
#endif
