/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_boss.h
 *     Endless world: World Bosses (2026-11-13; the user's own idea).
 * @par Purpose:
 *     A World Boss can be fought 24 hours of game time after the last one was beaten. A timer under the dial counts
 *     down to it; when the fight is ready a button lets each keeper pick up to five of their creatures (ten keepers
 *     at most). At
 *     the start of the round they are taken, behind the game's loading screen, to an arena in a world apart (a
 *     sealed room in an empty corner of the world plane where nothing else is; world_stream.h), where a creature of
 *     the game three and a half times its size comes to meet them, with an enormous amount of health.
 *     There is no Hand there: spells only. The boss draws the creatures to it as a Dungeon Heart does, and none can
 *     leave. Creatures that die are gone. A keeper whose creatures have all fallen goes home; when the boss falls, 30
 *     seconds later everyone goes home, and each player's loot (gold and books) waits in the room of their Dungeon
 *     Heart. A fight that is lost can be fought again at once: the timer runs again only when the boss is beaten.
 */
/******************************************************************************/
#ifndef DK_WORLD_BOSS_H
#define DK_WORLD_BOSS_H

#include "bflib_basics.h"
#include "globals.h"

struct Thing;
struct GuiMenu;

#ifdef __cplusplus
extern "C" {
#endif

enum WorldBossPhase {
    WBP_Waiting = 0,     /* counting down to the next fight */
    WBP_Open,            /* players may join */
    WBP_Fight,           /* the fight is on */
    WBP_Loot,            /* the boss fell: 30 seconds, then home */
};

/** The simulation's turn of an endless world. */
void world_boss_turn(void);
/** A piece of a player's join request (the packet): three of their creatures' thing indexes, the piece's place in the
 *  list and the list's length (see world_boss.c). */
void world_boss_packet_join(PlayerNumber plyr_idx, uint32_t par1, uint32_t par2);
/** Is this player's dungeon away at a fight (no raids come for it meanwhile)? */
TbBool world_boss_player_away(PlayerNumber plyr_idx);
/** A World Boss's blows: this percent of the creature's own (100 for anything else). */
long world_boss_damage_percent(const struct Thing *thing);
/** The same for its blows by hand (its weapon), which have a percent of their own. */
long world_boss_melee_percent(const struct Thing *thing);
/** Is this creature in a World Boss's arena while its fight is on? Nothing there is too scared to fight (the boss
 *  draws every creature as an enemy's Dungeon Heart does, and stands its own ground). */
TbBool world_boss_no_fear(const struct Thing *thing);
/** Are these two creatures of different keepers who are both at a World Boss fight, both in its arena? They leave
 *  each other alone there (keepers who are not allied are enemies everywhere else): they came for the boss. */
TbBool world_boss_truce(const struct Thing *thing1, const struct Thing *thing2);
/** The event's truce, for everything a keeper can do to another keeper's creature at the fight (the user,
 *  2026-11-14: "Players are truced here so they cant hurt eachother"): is this a creature of a keeper at the fight,
 *  in the arena, and `from` (the owner of a blow, a shot, an area effect or a keeper's spell) any keeper but its own?
 *  The boss (the heroes' player) is not held off. What the truce stops is the world's (world_truce.c: friendly fire). */
TbBool world_boss_spares(const struct Thing *victim, PlayerNumber from);
/** Is this creature a World Boss in its fight? It takes every attacker at once: the game gives a creature four
 *  places for attackers hand to hand and four for shooters, and the others wait for one; at a World Boss the
 *  attackers past those eight fight without a place (the user, 2026-11-14: "There shouldnt be a limit here"). */
TbBool world_boss_takes_all(const struct Thing *victim);
/** A World Boss's fight ends (it turns on another creature, or falls): its attackers without a place are let go,
 *  as those with a place are (melee: the hand-to-hand ones, else the shooters). Returns how many. */
long world_boss_release_attackers(struct Thing *victim, TbBool melee);
/** The chance that a blow lands, in percent of the game's own, by the blow (a shot thing): a World Boss's blows land
 *  less often (100 for any other creature's). */
long world_boss_hit_percent(const struct Thing *shotng);
/** The turns one of a creature's abilities takes to come back: a World Boss's take twice as long (anything else:
 *  the turns given). */
long world_boss_reset_turns(const struct Thing *thing, int inst_idx, long turns);
/** Is this the World Boss in its fight? It carries no health flower: its health is the bar at the top of the screen
 *  (the user, 2026-11-14: "Remove the flower health from the Avatar and design a health bar that appears at the top
 *  of the screen like Soulslike games such as Elden Ring do"). */
TbBool world_boss_no_flower(const struct Thing *thing);
/** The size a World Boss's body is drawn at when it falls: its death and its corpse are as big as it was alive (the
 *  user, 2026-11-14). 0: not a World Boss in its fight, and the body has the game's own size. */
long world_boss_corpse_size(const struct Thing *thing);
/** A World Boss's Word of Power: it reaches further (map units added to the spell's reach; 0 for any other caster) ... */
MapCoordDelta world_boss_wop_reach(const struct Thing *caster);
/** ... its flames fly as much further (percent of the speed an effect's pieces are thrown out with; 100 for any other
 *  effect) ... */
long world_boss_effect_spread_percent(const struct Thing *efftng);
/** ... and it throws the keepers' creatures within that reach three slabs further back than the spell itself does.
 *  Called when the spell goes off (`reach` in map units, from `pos`). */
void world_boss_wop_cast(struct Thing *caster, const struct Coord3d *pos, MapCoordDelta reach);
/** Cleave, the Avatar's own: half of its blows by hand also throw back, by two slabs, every creature of the keepers
 *  in front of it. Called at each blow by hand that reaches its target. */
void world_boss_cleave(struct Thing *shooter, struct Thing *shotng, const struct Thing *target);
/** This machine's keeper is at a World Boss fight: its camera stays inside this box (map coordinates) ... */
TbBool world_boss_view_box(MapCoord *x1, MapCoord *y1, MapCoord *x2, MapCoord *y2);
/** ... and its full-screen map stays on the arena (world slabs); only the map's zoom works. */
TbBool world_boss_map_locked(int64_t *wx, int64_t *wy);
/** The Hand can't pick up anything in the arena but its own player's loot. */
TbBool world_boss_blocks_pickup(const struct Thing *thing, PlayerNumber plyr_idx);
/** A World Boss's loot lying in the arena: whose (PLAYER_NONE: not loot); hidden from every other player; taken by
 *  its player's Hand (true: collected, it goes to the storage at home). */
PlayerNumber world_boss_loot_owner(const struct Thing *thing);
/** The Hand of a keeper takes its share's gold pile: the gold goes straight to that keeper's gold (the counter), not
 *  into the Hand (true: taken). There is no Treasury in the arena to drop it in. */
TbBool world_boss_take_gold(struct Thing *thing, PlayerNumber plyr_idx);
TbBool world_boss_loot_hidden(const struct Thing *thing);
TbBool world_boss_collect(struct Thing *thing, PlayerNumber plyr_idx);
/** The chunk manager: the arena's chunks stay loaded during a fight. */
void world_boss_want_chunks(void (*want)(int64_t wx, int64_t wy, int r));
/** This machine: the timer under the dial (at x, y; w wide), the join button and the camera. */
void world_boss_draw(int x, int y, int w);
void world_boss_local_turn(void);

extern struct GuiMenu world_boss_bar_menu;
extern struct GuiMenu world_boss_pick_menu;

void world_boss_reset(void);
size_t world_boss_save_size(void);
void world_boss_save(uint8_t *buf);
TbBool world_boss_load(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
