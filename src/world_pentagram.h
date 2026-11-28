/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_pentagram.h
 *     Endless worlds: the Pentagram, the keeper's skill web (Player Upgrades).
 * @par Purpose:
 *     The user's design (2026-11-27; ideas\PENTAGRAM-2026-11-27.md): a web of 216 skills over a pentagram, ten Foci,
 *     Minor and Major skills, Keystone pairs (one of two, after 10 points in their Focus), twin Keystones (6 points in
 *     each of two Foci) and the Heart in the middle, the last point of all (level 262), which lets the keeper summon a
 *     level 100 Horned Reaper once a day. One Ability Point per player level. The skills' data is generated
 *     (world_pentagram_data.h); each skill names an effect (enum WpgEffect) which the game's systems ask for with
 *     world_pentagram_fx(). The web's state is each seat's, in struct Game (thin clients get it), changed only by the
 *     simulating machine (packets PckA_WorldPentagram*). Saved with the world (PNT1).
 * @par Comment:
 *     Player levels and XP (decided with the user the same day): cap 262; XP gains go through world_pentagram_xp(),
 *     which multiplies by the circle where they happened, scales raids, wild packs and keepers' Hearts by strength,
 *     caps repeatable XP a day, and gives none while the caretaker keeps the dungeon.
 */
/******************************************************************************/
#ifndef DK_WORLD_PENTAGRAM_H
#define DK_WORLD_PENTAGRAM_H

#include "bflib_basics.h"
#include "globals.h"
#include "world_pentagram_fx.h"
#include "world_pentagram_hooks.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Thing;

#define WPG_STATE_BYTES ((WPG_NODES * 2 + 7) / 8)
#define WPG_HEART_NODE 0      /* the Heart: the web's start, and its last point */
#define WPG_LEVEL_SOUND 76    /* a level-up's sound: the magic stars' shimmer (sounds.cfg SPELL_STARS) */

#define WPG_SEEN_MAX 8
#define WPG_SIEGE_MAX 12

/** A seat's web and the state of its skills that keep time (in struct Game). */
struct WorldPentagram {
    uint8_t pts[WPG_STATE_BYTES]; /* 2 bits a node: its points (a keystone: 1 when chosen) */
    uint8_t version;              /* changes with the web (every machine's effect totals follow it) */
    uint8_t reaper_angry;         /* the summoned Reaper was slapped */
    uint16_t reaper_thing;        /* the Reaper while he is out (0: none) */
    uint16_t xp_day_done;         /* repeatable XP (raids, wild packs) counted in full today */
    uint32_t xp_day;              /* the world day xp_day_done is for */
    uint32_t reaper_day;          /* the world day of the last summon (0: never) */
    uint32_t reaper_until;        /* the game turn he goes (0: none out) */
    uint32_t rest_declared;       /* Day of Rest: the world day it was last declared on (0: never) */
    uint32_t rest_day;            /* ... and the day it makes safe (0: none) */
    uint32_t eclipse_night;       /* Eclipse: the world day whose night's free spell was used */
    uint32_t minefield_day;       /* Minefield: the world day whose first raid has come */
    uint32_t soul_credit;         /* Soul Harvest: gold banked for the next spell */
    int16_t heart_pct_applied;    /* the Heart's extra health (percent) already given to it */
    uint16_t omen;                /* Omen: at nightfall, tomorrow's raids: (tomorrow's day & 0xFF) << 8 | (raids + 1) */
    int32_t watch_x, watch_y;     /* Night Watch: the last raid's landing (world slab) ... */
    uint32_t watch_until;         /* ... pinged on the maps until this turn */
    uint8_t seen_n;               /* Night Sentinels: raiders the Guard Posts see (world slabs), shown till seen_until */
    uint8_t siege_n;              /* Siege Maps: the strongholds seen (world slab boxes) */
    uint16_t pad2;
    uint32_t seen_until;
    int32_t seen_x[WPG_SEEN_MAX], seen_y[WPG_SEEN_MAX];
    int32_t siege[WPG_SIEGE_MAX][4];
    uint32_t biome_mask;          /* the biomes a portal was taken in (the first one in each: XP, Scout's Reward) */
};

/* The web's rules (any machine: they read struct Game) */
int world_pentagram_points(PlayerNumber plyr_idx, int node);
int world_pentagram_spent(PlayerNumber plyr_idx);
int world_pentagram_total(PlayerNumber plyr_idx);   /* Ability Points: the player level */
int world_pentagram_free(PlayerNumber plyr_idx);
int world_pentagram_focus_points(PlayerNumber plyr_idx, int focus);
/** The keystone chosen in a pair (node), or -1. */
int world_pentagram_group_pick(PlayerNumber plyr_idx, int group);
TbBool world_pentagram_has(PlayerNumber plyr_idx, int node);
TbBool world_pentagram_can_learn(PlayerNumber plyr_idx, int node);
/** Why a node can't be learnt (for the card), or "" when it can. */
const char *world_pentagram_why(PlayerNumber plyr_idx, int node);
/** The sum of an effect over the keeper's skills (0 outside endless worlds). */
int world_pentagram_fx(PlayerNumber plyr_idx, enum WpgEffect fx);
/** Whether the keeper has an effect at all. */
#define world_pentagram_on(plyr_idx, fx) (world_pentagram_fx((plyr_idx), (fx)) != 0)
/** Reset Tree's price: 500 gold a point spent. */
long world_pentagram_reset_cost(PlayerNumber plyr_idx);
struct WorldPentagram *world_pentagram_state(PlayerNumber plyr_idx);

/* Game state (the simulating machine; from packets) */
/** Learn the points in `nodes` in order (255: none); stops at the first that breaks the rules. */
void world_pentagram_packet_learn(PlayerNumber plyr_idx, const uint8_t *nodes, int count);
void world_pentagram_packet_reset(PlayerNumber plyr_idx);
/** A seat starts again (keep_record) or gets a new player (!keep_record: the web goes too). */
void world_pentagram_seat_reset(PlayerNumber plyr_idx, TbBool keep_record);

/* This machine: asking for points (queued, sent as packets a turn at a time) */
void world_pentagram_send_learn(const uint8_t *nodes, int count);
void world_pentagram_send_reset(void);
/* Day of Rest (once every 5 world days: tomorrow is a safe day for this keeper alone) */
#define WPG_REST_EVERY 5
TbBool world_pentagram_rest_on(PlayerNumber plyr_idx, long day); /* is that world day this keeper's day of rest? */
/** Omen: by night, this keeper's forecast for tomorrow ("2 raids tomorrow"); false when there is none to show. */
TbBool world_pentagram_omen_text(PlayerNumber plyr_idx, char *buf, size_t len);
/** The Reaper's pointer (a PNG: the Heart's portrait, 44 px, round). */
const unsigned char *world_pentagram_reaper_cursor(unsigned int *len);
int world_pentagram_rest_wait(PlayerNumber plyr_idx);           /* days before it may be declared (0: now; -1: not learnt) */
void world_pentagram_send_rest(void);                           /* this machine: declare it (a packet) */
TbBool world_pentagram_sending(void);
/** Each turn on a machine with a keeper: one packet of what is waiting. */
void world_pentagram_local_turn(void);
/** packets.c: a Pentagram packet (Learn: 8 node numbers in p1, p2; Reset). */
void world_pentagram_packet(PlayerNumber plyr_idx, unsigned char action, uint32_t p1, uint32_t p2);

/* XP (the simulating machine) */
enum WpgXpKind {
    WpgXp_KeeperHeart = 0, /* a keeper's Heart: scaled by the armies */
    WpgXp_OutpostHeart,    /* a heroes outpost's Heart */
    WpgXp_Fortress,        /* a hero fortress razed */
    WpgXp_Boss,            /* a World Boss beaten */
    WpgXp_Raid,            /* a raid beaten (repeatable) */
    WpgXp_Pack,            /* a wild pack wiped out (repeatable) */
    WpgXp_Portal,          /* the first portal in a biome */
    WpgXp_Level,           /* a room, spell or trap level */
    WpgXp_Admin,           /* the server's admin: as given */
};
/** A keeper earns XP: the rules applied (circle, strength, the day's cap, the caretaker); `enemy_level` is the beaten
 *  side's level (raids and packs), `victim` the keeper whose Heart fell; slb_x/slb_y where it happened (-1: none). */
void world_pentagram_xp(PlayerNumber plyr_idx, enum WpgXpKind kind, long base, int enemy_level, PlayerNumber victim,
    MapSlabCoord slb_x, MapSlabCoord slb_y);
/** The level of an XP total, and the XP a level begins at (the user's curve: quick to 20, then 4,000 a level). */
int world_pentagram_level_of(uint32_t xp);
uint32_t world_pentagram_level_xp(int level);

/* Saved with the world: PNT1 */
size_t world_pentagram_save_size(void);
void world_pentagram_save(uint8_t *buf);
TbBool world_pentagram_load(const uint8_t *buf, size_t len);
void world_pentagram_reset(void);

/** Each world turn (the simulating machine): the skills that keep time (regeneration, Heartfire, Garrison, Living
 *  Fortress, the Reaper). */
void world_pentagram_turn(void);
/** The doors' applied Bolted Doors share: none (a new world), or as the seat's loaded web gives it (world_pentagram_fx.c). */
void world_pentagram_fx_reset(void);
void world_pentagram_fx_loaded(PlayerNumber plyr_idx);
/** The skill's icon as RGBA (decoded once; NULL: none). */
const unsigned char *world_pentagram_icon(int icon, int *w, int *h);
/** The Heart's picture's icon (the Reaper's painted portrait). */
int world_pentagram_heart_icon(void);

#ifdef __cplusplus
}
#endif
#endif
