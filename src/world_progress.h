/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_progress.h
 *     Endless world: progression by research books.
 * @par Purpose:
 *     A human keeper starts with the Treasury, Lair and Hatchery, Summon Imp and Possession (the engine adds Hand and
 *     Slap). Everything else is learnt from research books found in reward caves and hero fortresses, dropped by
 *     destroyed keepers and carried by raid parties. A book's grade (1-5) follows how far from the start it was found.
 *     Clicking a book on your own ground offers up to three lore books, each naming one thing to learn; click one.
 *     The first book always teaches the Library. When nothing is left that a book can teach, it gives a boon.
 * @par Turned on by:
 *     ENDLESS_PROGRESSION(1) in the level script (2: books teach their first option at once), for new runs. A save
 *     without the progression section plays as before.
 */
/******************************************************************************/
#ifndef DK_WORLD_PROGRESS_H
#define DK_WORLD_PROGRESS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;
struct PlayerInfo;
struct WgStructRef;

#define WORLD_BOOK_KIND_FIRST 136   /* 128 + 8 * grade + biome: grades 1-5, biomes 0-7 */
#define WORLD_LORE_KIND_FIRST 200   /* 200 + item */
#define WORLD_OUTPOST_BOOK_KIND 180 /* a heroes' spellbook, from an outpost's heart */
#define WORLD_LEVELUP_KIND_FIRST 184 /* 184 + entry: a lore book that raises a room's or a spell's level (to 195) */

/** ENDLESS_PROGRESSION(mode): mode 1 draft (choose 1 of 3), 2 automatic. For the level being loaded. */
void world_progress_request(int mode);
void world_progress_level_start(void);
void world_progress_level_end(void);
TbBool world_progress_active(void);
/** The first world turn of a new run: every human keeper gets the starting kit; book tooltips. */
void world_progress_first_turn(void);
/** A player joined the running world: the starting kit, unless that seat had it. */
void world_progress_new_player(PlayerNumber plyr_idx);
/** Each world turn (server or single player): test hooks. */
void world_progress_turn(void);
/** A thin client's turn: its test hook (reading books goes through packets). */
void world_progress_client_turn(void);

/** Placement in a fresh chunk: how many books a structure holds (0-2) and their kind. Seeded by the world seed and
 *  the structure, so a seed always places the same books. `guaranteed`: one of the two reward caves nearest the
 *  start (always a grade-1 book). */
int world_progress_books_of(const struct WgStructRef *ref, uint64_t world_seed, TbBool guaranteed, uint8_t *box_kind);
/** The object model of a research book (0 when the map pack doesn't have it). */
ThingModel world_progress_book_model(void);
/** A research book's kind for a grade and biome. */
uint8_t world_progress_book_kind(int grade, int biome);
/** The grade of a book found that far from the start. */
int world_progress_grade_at(double slabs_from_start);

/** A special box was clicked: reads it when it is a research or lore book (true: handled). */
TbBool world_progress_activate(struct Thing *thing, struct PlayerInfo *player);
/** Books dropped on the map (a destroyed keeper's grimoires). */
void world_progress_drop_books(MapSubtlCoord stl_x, MapSubtlCoord stl_y, int grade, int biome, int count);
/** A heroes outpost's heart fell: its spellbook lies there. */
void world_progress_drop_outpost_book(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
struct Room;
/** An imp put a special box into its keeper's Library: a heroes' spellbook opens a research there. */
void world_progress_book_shelved(struct Thing *thing, struct Room *room);
/** Endless worlds: an Increase Level or Steal Hero box an imp puts in a Library fuses with one of the same kind
 *  there (up to WORLD_BOX_MAX_UPGRADES upgrades, then a second box starts). */
#define WORLD_BOX_MAX_UPGRADES 10

/** Room levels (2026-11-09): a book that would give a Lair, Hatchery, Library or Training Room the keeper already has
 *  raises the level that room may reach (up to 3); the Library then researches it again, next in line, at twice and
 *  three times the room's research. Level 2 / 3: Lair heals 10% / 20% faster; Hatchery: hunger comes 10% / 20% later
 *  and food heals 5% / 10% more; Training Room +5% / +10% experience; Library researches 5% / 10% faster. */
enum WorldRoomBonus {
    WRB_LairHeal = 0,
    WRB_HatcheryHunger,
    WRB_HatcheryHeal,
    WRB_TrainingXp,
    WRB_LibraryResearch,
};
/** The percent a keeper's room level adds (0 outside endless worlds). */
int world_room_bonus(PlayerNumber plyr_idx, enum WorldRoomBonus what);
/** Spell levels (2026-11-09), from duplicate books like the rooms', researched at twice and four times the spell's
 *  research. Level 2 / 3: Create Imp makes level 2 / 3 imps; Sight of Evil reaches 3% / 6% wider; Speed Monster 3% /
 *  6% faster; Conceal Monster lasts 10 / 20 seconds longer; Heal's strongest cast heals 1150 / 1400 (830 at level
 *  1, the others in proportion); Lightning Strike 2% / 5% more damage; Protect Monster 3% more armour, and at level
 *  3 20 seconds longer too. Disease is off in endless worlds. */
enum WorldPowerBonus {
    WPB_ImpLevels = 0,  /* levels the new imp gets on top */
    WPB_SightRadius,    /* percent */
    WPB_SpeedEffect,    /* percent */
    WPB_ConcealTurns,   /* game turns */
    WPB_HealTop,        /* the strongest cast's healing (0: as configured) */
    WPB_LightningDamage,/* percent */
    WPB_ProtectArmour,  /* percent */
    WPB_ProtectTurns,   /* game turns */
    WPB_COUNT
};
int world_power_bonus(PlayerNumber plyr_idx, enum WorldPowerBonus what);
/** The keeper whose power is being cast right now (-1 otherwise): Heal and the longer Conceal and Protect count only
 *  for a keeper's own casts, not for creatures casting the same spells. */
extern PlayerNumber world_power_caster;
/** Extra game turns a keeper's spell level gives a spell cast with that keeper's power (Conceal, Protect). */
int world_spell_extra_turns(PlayerNumber plyr_idx, int spell_idx);
/** Heal: the healing of a cast at power_level, as the keeper's Heal level has it. */
long world_heal_amount(PlayerNumber plyr_idx, long configured, const int32_t *strengths, int count);
/** The Temple's "complete research": finishes the room or spell level the Library researches, if any. */
TbBool world_room_level_sacrifice(PlayerNumber plyr_idx);
/** The number on a spell's panel icon (as world_room_level_badge). */
TbBool world_power_level_badge(PlayerNumber plyr_idx, int pwkind, int *number, TbBool *reached);
/** The number on a room's panel icon: the next level, grey (allowed, not researched yet), or the level reached, white
 *  (the last one allowed). False: no number. */
TbBool world_room_level_badge(PlayerNumber plyr_idx, int rkind, int *number, TbBool *reached);
/** The Library working on a room's next level (next in line after the research in progress): true while it does,
 *  and the research of that turn is the level's. */
TbBool world_room_level_research(PlayerNumber plyr_idx);
/** The Library's research bar while it researches a room's level (false: none). */
TbBool world_room_level_research_bar(PlayerNumber plyr_idx, int *required, int *progress);
void world_progress_box_shelved(struct Thing *thing, struct Room *room);
/** How many times a box works: 1 + its upgrades (endless worlds' Increase Level and Steal Hero boxes), else 1. */
int world_progress_box_power(const struct Thing *thing);
/** A day raid came for a player: the first raid of a day from day 2 carries a book one time in two. */
void world_progress_raid(PlayerNumber plyr_idx, long day, int raid_idx, uint64_t world_seed);
/** A creature died: a raid hero drops its party's book. */
void world_progress_creature_died(struct Thing *creatng);

/** The progression's state for the world save ("PRG1" section): size, write, read. */
size_t world_progress_save_size(void);
void world_progress_save(uint8_t *buf);
TbBool world_progress_load(const uint8_t *buf, size_t len);

/* Player levels (endless worlds): 1000 XP for every keeper or heroes heart a player destroys; level 10 at most. */
#define WORLD_PLAYER_LEVEL_MAX 10
/** A heart took damage from a player (shots, explosions): who hit it last gets it when it goes. */
void world_heart_hit(const struct Thing *heartng, PlayerNumber by);
/** A heart is destroyed: who hit it last gets the XP. */
void world_heart_destroyed(const struct Thing *heartng);
uint32_t world_player_xp(PlayerNumber plyr_idx);
void world_set_player_xp(PlayerNumber plyr_idx, uint32_t xp);
int world_player_level(PlayerNumber plyr_idx);
/** Every machine's turn (presentation): level-ups and creatures unlocked are announced. */
void world_progress_present(void);
/** A human keeper owns a portal in this biome (every portal update): its creatures come to that keeper, for good. */
void world_progress_portal_owned(PlayerNumber plyr_idx, int biome);
/** A new world, a load, the end: no XP, nothing seen yet. */
void world_levels_reset(void);
/** A player's record kept with the world (save section PST1) for a dedicated server's console. */
struct WorldPlayerStats {
    uint32_t hearts;     /* keeper hearts destroyed */
    uint32_t outposts;   /* heroes outposts destroyed */
    uint32_t portals;    /* portals captured */
    uint32_t play_turns; /* game turns played while connected */
};
const struct WorldPlayerStats *world_player_stats(PlayerNumber plyr_idx);
void world_set_player_stats(PlayerNumber plyr_idx, const struct WorldPlayerStats *st);
void world_player_played_turn(PlayerNumber plyr_idx);
/** XP from the server's admin (give <name> XP <amount>): levels follow. */
void world_add_player_xp(PlayerNumber plyr_idx, uint32_t xp);
void world_player_portal_captured(PlayerNumber plyr_idx, int biome);
/** Start again: the seat's XP (player level), records and research-book progression from zero; the kit is given
 *  again when the seat next starts. */
void world_progress_seat_reset(PlayerNumber plyr_idx);

/* Passive upgrades from levelling up (the rogue-like part, to come): kept by player (the seat's owner, its reconnect
   token; 0 in a single-player run), not by seat, so a player keeps them through "Start again". None exist yet: the
   store, its save section (PRK1) and the hooks are ready for them. */
#define WORLD_PERK_SLOTS 32
struct WorldPerks {
    uint64_t token;                   /* whose (0: the single-player keeper) */
    uint8_t level[WORLD_PERK_SLOTS];  /* each upgrade's level (0: not had) */
};
/** A player's upgrades (NULL when there are none and create is false). */
struct WorldPerks *world_perks_of(uint64_t token, TbBool create);
/** A seat starts (joins, or starts again): its player's upgrades take effect on its dungeon. */
void world_perks_seat_started(PlayerNumber plyr_idx);
/** A seat's player reached a level: where an upgrade will be given. */
void world_perks_level_reached(PlayerNumber plyr_idx, int level);
int world_perks_count(void);
const struct WorldPerks *world_perks_entry(int i);
void world_perks_reset(void);

#ifdef __cplusplus
}
#endif
#endif
