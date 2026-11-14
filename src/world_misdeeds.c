/******************************************************************************/
// Endless Keeper: Misdeeds.
/******************************************************************************/
/** @file world_misdeeds.c
 *     Misdeeds: the game remembers what each creature of a keeper did, rewards it with Vices, lets the keeper name
 *     it (Rename), and shows it all on the Rap Sheet tab of the creature query panel. Bad Influence too.
 *
 *     Storage: a side table of records. A creature holds only the number of its record (CreatureControl::
 *     misdeed_id, 0 = none); a chunk's frozen creature record keeps that number in two of its spare bytes, and the
 *     table itself is the world save's "MSD1" extra. No existing save section changes its layout, and an old save
 *     simply has no records. Records are made for the creatures of human keepers only, on their first sample.
 *
 *     Cost: events (a kill, a hit) are O(1) lookups; everything that needs watching (lava, water, work, fights,
 *     distance) is sampled one turn in MD_SAMPLE per creature from a call the engine already makes per creature.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/RendererManager.h"
#include "world_misdeeds.h"
#include "world_stream.h"
#include "world_caretaker.h"
#include "world_daynight.h"
#include "world_wild.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include "bflib_basics.h"
#include "bflib_guibtns.h"
#include "bflib_mouse.h"
#include "bflib_sprfnt.h"
#include "bflib_video.h"
#include "bflib_vidraw.h"
#include "bflib_datetm.h"
#include "config.h"
#include "config_creature.h"
#include "config_crtrstates.h"
#include "creature_control.h"
#include "creature_states.h"
#include "dungeon_data.h"
#include "engine_render.h"
#include "front_input.h"
#include "frontend.h"
#include "frontmenu_ingame_tabs.h"
#include "game_legacy.h"
#include "globals.h"
#include "gui_draw.h"
#include "gui_frontbtns.h"
#include "gui_frontmenu.h"
#include "kjm_input.h"
#include "map_data.h"
#include "packets.h"
#include "player_data.h"
#include "player_instances.h"
#include "room_data.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_effects.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "thing_objects.h"
#include "thing_stats.h"
#include "vidfade.h"
#include "net_state.h"
#include "net_game.h"
#include "thing_corpses.h"
#include "map_blocks.h"
#include "creature_instances.h"
#include "world_gen.h"
#include "thing_shots.h"
#include "creature_groups.h"
#include "creature_states_mood.h"
#include "player_utils.h"
#include "magic_powers.h"
#include "config_magic.h"
#include "server_admin.h"
#include "world_wild.h"
#include "power_hand.h"
#include "room_lair.h"
#include "creature_states_hero.h"
#include "thing_objects.h"
#include "thing_factory.h"
#include "keeperfx.hpp"
#include "spdigger_stack.h"
#include "creature_states_spdig.h"
#include "bflib_planar.h"
#include "world_progress.h"
#include "creature_states_train.h"
#include "world_creature_window.h"
#include "world_defectors.h"
#include "post_inc.h"

#define MD_SAMPLE 32            /* a creature is sampled one turn in this many */
#define MD_WALK_SLABS 16        /* the most slabs (x + y) a creature can walk between two samples */
#define MD_STAND_DAY 150        /* samples on lava, and in water, that count in one day (4 minutes of a 20-minute day):
                                   a creature parked on lava or walled into a pool earned those ranks for nothing */
#define MD_FIGHT_GAP (60 * 20)  /* a fight counts (as begun at night, as come out of standing) when it began this long
                                   after the last one that counted: picking a creature up and dropping it back, or
                                   anything else that stops and restarts a fight, made a new fight every few seconds */
#define MD_AWAY_SLABS 20        /* "away from home": this far from its keeper's heart */
#define MD_VICE_SLOTS 5
#define MD_BANE_PERCENT 20      /* Do-Gooder's Bane: more damage to heroes */
#define MD_TERRITORIAL_SLABS 3  /* Territorial: how near the weaker creature must be */
#define MD_TERRITORIAL_SHARE 50 /* ... and the share of the hit it takes for it */
#define MD_NIGHT_SPEED 25       /* Afraid of Nothing in the Dark: faster at night */
#define MD_NIGHT_MISS 25        /* ... and the chance a hit on it misses at night */
#define MD_SHORTCUT_SPEED 20    /* Knows a Shortcut: its group is faster away from home */
#define MD_SHORTCUT_SLABS 6     /* ... and how near "its group" is */
#define MD_INFLUENCE_LEVELS 20  /* Bad Influence: this many levels above the trainee */
#define MD_INFLUENCE_PERCENT 10 /* ... gives this much more training experience */
#define MD_BODY_VICES 2         /* Unfinished Business: a creature with this many vices leaves a body for a day */
#define MD_BODY_MAX 64
#define MD_BLOOD_MAX 64         /* Bad Blood: inheritances waiting (one per keeper and kind) */

enum MdCounter {
    MdC_KillsHero, MdC_KillsWild, MdC_KillsKeeper, MdC_Soaked, MdC_NightFights, MdC_HomeFights, MdC_Lava, MdC_Water,
    MdC_Work, MdC_FarDist, MdC_AwayDist, MdC_Outposts, MdC_Hearts, MdC_Survived, MdC_Falls, MdC_Spare,
    MdC_COUNT
};

enum MdVice { MdV_None, MdV_Bane, MdV_Territorial, MdV_Night, MdV_Asbestos, MdV_Shortcut,
    /* More vices: one for every kind of misdeed that had none */
    MdV_Poacher, MdV_Backstabber, MdV_Glutton, MdV_Webbed, MdV_Workaholic, MdV_LongWay, MdV_Heartbreaker, MdV_Scarred,
    MdV_Angry, MdV_COUNT };
#define MD_OLD_VICES MdV_Shortcut /* the five that melt in pairs (Deadly Pairs) */

enum MdFlags { MdF_Renamed = 0x01, MdF_InFight = 0x02, MdF_Down = 0x04, MdF_Frozen = 0x08, MdF_HasLast = 0x10,
    MdF_Dying = 0x20 /* it is dying now: gone without a body, it is lost for good */,
    MdF_SinUsed = 0x40 /* Deadly Pairs: it melted its pair (once only; the two slots stay used) */,
    MdF_Portal = 0x80 /* Alumni: its keeper's portal brought it */ };
enum MdSpeedBits { MdS_Night = 0x01, MdS_Shortcut = 0x02 };

#pragma pack(1)
struct MdRec {
    uint8_t used;
    uint8_t flags;
    uint8_t vices[MD_VICE_SLOTS];   /* MdVice, in the order earned */
    uint8_t offer[2];               /* the two vices the next slot offers (0: none) */
    uint8_t speed_bits;             /* MdSpeedBits now in its top speed */
    uint16_t joined_day;
    uint32_t influence_turn;        /* Bad Influence seen at this turn */
    int32_t last_wx, last_wy;       /* world slab at its last sample */
    uint32_t c[MdC_COUNT];          /* (MdC_Spare: the vices lost to Unfinished Business; their slots stay empty) */
    uint8_t blood;                  /* Bad Blood: the vice it inherits at level 10 (0: none) */
    /* Deeper Vices (records saved before it end here: the fields below load as 0) */
    uint32_t rank_base[MD_VICE_SLOTS]; /* its misdeed's count when the vice in that slot was taken (or lost a rank) */
    uint8_t rank[MD_VICE_SLOTS];       /* the slot's rank as last seen, 1-5 (0: none, or not seen yet) */
    /* Deadly Pairs */
    uint8_t sin;                       /* the sin two of its vices were melted into (0: none) */
    uint8_t rack_marks;                /* the vice slots its keeper marked for the rack (bits) */
    uint8_t sin_speed;                 /* the sin's bonus in its top speed now, in percent */
    uint16_t rack_samples;             /* samples spent on the rack with two vices marked */
    /* Virtues */
    uint16_t flights;                  /* samples spent running from a fight */
    uint16_t slept_raids;              /* samples asleep while heroes raided its keeper */
    uint16_t knockouts;                /* enemies it knocked out */
    uint8_t virtues;                   /* MdVirtue bits it has; the same bits << 4: corrected, never earned again */
    uint16_t rest_day;                 /* corrected on the rack: no work until this day is over (0: none) */
    /* Lessons: a named creature chooses how it grows (slots: its kind's list of abilities) */
    uint8_t mastery[LEARNED_INSTANCES_COUNT]; /* how often the slot's ability was mastered */
    uint8_t lesson_delay[LEARNED_INSTANCES_COUNT]; /* tens of levels the slot's ability is put off by */
    uint8_t lessons_done;              /* tenth levels decided (or passed while it had no name) */
    uint8_t lesson_learn;              /* the offer: 1 + the slot it would learn now (0: nothing offered) */
    uint8_t lesson_master;             /* ... and 1 + the slot it would master (0: none) */
    /* Regimens */
    uint8_t regimen;                   /* the regimen it trains on (MdRegimen; 0: none) */
    uint8_t reg_levels[4];             /* levels earned in the Training Room on each regimen */
    uint8_t reg_last_level;            /* its level at the last sample (0: not seen) */
    /* Contracts */
    uint8_t contract;                  /* MdContract (0: Gold, as before) */
    uint8_t contract_done;             /* tenth levels renegotiated (or passed) */
    uint8_t contract_open;             /* a renegotiation is open: its keeper may change the contract until its next level */
    uint16_t last_kill_day;            /* Glory: the day of its last kill */
    uint16_t contract_day;             /* the day at its last sample */
    /* Past Lives */
    uint8_t past_lives;                /* the lives before this one */
    uint16_t past_model;               /* the kind it was in its last life (0: none) */
    uint16_t foreign_inst;             /* the ability it remembers from its last life (0: none) */
    /* THE SAVED RECORD ONLY GROWS AT ITS END. A save is loaded by copying the first bytes of each saved record into
       this one, so every feature's fields come after those of the features before it, in the order the features were
       built, and a new field goes last (the asserts below the struct hold the order). Audit 2026-11-13: from here on
       later fixes had landed in the middle, so a save of a build with part of these features was misread by a build
       with more of them. */
    /* Seeing Through Their Eyes */
    uint16_t driven;                   /* kills made while its keeper possessed it */
    uint8_t lent;                      /* it has Lightning lent by its party's possessed Mistress */
    uint8_t imp_book;                  /* it carries a book its keeper had it carry while possessed */
    /* Marching Order */
    uint8_t post;                      /* MdPost: its post in a Barracks party (0: none) */
    uint8_t march_speed;               /* the post's (or the standard's) bonus in its top speed now, in percent */
    /* ... on a party's leader: the gift of the standard carried in its party, and when it was last seen
       (audit 2026-11-13: one table of 48 for the whole world pushed parties' gifts out) */
    uint8_t std_gift;
    uint32_t std_turn;
    /* Breeds */
    uint8_t breed;                     /* MdBreed: what the biome of the portal that brought it made it (0: none) */
    /* Strays */
    uint8_t stray_by;                  /* 1 + the keeper feeding it (0: not a stray) */
    uint8_t stray_nights;              /* the nights it was fed */
    uint8_t stray_level;               /* its level in the wild */
    uint16_t stray_night;              /* the night it was last fed */
    /* More vices */
    uint32_t angry_until;              /* Comes Back Angry: the turn its anger ends */
    /* Counts that could be had for nothing (audit 2026-11-13): see MD_STAND_DAY, MD_FIGHT_GAP and far_base's use */
    uint16_t stand_day;                /* the day its lava and water samples were last counted on */
    uint8_t stand_lava;                /* ... and how many of each have counted that day */
    uint8_t stand_water;
    uint32_t fight_turn;               /* the turn the last fight that counted began (0: none yet) */
    uint8_t fight_counts;              /* 1: the fight it is in counts (it began MD_FIGHT_GAP or more after the last) */
    uint16_t far_base;                 /* how far from home it was put down (the Hand, a jump), or has walked nearer since */
};
_Static_assert(offsetof(struct MdRec, post) == offsetof(struct MdRec, driven) + 4, "Marching Order's fields follow Seeing Through Their Eyes'");
_Static_assert(offsetof(struct MdRec, breed) == offsetof(struct MdRec, post) + 7, "Breeds' field follows Marching Order's");
_Static_assert(offsetof(struct MdRec, stray_by) == offsetof(struct MdRec, breed) + 1, "Strays' fields follow Breeds'");
_Static_assert(offsetof(struct MdRec, angry_until) == offsetof(struct MdRec, stray_by) + 5, "More Vices' field follows Strays'");
_Static_assert(offsetof(struct MdRec, stand_day) == offsetof(struct MdRec, angry_until) + 4, "the audit's fields follow More Vices'");
_Static_assert(sizeof(struct MdRec) == offsetof(struct MdRec, stand_day) + 11, "a new field goes after far_base, with an assert of its own");

/* What a joined (thin) client's Rap Sheet draws of the record of the creature its panel shows: the server packs it
   into the game state's 128 bytes for that player (md_fill_views), and the client unpacks it (md_peek). The saved
   record is free to grow; this view is not. */
struct MdView {
    uint8_t used, flags;
    uint8_t vices[MD_VICE_SLOTS];
    uint8_t offer[2];
    uint16_t joined_day;
    uint32_t c[MdC_COUNT];
    uint8_t blood;
    uint8_t rank[MD_VICE_SLOTS];       /* Deeper Vices: each slot's rank ... */
    uint8_t rank_pct[MD_VICE_SLOTS];   /* ... and how far it is towards the next one */
    uint8_t sin, rack_marks;
    uint16_t rack_samples;
    uint8_t virtues;
    uint16_t rest_day;
    uint8_t mastery[LEARNED_INSTANCES_COUNT];
    uint8_t lessons_done, lesson_learn, lesson_master;
    uint8_t regimen, reg_levels[4];
    uint8_t contract, contract_open;
    uint8_t past_lives;
    uint16_t past_model, foreign_inst;
    uint16_t driven;
    uint8_t post;
    uint8_t breed;
};
#define MD_REC_SIZE_BEFORE_RANKS 89    /* the record's size before Deeper Vices: such records get their bases on load */
_Static_assert(offsetof(struct MdRec, rank_base) == MD_REC_SIZE_BEFORE_RANKS, "Deeper Vices' fields follow the old record");

/* Unfinished Business: a fallen creature's body and the record that waits with it */
struct MdBody {
    uint16_t rec;                   /* 0: free */
    uint16_t thing;                 /* the dead creature thing */
    uint32_t made_turn;             /* ... and its creation turn */
    uint32_t expire_turn;
    int32_t wx, wy;                 /* world slab where it lay at the last look */
    uint16_t model;
    uint8_t owner, level;           /* level: 0-based */
    uint8_t missing, pad[3];
    char name[24];
};
struct MdBlood { uint16_t model; uint8_t owner, vice; };
#pragma pack()

#define MD_SAVE_MAGIC 0x3144534Du /* "MSD1" */

static struct MdRec *md_recs = NULL; /* [1..md_top] */
static int md_cap = 0;
static int md_top = 0;
static struct MdBody md_body[MD_BODY_MAX];
static int md_body_count = 0;
static struct MdBlood md_blood[MD_BLOOD_MAX];
/* the creature that is dying this moment (set by world_misdeeds_dying, used before its thing is gone) */
static struct { uint16_t rec; uint16_t model; uint8_t owner, level; char name[24]; } md_dying;
#define MD_BODY_MAGIC 0x3142534Du /* "MSB1": the bodies and inheritances after the records */

/* the creatures whose vice works on others, seen at their last sample */
struct MdNear { ThingIndex idx; uint32_t turn; uint8_t reach; uint8_t rank; }; /* reach, rank: its vice's */
#define MD_NEAR_MAX 48
static struct MdNear md_terr[MD_NEAR_MAX];
static struct MdNear md_short[MD_NEAR_MAX];

static const int md_vice_levels[MD_VICE_SLOTS] = {10, 25, 50, 75, 100};
static const char *const md_vice_names[MdV_COUNT] = {
    "", "Do-Gooder's Bane", "Territorial", "Afraid of Nothing in the Dark", "Asbestos Hide", "Knows a Shortcut",
    "Poacher", "Backstabber", "Glutton for Punishment", "Webbed Toes", "Workaholic", "Long Way From Home",
    "Heartbreaker", "Battle-Scarred", "Comes Back Angry"};
static const char *const md_vice_short[MdV_COUNT] = {
    "", "Do-Gooder's Bane", "Territorial", "No Fear in the Dark", "Asbestos Hide", "Knows a Shortcut",
    "Poacher", "Backstabber", "Glutton for Punishment", "Webbed Toes", "Workaholic", "Long Way From Home",
    "Heartbreaker", "Battle-Scarred", "Comes Back Angry"};
static const char *const md_vice_does[MdV_COUNT] = {
    "", "+20% damage to heroes", "Shields the weak on your land", "Swift, hard to hit at night",
    "Strolls through lava unharmed", "Its group hurries far from home",
    "+20% damage to wild creatures", "+15% damage to rival keepers' creatures", "Hits harder when badly hurt",
    "+30% speed in water", "Works 15% faster", "+15% damage far from home", "+20% damage to foes on their own ground",
    "Takes less harm when near death", "Hits harder after it gets up"};
/* what earns each vice, and its weight when the two strongest misdeeds are chosen (score: count * weight / div;
   Heartbreaker counts outposts and hearts together) */
static const int md_vice_source[MdV_COUNT] = {0, MdC_KillsHero, MdC_HomeFights, MdC_NightFights, MdC_Lava, MdC_AwayDist,
    MdC_KillsWild, MdC_KillsKeeper, MdC_Soaked, MdC_Water, MdC_Work, MdC_FarDist, MdC_Outposts, MdC_Survived, MdC_Falls};
static const int md_vice_weight[MdV_COUNT] = {0, 40, 4, 40, 8, 1, 25, 60, 1, 8, 4, 6, 300, 30, 50};
static const int md_vice_div[MdV_COUNT] = {1, 1, 1, 1, 1, 1, 1, 1, 10, 1, 1, 1, 1, 1, 1};
/* More vices: their strength at rank I (each rank above adds a quarter) */
#define MD_POACHER_PERCENT 20      /* Poacher: more damage to wild creatures */
#define MD_BACKSTAB_PERCENT 15     /* Backstabber: more damage to other keepers' creatures */
#define MD_GLUTTON_PERCENT 20      /* Glutton for Punishment: more damage below half its health */
#define MD_WEBBED_SPEED 30         /* Webbed Toes: faster standing in water */
#define MD_WORKAHOLIC_PERCENT 15   /* Workaholic: faster at research and manufacture */
#define MD_LONGWAY_PERCENT 15      /* Long Way From Home: more damage at MD_AWAY_SLABS or more from its heart */
#define MD_HEARTBREAK_PERCENT 20   /* Heartbreaker: more damage to a foe standing on its own ground */
#define MD_SCARRED_PERCENT 20      /* Battle-Scarred: less damage taken below a quarter of its health */
#define MD_ANGRY_PERCENT 25        /* Comes Back Angry: more damage for a while after it gets up from a fall */
#define MD_ANGRY_TURNS (60 * 20)   /* (a minute) */

/* Deeper Vices: a vice rises through ranks as its misdeed goes on being counted. Rank I comes with the vice; rank II
   takes md_rank_unit deeds after that, and each rank after needs twice as many as the one before (1, 2, 4, 8 units:
   rank V takes 15). Each rank above I adds a quarter of the vice's base strength. */
#define MD_RANKS 5
static const uint32_t md_rank_unit[MdV_COUNT] = {0, 100, 400, 50, 1000, 2000, 150, 50, 20000, 1000, 2000, 30, 3, 60, 10};
static const char *const md_roman[MD_RANKS + 1] = {"", "I", "II", "III", "IV", "V"};

static int md_log_budget = 24; /* effect log lines per run */
static TbBool md_jointest_client = false; /* test hook: a joined client logs the notices it gets */
static void md_lost(struct MdRec *rec, PlayerNumber owner, ThingModel model, const char *name);
static void md_tell(PlayerNumber plyr_idx, const char *text);
static void md_tell_sin(PlayerNumber plyr_idx, int sin, const char *who);
static const char *md_called(const struct Thing *thing, char *buf, size_t size);
static void md_log_ranks(const char *when, const struct MdRec *rec);

/******************************************************************************/
/* Records */

static struct MdRec *md_rec(int id)
{
    if ((id <= 0) || (id > md_top) || !md_recs[id].used)
        return NULL;
    return &md_recs[id];
}

_Static_assert(sizeof(struct MdView) <= sizeof(game.world_misdeed_view[0]), "a Misdeeds view must fit its place in the game state");
static struct MdRec md_view_rec; /* a thin client's record, unpacked from its view */
static void md_view_unpack(const struct MdView *view, struct MdRec *rec);

static struct MdRec *md_peek(const struct Thing *thing)
{
    if (thing->class_id != TCls_Creature)
        return NULL;
    if (netstate_is_client() && netstate_thin_mode())
    {
        /* a thin client has no records: the server sends the one its panel shows (md_fill_views) */
        const PlayerNumber me = my_player_number;
        if ((me < 0) || (me >= PLAYERS_MAX) || (game.world_misdeed_view_thing[me] != thing->index))
            return NULL;
        const struct MdView *view = (const struct MdView *)game.world_misdeed_view[me];
        if (!view->used)
            return NULL;
        md_view_unpack(view, &md_view_rec);
        return &md_view_rec;
    }
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (creature_control_invalid(cctrl) || (cctrl->misdeed_id == 0))
        return NULL;
    return md_rec(cctrl->misdeed_id);
}

int world_misdeeds_vices_of(const struct Thing *thing, const char **names, const char **does, int max)
{
    if (!world_mode_active() || !thing_exists(thing) || (thing->class_id != TCls_Creature))
        return 0;
    const struct MdRec *rec = md_peek(thing);
    if (rec == NULL)
        return (netstate_is_client() && netstate_thin_mode()) ? -1 : 0;
    int n = 0;
    for (int i = 0; (i < MD_VICE_SLOTS) && (n < max); i++)
    {
        const int v = rec->vices[i];
        if ((v <= MdV_None) || (v >= MdV_COUNT))
            continue;
        names[n] = md_vice_names[v];
        does[n] = md_vice_does[v];
        n++;
    }
    return n;
}

static int md_new(void)
{
    int id = 0;
    for (int i = 1; i <= md_top; i++)
        if (!md_recs[i].used)
        {
            id = i;
            break;
        }
    if (id == 0)
    {
        if (md_top >= 65535)
            return 0;
        if (md_top + 1 >= md_cap)
        {
            const int ncap = md_cap + 256;
            struct MdRec *n = (struct MdRec *)realloc(md_recs, sizeof(struct MdRec) * ncap);
            if (n == NULL)
                return 0;
            memset(n + md_cap, 0, sizeof(struct MdRec) * 256);
            md_recs = n;
            md_cap = ncap;
        }
        id = ++md_top;
    }
    memset(&md_recs[id], 0, sizeof(struct MdRec));
    md_recs[id].used = 1;
    md_recs[id].joined_day = (uint16_t)world_day_number();
    return id;
}

static struct MdRec *md_get(struct Thing *thing, struct CreatureControl *cctrl)
{
    struct MdRec *rec = md_rec(cctrl->misdeed_id);
    if (rec != NULL)
        return rec;
    const int id = md_new();
    cctrl->misdeed_id = (unsigned short)id;
    return md_rec(id);
}

static TbBool md_has(const struct MdRec *rec, int vice)
{
    for (int i = 0; i < MD_VICE_SLOTS; i++)
        if (rec->vices[i] == vice)
            return true;
    return false;
}

static int md_vice_count(const struct MdRec *rec)
{
    int n = 0;
    for (int i = 0; i < MD_VICE_SLOTS; i++)
        if (rec->vices[i] != MdV_None)
            n++;
    return n;
}

/** Deeds of its misdeed a vice needs, counted from when it was taken, to stand at this rank (rank I: none). */
static uint32_t md_rank_deeds(int vice, int rank)
{
    return md_rank_unit[vice] * (((uint32_t)1 << (rank - 1)) - 1);
}

/** A vice's deeds: the count of the misdeed that earns it (Heartbreaker: outposts and hearts taken). */
static uint32_t md_deeds(const struct MdRec *rec, int vice)
{
    const uint32_t n = rec->c[md_vice_source[vice]];
    return (vice == MdV_Heartbreaker) ? n + rec->c[MdC_Hearts] : n;
}

/** The rank of the vice in this slot now, from its misdeed's count: 0 for an empty slot. */
static int md_slot_rank(const struct MdRec *rec, int slot)
{
    const int vice = rec->vices[slot];
    if ((vice <= MdV_None) || (vice >= MdV_COUNT))
        return 0;
    const uint32_t count = md_deeds(rec, vice);
    const uint32_t deeds = (count > rec->rank_base[slot]) ? count - rec->rank_base[slot] : 0;
    int rank = 1;
    while ((rank < MD_RANKS) && (deeds >= md_rank_deeds(vice, rank + 1)))
        rank++;
    return rank;
}

/** The rank of this vice on the creature; 0 when it hasn't got it. */
static int md_vice_rank(const struct MdRec *rec, int vice)
{
    for (int i = 0; i < MD_VICE_SLOTS; i++)
        if (rec->vices[i] == vice)
            return md_slot_rank(rec, i);
    return 0;
}

/** A vice comes into a slot at rank I: its deeds count from now. */
static void md_slot_start(struct MdRec *rec, int slot)
{
    rec->rank_base[slot] = md_deeds(rec, rec->vices[slot]);
    rec->rank[slot] = 1;
}

/** A vice's strength at a rank: its base value plus a quarter of it for each rank above I. */
static int md_ranked(int base, int rank)
{
    return base + base * max(rank - 1, 0) / 4;
}

/* Territorial at a rank: the share of the hit it takes, and how near the weaker creature may be */
#define MD_TERR_SHARE(rank) (MD_TERRITORIAL_SHARE + 5 * (max((rank), 1) - 1))
#define MD_TERR_REACH(rank) (MD_TERRITORIAL_SLABS + (max((rank), 1) - 1) / 2)
/* Knows a Shortcut at a rank: how near the creatures of its group may be */
#define MD_SHORT_REACH(rank) (MD_SHORTCUT_SLABS + max((rank), 1) - 1)

/** What a vice does at a rank, in a line of the Rap Sheet. */
static void md_vice_does_text(int vice, int rank, char *out, size_t size)
{
    switch (vice)
    {
    case MdV_Bane:
        snprintf(out, size, "+%d%% damage to heroes", md_ranked(MD_BANE_PERCENT, rank));
        break;
    case MdV_Territorial:
        snprintf(out, size, "Takes %d%% of hits for the weak", MD_TERR_SHARE(rank));
        break;
    case MdV_Night:
        snprintf(out, size, "Night: +%d%% speed, %d%% dodge", md_ranked(MD_NIGHT_SPEED, rank), md_ranked(MD_NIGHT_MISS, rank));
        break;
    case MdV_Asbestos:
        snprintf(out, size, (rank <= 1) ? "Strolls through lava unharmed" : "Lava-proof, and lava heals it");
        break;
    case MdV_Shortcut:
        snprintf(out, size, "Its group: +%d%% speed far out", md_ranked(MD_SHORTCUT_SPEED, rank));
        break;
    case MdV_Poacher:
        snprintf(out, size, "+%d%% damage to wild creatures", md_ranked(MD_POACHER_PERCENT, rank));
        break;
    case MdV_Backstabber:
        snprintf(out, size, "+%d%% damage to rivals' creatures", md_ranked(MD_BACKSTAB_PERCENT, rank));
        break;
    case MdV_Glutton:
        snprintf(out, size, "Under half health: +%d%% damage", md_ranked(MD_GLUTTON_PERCENT, rank));
        break;
    case MdV_Webbed:
        snprintf(out, size, "+%d%% speed in water", md_ranked(MD_WEBBED_SPEED, rank));
        break;
    case MdV_Workaholic:
        snprintf(out, size, "Works %d%% faster", md_ranked(MD_WORKAHOLIC_PERCENT, rank));
        break;
    case MdV_LongWay:
        snprintf(out, size, "Far from home: +%d%% damage", md_ranked(MD_LONGWAY_PERCENT, rank));
        break;
    case MdV_Heartbreaker:
        snprintf(out, size, "+%d%% damage to foes on their ground", md_ranked(MD_HEARTBREAK_PERCENT, rank));
        break;
    case MdV_Scarred:
        snprintf(out, size, "Near death: %d%% less harm", md_ranked(MD_SCARRED_PERCENT, rank));
        break;
    case MdV_Angry:
        snprintf(out, size, "After a fall: +%d%% damage", md_ranked(MD_ANGRY_PERCENT, rank));
        break;
    default:
        out[0] = 0;
        break;
    }
}

/** Unfinished Business, Deeper Vices: the highest-ranked vice drops one rank (its deeds count again from the rank it
 *  falls to). Returns the slot, or -1 when every vice is at rank I (then a whole vice is lost, as before). */
static int md_lose_rank(struct MdRec *rec)
{
    int best = -1, best_rank = 1;
    for (int i = 0; i < MD_VICE_SLOTS; i++)
    {
        const int r = md_slot_rank(rec, i);
        if (r > best_rank)
        {
            best = i;
            best_rank = r;
        }
    }
    if (best < 0)
        return -1;
    const int vice = rec->vices[best];
    rec->rank_base[best] = md_deeds(rec, vice) - md_rank_deeds(vice, best_rank - 1);
    rec->rank[best] = (uint8_t)(best_rank - 1);
    return best;
}

/* Deadly Pairs: two vices melted on the rack (its own keeper's Torture Chamber) into one sin. Which pairs make which
   sin, and the sins' names and lines, are for the players to find: they are kept encoded (made by a script that is
   not part of the source). A creature melts once; its two slots stay used. */
/* Virtues: the mirror of the vices, and in this game a virtue is a flaw. A creature of level 10 or more earns one when
   its record shows the habit (once only); a keeper's spell cast on it on the rack of their own Torture Chamber
   corrects one, at the cost of its work until the next day is over. */
enum MdVirtue { MdT_Prudence = 0x01, MdT_Temperance = 0x02, MdT_Mercy = 0x04, MdT_ALL = 0x07 };
#define MD_VIRTUES 3
static const char *const md_virtue_names[MD_VIRTUES] = {"Prudence", "Temperance", "Mercy"};
static const char *const md_virtue_does[MD_VIRTUES] = {"runs from fights sooner", "trains a quarter slower",
    "won't finish a downed enemy"};
#define MD_PRUDENCE_FLIGHTS 40   /* samples running from fights (about 64 seconds in all) */
#define MD_PRUDENCE_FEAR 150     /* ... and how much sooner it runs: 15% of its health, in the fear check's tenths */
#define MD_TEMPERANCE_SLEPT 30   /* samples asleep while heroes raid its keeper (about 48 seconds) */
#define MD_TEMPERANCE_TRAIN 25   /* ... and how much slower it trains, in percent */
#define MD_MERCY_KNOCKOUTS 8     /* enemies knocked out, with no more than a quarter as many killed */

/* Lessons: at a tenth level where its kind's next ability is due, a creature with a name (renamed) holds it back and
   is offered a choice: learn it, or master an ability it knows. Mastering makes that ability quicker to use again and
   its shots stronger, and puts every ability it hasn't learnt off by ten levels. Creatures without a name learn as
   before. */
#define MD_LESSON_STEP 10
#define MD_LESSONS_UNSET 255     /* a record from a save before Lessons: its lessons start where it stands */
#define MD_MASTERY_MAX 5
#define MD_MASTERY_SPEED 30      /* per mastery: its reset time is divided by (100 + this) / 100 */
#define MD_MASTERY_DAMAGE 25     /* per mastery: its shots' damage, in percent */
#define MD_MASTERY_RANGE 10      /* per mastery: the farthest it is used from, in percent */

/* Regimens: a creature trains on one of four regimens. Each level it earns in the Training Room on one brings a little
   more of that regimen's stat and a little less of the others: +3% and -1% a level, capped at +45% and -15%.
   A creature keeps MD_REG_LEVELS regimen levels in all (md_regimen_level), so the four changes always add up to
   nothing: what one stat gains, the other three pay. */
enum MdRegimen { MdR_None, MdR_Brawn, MdR_Guard, MdR_Aim, MdR_Grit, MdR_COUNT };
static const char *const md_regimen_names[MdR_COUNT] = {"None", "Brawn", "Guard", "Aim", "Grit"};
#define MD_REG_UP 3
#define MD_REG_DOWN 1
#define MD_REG_UP_MAX 45
#define MD_REG_DOWN_MAX 15
#define MD_REG_LEVELS 15        /* the regimen levels a creature keeps in all (MD_REG_UP_MAX / MD_REG_UP) */

/** Regimens: a level earned on regimen r (0-3). Past MD_REG_LEVELS in all, the new level takes the place of one on the
 *  regimen the creature has most of (so it can be trained over to another regimen, a level at a time). Audit
 *  2026-11-13: the levels were kept without limit and the loss was capped at 15%, so fifteen levels on each of the
 *  four regimens gave +30% on all four stats. */
static void md_regimen_level(struct MdRec *rec, int r)
{
    for (;;)
    {
        int total = 0, from = -1;
        for (int i = 0; i < 4; i++)
        {
            total += rec->reg_levels[i];
            if ((i != r) && (rec->reg_levels[i] > 0) && ((from < 0) || (rec->reg_levels[i] > rec->reg_levels[from])))
                from = i;
        }
        if (total < MD_REG_LEVELS)
            break;
        if (from >= 0)
            rec->reg_levels[from]--;
        else if (total > MD_REG_LEVELS)
            rec->reg_levels[r]--; /* (an older record with more: down to what it keeps) */
        else
            return; /* all it keeps are on this regimen already */
    }
    rec->reg_levels[r]++;
}

/** Regimens: the percent a creature's stat (MdR_Brawn strength ... MdR_Grit health) has from its regimens. */
static int md_regimen_pct(const struct MdRec *rec, int stat)
{
    int own = 0, others = 0;
    for (int r = 0; r < 4; r++)
    {
        if (r + 1 == stat)
            own += rec->reg_levels[r];
        else
            others += rec->reg_levels[r];
    }
    return 100 + min(MD_REG_UP * own, MD_REG_UP_MAX) - min(MD_REG_DOWN * others, MD_REG_DOWN_MAX);
}

/* Contracts: every ten levels a creature renegotiates its pay. A named creature's keeper chooses (the Rap Sheet's
   contract row, while the renegotiation is open: until its next level); others take their kind's default. */
enum MdContract { MdK_Gold, MdK_Plunder, MdK_Board, MdK_Glory, MdK_COUNT };
static const char *const md_contract_names[MdK_COUNT] = {"Gold", "Plunder", "Bed and Board", "Glory"};
static const char *const md_contract_does[MdK_COUNT] = {"its wage, as always", "no wage; a bounty per kill, unpaid a day without one",
    "no wage; eats double, a bigger lair", "no wage; fury after a day without a kill"};
#define MD_PLUNDER_SHARE 25      /* Plunder: each kill costs its keeper this percent of its gold wage */

/** Contracts: a kind's own contract (the one creatures without a name take). */
static int md_contract_default(ThingModel model)
{
    static ThingModel board = -1;
    if (board < 0)
        board = (ThingModel)max(get_id(creature_desc, "BILE_DEMON"), 0L);
    return (model == board) ? MdK_Board : MdK_Gold;
}

/* Past Lives: a keeper's own creature of level 30 or more sacrificed in their own Temple leaves its soul, and the next
   creature their portals send carries over its name, one of its vices (at rank I) and one ability of its old kind that
   the new kind could never learn. One soul waits per keeper (the latest). */
#define MD_SOUL_LEVEL 30
#define MD_SOUL_ABILITIES 4
#define MD_SOUL_MAGIC 0x3153534Du /* "MSS1": the souls waiting, after the records (and the bodies) */
#pragma pack(1)
struct MdSoul {
    uint8_t used, owner, lives, vice;
    uint16_t model;
    uint16_t abilities[MD_SOUL_ABILITIES]; /* its later abilities, the latest learnt first */
    char name[24];
};
#pragma pack()
static struct MdSoul md_soul[PLAYERS_MAX];
static TbBool md_soul_owner_ok(int plyr_idx)
{
    return (plyr_idx >= 0) && (plyr_idx < PLAYERS_MAX);
}

/* Seeing Through Their Eyes: a reason to take over each of these kinds. Kills made while possessed are "Driven". */
#define MD_EYES_TRAP_SLABS 8     /* a possessed Fly sees other keepers' hidden traps this near */
#define MD_EYES_DIG 3            /* a possessed Imp digs this many times as hard */
#define MD_EYES_AIM 30           /* a possessed Warlock's shots, percent harder */
static ThingModel md_kind(const char *name)
{
    const long id = get_id(creature_desc, name);
    return (ThingModel)((id > 0) ? id : 0);
}
static TbBool md_driven_by(const struct Thing *thing)
{
    return flag_is_set(thing->alloc_flags, TAlF_IsControlled);
}
static ThingIndex md_vampire_read[PLAYERS_MAX]; /* the last hero each keeper's Vampire read */

/* Marching Order: in a party formed in the Barracks, each creature's post (chosen on its Rap Sheet) gives it a bonus,
   and the standard-bearer's kind gives the whole party one gift. A post counts only in a party. */
enum MdPost { MdP_None, MdP_Van, MdP_Flank, MdP_Rear, MdP_Standard, MdP_COUNT };
static const char *const md_post_names[MdP_COUNT] = {"None", "Van", "Flank", "Rear", "Standard"};
static const char *const md_post_does[MdP_COUNT] = {"", "takes fewer hits", "quicker on its feet", "reaches further",
    "its kind's gift to the party"};
#define MD_VAN_TAKEN 85          /* Van: percent of a hit it takes */
#define MD_FLANK_SPEED 15        /* Flank: faster, percent */
#define MD_REAR_REACH 25         /* Rear: its abilities reach further, percent */
enum MdGift { MdG_None, MdG_Harder, MdG_Sooner, MdG_Wounded };
#define MD_GIFT_HARDER 10        /* an Orc's standard: the party hits harder, percent */
#define MD_GIFT_SOONER 20        /* a Warlock's: the party's abilities come back sooner (reset x100/(100+this)) */
#define MD_GIFT_WOUNDED 25       /* a Dark Mistress's: its wounded (below half health) move faster, percent */

/** Marching Order: the post a creature holds now (MdP_None outside a party). */
static int md_post_now(const struct Thing *thing, const struct MdRec *rec)
{
    if ((rec == NULL) || (rec->post == MdP_None) || (rec->post >= MdP_COUNT) || !creature_is_group_member(thing))
        return MdP_None;
    return rec->post;
}

/** Marching Order: the gift of the standard carried in this creature's party (MdG_None: none). */
static int md_party_gift(const struct Thing *thing)
{
    if (!creature_is_group_member(thing))
        return MdG_None;
    const struct Thing *leader = get_group_leader(thing);
    if (thing_is_invalid(leader))
        return MdG_None;
    const struct MdRec *lrec = md_peek(leader);
    if ((lrec != NULL) && (lrec->std_gift != MdG_None) && (lrec->std_turn + 3 * MD_SAMPLE >= (uint32_t)game.play_gameturn))
        return lrec->std_gift;
    return MdG_None;
}

/** Marching Order: a standard-bearer's gift, kept on its party's leader (each party its own). */
static void md_standard_add(struct Thing *leader, int gift)
{
    struct CreatureControl *lctrl = creature_control_get_from_thing(leader);
    struct MdRec *lrec = creature_control_invalid(lctrl) ? NULL : md_get(leader, lctrl);
    if (lrec == NULL)
        return;
    lrec->std_gift = (uint8_t)gift;
    lrec->std_turn = (uint32_t)game.play_gameturn;
}

/* Alumni: every creature a keeper's portals brought adds to their renown while it serves: one step at level 10, one
   at 25 and one at 50. Renown raises newcomers' level (1 + renown / 5, never within 5 levels of the keeper's best) and
   shortens the wait between arrivals (divided by (100 + renown) / 100, renown counting up to 50). Counted for every
   keeper in one pass every 256 turns. */
#define MD_RENOWN_PERIOD 256
#define MD_RENOWN_PER_LEVEL 5
#define MD_RENOWN_BENEATH 5
#define MD_RENOWN_WAIT_MAX 50
static int md_renown[PLAYERS_MAX];
static int md_best_level[PLAYERS_MAX];
static int md_newcomer_level(int plyr_idx);

static void md_renown_pass(void)
{
    if ((game.play_gameturn % MD_RENOWN_PERIOD) != 7)
        return;
    static int last[PLAYERS_MAX];
    memset(md_renown, 0, sizeof(md_renown));
    memset(md_best_level, 0, sizeof(md_best_level));
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    long k = slist->index;
    for (int n = 0; (k > 0) && (n < THINGS_COUNT); n++)
    {
        const struct Thing *thing = thing_get(k);
        if (thing_is_invalid(thing))
            break;
        k = thing->next_of_class;
        if (!md_soul_owner_ok(thing->owner) || (thing->owner >= PLAYERS_COUNT) || !world_player_is_human(thing->owner))
            continue;
        const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        if (creature_control_invalid(cctrl))
            continue;
        const int level = cctrl->exp_level + 1;
        md_best_level[thing->owner] = max(md_best_level[thing->owner], level);
        const struct MdRec *rec = (cctrl->misdeed_id != 0) ? md_rec(cctrl->misdeed_id) : NULL;
        if ((rec != NULL) && flag_is_set(rec->flags, MdF_Portal))
            md_renown[thing->owner] += (level >= 10) + (level >= 25) + (level >= 50);
    }
    for (int p = 0; p < PLAYERS_COUNT; p++)
    {
        if (md_renown[p] / 10 > last[p] / 10)
        {
            SYNCLOG("Misdeeds: Alumni: player %d's portals have renown %d (best level %d)", p, md_renown[p], md_best_level[p]);
            char text[128];
            snprintf(text, sizeof(text), "Your portals' renown grows to %d: recruits now arrive at level %d, and sooner.",
                md_renown[p], md_newcomer_level(p));
            md_tell((PlayerNumber)p, text);
        }
        last[p] = md_renown[p];
    }
}

/* Breeds: a creature takes after the biome of the portal that brought it, where its kind has a breed there (one at
   most). The breed is shown on the Rap Sheet. */
enum MdBreed { MdB_None, MdB_FrostSpider, MdB_TombSpider, MdB_FireBeetle, MdB_GrottoFly, MdB_COUNT };
static const char *const md_breed_names[MdB_COUNT] = {"", "Frost Spider", "Tomb Spider", "Volcanic Beetle", "Grotto Fly"};
static const char *const md_breed_does[MdB_COUNT] = {"", "Freezes from level 1", "Its bite is poisoned",
    "Barely feels fire", "Drinks its victims' blood"};
#define MD_TOMB_BITE 35          /* Tomb Spider: its hits, percent harder (the poison) */
#define MD_BEETLE_FIRE 25        /* Volcanic Beetle: percent of fire damage it takes */
#define MD_FLY_DRINK 25          /* Grotto Fly: percent of the damage it deals that heals it */

static int md_breed_for(ThingModel model, int biome)
{
    const TbBool frozen = (biome == WgB_Frozen), ruins = (biome == WgB_Ruins), sand = (biome == WgB_Sandstone);
    const TbBool lava = (biome == WgB_Volcanic) || (biome == WgB_LavaOcean) || (biome == WgB_LavaRivers);
    const TbBool water = (biome == WgB_Flooded) || (biome == WgB_FloodedOcean) || (biome == WgB_FloodedRivers);
    (void)sand;
    if (model == md_kind("SPIDER"))
        return frozen ? MdB_FrostSpider : ruins ? MdB_TombSpider : MdB_None;
    if (model == md_kind("BUG"))
        return lava ? MdB_FireBeetle : MdB_None;
    if (model == md_kind("FLY"))
        return water ? MdB_GrottoFly : MdB_None;
    return MdB_None;
}

/** Breeds: what a breed adds to a creature that has it (the Frost Spider's Freeze). Called when it arrives and on
 *  every sample (a thaw makes the creature anew). */
static void md_breed_apply(struct Thing *thing, struct CreatureControl *cctrl, const struct MdRec *rec)
{
    if (rec->breed == MdB_FrostSpider)
    {
        const long freeze = get_id(instance_desc, "FREEZE");
        if ((freeze > 0) && (freeze < INSTANCE_TYPES_MAX) && !cctrl->instance_available[freeze])
            cctrl->instance_available[freeze] = true;
    }
}

/** Is a Dark Mistress its keeper has taken over in this creature's party (not the creature itself)? */
static TbBool md_party_has_ridden_mistress(const struct Thing *thing)
{
    const struct Thing *leader = get_group_leader(thing);
    if (thing_is_invalid(leader))
        return false;
    const struct Thing *m = leader;
    for (int n = 0; (n < GROUP_MEMBERS_COUNT + 1) && thing_exists(m) && (m->class_id == TCls_Creature); n++)
    {
        if ((m != thing) && (m->owner == thing->owner) && (m->model == md_kind("DARK_MISTRESS")) && md_driven_by(m))
            return true;
        const struct CreatureControl *mctrl = creature_control_get_from_thing(m);
        if (creature_control_invalid(mctrl) || (mctrl->next_in_group == 0))
            break;
        m = thing_get(mctrl->next_in_group);
    }
    return false;
}

/** Seeing Through Their Eyes: a possessed Imp picks up a book (a spellbook, or a heroes' research book) beside it and
 *  carries it; walked into its keeper's own Library, it shelves it (the stock drop); let go while carrying, it puts the
 *  book down where it is (the keeper's imps fetch it the usual way). Called on every sample. */
static void md_imp_book(struct Thing *thing, struct CreatureControl *cctrl, struct MdRec *rec)
{
    if (thing->model != md_kind("IMP"))
        return;
    struct Thing *book = (cctrl->dragtng_idx > 0) ? thing_get(cctrl->dragtng_idx) : INVALID_THING;
    const TbBool carrying = thing_exists(book) && (book->class_id == TCls_Object) && (thing_is_spellbook(book) ||
        thing_is_special_box(book));
    if (!md_driven_by(thing))
    {
        /* (only a book its keeper had it carry: an imp carrying one on its own stock job is left to it) */
        if (rec->imp_book)
        {
            rec->imp_book = 0;
            if (carrying)
            {
                creature_drop_dragged_object(thing, book);
                SYNCLOG("Misdeeds: Eyes: an Imp of player %d, let go, puts the book down", (int)thing->owner);
            }
        }
        return;
    }
    if (carrying)
    {
        rec->imp_book = 1; /* (its keeper carries it now) */
        struct Room *room = get_room_thing_is_on(thing);
        if (!room_is_invalid(room) && room_role_matches(room->kind, RoRoF_PowersStorage) && (room->owner == thing->owner) &&
            (room->used_capacity < room->total_capacity))
        {
            SYNCLOG("Misdeeds: Eyes: a possessed Imp of player %d shelves a %s in its Library", (int)thing->owner, thing_model_name(book));
            rec->imp_book = 0;
            creature_drops_spell_object_in_library(thing);
            md_tell(thing->owner, "Your Imp shelves the book in your Library.");
        }
        return;
    }
    if (cctrl->dragtng_idx > 0)
        return;
    const struct StructureList *slist = get_list_for_thing_class(TCls_Object);
    long k = slist->index;
    for (int n = 0; (k > 0) && (n < THINGS_COUNT); n++)
    {
        struct Thing *obj = thing_get(k);
        if (thing_is_invalid(obj))
            break;
        k = obj->next_of_class;
        if ((!thing_is_spellbook(obj) && !thing_is_special_box(obj)) || (get_chessboard_distance(&thing->mappos, &obj->mappos) >= subtile_coord(2, 0)) ||
            !thing_can_be_picked_to_place_in_player_room_of_role(obj, thing->owner, RoRoF_PowersStorage, TngFRPickF_Default))
            continue;
        creature_drag_object(thing, obj);
        rec->imp_book = 1;
        SYNCLOG("Misdeeds: Eyes: a possessed Imp of player %d picks up a %s", (int)thing->owner, thing_model_name(obj));
        md_tell(thing->owner, "Your Imp picks up the book: carry it into your Library.");
        return;
    }
}

/** Seeing Through Their Eyes: while its keeper rides a Dark Mistress, the rest of her party (a group) can cast her
 *  Lightning; it is taken back when she is let go or it leaves the party. Called on every sample. */
static void md_lend_lightning(struct Thing *thing, struct CreatureControl *cctrl, struct MdRec *rec)
{
    const long bolt = get_id(instance_desc, "LIGHTNING");
    if ((bolt <= 0) || (bolt >= INSTANCE_TYPES_MAX))
        return;
    const TbBool lend = md_party_has_ridden_mistress(thing);
    if (lend && !rec->lent && !cctrl->instance_available[bolt])
    {
        cctrl->instance_available[bolt] = true;
        rec->lent = 1;
        SYNCLOG("Misdeeds: Eyes: a %s index %d of player %d is lent its party's Mistress's Lightning", thing_model_name(thing),
            (int)thing->index, (int)thing->owner);
    } else if (lend && rec->lent && !cctrl->instance_available[bolt])
    {
        /* the loan stands, but the ability is gone: a chunk that froze and came back made the creature's abilities
           anew (audit 2026-11-13: it stayed without until its Mistress was let go and taken over again) */
        cctrl->instance_available[bolt] = true;
    } else if (!lend && rec->lent)
    {
        rec->lent = 0;
        cctrl->instance_available[bolt] = false;
        creature_increase_available_instances(thing); /* (it keeps Lightning if it has learnt it itself) */
        SYNCLOG("Misdeeds: Eyes: a %s index %d of player %d gives the Lightning back (it has it now: %d)", thing_model_name(thing),
            (int)thing->index, (int)thing->owner, (int)cctrl->instance_available[bolt]);
    }
}

#define MD_SINS 10
#define MD_RACK_TURNS 400       /* turns on the rack, with two vices marked, before they melt */
#define MD_SIN_TEXT_MAX 48
static const uint8_t md_s0_1[] = {0x34,0xE2,0xFB,0xC5,0xCA,0xEF,0xA2,0x84,0x76,0x70,0x4C,0x50,0x34,0x57};
static const uint8_t md_s0_2[] = {0xCE,0xD8,0xDA,0xB8,0xB5,0xCE,0x90,0x76,0x01,0x46,0x2B,0x31,0x45,0x3E,0xE2,0xF9,0xDB,0xCE,0xA3,0xDC};
static const uint8_t md_s0_3[] = {0xFA,0xBB,0xB3,0x86,0x8F,0x79,0x71,0x41,0x25,0x71,0x38,0x16,0xE5,0xF9,0xC9,0xC3,0xC8};
static const uint8_t md_s0_4[] = {0x9B,0x87,0x97,0x6D,0x3B,0x67,0x53,0x27,0x39,0x15,0x81};
static const uint8_t md_s0_5[] = {0xB8,0x6E,0x7F,0x41,0x4E,0x6B,0x0B,0x0C,0x0A,0xEC,0xC8,0xB1};
static const uint8_t md_s0_6[] = {0x5D,0x43,0x56,0x3A,0x2D,0x02,0x08,0xF8,0xF2,0xC0,0xDA,0xD0};
static const uint8_t md_s0_7[] = {0x7C,0x2A,0x3B,0x02,0x0F,0xE8,0xE8,0xCF,0x9C,0x9F,0xAB,0x9C,0x68,0x11};
static const uint8_t md_s0_8[] = {0x16,0x09,0x17,0xE3,0xE5,0x88,0xFC,0xB3,0xBE,0x9F,0xFD};
static const uint8_t md_s0_9[] = {0x3C,0xEA,0xF3,0xCD,0xC2,0xE7,0x8A,0x9C,0x94,0x65,0x79,0x5F,0x3E};
static const uint8_t md_s0_10[] = {0xD7,0xCB,0xC1,0xA1,0xA2,0x87,0x9B,0x63,0x7C,0x58,0x3B};
static const uint8_t md_s1_1[] = {0x22,0x1E,0xFE,0xF2,0x8E,0xD4,0xB9,0x8D,0x9E,0x70,0x34,0x44,0x50,0x33,0x3D,0x1B,0x5A,0xEF,0xE9,0xDE,0xD5,0xEF,0x88,0x94,0x63,0x7F,0x04,0x5C,0x32,0x57};
static const uint8_t md_s1_2[] = {0xA2,0xA9,0x9E,0x99,0xED,0xBA,0x8E,0x6D,0x70,0x45,0x56,0x64,0x3A,0x08,0x57,0xE1,0xED,0xD9,0x9B,0xA3,0xAA,0x80,0xDF,0x7C,0x40,0x5C,0x27,0x54};
static const uint8_t md_s1_3[] = {0xE4,0xD8,0xBC,0xBA,0xC1,0x8D,0x7C,0x70,0x5F,0x27,0x69,0x43,0x5F,0xB1,0xA6,0x82,0x98,0xAD,0xBB,0x86,0x9D,0x6A,0x7B,0x0F,0x29,0x3F,0x42,0x1F,0xE5,0xE3,0xC7,0xB7};
static const uint8_t md_s1_4[] = {0x81,0xB9,0x9B,0xDA,0x64,0x69,0x59,0x04,0x6F,0x4B,0x42,0xB2,0xB6,0x84,0xD1,0xA7,0xBA,0x89,0x9E,0x6F,0x37,0x0C,0x16,0x7F,0x6A,0x55,0xA1,0xE1,0xD3,0xD1,0xA0,0xB2,0xE7};
static const uint8_t md_s1_5[] = {0xA9,0x80,0x66,0x39,0x46,0x5A,0x22,0x39,0x4E,0x1E,0xE4,0x81,0xDC,0xAA,0xB3,0x8D,0x82,0x3D,0x38,0x04,0x09,0x7E,0x79,0x4D,0x16,0xFA,0xD2,0xC5,0xC2};
static const uint8_t md_s1_6[] = {0x49,0x77,0x51,0x59,0x64,0x2A,0x19,0x13,0xE2,0xF8,0x94,0xE0,0xBC,0x87,0x9D,0x60,0x66,0x06,0x58,0x26,0x79,0x05,0x0C,0xE2,0xBD,0xC2,0xDE,0xBE,0x85,0xF2};
static const uint8_t md_s1_7[] = {0x0F,0x01,0x76,0x72,0x48,0x0A,0xFA,0xFE,0xC9,0xD9,0xEE,0xB0,0x9E,0x21,0x7B,0x57,0x47,0x65,0x39,0x10,0x16,0xA9,0xF6,0xCA,0xD2,0xA9,0xDE};
static const uint8_t md_s1_8[] = {0x0F,0x35,0x13,0x17,0xAA,0xE8,0xDB,0xD5,0xA4,0xBA,0xD6,0xDE,0x61,0x49,0x56,0x2A,0x27,0x44,0x1D,0xEF,0xE3,0xDB,0x99,0xA8,0xAE,0x9E,0x93,0x2E,0x7D,0x51,0x22,0x39,0x63};
static const uint8_t md_s1_9[] = {0x2C,0x1A,0xE3,0xFD,0xD2,0x9B,0xE8,0xBF,0x8B,0x89,0x2C,0x72,0x5B,0x4B,0x6A,0x41,0x59,0xB7,0xA1,0x80,0x96,0xB4,0xA8,0x8C,0x9F,0x6F,0x1C};
static const uint8_t md_s1_10[] = {0xCD,0xF3,0xD5,0xD5,0xE8,0xA6,0x95,0x97,0x66,0x7C,0x10,0x1C,0x3C,0x2B,0x06,0xE3,0xFA,0x82,0xDA,0xAA,0xF5,0x8A,0x96,0x7E,0x78,0x06,0x1B,0x2D,0x3B,0x0F,0x0D,0x90};
static const uint8_t md_sin_pairs[MD_SINS] = {0x28,0x5E,0x49,0x7B,0x5C,0xB4,0x84,0x86,0xF6,0x91};
static const uint8_t *const md_sin_t[2][MD_SINS] = {
    {md_s0_1, md_s0_2, md_s0_3, md_s0_4, md_s0_5, md_s0_6, md_s0_7, md_s0_8, md_s0_9, md_s0_10},
    {md_s1_1, md_s1_2, md_s1_3, md_s1_4, md_s1_5, md_s1_6, md_s1_7, md_s1_8, md_s1_9, md_s1_10}};

static uint8_t md_sin_key(int i, int sin, int kind)
{
    return (uint8_t)(0x5B + i * 17 + (sin + kind * 16) * 31);
}

/** A sin's name (kind 0) or line (kind 1), decoded into buf. */
static const char *md_sin_text(int kind, int sin, char *buf)
{
    buf[0] = 0;
    if ((sin < 1) || (sin > MD_SINS))
        return buf;
    const uint8_t *src = md_sin_t[kind][sin - 1];
    for (int i = 0; i < MD_SIN_TEXT_MAX - 1; i++)
    {
        buf[i] = (char)(src[i] ^ md_sin_key(i, sin, kind));
        if (buf[i] == 0)
            break;
    }
    buf[MD_SIN_TEXT_MAX - 1] = 0;
    return buf;
}

/** The sin these two vices melt into (0: none). */
static int md_sin_of(int a, int b)
{
    for (int s = 0; s < MD_SINS; s++)
    {
        const uint8_t v = md_sin_pairs[s] ^ md_sin_key(s, 0, 2);
        if ((((v >> 4) == a) && ((v & 15) == b)) || (((v >> 4) == b) && ((v & 15) == a)))
            return s + 1;
    }
    return 0;
}

/** Sins that keep their creature lava-proof. */
static TbBool md_sin_lava_proof(int sin)
{
    return (sin == 3) || (sin == 6) || (sin == 8) || (sin == 10);
}

static int md_bits(unsigned v)
{
    int n = 0;
    for (; v != 0; v &= v - 1)
        n++;
    return n;
}

/** The vice slots that hold a vice (bits). */
static uint8_t md_filled_slots(const struct MdRec *rec)
{
    uint8_t bits = 0;
    for (int i = 0; i < MD_VICE_SLOTS; i++)
        if (rec->vices[i] != MdV_None)
            bits |= (uint8_t)(1 << i);
    return bits;
}

/** The slots a record has used up: its vices, the vices it lost in the grave, and the pair it melted. */
static int md_used_slots(const struct MdRec *rec)
{
    return md_vice_count(rec) + (int)rec->c[MdC_Spare] + (flag_is_set(rec->flags, MdF_SinUsed) ? 2 : 0);
}

/** The slots a creature of this level (1-based) has earned. */
static int md_slots_for_level(int level)
{
    int n = 0;
    for (int i = 0; i < MD_VICE_SLOTS; i++)
        if (level >= md_vice_levels[i])
            n++;
    return n;
}

/** A slot is waiting and nothing is offered yet: offer the vices of its two strongest misdeeds. */
static void md_check_offer(struct MdRec *rec, const struct Thing *thing, int level)
{
    if ((rec->offer[0] != MdV_None) || (md_used_slots(rec) >= md_slots_for_level(level)))
        return;
    if ((rec->blood != MdV_None) && (rec->blood < MdV_COUNT))
    {
        /* Bad Blood: the slot takes the vice of the one lost before it, with no pick */
        const int vice = rec->blood;
        rec->blood = MdV_None;
        if (!md_has(rec, vice))
        {
            for (int i = 0; i < MD_VICE_SLOTS; i++)
                if (rec->vices[i] == MdV_None)
                {
                    rec->vices[i] = (uint8_t)vice;
                    md_slot_start(rec, i);
                    break;
                }
            SYNCLOG("Misdeeds: Bad Blood: %s index %d (level %d) inherited %s", thing_model_name(thing), (int)thing->index,
                level, md_vice_names[vice]);
            return;
        }
    }
    long best[2] = {-1, -1};
    int pick[2] = {MdV_None, MdV_None};
    for (int v = 1; v < MdV_COUNT; v++)
    {
        if (md_has(rec, v))
            continue;
        const long count = (long)md_deeds(rec, v); /* (Heartbreaker: outposts and hearts it helped take) */
        const long score = count * md_vice_weight[v] / md_vice_div[v];
        if (score > best[0])
        {
            best[1] = best[0];
            pick[1] = pick[0];
            best[0] = score;
            pick[0] = v;
        } else if (score > best[1])
        {
            best[1] = score;
            pick[1] = v;
        }
    }
    rec->offer[0] = (uint8_t)pick[0];
    rec->offer[1] = (uint8_t)pick[1];
    if (pick[0] != MdV_None)
        SYNCLOG("Misdeeds: %s index %d (level %d, %s) earned a vice: the keeper picks %s or %s",
            thing_model_name(thing), (int)thing->index, level, creature_own_name(thing), md_vice_names[pick[0]],
            (pick[1] != MdV_None) ? md_vice_names[pick[1]] : "nothing else");
}

static void md_near_add(struct MdNear *list, ThingIndex idx, int reach, int rank)
{
    int slot = 0;
    for (int i = 0; i < MD_NEAR_MAX; i++)
    {
        if (list[i].idx == idx)
        {
            slot = i;
            break;
        }
        if (list[i].turn < list[slot].turn)
            slot = i;
    }
    list[slot].idx = idx;
    list[slot].turn = (uint32_t)game.play_gameturn;
    list[slot].reach = (uint8_t)reach;
    list[slot].rank = (uint8_t)rank;
}

/** A creature of this owner on the list, seen lately, within its vice's reach of the thing (not the thing itself
 *  unless self_ok): its vice works here. Of several, the one of the highest rank; *rank_out gets that rank. */
static struct Thing *md_near_find(const struct MdNear *list, const struct Thing *thing, TbBool self_ok, int *rank_out)
{
    struct Thing *found = INVALID_THING;
    int found_rank = 0;
    for (int i = 0; i < MD_NEAR_MAX; i++)
    {
        if ((list[i].idx == 0) || (list[i].turn + 3 * MD_SAMPLE < (uint32_t)game.play_gameturn))
            continue;
        if ((list[i].idx == thing->index) && !self_ok)
            continue;
        struct Thing *other = thing_get(list[i].idx);
        if (!thing_exists(other) || (other->class_id != TCls_Creature) || (other->owner != thing->owner) ||
            (other->health < 0))
            continue;
        const int slabs = list[i].reach;
        if ((abs(coord_slab(other->mappos.x.val) - coord_slab(thing->mappos.x.val)) <= slabs) &&
            (abs(coord_slab(other->mappos.y.val) - coord_slab(thing->mappos.y.val)) <= slabs) && (list[i].rank > found_rank))
        {
            found = other;
            found_rank = list[i].rank;
        }
    }
    if (rank_out != NULL)
        *rank_out = found_rank;
    return found;
}

static TbBool md_on_own_land(const struct Thing *thing)
{
    const struct SlabMap *slb = get_slabmap_for_subtile(thing->mappos.x.stl.num, thing->mappos.y.stl.num);
    return !slabmap_block_invalid(slb) && (slabmap_owner(slb) == thing->owner);
}

/** Slabs from its keeper's heart; -1 when the heart isn't on the map. In the world apart (a World Boss's arena) a
 *  creature is away from home and no more: the place is on no map with its heart, and the slabs between them on the
 *  world plane (several hundred thousand) are no distance it has come (they would be counted as the farthest it has
 *  been from home, and no counter holds them). */
static long md_home_distance(const struct Thing *thing)
{
    const struct Thing *heart = get_player_soul_container(thing->owner);
    if (!thing_exists(heart))
        return -1;
    if (world_apart_thing(thing))
        return MD_AWAY_SLABS;
    const long dx = labs((long)coord_slab(thing->mappos.x.val) - coord_slab(heart->mappos.x.val));
    const long dy = labs((long)coord_slab(thing->mappos.y.val) - coord_slab(heart->mappos.y.val));
    return max(dx, dy) + min(dx, dy) / 2;
}

/** Deeper Vices: how far the vice in a slot is towards its next rank, in percent (0 at rank V). */
static int md_rank_pct(const struct MdRec *rec, int slot)
{
    const int vice = rec->vices[slot];
    const int rank = md_slot_rank(rec, slot);
    if ((rank <= 0) || (rank >= MD_RANKS))
        return 0;
    const uint32_t count = md_deeds(rec, vice);
    const uint32_t deeds = count - min(rec->rank_base[slot], count);
    const uint32_t from = md_rank_deeds(vice, rank), to = md_rank_deeds(vice, rank + 1);
    return (int)min((uint64_t)(deeds - min(deeds, from)) * 100 / max(to - from, 1u), (uint64_t)99);
}

long world_misdeeds_regimen_stat(const struct Thing *thing, int stat, long value)
{
    if ((md_top == 0) || !world_mode_active() || (stat <= MdR_None) || (stat >= MdR_COUNT))
        return value;
    const struct MdRec *rec = md_peek(thing);
    if ((rec == NULL) || ((rec->reg_levels[0] | rec->reg_levels[1] | rec->reg_levels[2] | rec->reg_levels[3]) == 0))
        return value;
    value = value * md_regimen_pct(rec, stat) / 100;
    if ((stat == MdR_Guard) || (stat == MdR_Aim))
        value = min(value, 255L); /* (as the stock caps defence and dexterity) */
    return max(value, 1L);
}

/** Contracts: the places a creature's lair takes in a Lair: twice its kind's on Bed and Board (its bigger lair is
 *  real room; audit 2026-11-13: it was only drawn bigger, so the contract cost double food and nothing else). */
int world_misdeeds_lair_places(const struct Thing *creatng, int places)
{
    if ((md_top == 0) || (places <= 0) || !world_mode_active() || !thing_exists(creatng) || (creatng->class_id != TCls_Creature))
        return places;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    const struct MdRec *rec = (creature_control_invalid(cctrl) || (cctrl->misdeed_id == 0)) ? NULL : md_rec(cctrl->misdeed_id);
    return ((rec != NULL) && (rec->contract == MdK_Board)) ? places * 2 : places;
}

/** Contracts: a creature takes a contract. Going on or off Bed and Board changes the room its lair takes, so it gives
 *  its lair up first (counted at the old size) and makes a new one the stock way (counted at the new size). */
static void md_contract_set(struct Thing *thing, struct MdRec *rec, int contract)
{
    if ((rec->contract == MdK_Board) != (contract == MdK_Board))
        remove_creature_lair(thing);
    rec->contract = (uint8_t)contract;
}

/** Contracts: Bed and Board: the size its lair totem is drawn at (half again as big). */
int world_misdeeds_lair_size(const struct Thing *creatng, int size)
{
    if ((md_top == 0) || !world_mode_active() || !thing_exists(creatng) || (creatng->class_id != TCls_Creature))
        return size;
    const struct MdRec *rec = md_peek(creatng);
    return ((rec != NULL) && (rec->contract == MdK_Board)) ? size * 3 / 2 : size;
}

/** Contracts: a contract without a wage. The stock payday owes a wage to every creature whose KIND has one, and a
 *  creature collecting a wage of nothing never left the Treasury again (audit 2026-11-13): such a creature is owed no
 *  payday, and one already on its way there turns back. */
TbBool world_misdeeds_no_wage(const struct Thing *thing)
{
    if ((md_top == 0) || !world_mode_active())
        return false;
    const struct MdRec *rec = md_peek(thing);
    return (rec != NULL) && (rec->contract != MdK_Gold) && (rec->contract < MdK_COUNT);
}

GoldAmount world_misdeeds_pay(const struct Thing *thing, GoldAmount pay)
{
    return world_misdeeds_no_wage(thing) ? 0 : pay;
}

long world_misdeeds_range(const struct Thing *thing, long range)
{
    if ((md_top == 0) || !world_mode_active() || (thing->class_id != TCls_Creature) || (range <= 0) || (range >= LONG_MAX / 200))
        return range; /* (an unlimited range, INT_MAX for most shots, stays so: scaling it would overflow) */
    return (md_post_now(thing, md_peek(thing)) == MdP_Rear) ? range * (100 + MD_REAR_REACH) / 100 : range;
}

long world_misdeeds_dig_damage(const struct Thing *digger, long damage)
{
    if ((md_top == 0) || !world_mode_active() || !md_driven_by(digger) || (digger->model != md_kind("IMP")))
        return damage;
    return damage * MD_EYES_DIG;
}

long world_misdeeds_hunger(const struct Thing *thing, long hunger)
{
    if ((md_top == 0) || !world_mode_active())
        return hunger;
    const struct MdRec *rec = md_peek(thing);
    return ((rec != NULL) && (rec->contract == MdK_Board)) ? hunger * 2 : hunger;
}

static void md_view_pack(const struct MdRec *rec, struct MdView *view)
{
    memset(view, 0, sizeof(*view));
    view->contract = rec->contract;
    view->contract_open = rec->contract_open;
    view->past_lives = rec->past_lives;
    view->driven = rec->driven;
    view->post = rec->post;
    view->breed = rec->breed;
    view->past_model = rec->past_model;
    view->foreign_inst = rec->foreign_inst;
    view->regimen = rec->regimen;
    memcpy(view->reg_levels, rec->reg_levels, sizeof(view->reg_levels));
    view->used = rec->used;
    view->flags = rec->flags;
    memcpy(view->vices, rec->vices, sizeof(view->vices));
    memcpy(view->offer, rec->offer, sizeof(view->offer));
    view->joined_day = rec->joined_day;
    memcpy(view->c, rec->c, sizeof(view->c));
    view->blood = rec->blood;
    for (int i = 0; i < MD_VICE_SLOTS; i++)
    {
        view->rank[i] = (uint8_t)md_slot_rank(rec, i);
        view->rank_pct[i] = (uint8_t)md_rank_pct(rec, i);
    }
    view->sin = rec->sin;
    view->rack_marks = rec->rack_marks;
    view->rack_samples = rec->rack_samples;
    view->virtues = rec->virtues;
    view->rest_day = rec->rest_day;
    memcpy(view->mastery, rec->mastery, sizeof(view->mastery));
    view->lessons_done = rec->lessons_done;
    view->lesson_learn = rec->lesson_learn;
    view->lesson_master = rec->lesson_master;
}

static void md_view_unpack(const struct MdView *view, struct MdRec *rec)
{
    memset(rec, 0, sizeof(*rec));
    rec->contract = view->contract;
    rec->contract_open = view->contract_open;
    rec->past_lives = view->past_lives;
    rec->driven = view->driven;
    rec->post = view->post;
    rec->breed = view->breed;
    rec->past_model = view->past_model;
    rec->foreign_inst = view->foreign_inst;
    rec->regimen = view->regimen;
    memcpy(rec->reg_levels, view->reg_levels, sizeof(rec->reg_levels));
    rec->used = view->used;
    rec->flags = view->flags;
    memcpy(rec->vices, view->vices, sizeof(rec->vices));
    memcpy(rec->offer, view->offer, sizeof(rec->offer));
    rec->joined_day = view->joined_day;
    memcpy(rec->c, view->c, sizeof(rec->c));
    rec->blood = view->blood;
    for (int i = 0; i < MD_VICE_SLOTS; i++)
    {
        /* the rank and progress the server saw, as a count to rank from */
        const int vice = rec->vices[i];
        if ((vice <= MdV_None) || (vice >= MdV_COUNT) || (view->rank[i] == 0))
            continue;
        const int rank = view->rank[i];
        const uint32_t from = md_rank_deeds(vice, rank);
        const uint32_t to = (rank < MD_RANKS) ? md_rank_deeds(vice, rank + 1) : from;
        const uint32_t deeds = from + (uint32_t)((uint64_t)(to - from) * view->rank_pct[i] / 100);
        const uint32_t count = md_deeds(rec, vice);
        rec->rank_base[i] = (count >= deeds) ? count - deeds : 0;
        rec->rank[i] = (uint8_t)rank;
    }
    rec->sin = view->sin;
    rec->rack_marks = view->rack_marks;
    rec->rack_samples = view->rack_samples;
    rec->virtues = view->virtues;
    rec->rest_day = view->rest_day;
    memcpy(rec->mastery, view->mastery, sizeof(rec->mastery));
    rec->lessons_done = view->lessons_done;
    rec->lesson_learn = view->lesson_learn;
    rec->lesson_master = view->lesson_master;
}

/** Lessons: the level a slot's ability comes at for this creature: its kind's, put off by its masteries, and held back
 *  (255) while that tenth level's lesson is undecided. */
static int md_learn_level(const struct MdRec *rec, int slot, int natural)
{
    if ((rec == NULL) || !flag_is_set(rec->flags, MdF_Renamed) || (natural < MD_LESSON_STEP) || (rec->lessons_done == MD_LESSONS_UNSET))
        return natural;
    const int level = natural + MD_LESSON_STEP * rec->lesson_delay[slot];
    return (level > MD_LESSON_STEP * rec->lessons_done) ? 255 : level;
}

long world_misdeeds_inst_range(const struct Thing *thing, CrInstance inst, long range)
{
    if ((md_top == 0) || !world_mode_active() || (thing->class_id != TCls_Creature) || (range <= 0) || (range >= LONG_MAX / 200))
        return range; /* (an unlimited range, INT_MAX for most shots, stays so: scaling it would overflow) */
    const struct MdRec *rec = md_peek(thing);
    if (rec == NULL)
        return range;
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
    for (int i = 0; i < LEARNED_INSTANCES_COUNT; i++)
        if ((crconf->learned_instance_id[i] == inst) && (rec->mastery[i] > 0))
        {
            range = range * (100 + MD_MASTERY_RANGE * rec->mastery[i]) / 100;
            break;
        }
    return world_misdeeds_range(thing, range); /* Marching Order: the Rear's reach, in every fight (not only at a post) */
}

int world_misdeeds_learn_level(const struct Thing *thing, int slot, int natural)
{
    if ((md_top == 0) || !world_mode_active() || (slot < 0) || (slot >= LEARNED_INSTANCES_COUNT))
        return natural;
    return md_learn_level(md_peek(thing), slot, natural);
}

/** Lessons: the next lesson, when the creature has reached its tenth level: what it would learn there (an ability of
 *  its kind due at or before it that it hasn't got) and what it would master (the one it knows with the fewest
 *  masteries, the later one on a tie). Nothing due: the tenth level passes without a choice. */
static void md_lesson_check(struct MdRec *rec, struct Thing *thing, const struct CreatureControl *cctrl, int level)
{
    const int tenth = min(level / MD_LESSON_STEP, MD_LESSONS_UNSET - 1);
    if (!flag_is_set(rec->flags, MdF_Renamed) || (rec->lessons_done == MD_LESSONS_UNSET))
    {
        rec->lessons_done = (uint8_t)tenth; /* (no name: it learns as before, and its lessons start where it is named) */
        return;
    }
    if ((rec->lesson_learn != 0) || (rec->lessons_done >= tenth))
        return;
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
    const int at = MD_LESSON_STEP * (rec->lessons_done + 1);
    int learn = -1, master = -1;
    for (int i = 0; i < LEARNED_INSTANCES_COUNT; i++)
    {
        const int k = crconf->learned_instance_id[i];
        if (k <= 0)
            continue;
        const int natural = crconf->learned_instance_level[i];
        if (cctrl->instance_available[k])
        {
            if ((rec->mastery[i] < MD_MASTERY_MAX) && ((master < 0) || (rec->mastery[i] <= rec->mastery[master])))
                master = i;
        } else if ((natural >= MD_LESSON_STEP) && (natural + MD_LESSON_STEP * rec->lesson_delay[i] <= at) && (learn < 0))
            learn = i;
    }
    if (learn < 0)
    {
        rec->lessons_done++;
        return;
    }
    rec->lesson_learn = (uint8_t)(learn + 1);
    rec->lesson_master = (uint8_t)(master + 1);
    SYNCLOG("Misdeeds: Lessons: %s index %d (%s) at level %d: the keeper picks learning %s or mastering %s",
        thing_model_name(thing), (int)thing->index, creature_own_name(thing), level,
        creature_instance_code_name(crconf->learned_instance_id[learn]),
        (master >= 0) ? creature_instance_code_name(crconf->learned_instance_id[master]) : "nothing");
}

/** Lessons: the keeper's choice (the pick packet with choice 20 learn, 21 master). */
static void md_lesson_decide(struct MdRec *rec, struct Thing *thing, int master)
{
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    const int learn = rec->lesson_learn - 1, slot = rec->lesson_master - 1;
    if ((learn < 0) || (master && (slot < 0)))
    {
        SYNCLOG("Misdeeds: Lessons: choice %d for thing %d refused: nothing offered", master, (int)thing->index);
        return;
    }
    if (master)
    {
        rec->mastery[slot]++;
        for (int i = 0; i < LEARNED_INSTANCES_COUNT; i++)
        {
            const int k = crconf->learned_instance_id[i];
            if ((k > 0) && !cctrl->instance_available[k] && (crconf->learned_instance_level[i] >= MD_LESSON_STEP) &&
                (rec->lesson_delay[i] < 255))
                rec->lesson_delay[i]++;
        }
    }
    rec->lessons_done++;
    rec->lesson_learn = 0;
    rec->lesson_master = 0;
    creature_increase_available_instances(thing);
    SYNCLOG("Misdeeds: Lessons: %s index %d (%s) %s %s (lessons decided %d)", thing_model_name(thing), (int)thing->index,
        creature_own_name(thing), master ? "mastered" : "learnt",
        creature_instance_code_name(crconf->learned_instance_id[master ? slot : learn]), (int)rec->lessons_done);
}

TbBool world_misdeeds_reset_time(const struct Thing *thing, CrInstance inst, int32_t *ritime)
{
    if ((md_top == 0) || !world_mode_active())
        return false;
    const struct MdRec *rec = md_peek(thing);
    if (rec == NULL)
        return false;
    TbBool changed = false;
    if (md_party_gift(thing) == MdG_Sooner) /* Marching Order: a Warlock's standard */
    {
        *ritime = *ritime * 100 / (100 + MD_GIFT_SOONER);
        changed = true;
    }
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
    for (int i = 0; i < LEARNED_INSTANCES_COUNT; i++)
        if ((crconf->learned_instance_id[i] == inst) && (rec->mastery[i] > 0))
        {
            *ritime = *ritime * 100 / (100 + MD_MASTERY_SPEED * rec->mastery[i]);
            return true;
        }
    return changed;
}

/** The shot an ability fires: directly, or through the spell it casts (0: none). */
static ThingModel md_instance_shot(CrInstance inst)
{
    const ThingModel direct = get_instance_shot_model(inst);
    if (direct != 0)
        return direct;
    const struct InstanceInfo *inst_inf = creature_instance_info_get(inst);
    if ((inst_inf == NULL) || !instance_action_starts_cooldown(inst) || (inst_inf->func_params[0] <= 0))
        return 0;
    const struct SpellConfig *spconf = get_spell_config((SpellKind)inst_inf->func_params[0]);
    return (spconf != NULL) ? spconf->shot_model : 0;
}

/** Breeds: a fire shot: one with the FIRE property, or one of the stock fire shots (the stock config marks none). */
static TbBool md_shot_is_fire(ThingModel model, const struct ShotConfigStats *shotst)
{
    if (flag_is_set(shotst->model_flags, ShMF_Fire))
        return true;
    /* the stock fire shots, looked up once a game (audit 2026-11-13: string scans on every hit); a new game starts
       from a low turn and looks them up again */
    static ThingModel fire_ids[3];
    static GameTurn looked_at = 0;
    static TbBool looked = false;
    if (!looked || (game.play_gameturn < looked_at))
    {
        static const char *const fire[3] = {"SHOT_FIREBALL", "SHOT_FIREBOMB", "SHOT_FLAME_BREATH"};
        for (int i = 0; i < 3; i++)
        {
            const long id = get_id(shot_desc, fire[i]);
            fire_ids[i] = (ThingModel)((id > 0) ? id : 0);
        }
        looked = true;
    }
    looked_at = game.play_gameturn;
    for (int i = 0; i < 3; i++)
        if ((fire_ids[i] > 0) && (model == fire_ids[i]))
            return true;
    return false;
}

HitPoints world_misdeeds_shot_damage(const struct Thing *shotng, const struct Thing *shooter, const struct Thing *target,
    HitPoints dmg)
{
    if ((md_top != 0) && (dmg > 0) && world_mode_active() && thing_exists(target) && (target->class_id == TCls_Creature))
    {
        /* Breeds: a Volcanic Beetle barely feels fire */
        const struct MdRec *trec = md_peek(target);
        const struct ShotConfigStats *shotst = get_shot_model_stats(shotng->model);
        if ((trec != NULL) && (trec->breed == MdB_FireBeetle) && md_shot_is_fire(shotng->model, shotst))
            dmg = max(dmg * MD_BEETLE_FIRE / 100, (HitPoints)1);
    }
    if ((md_top == 0) || (dmg <= 0) || !world_mode_active() || !thing_exists(shooter) || (shooter->class_id != TCls_Creature))
        return dmg;
    const struct MdRec *rec = md_peek(shooter);
    if (rec == NULL)
        return dmg;
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(shooter);
    for (int i = 0; i < LEARNED_INSTANCES_COUNT; i++)
        if ((rec->mastery[i] > 0) && (crconf->learned_instance_id[i] > 0) &&
            (md_instance_shot(crconf->learned_instance_id[i]) == shotng->model))
        {
            const HitPoints ndmg = dmg * (100 + MD_MASTERY_DAMAGE * rec->mastery[i]) / 100;
            if (md_log_budget > 0)
            {
                md_log_budget--;
                SYNCLOG("Misdeeds: Lessons: %s index %d, %s mastered %d: a shot hits for %d instead of %d", thing_model_name(shooter),
                    (int)shooter->index, creature_instance_code_name(crconf->learned_instance_id[i]), (int)rec->mastery[i],
                    (int)ndmg, (int)dmg);
            }
            return ndmg;
        }
    return dmg;
}

/******************************************************************************/
/* Counting */

/** Deadly Pairs: the two marked vices melt into their sin. The other vices keep their order and ranks. */
static void md_melt(struct MdRec *rec, struct Thing *thing)
{
    int slot[2], n = 0;
    for (int i = 0; (i < MD_VICE_SLOTS) && (n < 2); i++)
        if ((rec->rack_marks & (1 << i)) && (rec->vices[i] != MdV_None))
            slot[n++] = i;
    const int sin = (n == 2) ? md_sin_of(rec->vices[slot[0]], rec->vices[slot[1]]) : 0;
    const unsigned samples = rec->rack_samples;
    rec->rack_marks = 0;
    rec->rack_samples = 0;
    if (sin == 0)
        return;
    int k = 0;
    for (int i = 0; i < MD_VICE_SLOTS; i++)
    {
        if ((i == slot[0]) || (i == slot[1]) || (rec->vices[i] == MdV_None))
            continue;
        rec->vices[k] = rec->vices[i];
        rec->rank_base[k] = rec->rank_base[i];
        rec->rank[k] = rec->rank[i];
        k++;
    }
    for (; k < MD_VICE_SLOTS; k++)
    {
        rec->vices[k] = MdV_None;
        rec->rank_base[k] = 0;
        rec->rank[k] = 0;
    }
    rec->sin = (uint8_t)sin;
    rec->flags |= MdF_SinUsed;
    /* off the rack at once (stock torture would go on until it dies), and without the anger the rack added */
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
    if (crconf->annoy_in_torture > 0) /* (a kind that likes it keeps what it got) */
        anger_apply_anger_to_creature(thing, -(long)samples * MD_SAMPLE * crconf->annoy_in_torture, AngR_Other, 1);
    if (creature_is_being_tortured_including_kinky(thing))
        set_start_state(thing);
    SYNCLOG("Misdeeds: Deadly Pairs: %s index %d of player %d melted two vices on the rack into sin %d; %d vices left",
        thing_model_name(thing), (int)thing->index, (int)thing->owner, sin, md_vice_count(rec));
    char who[48];
    md_tell_sin(thing->owner, sin, md_called(thing, who, sizeof(who)));
}

void world_misdeeds_creature_tick(struct Thing *thing)
{
    if (((game.play_gameturn + thing->index) & (MD_SAMPLE - 1)) != 0)
        return;
    if (!world_mode_active() || !world_player_is_human(thing->owner))
        return;
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (creature_control_invalid(cctrl) || (thing->health < 0))
        return;
    struct MdRec *rec = md_get(thing, cctrl);
    if (rec == NULL)
        return;
    rec->flags &= ~MdF_Frozen;
    if (rec->stray_by != 0)
        rec->stray_by = 0; /* Strays: a fed wild creature that became a keeper's some other way (its Prison) is no stray */
    if (world_caretaker_active(thing->owner))
    {
        /* its keeper is away: the caretaker keeps the dungeon, and its creatures' misdeeds wait for the keeper (no
           counting, no ranks, no offers); no distance is counted across the time away */
        rec->flags &= ~(MdF_HasLast | MdF_InFight);
        rec->last_kill_day = (uint16_t)world_day_number(); /* Contracts: no fury for the days away (audit 2026-11-13) */
        return;
    }
    if (thing_is_picked_up(thing))
    {
        rec->rack_samples = 0;
        return;
    }
    /* lava and water: MD_STAND_DAY samples of each count in a day */
    if (rec->stand_day != (uint16_t)world_day_number())
    {
        rec->stand_day = (uint16_t)world_day_number();
        rec->stand_lava = 0;
        rec->stand_water = 0;
    }
    if (flag_is_set(thing->movement_flags, TMvF_IsOnLava))
    {
        if (rec->stand_lava < MD_STAND_DAY)
        {
            rec->stand_lava++;
            rec->c[MdC_Lava]++;
        }
        /* Asbestos Hide from rank II: the lava heals it, a percent of its health per rank above I each sample */
        const int arank = md_vice_rank(rec, MdV_Asbestos);
        if ((arank >= 2) && (thing->health < cctrl->max_health) && !creature_is_being_unconscious(thing))
            thing->health = min(thing->health + max(cctrl->max_health * (arank - 1) / 100, (HitPoints)1), cctrl->max_health);
    }
    if (flag_is_set(thing->movement_flags, TMvF_IsOnWater) && (rec->stand_water < MD_STAND_DAY))
    {
        rec->stand_water++;
        rec->c[MdC_Water]++;
    }
    /* work */
    switch (get_creature_state_besides_interruptions(thing))
    {
    case CrSt_Training:
    case CrSt_Researching:
    case CrSt_Manufacturing:
    case CrSt_Guarding:
    case CrSt_AtGuardPostRoom:
    case CrSt_ImpDigsDirt:
    case CrSt_ImpMinesGold:
    case CrSt_ImpImprovesDungeon:
    case CrSt_ImpConvertsDungeon:
    case CrSt_ImpReinforces:
        rec->c[MdC_Work]++;
        break;
    default:
        break;
    }
    /* fights and falls */
    const TbBool night = !world_is_day();
    const TbBool down = creature_is_being_unconscious(thing);
    const TbBool fight = (cctrl->combat_flags != 0);
    const TbBool own_land = md_on_own_land(thing);
    /* (put down by the Hand since the last sample) */
    const TbBool dropped = (cctrl->dropped_turn != 0) && (cctrl->dropped_turn + MD_SAMPLE >= game.play_gameturn);
    if (fight)
    {
        if (!flag_is_set(rec->flags, MdF_InFight))
        {
            /* a fight begins: it counts when the last one that counted began MD_FIGHT_GAP or more ago */
            rec->fight_counts = (rec->fight_turn == 0) || ((uint32_t)game.play_gameturn - rec->fight_turn >= MD_FIGHT_GAP);
            if (rec->fight_counts)
            {
                rec->fight_turn = (uint32_t)game.play_gameturn;
                if (night)
                    rec->c[MdC_NightFights]++;
            }
        }
        if (own_land)
            rec->c[MdC_HomeFights]++;
        rec->flags |= MdF_InFight;
    } else
    {
        /* out of a fight standing: not when its keeper's Hand took it out of the fight */
        if (flag_is_set(rec->flags, MdF_InFight) && !down && !dropped && rec->fight_counts)
            rec->c[MdC_Survived]++;
        rec->fight_counts = 0;
        rec->flags &= ~MdF_InFight;
    }
    if (down && !flag_is_set(rec->flags, MdF_Down))
        rec->c[MdC_Falls]++;
    if (!down && flag_is_set(rec->flags, MdF_Down) && (md_vice_rank(rec, MdV_Angry) > 0))
    {
        rec->angry_until = (uint32_t)game.play_gameturn + MD_ANGRY_TURNS;
        SYNCLOG("Misdeeds: Comes Back Angry: %s index %d of player %d gets up angry", thing_model_name(thing), (int)thing->index,
            (int)thing->owner);
    }
    if (down)
        rec->flags |= MdF_Down;
    else
        rec->flags &= ~MdF_Down;
    /* distance */
    const long home = md_home_distance(thing);
    int64_t wx = 0, wy = 0;
    world_slab_to_world(coord_slab(thing->mappos.x.val), coord_slab(thing->mappos.y.val), &wx, &wy);
    if (home >= 0)
    {
        /* only ground it walked: not a drop from the hand since the last sample, nor a jump no walk can make (a
           creature at its top speed does about 11 slabs a sample; a Teleport goes farther) */
        const TbBool has_last = flag_is_set(rec->flags, MdF_HasLast);
        const long moved = has_last ? labs((long)(wx - rec->last_wx)) + labs((long)(wy - rec->last_wy)) : 0;
        /* The farthest it has been from home, on its own feet: counted from where it was put down (the Hand, a jump,
           or where it was first seen), or from nearer home if it has walked nearer since. Audit 2026-11-13: the
           distance alone was counted, so one drop from the Hand at a far outpost gave Long Way From Home its top
           rank for good. */
        const TbBool first = !has_last && (rec->c[MdC_FarDist] == 0) && (rec->far_base == 0);
        if (dropped || (moved > MD_WALK_SLABS) || first)
            rec->far_base = (uint16_t)min(home, 65535L);
        else if (home < (long)rec->far_base)
            rec->far_base = (uint16_t)home;
        if (home - (long)rec->far_base > (long)rec->c[MdC_FarDist])
            rec->c[MdC_FarDist] = (uint32_t)(home - (long)rec->far_base);
        if ((home >= MD_AWAY_SLABS) && has_last && !dropped && (moved <= MD_WALK_SLABS))
            rec->c[MdC_AwayDist] += (uint32_t)moved;
    }
    rec->last_wx = (int32_t)wx;
    rec->last_wy = (int32_t)wy;
    rec->flags |= MdF_HasLast;
    /* Deadly Pairs: two marked vices, on the rack of its own keeper's Torture Chamber, melt in time */
    rec->rack_marks &= md_filled_slots(rec); /* (a mark on a slot that has since emptied) */
    if ((rec->sin == 0) && !flag_is_set(rec->flags, MdF_SinUsed) && (md_bits(rec->rack_marks) == 2) &&
        creature_is_being_tortured_including_kinky(thing))
    {
        const struct Room *rack = get_room_thing_is_on(thing);
        if (!room_is_invalid(rack) && (rack->owner == thing->owner))
        {
            rec->rack_samples++;
            if ((uint32_t)rec->rack_samples * MD_SAMPLE >= MD_RACK_TURNS)
                md_melt(rec, thing);
        } else
            rec->rack_samples = 0;
    } else
        rec->rack_samples = 0;
    /* Virtues: the habits, and a virtue for a creature whose record shows one */
    if (creature_is_fleeing_combat(thing) && (rec->flights < 65535))
        rec->flights++;
    if ((get_creature_state_besides_interruptions(thing) == CrSt_CreatureSleep) && world_raid_on(thing->owner) &&
        (rec->slept_raids < 65535))
        rec->slept_raids++;
    if (cctrl->exp_level + 1 >= md_vice_levels[0])
    {
        const uint32_t kills = rec->c[MdC_KillsHero] + rec->c[MdC_KillsWild] + rec->c[MdC_KillsKeeper];
        const uint8_t earned = (rec->virtues | (rec->virtues >> 4)) & MdT_ALL;
        uint8_t gets = 0;
        if (!(earned & MdT_Prudence) && (rec->flights >= MD_PRUDENCE_FLIGHTS))
            gets = MdT_Prudence;
        else if (!(earned & MdT_Temperance) && (rec->slept_raids >= MD_TEMPERANCE_SLEPT))
            gets = MdT_Temperance;
        else if (!(earned & MdT_Mercy) && (rec->knockouts >= MD_MERCY_KNOCKOUTS) && (kills * 4 <= rec->knockouts))
            gets = MdT_Mercy;
        if (gets != 0)
        {
            rec->virtues |= gets;
            const int v = (gets == MdT_Prudence) ? 0 : (gets == MdT_Temperance) ? 1 : 2;
            SYNCLOG("Misdeeds: Virtues: %s index %d (%s) of player %d earned %s (flights %u, slept through raids %u, knockouts %u, "
                "kills %u)", thing_model_name(thing), (int)thing->index, creature_own_name(thing), (int)thing->owner,
                md_virtue_names[v], (unsigned)rec->flights, (unsigned)rec->slept_raids, (unsigned)rec->knockouts, (unsigned)kills);
            char text[160], who[48];
            snprintf(text, sizeof(text), "%s has earned a virtue: %s. It %s. Your Torture Chamber can correct that.",
                md_called(thing, who, sizeof(who)), md_virtue_names[v], md_virtue_does[v]);
            md_tell(thing->owner, text);
        }
    }
    /* sin 6's lasting part */
    if ((rec->sin == 6) && own_land && (thing->health < cctrl->max_health))
        thing->health = min(thing->health + max(cctrl->max_health * 3 / 100, (HitPoints)1), cctrl->max_health);
    /* vices */
    const int level = cctrl->exp_level + 1;
    md_check_offer(rec, thing, level);
    md_lesson_check(rec, thing, cctrl, level);
    /* Seeing Through Their Eyes: a possessed Dark Mistress lends her Lightning to her party; an Imp carries a book home */
    md_lend_lightning(thing, cctrl, rec);
    md_imp_book(thing, cctrl, rec);
    if (rec->breed != MdB_None)
        md_breed_apply(thing, cctrl, rec); /* (after the lending: giving Lightning back recounts its abilities) */
    /* Seeing Through Their Eyes: a possessed Fly sees other keepers' hidden traps near it */
    if (md_driven_by(thing) && (thing->model == md_kind("FLY")))
    {
        int found = 0;
        const struct StructureList *slist = get_list_for_thing_class(TCls_Trap);
        long k = slist->index;
        for (int n = 0; (k > 0) && (n < THINGS_COUNT); n++)
        {
            struct Thing *trap = thing_get(k);
            if (thing_is_invalid(trap))
                break;
            k = trap->next_of_class;
            if ((trap->owner == thing->owner) || trap->trap.revealed ||
                (abs(coord_slab(trap->mappos.x.val) - coord_slab(thing->mappos.x.val)) > MD_EYES_TRAP_SLABS) ||
                (abs(coord_slab(trap->mappos.y.val) - coord_slab(thing->mappos.y.val)) > MD_EYES_TRAP_SLABS))
                continue;
            trap->trap.revealed = 1;
            found++;
        }
        if (found > 0)
        {
            SYNCLOG("Misdeeds: Eyes: a possessed Fly of player %d spots %d hidden traps", (int)thing->owner, found);
            char text[96];
            snprintf(text, sizeof(text), "Your Fly spots %d hidden trap%s.", found, (found > 1) ? "s" : "");
            md_tell(thing->owner, text);
        }
    }
    if ((rec->foreign_inst > 0) && (rec->foreign_inst < INSTANCE_TYPES_MAX) && !cctrl->instance_available[rec->foreign_inst])
        cctrl->instance_available[rec->foreign_inst] = true; /* Past Lives: what it remembers stays */
    /* Regimens: levels earned in the Training Room count for its regimen */
    if ((rec->reg_last_level != 0) && (level > rec->reg_last_level) && (rec->regimen > MdR_None) && (rec->regimen < MdR_COUNT) &&
        (get_creature_state_besides_interruptions(thing) == CrSt_Training))
    {
        const int r = rec->regimen - 1;
        for (int n = min(level - rec->reg_last_level, 2 * MD_REG_LEVELS); n > 0; n--)
            md_regimen_level(rec, r);
        const HitPoints before = cctrl->max_health;
        cctrl->max_health = calculate_correct_creature_max_health(thing);
        if (thing->health > cctrl->max_health)
            thing->health = cctrl->max_health;
        SYNCLOG("Misdeeds: Regimens: %s index %d of player %d earned level %d on %s (%d levels on it): Str %+d%%, Def %+d%%, "
            "Dex %+d%%, HP %+d%% (max health %d -> %d)", thing_model_name(thing), (int)thing->index, (int)thing->owner, level,
            md_regimen_names[rec->regimen], (int)rec->reg_levels[r], md_regimen_pct(rec, MdR_Brawn) - 100,
            md_regimen_pct(rec, MdR_Guard) - 100, md_regimen_pct(rec, MdR_Aim) - 100, md_regimen_pct(rec, MdR_Grit) - 100,
            (int)before, (int)cctrl->max_health);
    }
    /* Contracts: a renegotiation closes at its next level; a tenth level opens one */
    if (rec->contract_open && (level > MD_LESSON_STEP * rec->contract_done))
        rec->contract_open = 0;
    if ((rec->contract_done < level / MD_LESSON_STEP) && (rec->reg_last_level != 0))
    {
        rec->contract_done = (uint8_t)min(level / MD_LESSON_STEP, 255);
        if (flag_is_set(rec->flags, MdF_Renamed))
        {
            rec->contract_open = 1;
            SYNCLOG("Misdeeds: Contracts: %s index %d (%s) of player %d renegotiates at level %d (now %s)", thing_model_name(thing),
                (int)thing->index, creature_own_name(thing), (int)thing->owner, level, md_contract_names[rec->contract]);
            char text[160];
            char who[48];
            snprintf(text, sizeof(text), "%s wants to renegotiate its pay: see its Rap Sheet.", md_called(thing, who, sizeof(who)));
            md_tell(thing->owner, text);
        } else if (rec->contract != md_contract_default(thing->model))
        {
            md_contract_set(thing, rec, md_contract_default(thing->model));
            SYNCLOG("Misdeeds: Contracts: %s index %d of player %d takes its kind's contract, %s", thing_model_name(thing),
                (int)thing->index, (int)thing->owner, md_contract_names[rec->contract]);
        }
    } else if (rec->reg_last_level == 0)
        rec->contract_done = (uint8_t)min(level / MD_LESSON_STEP, 255); /* (first seen: renegotiations start from here) */
    const long today = world_day_number();
    /* Glory and Plunder: a whole day without a kill is a day unpaid (audit 2026-11-13: Plunder made a creature that
       never fights free for good) */
    if ((rec->contract_day != 0) && (today > rec->contract_day) && ((rec->contract == MdK_Glory) || (rec->contract == MdK_Plunder)) &&
        ((long)rec->last_kill_day < today - 1))
    {
        anger_make_creature_angry(thing, AngR_NotPaid);
        SYNCLOG("Misdeeds: Contracts: %s: %s index %d of player %d had no kill on day %ld and is %s",
            md_contract_names[rec->contract], thing_model_name(thing), (int)thing->index, (int)thing->owner, today - 1,
            (rec->contract == MdK_Glory) ? "furious" : "unpaid");
    }
    rec->contract_day = (uint16_t)today;
    rec->reg_last_level = (uint8_t)min(level, 255);
    /* Bed and Board: a better rest in its lair (a percent of its health more each sample asleep); its lair is bigger:
       drawn half again as big (world_misdeeds_lair_size) and taking twice its kind's room (world_misdeeds_lair_places) */
    if ((rec->contract == MdK_Board) && (get_creature_state_besides_interruptions(thing) == CrSt_CreatureSleep) &&
        (thing->health < cctrl->max_health))
        thing->health = min(thing->health + max(cctrl->max_health / 100, (HitPoints)1), cctrl->max_health);
    /* Deeper Vices: a vice whose misdeed went on being counted rises a rank */
    for (int i = 0; i < MD_VICE_SLOTS; i++)
    {
        const int r = md_slot_rank(rec, i);
        if ((rec->rank[i] != 0) && (r > rec->rank[i]))
        {
            const int vice = rec->vices[i];
            SYNCLOG("Misdeeds: Deeper Vices: %s index %d (%s) of player %d: %s rose to rank %s (%u deeds since it was taken)",
                thing_model_name(thing), (int)thing->index, creature_own_name(thing), (int)thing->owner, md_vice_names[vice],
                md_roman[r], (unsigned)(md_deeds(rec, vice) - rec->rank_base[i]));
            char text[160], who[48];
            snprintf(text, sizeof(text), "%s's vice %s rose to rank %s.", md_called(thing, who, sizeof(who)), md_vice_names[vice],
                md_roman[r]);
            md_tell(thing->owner, text);
        }
        rec->rank[i] = (uint8_t)r;
    }
    const TbBool away = (home >= MD_AWAY_SLABS);
    const int terr_rank = md_vice_rank(rec, MdV_Territorial);
    if ((terr_rank > 0) && own_land)
        md_near_add(md_terr, thing->index, MD_TERR_REACH(terr_rank), terr_rank);
    const int short_rank = md_vice_rank(rec, MdV_Shortcut);
    if ((short_rank > 0) && away)
        md_near_add(md_short, thing->index, MD_SHORT_REACH(short_rank), short_rank);
    /* the speed bits: night and shortcut, and the ranks they come at (so a rank change recalculates the speed) */
    uint8_t bits = 0;
    const int night_rank = md_vice_rank(rec, MdV_Night);
    if (night && (night_rank > 0))
        bits |= MdS_Night | (uint8_t)(night_rank << 5);
    int group_rank = 0;
    if (away && !thing_is_invalid(md_near_find(md_short, thing, true, &group_rank)))
        bits |= MdS_Shortcut | (uint8_t)(group_rank << 2);
    /* Marching Order: a standard-bearer's gift for its party; the post's speed */
    const int post = md_post_now(thing, rec);
    if (post == MdP_Standard)
    {
        struct Thing *leader = get_group_leader(thing);
        const int gift = (thing->model == md_kind("ORC")) ? MdG_Harder : (thing->model == md_kind("SORCEROR")) ? MdG_Sooner :
            (thing->model == md_kind("DARK_MISTRESS")) ? MdG_Wounded : MdG_None;
        if (!thing_is_invalid(leader) && (gift != MdG_None))
            md_standard_add(leader, gift);
    }
    int march_pct = (post == MdP_Flank) ? MD_FLANK_SPEED : 0;
    if ((thing->health < cctrl->max_health / 2) && (md_party_gift(thing) == MdG_Wounded))
        march_pct += MD_GIFT_WOUNDED;
    /* the sin's speed */
    int sin_pct = 0;
    if (rec->sin == 4)
        sin_pct = away ? 15 : 0;
    else if (rec->sin == 7)
        sin_pct = own_land ? 40 : 0;
    else if (rec->sin == 9)
        sin_pct = (night && away) ? 45 : 0;
    else if (rec->sin == 10)
        sin_pct = flag_is_set(thing->movement_flags, TMvF_IsOnLava) ? 60 : away ? 20 : 0;
    if ((bits != rec->speed_bits) || (sin_pct != rec->sin_speed) || (march_pct != rec->march_speed))
    {
        const long before = cctrl->max_speed;
        rec->speed_bits = bits;
        rec->sin_speed = (uint8_t)sin_pct;
        rec->march_speed = (uint8_t)march_pct;
        cctrl->max_speed = calculate_correct_creature_maxspeed(thing);
        if (md_log_budget > 0)
        {
            md_log_budget--;
            SYNCLOG("Misdeeds: %s index %d speed %ld -> %ld (%s%s, sin %d%%)", thing_model_name(thing), (int)thing->index,
                before, (long)cctrl->max_speed, (bits & MdS_Night) ? "night " : "", (bits & MdS_Shortcut) ? "shortcut" : "", sin_pct);
        }
    }
    /* Bad Influence: a creature of its kind, far above it, training in the same room */
    if (get_creature_state_besides_interruptions(thing) == CrSt_Training)
    {
        const struct Room *room = get_room_thing_is_on(thing);
        if (!room_is_invalid(room))
        {
            ThingIndex i = room->creatures_list;
            for (int k = 0; (i != 0) && (k < 64); k++)
            {
                const struct Thing *other = thing_get(i);
                const struct CreatureControl *octrl = creature_control_get_from_thing(other);
                if (!thing_exists(other) || creature_control_invalid(octrl))
                    break;
                if ((other->model == thing->model) && (other->owner == thing->owner) &&
                    (octrl->exp_level >= cctrl->exp_level + MD_INFLUENCE_LEVELS) &&
                    (get_creature_state_besides_interruptions(other) == CrSt_Training))
                {
                    if (rec->influence_turn + 4 * MD_SAMPLE < (uint32_t)game.play_gameturn)
                        SYNCLOG("Misdeeds: Bad Influence: %s index %d (level %d) learns from index %d (level %d)",
                            thing_model_name(thing), (int)thing->index, level, (int)other->index, octrl->exp_level + 1);
                    rec->influence_turn = (uint32_t)game.play_gameturn;
                    break;
                }
                i = octrl->next_in_room;
            }
        }
    }
}

/** Virtues: corrected on the rack, it does no work until its rest day is over. */
static TbBool md_resting(const struct MdRec *rec)
{
    return (rec->rest_day != 0) && (world_day_number() <= (long)rec->rest_day);
}

TbBool world_misdeeds_resting(const struct Thing *thing)
{
    if ((md_top == 0) || !world_mode_active())
        return false;
    const struct MdRec *rec = md_peek(thing);
    return (rec != NULL) && md_resting(rec);
}

int world_misdeeds_train_bonus(const struct Thing *thing)
{
    if (!world_mode_active())
        return 0;
    const struct MdRec *rec = md_peek(thing);
    if (rec == NULL)
        return 0;
    if (md_resting(rec))
        return -100;
    int bonus = 0;
    if (rec->virtues & MdT_Temperance)
        bonus -= MD_TEMPERANCE_TRAIN;
    if ((rec->influence_turn != 0) && (rec->influence_turn + 2 * MD_SAMPLE >= (uint32_t)game.play_gameturn))
        bonus += MD_INFLUENCE_PERCENT;
    return bonus;
}

int world_misdeeds_work_percent(const struct Thing *thing)
{
    if ((md_top == 0) || !world_mode_active())
        return 100;
    const struct MdRec *rec = md_peek(thing);
    if (rec == NULL)
        return 100;
    if (md_resting(rec))
        return 0;
    const int r = md_vice_rank(rec, MdV_Workaholic);
    return (r > 0) ? 100 + md_ranked(MD_WORKAHOLIC_PERCENT, r) : 100;
}

long world_misdeeds_fear(const struct Thing *thing, long fear)
{
    if ((md_top == 0) || !world_mode_active())
        return fear;
    const struct MdRec *rec = md_peek(thing);
    /* (a creature that fights to the death, fear 0, stays so: audit 2026-11-13) */
    return ((rec != NULL) && (rec->virtues & MdT_Prudence) && (fear > 0)) ? fear + MD_PRUDENCE_FEAR : fear;
}

void world_misdeeds_knocked_out(struct Thing *victim, struct Thing *killertng)
{
    if ((md_top == 0) || !world_mode_active() || !thing_exists(killertng) || (killertng->class_id != TCls_Creature) ||
        (killertng->owner == victim->owner) || !world_player_is_human(killertng->owner))
        return;
    struct CreatureControl *kctrl = creature_control_get_from_thing(killertng);
    struct MdRec *rec = creature_control_invalid(kctrl) ? NULL : md_get(killertng, kctrl);
    if ((rec != NULL) && (rec->knockouts < 65535))
        rec->knockouts++;
}

void world_misdeeds_power_on(PlayerNumber plyr_idx, PowerKind pwkind, struct Thing *thing)
{
    if ((md_top == 0) || !world_mode_active() || !thing_exists(thing) || (thing->class_id != TCls_Creature) ||
        (thing->owner != plyr_idx))
        return;
    /* a spell cures (the page): not the Hand, a slap or a pick-up, which go through the same function; and only the
       keeper's own cast, not the caretaker's or a computer keeper's (audit 2026-11-13) */
    if ((pwkind == PwrK_HAND) || (pwkind == PwrK_SLAP) || (pwkind == PwrK_PICKUPCRTR) || (pwkind == PwrK_PICKUPGOLD) ||
        (pwkind == PwrK_PICKUPFOOD) || !world_player_is_human(plyr_idx) || world_caretaker_active(plyr_idx))
        return;
    struct MdRec *rec = md_peek(thing);
    if ((rec == NULL) || ((rec->virtues & MdT_ALL) == 0) || !creature_is_being_tortured_including_kinky(thing))
        return;
    const struct Room *rack = get_room_thing_is_on(thing);
    if (room_is_invalid(rack) || (rack->owner != plyr_idx))
        return;
    int v = 0;
    while (!(rec->virtues & (1 << v)))
        v++;
    rec->virtues = (uint8_t)((rec->virtues & ~(1 << v)) | (1 << (v + 4)));
    rec->rest_day = (uint16_t)(world_day_number() + 1);
    SYNCLOG("Misdeeds: Virtues: %s index %d of player %d: %s corrected on the rack by power %d; no work until day %d is over",
        thing_model_name(thing), (int)thing->index, (int)plyr_idx, md_virtue_names[v], (int)pwkind, (int)rec->rest_day);
    char text[160], who[48];
    snprintf(text, sizeof(text), "%s is cured of %s on the rack. It won't work until day %d is over.",
        md_called(thing, who, sizeof(who)), md_virtue_names[v], (int)rec->rest_day);
    md_tell(plyr_idx, text);
}

/* A kill is booked when its victim really dies (world_misdeeds_dying): a knocked-out hero is not a kill. */
static struct { ThingIndex victim, killer; GameTurn turn; } md_kill_pending;

void world_misdeeds_on_kill(struct Thing *victim, struct Thing *killertng)
{
    if (!world_mode_active())
        return;
    md_kill_pending.victim = victim->index;
    md_kill_pending.killer = thing_exists(killertng) ? killertng->index : 0;
    md_kill_pending.turn = game.play_gameturn;
}

/* A sin's reveal for a joined player: the server sends only its number; the client decodes the texts itself. */
#define MD_NOTE_MARK "\x01MS"

static void md_show_sin(int sin, const char *who)
{
    char name[MD_SIN_TEXT_MAX], line[MD_SIN_TEXT_MAX], text[96];
    snprintf(text, sizeof(text), "%s's vices melted into a sin on the rack:", who);
    message_add(MsgType_Blank, 0, text);
    snprintf(text, sizeof(text), "%s: %s.", md_sin_text(0, sin, name), md_sin_text(1, sin, line));
    message_add(MsgType_Blank, 0, text);
}

static void md_tell_sin(PlayerNumber plyr_idx, int sin, const char *who)
{
    if (is_my_player_number(plyr_idx) && !netstate_dedicated()) /* (a dedicated server's own number is a joined player's) */
    {
        md_show_sin(sin, who);
        return;
    }
    if (!netstate_is_server() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT))
        return;
    const NetUserId u = get_player_primary_user(get_player(plyr_idx));
    if ((u <= SERVER_ID) || (u >= MAX_NET_USERS))
        return;
    char text[48];
    snprintf(text, sizeof(text), MD_NOTE_MARK "%d %s", sin, who);
    server_admin_notice(u, text);
}

TbBool world_misdeeds_net_notice(const char *text)
{
    if (md_jointest_client && (text != NULL))
        SYNCLOG("Join test (client): a notice from the server: \"%s\"", (strncmp(text, MD_NOTE_MARK, 3) == 0) ? "(coded)" : text);
    if ((text == NULL) || (strncmp(text, MD_NOTE_MARK, 3) != 0))
        return false;
    const int sin = atoi(text + 3);
    const char *who = strchr(text + 3, ' ');
    if ((sin >= 1) && (sin <= MD_SINS))
        md_show_sin(sin, (who != NULL) ? who + 1 : "a creature");
    return true;
}

/** The victim of the last kill really dies: the killer's Misdeeds count it. */
static void md_book_kill(struct Thing *victim)
{
    if ((md_kill_pending.victim != victim->index) || (md_kill_pending.turn != game.play_gameturn))
        return;
    struct Thing *killertng = thing_get(md_kill_pending.killer);
    md_kill_pending.victim = 0;
    if (thing_exists(killertng) && (killertng->class_id == TCls_Creature) && world_player_is_human(killertng->owner) &&
        (killertng->owner != victim->owner) && !world_caretaker_active(killertng->owner))
    {
        struct CreatureControl *kctrl = creature_control_get_from_thing(killertng);
        struct MdRec *rec = creature_control_invalid(kctrl) ? NULL : md_get(killertng, kctrl);
        if (rec != NULL)
        {
            const int what = world_wild_creature(victim) ? MdC_KillsWild : (victim->owner == PLAYER_GOOD) ? MdC_KillsHero :
                is_neutral_thing(victim) ? MdC_KillsWild : MdC_KillsKeeper;
            rec->c[what]++;
            if (md_driven_by(killertng) && (rec->driven < 65535))
                rec->driven++; /* Seeing Through Their Eyes: Driven */
            rec->last_kill_day = (uint16_t)world_day_number();
            if (rec->contract == MdK_Plunder)
            {
                const GoldAmount wage = calculate_correct_creature_pay_base(killertng);
                const GoldAmount bounty = max(wage * MD_PLUNDER_SHARE / 100, (GoldAmount)1);
                const TbBool paid = (take_money_from_dungeon(killertng->owner, bounty, 1) >= 0);
                if (paid)
                    create_price_effect(&killertng->mappos, killertng->owner, bounty);
                else
                    anger_make_creature_angry(killertng, AngR_NotPaid);
                SYNCLOG("Misdeeds: Contracts: Plunder: %s index %d of player %d %s a bounty of %ld", thing_model_name(killertng),
                    (int)killertng->index, (int)killertng->owner, paid ? "is paid" : "is NOT paid", (long)bounty);
            }
            if ((rec->sin == 1) && (what == MdC_KillsHero) && !world_is_day() && (killertng->health >= 0) &&
                (killertng->health < kctrl->max_health))
            {
                const HitPoints heal = max(kctrl->max_health / 4, (HitPoints)1);
                killertng->health = min(killertng->health + heal, kctrl->max_health);
                SYNCLOG("Misdeeds: sin 1: %s index %d healed %d by a hero kill at night", thing_model_name(killertng),
                    (int)killertng->index, (int)heal);
            }
            if (md_log_budget > 0)
            {
                md_log_budget--;
                SYNCLOG("Misdeeds: %s index %d killed a %s (%s): heroes %u, wild %u, keepers' %u", thing_model_name(killertng),
                    (int)killertng->index, thing_model_name(victim), (what == MdC_KillsHero) ? "hero" :
                    (what == MdC_KillsWild) ? "wild" : "keeper's", (unsigned)rec->c[MdC_KillsHero],
                    (unsigned)rec->c[MdC_KillsWild], (unsigned)rec->c[MdC_KillsKeeper]);
            }
        }
    }
}

void world_misdeeds_on_damage(struct Thing *victim, HitPoints dmg, PlayerNumber dealer)
{
    /* only a hit from an enemy of its keeper is "hurt taken": not its own keeper's slap, lava, or a trap without
       a dealer (audit 2026-11-13: a slap-and-heal loop bought ranks with no fight) */
    if ((dealer < 0) || (dealer >= PLAYERS_COUNT) || (dealer == victim->owner) || !players_are_enemies(victim->owner, dealer))
        return;
    if (dmg <= 0)
        return;
    struct MdRec *rec = md_peek(victim);
    if ((rec != NULL) && !world_caretaker_active(victim->owner))
        rec->c[MdC_Soaked] += (uint32_t)dmg;
}

void world_misdeeds_heart_destroyed(const struct Thing *heartng)
{
    if (!world_mode_active() || !thing_exists(heartng))
        return;
    const int what = (heartng->owner == PLAYER_GOOD) ? MdC_Outposts : MdC_Hearts;
    int n = 0;
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        if (!thing_exists(thing) || (thing->class_id != TCls_Creature) || (thing->owner == heartng->owner) ||
            !world_player_is_human(thing->owner) || thing_is_picked_up(thing) || world_caretaker_active(thing->owner))
            continue;
        if ((abs(coord_slab(thing->mappos.x.val) - coord_slab(heartng->mappos.x.val)) > 9) ||
            (abs(coord_slab(thing->mappos.y.val) - coord_slab(heartng->mappos.y.val)) > 9))
            continue;
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        struct MdRec *rec = creature_control_invalid(cctrl) ? NULL : md_get(thing, cctrl);
        if (rec != NULL)
        {
            rec->c[what]++;
            n++;
        }
    }
    SYNCLOG("Misdeeds: %s heart destroyed: %d creatures helped", (what == MdC_Outposts) ? "an outpost's" : "a keeper's", n);
}

/******************************************************************************/
/* Vices' effects */

HitPoints world_misdeeds_adjust_damage(struct Thing *victim, struct Thing *attacker, HitPoints dmg)
{
    if ((dmg <= 0) || (md_top == 0) || !world_mode_active() || (victim->class_id != TCls_Creature))
        return dmg;
    const TbBool has_attacker = thing_exists(attacker) && (attacker->class_id == TCls_Creature);
    if (has_attacker && (victim->owner == PLAYER_GOOD) && !world_wild_creature(victim))
    {
        const struct MdRec *arec = md_peek(attacker);
        const int bane_rank = (arec != NULL) ? md_vice_rank(arec, MdV_Bane) : 0;
        if (bane_rank > 0)
        {
            const HitPoints ndmg = dmg * (100 + md_ranked(MD_BANE_PERCENT, bane_rank)) / 100;
            if (md_log_budget > 0)
            {
                md_log_budget--;
                SYNCLOG("Misdeeds: Do-Gooder's Bane %s: %s index %d hits hero %s for %d instead of %d", md_roman[bane_rank],
                    thing_model_name(attacker), (int)attacker->index, thing_model_name(victim), (int)ndmg, (int)dmg);
            }
            dmg = ndmg;
        }
    }
    /* More vices: the attacker's (never against its own keeper's creatures or an ally's: a co-op pair) */
    if (has_attacker && (victim->owner != attacker->owner) && !players_are_mutual_allies(attacker->owner, victim->owner))
    {
        const struct MdRec *arec = md_peek(attacker);
        if (arec != NULL)
        {
            int pct = 0, r;
            const TbBool wild = world_wild_creature(victim);
            if (wild && ((r = md_vice_rank(arec, MdV_Poacher)) > 0))
                pct += md_ranked(MD_POACHER_PERCENT, r);
            if (!wild && (victim->owner != PLAYER_GOOD) && !is_neutral_thing(victim) && ((r = md_vice_rank(arec, MdV_Backstabber)) > 0))
                pct += md_ranked(MD_BACKSTAB_PERCENT, r);
            const struct CreatureControl *actrl = creature_control_get_from_thing(attacker);
            if (!creature_control_invalid(actrl) && (attacker->health < actrl->max_health / 2) &&
                ((r = md_vice_rank(arec, MdV_Glutton)) > 0))
                pct += md_ranked(MD_GLUTTON_PERCENT, r);
            if (((r = md_vice_rank(arec, MdV_LongWay)) > 0) && (md_home_distance(attacker) >= MD_AWAY_SLABS))
                pct += md_ranked(MD_LONGWAY_PERCENT, r);
            if (((r = md_vice_rank(arec, MdV_Heartbreaker)) > 0) && md_on_own_land(victim))
                pct += md_ranked(MD_HEARTBREAK_PERCENT, r);
            if (((r = md_vice_rank(arec, MdV_Angry)) > 0) && (arec->angry_until > (uint32_t)game.play_gameturn))
                pct += md_ranked(MD_ANGRY_PERCENT, r);
            if (pct > 0)
            {
                if (md_log_budget > 0)
                {
                    md_log_budget--;
                    SYNCLOG("Misdeeds: more vices: %s index %d hits %s for %d instead of %d", thing_model_name(attacker),
                        (int)attacker->index, thing_model_name(victim), (int)(dmg * (100 + pct) / 100), (int)dmg);
                }
                dmg = dmg * (100 + pct) / 100;
            }
        }
    }
    /* Breeds: a Tomb Spider's poisoned bite; a Grotto Fly drinks */
    {
        const struct MdRec *brec = has_attacker ? md_peek(attacker) : NULL;
        if ((brec != NULL) && (brec->breed == MdB_TombSpider))
            dmg = dmg * (100 + MD_TOMB_BITE) / 100;
        if ((brec != NULL) && (brec->breed == MdB_GrottoFly) && (attacker->health >= 0))
        {
            const struct CreatureControl *actrl = creature_control_get_from_thing(attacker);
            if (!creature_control_invalid(actrl))
                attacker->health = min(attacker->health + max(dmg * MD_FLY_DRINK / 100, (HitPoints)1), actrl->max_health);
        }
    }
    /* Marching Order: the party of an Orc's standard hits harder; the Van takes less */
    if (has_attacker && (md_party_gift(attacker) == MdG_Harder))
        dmg = dmg * (100 + MD_GIFT_HARDER) / 100;
    if (md_post_now(victim, md_peek(victim)) == MdP_Van)
        dmg = dmg * MD_VAN_TAKEN / 100;
    /* Seeing Through Their Eyes: a possessed Warlock aims; a possessed Vampire reads a hero */
    if (has_attacker && md_driven_by(attacker) && world_player_is_human(attacker->owner))
    {
        if (attacker->model == md_kind("SORCEROR"))
            dmg = dmg * (100 + MD_EYES_AIM) / 100;
        else if ((attacker->model == md_kind("VAMPIRE")) && (victim->owner == PLAYER_GOOD) && !world_wild_creature(victim) &&
            md_soul_owner_ok(attacker->owner) && (md_vampire_read[attacker->owner] != victim->index))
        {
            md_vampire_read[attacker->owner] = victim->index;
            const struct CreatureControl *vctrl = creature_control_get_from_thing(victim);
            char text[120];
            snprintf(text, sizeof(text), "The %s is level %d and carries %ld gold.", creature_code_name(victim->model),
                creature_control_invalid(vctrl) ? 0 : vctrl->exp_level + 1, (long)victim->creature.gold_carried);
            SYNCLOG("Misdeeds: Eyes: a Vampire of player %d reads: %s", (int)attacker->owner, text);
            md_tell(attacker->owner, text);
        }
    }
    /* Virtues: Mercy won't finish a downed enemy */
    if (has_attacker && creature_is_being_unconscious(victim))
    {
        const struct MdRec *mrec = md_peek(attacker);
        if ((mrec != NULL) && (mrec->virtues & MdT_Mercy))
        {
            if (md_log_budget > 0)
            {
                md_log_budget--;
                SYNCLOG("Misdeeds: Mercy: %s index %d spares the downed %s index %d", thing_model_name(attacker), (int)attacker->index,
                    thing_model_name(victim), (int)victim->index);
            }
            return 0;
        }
    }
    /* Deadly Pairs: the attacker's sin */
    const struct MdRec *srec = has_attacker ? md_peek(attacker) : NULL;
    if ((srec != NULL) && (srec->sin != 0) && (victim->owner != attacker->owner))
    {
        int pct = 0;
        if ((srec->sin == 2) && md_on_own_land(attacker))
            pct = 35;
        else if ((srec->sin == 3) && flag_is_set(attacker->movement_flags, TMvF_IsOnLava))
            pct = 40;
        else if ((srec->sin == 4) && (md_home_distance(attacker) >= MD_AWAY_SLABS))
            pct = 30;
        if (pct > 0)
        {
            if (md_log_budget > 0)
            {
                md_log_budget--;
                SYNCLOG("Misdeeds: sin %d: %s index %d hits for %d instead of %d", (int)srec->sin, thing_model_name(attacker),
                    (int)attacker->index, (int)(dmg * (100 + pct) / 100), (int)dmg);
            }
            dmg = dmg * (100 + pct) / 100;
        }
    }
    const struct MdRec *vrec = md_peek(victim);
    if ((vrec != NULL) && (vrec->sin == 5) && !world_is_day() && md_on_own_land(victim))
        dmg = dmg * 65 / 100;
    if ((vrec != NULL) && (vrec->sin == 8) && !world_is_day() && has_attacker && (attacker->owner != victim->owner) &&
        (attacker->health >= 0))
    {
        const HitPoints back = dmg * 30 / 100;
        if (back > 0)
        {
            apply_damage_to_thing_and_display_health(attacker, back, victim->owner);
            if (md_log_budget > 0)
            {
                md_log_budget--;
                SYNCLOG("Misdeeds: sin 8: %s index %d, %s index %d: %d of %d", thing_model_name(victim), (int)victim->index,
                    thing_model_name(attacker), (int)attacker->index, (int)back, (int)dmg);
            }
        }
    }
    const int scar_rank = (vrec != NULL) ? md_vice_rank(vrec, MdV_Scarred) : 0;
    if (scar_rank > 0)
    {
        const struct CreatureControl *sctrl = creature_control_get_from_thing(victim);
        if (!creature_control_invalid(sctrl) && (victim->health < sctrl->max_health / 4))
        {
            const HitPoints ndmg = dmg * (100 - min(md_ranked(MD_SCARRED_PERCENT, scar_rank), 60)) / 100;
            if (md_log_budget > 0)
            {
                md_log_budget--;
                SYNCLOG("Misdeeds: Battle-Scarred: %s index %d takes %d instead of %d", thing_model_name(victim), (int)victim->index,
                    (int)ndmg, (int)dmg);
            }
            dmg = ndmg;
        }
    }
    const int dark_rank = (vrec != NULL) ? md_vice_rank(vrec, MdV_Night) : 0;
    if ((dark_rank > 0) && !world_is_day() && (GAME_RANDOM(100) < md_ranked(MD_NIGHT_MISS, dark_rank)))
    {
        if (md_log_budget > 0)
        {
            md_log_budget--;
            SYNCLOG("Misdeeds: the dark: a hit of %d on %s index %d misses", (int)dmg, thing_model_name(victim), (int)victim->index);
        }
        return 0;
    }
    if (world_player_is_human(victim->owner))
    {
        int guard_rank = 0;
        struct Thing *guard = md_near_find(md_terr, victim, false, &guard_rank);
        if (!thing_is_invalid(guard) && md_on_own_land(guard) && !creature_is_being_unconscious(guard))
        {
            const struct CreatureControl *gctrl = creature_control_get_from_thing(guard);
            const struct CreatureControl *vctrl = creature_control_get_from_thing(victim);
            const HitPoints share = dmg * MD_TERR_SHARE(guard_rank) / 100;
            if (!creature_control_invalid(gctrl) && !creature_control_invalid(vctrl) && (gctrl->exp_level > vctrl->exp_level) &&
                (share > 0))
            {
                apply_damage_to_thing_and_display_health(guard, share, has_attacker ? attacker->owner : -1);
                if (md_log_budget > 0)
                {
                    md_log_budget--;
                    SYNCLOG("Misdeeds: Territorial: %s index %d takes %d of a hit of %d on %s index %d", thing_model_name(guard),
                        (int)guard->index, (int)share, (int)dmg, thing_model_name(victim), (int)victim->index);
                }
                dmg -= share;
            }
        }
    }
    return dmg;
}

TbBool world_misdeeds_fearless_in_dark(const struct Thing *thing)
{
    if (!world_mode_active() || !thing_exists(thing))
        return false;
    const struct MdRec *rec = md_peek(thing);
    return (rec != NULL) && md_has(rec, MdV_Night);
}

long world_misdeeds_speed(const struct Thing *thing, long speed)
{
    if (md_top == 0)
        return speed;
    const struct MdRec *rec = md_peek(thing);
    if ((rec == NULL) || !world_mode_active())
        return speed;
    {
        /* Webbed Toes */
        const int webbed = md_vice_rank(rec, MdV_Webbed);
        if (webbed > 0)
        {
            const struct SlabMap *slb = get_slabmap_for_subtile(thing->mappos.x.stl.num, thing->mappos.y.stl.num);
            if (!slabmap_block_invalid(slb) && (slb->kind == SlbT_WATER))
                speed = speed * (100 + md_ranked(MD_WEBBED_SPEED, webbed)) / 100;
        }
    }
    if ((rec->speed_bits == 0) && (rec->sin_speed == 0) && (rec->march_speed == 0))
        return speed;
    if (rec->march_speed != 0)
        speed = speed * (100 + rec->march_speed) / 100;
    if (rec->sin_speed != 0)
        speed = speed * (100 + rec->sin_speed) / 100;
    if (rec->speed_bits & MdS_Night)
        speed = speed * (100 + md_ranked(MD_NIGHT_SPEED, (rec->speed_bits >> 5) & 7)) / 100;
    if (rec->speed_bits & MdS_Shortcut)
        speed = speed * (100 + md_ranked(MD_SHORTCUT_SPEED, (rec->speed_bits >> 2) & 7)) / 100;
    return speed;
}

TbBool world_misdeeds_lava_proof(const struct Thing *thing)
{
    if (md_top == 0)
        return false;
    const struct MdRec *rec = md_peek(thing);
    return (rec != NULL) && (md_has(rec, MdV_Asbestos) || md_sin_lava_proof(rec->sin)) && world_mode_active();
}

/******************************************************************************/
/* Freezing, deleting, saving */

void world_misdeeds_on_delete(struct CreatureControl *cctrl)
{
    if (cctrl->misdeed_id == 0)
        return;
    struct MdRec *rec = md_rec(cctrl->misdeed_id);
    if ((rec != NULL) && !flag_is_set(rec->flags, MdF_Frozen))
    {
        if (flag_is_set(rec->flags, MdF_Dying) && (cctrl->misdeed_id == md_dying.rec))
        {
            md_dying.rec = 0;
            md_lost(rec, md_dying.owner, md_dying.model, md_dying.name); /* no body: lost for good */
        }
        rec->used = 0;
    }
    cctrl->misdeed_id = 0;
}

uint16_t world_misdeeds_freeze(const struct Thing *thing)
{
    struct MdRec *rec = md_peek(thing);
    if (rec == NULL)
        return 0;
    rec->flags |= MdF_Frozen; /* its creature is deleted next (or lives on: the next sample clears this) */
    if (rec->stray_by != 0)
        SYNCLOG("Misdeeds: Strays: a stray %s of player %d's freezes with its chunk", creature_code_name(thing->model),
            (int)rec->stray_by - 1);
    rec->flags &= ~(MdF_HasLast | MdF_InFight);
    rec->speed_bits = 0;
    rec->sin_speed = 0;
    return creature_control_get_from_thing(thing)->misdeed_id;
}

void world_misdeeds_thaw(struct Thing *thing, uint16_t id)
{
    struct MdRec *rec = md_rec(id);
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if ((rec == NULL) || creature_control_invalid(cctrl))
        return;
    rec->flags &= ~MdF_Frozen;
    cctrl->misdeed_id = id;
    if (rec->stray_by != 0)
        SYNCLOG("Misdeeds: Strays: a stray %s of player %d's thaws (owner %d)", creature_code_name(thing->model),
            (int)rec->stray_by - 1, (int)thing->owner);
}

void world_misdeeds_reset(void)
{
    free(md_recs);
    md_recs = NULL;
    md_cap = 0;
    md_top = 0;
    memset(md_terr, 0, sizeof(md_terr));
    memset(md_short, 0, sizeof(md_short));
    memset(md_body, 0, sizeof(md_body));
    md_body_count = 0;
    memset(md_blood, 0, sizeof(md_blood));
    memset(&md_dying, 0, sizeof(md_dying));
    memset(md_soul, 0, sizeof(md_soul));
    memset(md_vampire_read, 0, sizeof(md_vampire_read));
    memset(md_renown, 0, sizeof(md_renown));
    memset(md_best_level, 0, sizeof(md_best_level));
}

/* the extra: magic, record size, count, then the records 1..count; then "MSB1", the bodies and the inheritances */
#define MD_TAIL_SIZE (4 + sizeof(md_body) + sizeof(md_blood))
static TbBool md_has_souls(void)
{
    for (int i = 0; i < PLAYERS_MAX; i++)
        if (md_soul[i].used)
            return true;
    return false;
}
static TbBool md_has_tail(void)
{
    if (md_body_count > 0)
        return true;
    for (int i = 0; i < MD_BLOOD_MAX; i++)
        if (md_blood[i].vice != MdV_None)
            return true;
    return false;
}

size_t world_misdeeds_save_size(void)
{
    int top = md_top;
    while ((top > 0) && !md_recs[top].used)
        top--;
    if ((top == 0) && !md_has_tail() && !md_has_souls())
        return 0;
    return 12 + (size_t)top * sizeof(struct MdRec) + (md_has_tail() ? MD_TAIL_SIZE : 0) + (md_has_souls() ? 4 + sizeof(md_soul) : 0);
}

void world_misdeeds_save(uint8_t *buf)
{
    int top = md_top;
    while ((top > 0) && !md_recs[top].used)
        top--;
    const uint32_t head[3] = {MD_SAVE_MAGIC, (uint32_t)sizeof(struct MdRec), (uint32_t)top};
    memcpy(buf, head, 12);
    if (top > 0)
        memcpy(buf + 12, &md_recs[1], (size_t)top * sizeof(struct MdRec));
    if (md_has_tail())
    {
        uint8_t *tail = buf + 12 + (size_t)top * sizeof(struct MdRec);
        const uint32_t magic = MD_BODY_MAGIC;
        memcpy(tail, &magic, 4);
        memcpy(tail + 4, md_body, sizeof(md_body));
        memcpy(tail + 4 + sizeof(md_body), md_blood, sizeof(md_blood));
    }
    if (md_has_souls())
    {
        uint8_t *s = buf + 12 + (size_t)top * sizeof(struct MdRec) + (md_has_tail() ? MD_TAIL_SIZE : 0);
        const uint32_t magic = MD_SOUL_MAGIC;
        memcpy(s, &magic, 4);
        memcpy(s + 4, md_soul, sizeof(md_soul));
    }
}

TbBool world_misdeeds_load(const uint8_t *buf, size_t len)
{
    world_misdeeds_reset();
    uint32_t head[3];
    if (len < 12)
        return false;
    memcpy(head, buf, 12);
    if ((head[0] != MD_SAVE_MAGIC) || (head[1] == 0) || (head[2] > 65535) || (len < 12 + (size_t)head[1] * head[2]))
        return false;
    const int cap = ((int)head[2] / 256 + 1) * 256 + 256;
    md_recs = (struct MdRec *)calloc((size_t)cap, sizeof(struct MdRec));
    if (md_recs == NULL)
        return false;
    md_cap = cap;
    md_top = (int)head[2];
    const size_t take = min((size_t)head[1], sizeof(struct MdRec)); /* an older, shorter record: the rest stays 0 */
    int ranked = 0;
    for (uint32_t i = 0; i < head[2]; i++)
    {
        struct MdRec *rec = &md_recs[i + 1];
        memcpy(rec, buf + 12 + (size_t)i * head[1], take);
        if (rec->used && (take <= offsetof(struct MdRec, mastery)))
            rec->lessons_done = MD_LESSONS_UNSET; /* (saved before Lessons) */
        if (rec->used && (take <= MD_REC_SIZE_BEFORE_RANKS))
        {
            /* saved before Deeper Vices: its vices start at rank I, their deeds counted from now */
            for (int s = 0; s < MD_VICE_SLOTS; s++)
                if ((rec->vices[s] > MdV_None) && (rec->vices[s] < MdV_COUNT))
                {
                    md_slot_start(rec, s);
                    ranked++;
                }
        }
    }
    if (ranked > 0)
        SYNCLOG("Misdeeds: Deeper Vices: %d vices of an older save start at rank I", ranked);
    const size_t recs_end = 12 + (size_t)head[1] * head[2];
    if (len >= recs_end + MD_TAIL_SIZE)
    {
        uint32_t magic = 0;
        memcpy(&magic, buf + recs_end, 4);
        if (magic == MD_BODY_MAGIC)
        {
            memcpy(md_body, buf + recs_end + 4, sizeof(md_body));
            memcpy(md_blood, buf + recs_end + 4 + sizeof(md_body), sizeof(md_blood));
            for (int i = 0; i < MD_BODY_MAX; i++)
            {
                if (md_body[i].rec == 0)
                    continue;
                if (md_rec(md_body[i].rec) == NULL)
                    memset(&md_body[i], 0, sizeof(md_body[i]));
                else
                {
                    md_body[i].name[sizeof(md_body[i].name) - 1] = 0;
                    md_body[i].missing = 0;
                    md_body_count++;
                }
            }
        }
    }
    /* Past Lives: the souls, after the records and (when there are any) the bodies */
    for (size_t at = recs_end; at + 4 + sizeof(md_soul) <= len; )
    {
        uint32_t magic = 0;
        memcpy(&magic, buf + at, 4);
        if (magic == MD_BODY_MAGIC)
        {
            at += MD_TAIL_SIZE;
            continue;
        }
        if (magic == MD_SOUL_MAGIC)
        {
            memcpy(md_soul, buf + at + 4, sizeof(md_soul));
            for (int i = 0; i < PLAYERS_MAX; i++)
                md_soul[i].name[sizeof(md_soul[i].name) - 1] = 0;
        }
        break;
    }
    SYNCLOG("Misdeeds: %d records loaded, %d bodies waiting%s", md_top, md_body_count, md_has_souls() ? ", souls waiting" : "");
    return true;
}

/******************************************************************************/
/* Unfinished Business and Bad Blood */

static void md_tell(PlayerNumber plyr_idx, const char *text)
{
    if (is_my_player_number(plyr_idx) && !netstate_dedicated()) /* (a dedicated server's own number is a joined player's) */
    {
        message_add(MsgType_Blank, 0, text);
        return;
    }
    /* a joined player: the server sends the text to its user (as the circles do) */
    if (!netstate_is_server() || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT))
        return;
    const NetUserId u = get_player_primary_user(get_player(plyr_idx));
    if ((u > SERVER_ID) && (u < MAX_NET_USERS))
        server_admin_notice(u, text);
}

static struct MdBody *md_body_of(const struct Thing *deadtng)
{
    if (md_body_count == 0)
        return NULL;
    for (int i = 0; i < MD_BODY_MAX; i++)
        if ((md_body[i].rec != 0) && (md_body[i].thing == deadtng->index) && (md_body[i].made_turn == (uint32_t)deadtng->creation_turn))
            return &md_body[i];
    return NULL;
}

static void md_body_drop(struct MdBody *body)
{
    memset(body, 0, sizeof(*body));
    if (md_body_count > 0)
        md_body_count--;
}

/** A creature is lost for good: the first vice it earned waits for the keeper's next creature of its kind. */
static void md_lost(struct MdRec *rec, PlayerNumber owner, ThingModel model, const char *name)
{
    const int vice = rec->vices[0];
    rec->used = 0;
    if ((vice == MdV_None) || !world_player_is_human(owner))
        return;
    int slot = -1;
    for (int i = 0; i < MD_BLOOD_MAX; i++)
    {
        if (md_blood[i].vice == MdV_None)
        {
            if (slot < 0)
                slot = i;
        } else if ((md_blood[i].owner == owner) && (md_blood[i].model == model))
        {
            slot = i; /* one inheritance waits per kind: the latest loss */
            break;
        }
    }
    if (slot < 0)
        return;
    md_blood[slot].owner = (uint8_t)owner;
    md_blood[slot].model = (uint16_t)model;
    md_blood[slot].vice = (uint8_t)vice;
    SYNCLOG("Misdeeds: Bad Blood: %s (%s) of player %d is lost for good; %s waits for the next %s", name,
        creature_code_name(model), (int)owner, md_vice_names[vice], creature_code_name(model));
    char text[160];
    snprintf(text, sizeof(text), "%s is lost for good. Bad Blood: your next %s inherits %s at level 10.", name,
        creature_code_name(model), md_vice_names[vice]);
    md_tell(owner, text);
}

/** The name a creature shows to players. Only a creature its keeper renamed has one (Rename, once, from level 10),
 *  and it shows from level 10 on; every other creature shows none. (The user, 2026-11-13: creatures showed the names
 *  the game makes up for them, above the flower under the Hand and in the panels.) NULL: no name to show. */
const char *world_misdeeds_shown_name(const struct Thing *thing)
{
    if (!thing_exists(thing) || (thing->class_id != TCls_Creature))
        return NULL;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (creature_control_invalid(cctrl) || (cctrl->creature_name[0] == 0) || (cctrl->exp_level + 1 < md_vice_levels[0]))
        return NULL;
    if (!(netstate_is_client() && netstate_thin_mode()))
    {
        /* (its record says whether its keeper gave the name; a joined machine has no records and goes by the name) */
        const struct MdRec *rec = (cctrl->misdeed_id != 0) ? md_rec(cctrl->misdeed_id) : NULL;
        if ((rec == NULL) || !flag_is_set(rec->flags, MdF_Renamed))
            return NULL;
    }
    return cctrl->creature_name;
}

/** What a message calls a creature: the name its keeper gave it, else "Your Orc" (the names rule above: the names
 *  the game makes up are never shown). The messages of the creature stack - a virtue earned, a vice's rank, a sin, a
 *  cure, a soul's return - named every creature by creature_own_name before the stack met that rule. */
static const char *md_called(const struct Thing *thing, char *buf, size_t size)
{
    const char *given = world_misdeeds_shown_name(thing);
    if (given != NULL)
    {
        snprintf(buf, size, "%s", given);
        return buf;
    }
    const struct CreatureModelConfig *crconf = creature_stats_get(thing->model);
    const char *kind = (crconf != NULL) ? get_string(crconf->namestr_idx) : NULL;
    snprintf(buf, size, "Your %s", ((kind != NULL) && (kind[0] != 0)) ? kind : creature_code_name(thing->model));
    return buf;
}

void world_misdeeds_dying(struct Thing *thing)
{
    if (world_mode_active())
        md_book_kill(thing);
    md_dying.rec = 0;
    if ((md_top == 0) || !world_mode_active() || !world_player_is_human(thing->owner))
        return;
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    struct MdRec *rec = creature_control_invalid(cctrl) ? NULL : md_rec(cctrl->misdeed_id);
    if (rec == NULL)
        return;
    rec->flags |= MdF_Dying;
    md_dying.rec = cctrl->misdeed_id;
    md_dying.owner = (uint8_t)thing->owner;
    md_dying.model = (uint16_t)thing->model;
    md_dying.level = (uint8_t)cctrl->exp_level;
    {
        /* what the messages about its body call it: the name its keeper gave it, else "Your Orc" */
        const char *given = world_misdeeds_shown_name(thing);
        const char *kind = get_string(creature_stats_get(thing->model)->namestr_idx);
        if (given != NULL)
            snprintf(md_dying.name, sizeof(md_dying.name), "%s", given);
        else
            snprintf(md_dying.name, sizeof(md_dying.name), "Your %s", ((kind != NULL) && (kind[0] != 0)) ? kind :
                creature_code_name(thing->model));
    }
}

uint16_t world_misdeeds_corpse_begin(struct Thing *thing)
{
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (creature_control_invalid(cctrl) || (cctrl->misdeed_id == 0) || (cctrl->misdeed_id != md_dying.rec))
        return 0;
    struct MdRec *rec = md_rec(cctrl->misdeed_id);
    if ((rec == NULL) || !flag_is_set(rec->flags, MdF_Dying) || (md_vice_count(rec) + ((rec->sin != 0) ? 2 : 0) < MD_BODY_VICES))
        return 0;
    const uint16_t id = cctrl->misdeed_id;
    cctrl->misdeed_id = 0; /* the record stays, with the body */
    return id;
}

void world_misdeeds_corpse_made(struct Thing *deadtng, uint16_t id)
{
    struct MdRec *rec = md_rec(id);
    if ((rec == NULL) || (id != md_dying.rec))
        return;
    md_dying.rec = 0;
    struct MdBody *body = NULL;
    for (int i = 0; (i < MD_BODY_MAX) && (body == NULL); i++)
        if (md_body[i].rec == 0)
            body = &md_body[i];
    if (thing_is_invalid(deadtng) || (body == NULL) || world_apart_thing(deadtng))
    {
        /* (in the world apart, a World Boss's arena: no imp can come for the body, and the arena goes when the fight
           is over; the creature is lost now, not after a day of waiting that would keep the arena's ground) */
        md_lost(rec, md_dying.owner, md_dying.model, md_dying.name);
        return;
    }
    int64_t wx = 0, wy = 0;
    world_slab_to_world(coord_slab(deadtng->mappos.x.val), coord_slab(deadtng->mappos.y.val), &wx, &wy);
    body->rec = id;
    body->thing = deadtng->index;
    body->made_turn = (uint32_t)deadtng->creation_turn;
    body->expire_turn = (uint32_t)game.play_gameturn + (uint32_t)world_day_turns();
    body->owner = md_dying.owner;
    body->model = md_dying.model;
    body->level = md_dying.level;
    body->wx = (int32_t)wx;
    body->wy = (int32_t)wy;
    memcpy(body->name, md_dying.name, sizeof(body->name));
    md_body_count++;
    rec->flags &= ~(MdF_HasLast | MdF_InFight | MdF_Down);
    rec->speed_bits = 0;
    rec->sin_speed = 0;
    SYNCLOG("Misdeeds: Unfinished Business: %s (%s, level %d, %d vices) of player %d left a body, thing %d, for %ld turns",
        body->name, creature_code_name(body->model), body->level + 1, md_vice_count(rec), (int)body->owner, (int)deadtng->index,
        (long)world_day_turns());
    char text[160];
    snprintf(text, sizeof(text), "%s has Unfinished Business: bring the body to your Graveyard within a day.", body->name);
    md_tell(body->owner, text);
}

TbBool world_misdeeds_body_keep(const struct Thing *deadtng)
{
    const struct MdBody *body = md_body_of(deadtng);
    return (body != NULL) && ((uint32_t)game.play_gameturn < body->expire_turn);
}

TbBool world_misdeeds_body_not_for(const struct Thing *deadtng, PlayerNumber plyr_idx)
{
    if (md_body_count == 0)
        return false;
    const struct MdBody *body = md_body_of(deadtng);
    return (body != NULL) && (body->owner != plyr_idx) && ((uint32_t)game.play_gameturn < body->expire_turn);
}

TbBool world_misdeeds_body_home(struct Thing *deadtng, struct Room *room)
{
    struct MdBody *body = md_body_of(deadtng);
    if (body == NULL)
        return false;
    if (room->owner != body->owner)
        return true; /* not this keeper's to bury (their imps don't pick it up either): it lies where it was dropped */
    struct MdRec *rec = md_rec(body->rec);
    const struct MdBody was = *body;
    md_body_drop(body);
    if (rec == NULL)
        return false;
    struct Coord3d pos = deadtng->mappos;
    struct Thing *thing = create_creature(&pos, was.model, was.owner);
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (thing_is_invalid(thing) || creature_control_invalid(cctrl))
    {
        md_lost(rec, was.owner, was.model, was.name);
        return false;
    }
    set_creature_level(thing, was.level);
    world_misdeeds_on_delete(cctrl); /* (a record the new thing got meanwhile) */
    cctrl->misdeed_id = was.rec;
    if (flag_is_set(rec->flags, MdF_Renamed))
    {
        /* (only a name its keeper gave it comes back with it) */
        memcpy(cctrl->creature_name, was.name, min(sizeof(cctrl->creature_name), sizeof(was.name)));
        cctrl->creature_name[sizeof(cctrl->creature_name) - 1] = 0;
    }
    /* Deeper Vices: one rank poorer, from its highest-ranked vice; with every vice at rank I, one vice poorer: the last
       one earned, and its slot stays empty */
    char text[160];
    const int dropped = md_lose_rank(rec);
    if (dropped >= 0)
    {
        const int vice = rec->vices[dropped];
        SYNCLOG("Misdeeds: Unfinished Business: %s (%s, level %d) of player %d is back as thing %d; %s fell to rank %s; %d vices",
            was.name, creature_code_name(was.model), was.level + 1, (int)was.owner, (int)thing->index, md_vice_names[vice],
            md_roman[rec->rank[dropped]], md_vice_count(rec));
        snprintf(text, sizeof(text), "%s is back from the grave, but %s fell to rank %s.", was.name, md_vice_names[vice],
            md_roman[rec->rank[dropped]]);
    } else
    {
        int gone = MdV_None;
        for (int i = MD_VICE_SLOTS - 1; i >= 0; i--)
            if (rec->vices[i] != MdV_None)
            {
                gone = rec->vices[i];
                rec->vices[i] = MdV_None;
                rec->rank_base[i] = 0;
                rec->rank[i] = 0;
                rec->rack_marks &= (uint8_t)~(1 << i);
                break;
            }
        if ((gone == MdV_None) && (rec->sin != 0))
        {
            /* Deadly Pairs: no vice left to lose: the sin goes (its two slots stay used) */
            SYNCLOG("Misdeeds: Unfinished Business: %s of player %d is back as thing %d without its sin %d", was.name,
                (int)was.owner, (int)thing->index, (int)rec->sin);
            rec->sin = 0;
            rec->sin_speed = 0;
            snprintf(text, sizeof(text), "%s is back from the grave, but its sin is gone.", was.name);
            rec->flags &= ~(MdF_Dying | MdF_Frozen);
            create_effect(&pos, TngEff_Explosion3, was.owner);
            delete_thing_structure(deadtng, 0);
            md_tell(was.owner, text);
            return true;
        }
        rec->c[MdC_Spare]++;
        SYNCLOG("Misdeeds: Unfinished Business: %s (%s, level %d) of player %d is back as thing %d without %s; %d vices left",
            was.name, creature_code_name(was.model), was.level + 1, (int)was.owner, (int)thing->index, md_vice_names[gone],
            md_vice_count(rec));
        snprintf(text, sizeof(text), "%s is back from the grave, but lost the vice %s.", was.name, md_vice_names[gone]);
    }
    rec->flags &= ~(MdF_Dying | MdF_Frozen);
    create_effect(&pos, TngEff_Explosion3, was.owner);
    delete_thing_structure(deadtng, 0);
    md_tell(was.owner, text);
    return true;
}

void world_misdeeds_sacrificed(struct Thing *thing, PlayerNumber temple_owner)
{
    if ((md_top == 0) || !world_mode_active() || (thing->owner != temple_owner) || !world_player_is_human(thing->owner) ||
        !md_soul_owner_ok(thing->owner))
        return;
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    const struct MdRec *rec = creature_control_invalid(cctrl) ? NULL : md_rec(cctrl->misdeed_id);
    if (creature_control_invalid(cctrl) || (cctrl->exp_level + 1 < MD_SOUL_LEVEL))
        return;
    struct MdSoul *soul = &md_soul[thing->owner];
    memset(soul, 0, sizeof(*soul));
    soul->used = 1;
    soul->owner = (uint8_t)thing->owner;
    soul->model = (uint16_t)thing->model;
    soul->lives = (uint8_t)min(((rec != NULL) ? rec->past_lives : 0) + 1, 255);
    {
        /* (the names rule: only a name its keeper gave it goes on with the soul; a made-up one is left behind) */
        const char *given = world_misdeeds_shown_name(thing);
        snprintf(soul->name, sizeof(soul->name), "%s", (given != NULL) ? given : "");
    }
    if (rec != NULL)
    {
        const int count = md_vice_count(rec);
        if (count > 0)
            soul->vice = rec->vices[GAME_RANDOM(count)];
    }
    /* its later abilities (not the first two every kind starts with), the latest learnt first */
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
    int n = 0;
    for (int i = LEARNED_INSTANCES_COUNT - 1; (i >= 2) && (n < MD_SOUL_ABILITIES); i--)
    {
        const int k = crconf->learned_instance_id[i];
        if ((k > 0) && cctrl->instance_available[k])
            soul->abilities[n++] = (uint16_t)k;
    }
    SYNCLOG("Misdeeds: Past Lives: %s (%s, level %d) of player %d gave its soul to the Temple: life %d, vice %s, %d abilities "
        "to remember", soul->name, creature_code_name(thing->model), cctrl->exp_level + 1, (int)thing->owner, (int)soul->lives,
        (soul->vice != MdV_None) ? md_vice_names[soul->vice] : "none", n);
    char text[160];
    snprintf(text, sizeof(text), "%s's soul goes to the Temple. It will return in the next creature your portal sends.", soul->name);
    md_tell(thing->owner, text);
}

/** Past Lives: a soul waiting for this keeper returns in a creature its portal just sent. */
static void md_soul_returns(struct Thing *thing)
{
    struct MdSoul *soul = &md_soul[thing->owner];
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    struct MdRec *rec = creature_control_invalid(cctrl) ? NULL : md_get(thing, cctrl);
    if (rec == NULL)
        return;
    const struct MdSoul was = *soul;
    memset(soul, 0, sizeof(*soul));
    if (was.name[0] != 0)
    {
        snprintf(cctrl->creature_name, sizeof(cctrl->creature_name), "%s", was.name);
        rec->flags |= MdF_Renamed; /* (it has the name its keeper gave it: it is named) */
    }
    rec->past_lives = was.lives;
    rec->past_model = was.model;
    if ((was.vice > MdV_None) && (was.vice < MdV_COUNT) && (rec->vices[0] == MdV_None))
    {
        rec->vices[0] = was.vice;
        md_slot_start(rec, 0);
    }
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
    for (int a = 0; (a < MD_SOUL_ABILITIES) && (rec->foreign_inst == 0); a++)
    {
        const int k = was.abilities[a];
        if ((k <= 0) || (k >= INSTANCE_TYPES_MAX))
            continue; /* (a number out of the table: a damaged save, or a creature file with fewer abilities) */
        TbBool its_own = false;
        for (int i = 0; i < LEARNED_INSTANCES_COUNT; i++)
            its_own = its_own || (crconf->learned_instance_id[i] == k);
        if (!its_own)
        {
            rec->foreign_inst = (uint16_t)k;
            cctrl->instance_available[k] = true;
        }
    }
    char who[48];
    if (was.name[0] != 0)
        snprintf(who, sizeof(who), "%s", was.name);
    else
    {
        /* (it had no name to show: the message calls it by the kind it was) */
        const struct CreatureModelConfig *old = creature_stats_get(was.model);
        const char *kind = (old != NULL) ? get_string(old->namestr_idx) : NULL;
        snprintf(who, sizeof(who), "Your %s", ((kind != NULL) && (kind[0] != 0)) ? kind : creature_code_name(was.model));
    }
    SYNCLOG("Misdeeds: Past Lives: %s returns as a %s (index %d) of player %d: life %d, vice %s, remembers %s", who,
        thing_model_name(thing), (int)thing->index, (int)thing->owner, (int)was.lives + 1,
        (rec->vices[0] != MdV_None) ? md_vice_names[rec->vices[0]] : "none",
        (rec->foreign_inst != 0) ? creature_instance_code_name(rec->foreign_inst) : "nothing");
    char text[160];
    {
        const struct CreatureModelConfig *now = creature_stats_get(thing->model);
        const char *kind = (now != NULL) ? get_string(now->namestr_idx) : NULL;
        snprintf(text, sizeof(text), "%s returns, born again as a %s.", who,
            ((kind != NULL) && (kind[0] != 0)) ? kind : creature_code_name(thing->model));
    }
    md_tell(thing->owner, text);
}

/** Alumni: the level a keeper's newcomers arrive at (1: no renown). */
static int md_newcomer_level(int plyr_idx)
{
    if ((plyr_idx < 0) || (plyr_idx >= PLAYERS_MAX))
        return 1;
    const int by_renown = 1 + md_renown[plyr_idx] / MD_RENOWN_PER_LEVEL;
    return max(1, min(by_renown, md_best_level[plyr_idx] - MD_RENOWN_BENEATH));
}

long world_misdeeds_entrance_wait(PlayerNumber plyr_idx, long wait)
{
    if ((wait <= 0) || !world_mode_active() || !md_soul_owner_ok(plyr_idx) || (md_renown[plyr_idx] <= 0))
        return wait;
    return wait * 100 / (100 + min(md_renown[plyr_idx], MD_RENOWN_WAIT_MAX));
}

void world_misdeeds_attracted(struct Thing *thing)
{
    if (!world_mode_active() || !world_player_is_human(thing->owner))
        return;
    {
        /* Alumni: a creature its keeper's portal brought, and the level renown gives it */
        struct CreatureControl *acctrl = creature_control_get_from_thing(thing);
        struct MdRec *arec = creature_control_invalid(acctrl) ? NULL : md_get(thing, acctrl);
        if (arec != NULL)
        {
            arec->flags |= MdF_Portal;
            /* Breeds: the biome of the portal it came through */
            const int biome = world_biome_at_engine_slab(subtile_slab(thing->mappos.x.stl.num), subtile_slab(thing->mappos.y.stl.num));
            arec->breed = (uint8_t)md_breed_for(thing->model, biome);
            if (arec->breed != MdB_None)
            {
                md_breed_apply(thing, acctrl, arec);
                SYNCLOG("Misdeeds: Breeds: a %s of player %d arrives from a %s portal: a %s", thing_model_name(thing),
                    (int)thing->owner, wg_biome_name(biome), md_breed_names[arec->breed]);
            }
        }
        const int level = md_newcomer_level(thing->owner);
        if (!creature_control_invalid(acctrl) && (level > acctrl->exp_level + 1))
        {
            set_creature_level(thing, level - 1);
            SYNCLOG("Misdeeds: Alumni: a %s arrives for player %d at level %d (renown %d)", thing_model_name(thing),
                (int)thing->owner, level, md_renown[thing->owner]);
        }
    }
    if (md_soul_owner_ok(thing->owner) && md_soul[thing->owner].used)
        md_soul_returns(thing);
    for (int i = 0; i < MD_BLOOD_MAX; i++)
    {
        if ((md_blood[i].vice == MdV_None) || (md_blood[i].owner != thing->owner) || (md_blood[i].model != thing->model))
            continue;
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        struct MdRec *rec = creature_control_invalid(cctrl) ? NULL : md_get(thing, cctrl);
        if (rec == NULL)
            return;
        rec->blood = md_blood[i].vice;
        md_blood[i].vice = MdV_None;
        SYNCLOG("Misdeeds: Bad Blood: %s index %d of player %d will inherit %s at level 10", thing_model_name(thing),
            (int)thing->index, (int)thing->owner, md_vice_names[rec->blood]);
        return;
    }
}

void world_misdeeds_want_chunks(void (*want)(int64_t wx, int64_t wy, int r))
{
    if (md_body_count == 0)
        return;
    for (int i = 0; i < MD_BODY_MAX; i++)
        if (md_body[i].rec != 0)
            want((int64_t)md_body[i].wx, (int64_t)md_body[i].wy, 0);
}

/** One turn in 64: each body is followed (imps drag it; a save or a door makes it a new thing); one that is gone, laid
 *  in a Graveyard some other way, or a day old is lost. */
static void md_bodies_turn(void)
{
    if ((md_body_count == 0) || ((game.play_gameturn & 63) != 0) || (netstate_is_client() && netstate_thin_mode()))
        return;
    for (int i = 0; i < MD_BODY_MAX; i++)
    {
        struct MdBody *body = &md_body[i];
        if (body->rec == 0)
            continue;
        struct MdRec *rec = md_rec(body->rec);
        struct Thing *deadtng = thing_get(body->thing);
        TbBool here = thing_exists(deadtng) && (deadtng->class_id == TCls_DeadCreature) &&
            ((uint32_t)deadtng->creation_turn == body->made_turn);
        if (!here)
        {
            /* the same body as another thing: same kind, owner and level, where this one lay */
            const struct StructureList *slist = get_list_for_thing_class(TCls_DeadCreature);
            long k = slist->index;
            for (int n = 0; (k > 0) && (n < THINGS_COUNT) && !here; n++)
            {
                struct Thing *other = thing_get(k);
                if (thing_is_invalid(other))
                    break;
                k = other->next_of_class;
                if ((other->model != body->model) || (other->owner != body->owner) || (other->corpse.exp_level != body->level) ||
                    (other->corpse.laid_to_rest != 0) || (md_body_of(other) != NULL))
                    continue;
                int64_t wx = 0, wy = 0;
                world_slab_to_world(coord_slab(other->mappos.x.val), coord_slab(other->mappos.y.val), &wx, &wy);
                if ((llabs(wx - body->wx) > 1) || (llabs(wy - body->wy) > 1))
                    continue;
                body->thing = other->index;
                body->made_turn = (uint32_t)other->creation_turn;
                deadtng = other;
                here = true;
            }
        }
        const TbBool late = ((uint32_t)game.play_gameturn >= body->expire_turn);
        if (here && !late && (deadtng->corpse.laid_to_rest == 0) && (rec != NULL))
        {
            int64_t wx = 0, wy = 0;
            world_slab_to_world(coord_slab(deadtng->mappos.x.val), coord_slab(deadtng->mappos.y.val), &wx, &wy);
            body->wx = (int32_t)wx;
            body->wy = (int32_t)wy;
            body->missing = 0;
            continue;
        }
        if (!here && !late && (++body->missing < 3))
            continue; /* (a moment without its thing: a load, a chunk coming back) */
        const struct MdBody was = *body;
        md_body_drop(body);
        if (rec != NULL)
        {
            SYNCLOG("Misdeeds: Unfinished Business: the body of %s was %s", was.name, late ? "left for a day" : "lost");
            md_lost(rec, was.owner, was.model, was.name);
        }
    }
}

/******************************************************************************/
/* Rename and the pick: player actions, through packets */

/* A name travels in one packet beside its creature's number: 48 bits, so nine characters out of forty. Letters are
   kept without their case and shown with a capital at the start of each word. */
static const char md_charset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'-."; /* 40 */

static int md_char_index(char ch)
{
    if ((ch >= 'a') && (ch <= 'z'))
        ch = (char)(ch - 'a' + 'A');
    const char *p = (ch != 0) ? strchr(md_charset, ch) : NULL;
    return (p != NULL) ? (int)(p - md_charset) : -1;
}

/** The name as it will be kept: allowed characters only, no spaces at its ends, a capital at the start of each
 *  word. Returns its length. */
static int md_clean_name(const char *in, char out[WORLD_MISDEED_NAME_MAX + 1])
{
    int n = 0;
    for (const char *p = in; (*p != 0) && (n < WORLD_MISDEED_NAME_MAX); p++)
    {
        const int ci = md_char_index(*p);
        if ((ci < 0) || ((*p == ' ') && ((n == 0) || (out[n - 1] == ' '))))
            continue;
        char ch = md_charset[ci];
        if ((ch >= 'A') && (ch <= 'Z') && (n > 0) && (out[n - 1] != ' '))
            ch = (char)(ch - 'A' + 'a');
        out[n++] = ch;
    }
    while ((n > 0) && (out[n - 1] == ' '))
        n--;
    out[n] = 0;
    return n;
}

static void md_pack_name(const char *name, ThingIndex idx, uint32_t *p1, uint32_t *p2)
{
    const int len = (int)strlen(name);
    uint64_t v = 0;
    for (int i = WORLD_MISDEED_NAME_MAX - 1; i >= 0; i--)
        v = v * 40 + (uint64_t)((i < len) ? max(md_char_index(name[i]), 0) : 0);
    *p1 = (uint32_t)(unsigned short)idx | (uint32_t)((v & 0xFFFF) << 16);
    *p2 = (uint32_t)(v >> 16);
}

static ThingIndex md_unpack_name(uint32_t p1, uint32_t p2, char out[WORLD_MISDEED_NAME_MAX + 1])
{
    char raw[WORLD_MISDEED_NAME_MAX + 1];
    uint64_t v = ((uint64_t)p2 << 16) | (p1 >> 16);
    for (int i = 0; i < WORLD_MISDEED_NAME_MAX; i++)
    {
        raw[i] = md_charset[v % 40];
        v /= 40;
    }
    raw[WORLD_MISDEED_NAME_MAX] = 0;
    md_clean_name(raw, out);
    return (ThingIndex)(p1 & 0xFFFF);
}

/** Can this player rename this creature now? NULL when it can, else why not. */
static const char *md_rename_refusal(PlayerNumber plyr_idx, const struct Thing *thing)
{
    if (!thing_exists(thing) || (thing->class_id != TCls_Creature) || (thing->owner != plyr_idx))
        return "not this keeper's creature";
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (creature_control_invalid(cctrl))
        return "no creature";
    if (cctrl->exp_level + 1 < md_vice_levels[0])
        return "below level 10";
    const struct MdRec *rec = md_peek(thing);
    if ((rec != NULL) && flag_is_set(rec->flags, MdF_Renamed))
        return "already renamed once";
    return NULL;
}

void world_misdeeds_packet_rename(PlayerNumber plyr_idx, uint32_t p1, uint32_t p2)
{
    if (!world_mode_active())
        return;
    char name[WORLD_MISDEED_NAME_MAX + 1];
    const ThingIndex idx = md_unpack_name(p1, p2, name);
    struct Thing *thing = thing_get(idx);
    const char *why = (name[0] == 0) ? "empty name" : md_rename_refusal(plyr_idx, thing);
    if (why != NULL)
    {
        SYNCLOG("Misdeeds: rename of thing %d to '%s' by player %d refused: %s", (int)idx, name, (int)plyr_idx, why);
        return;
    }
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    struct MdRec *rec = md_get(thing, cctrl);
    if (rec == NULL)
        return;
    SYNCLOG("Misdeeds: %s index %d of player %d renamed from '%s' to '%s'", thing_model_name(thing), (int)idx, (int)plyr_idx,
        creature_own_name(thing), name);
    snprintf(cctrl->creature_name, sizeof(cctrl->creature_name), "%s", name);
    rec->flags |= MdF_Renamed;
}

/** Deadly Pairs: a vice slot marked, or unmarked, for the rack (the pick packet with choice 2 + slot). At most two. */
static void md_packet_mark(PlayerNumber plyr_idx, struct Thing *thing, int slot)
{
    struct MdRec *rec = (thing_exists(thing) && (thing->owner == plyr_idx)) ? md_peek(thing) : NULL;
    const char *why = (rec == NULL) ? "no record" : (rec->vices[slot] == MdV_None) ? "no vice there" :
        ((rec->sin != 0) || flag_is_set(rec->flags, MdF_SinUsed)) ? "it has melted its pair" :
        (!(rec->rack_marks & (1 << slot)) && (md_bits(rec->rack_marks) >= 2)) ? "two are marked already" : NULL;
    if (why != NULL)
    {
        SYNCLOG("Misdeeds: rack mark of slot %d for thing %d by player %d refused: %s", slot, thing_exists(thing) ? (int)thing->index : 0,
            (int)plyr_idx, why);
        return;
    }
    rec->rack_marks ^= (uint8_t)(1 << slot);
    rec->rack_samples = 0;
    SYNCLOG("Misdeeds: %s index %d of player %d: vice slot %d %s for the rack (%d marked)", thing_model_name(thing),
        (int)thing->index, (int)plyr_idx, slot, (rec->rack_marks & (1 << slot)) ? "marked" : "unmarked", md_bits(rec->rack_marks));
}

void world_misdeeds_packet_pick(PlayerNumber plyr_idx, ThingIndex idx, int choice)
{
    if (!world_mode_active())
        return;
    struct Thing *thing = thing_get(idx);
    if ((choice >= 2) && (choice < 2 + MD_VICE_SLOTS))
    {
        const struct MdRec *mrec = (thing_exists(thing) && (thing->owner == plyr_idx)) ? md_peek(thing) : NULL;
        if ((mrec != NULL) && (mrec->vices[choice - 2] > MD_OLD_VICES))
        {
            md_tell(plyr_idx, "That vice doesn't melt on the rack.");
            return;
        }
        md_packet_mark(plyr_idx, thing, choice - 2);
        return;
    }
    if ((choice >= 60) && (choice < 60 + MdP_COUNT))
    {
        struct MdRec *prec = (thing_exists(thing) && (thing->owner == plyr_idx)) ? md_peek(thing) : NULL;
        if (prec != NULL)
        {
            prec->post = (uint8_t)(choice - 60);
            SYNCLOG("Misdeeds: Marching Order: %s index %d of player %d takes the post %s", thing_model_name(thing),
                (int)thing->index, (int)plyr_idx, md_post_names[prec->post]);
        }
        return;
    }
    if ((choice >= 50) && (choice < 50 + MdK_COUNT))
    {
        struct MdRec *crec = (thing_exists(thing) && (thing->owner == plyr_idx)) ? md_peek(thing) : NULL;
        const struct CreatureControl *ccctrl = (crec != NULL) ? creature_control_get_from_thing(thing) : NULL;
        if ((crec != NULL) && (ccctrl->exp_level + 1 > MD_LESSON_STEP * crec->contract_done))
            crec->contract_open = 0; /* (its next level closed the renegotiation, whether or not a sample saw it yet) */
        if ((crec == NULL) || !crec->contract_open)
        {
            SYNCLOG("Misdeeds: Contracts: change for thing %d by player %d refused: no renegotiation open", (int)idx, (int)plyr_idx);
            return;
        }
        md_contract_set(thing, crec, choice - 50);
        if (crec->contract_open == 1)
        {
            /* the first signing of this renegotiation starts the day's count afresh; flipping between contracts in the
               same window does not start it again (audit 2026-11-13) */
            crec->last_kill_day = (uint16_t)world_day_number();
            crec->contract_open = 2;
        }
        SYNCLOG("Misdeeds: Contracts: %s index %d of player %d signs %s", thing_model_name(thing), (int)thing->index, (int)plyr_idx,
            md_contract_names[crec->contract]);
        return;
    }
    if ((choice >= 30) && (choice < 30 + MdR_COUNT))
    {
        struct MdRec *rrec = (thing_exists(thing) && (thing->owner == plyr_idx)) ? md_peek(thing) : NULL;
        if (rrec != NULL)
        {
            rrec->regimen = (uint8_t)(choice - 30);
            SYNCLOG("Misdeeds: Regimens: %s index %d of player %d now trains on %s", thing_model_name(thing), (int)thing->index,
                (int)plyr_idx, md_regimen_names[rrec->regimen]);
        }
        return;
    }
    if ((choice == 20) || (choice == 21))
    {
        struct MdRec *lrec = (thing_exists(thing) && (thing->owner == plyr_idx)) ? md_peek(thing) : NULL;
        if (lrec != NULL)
            md_lesson_decide(lrec, thing, choice == 21);
        return;
    }
    struct MdRec *rec = (thing_exists(thing) && (thing->owner == plyr_idx)) ? md_peek(thing) : NULL;
    if ((rec == NULL) || (choice < 0) || (choice > 1) || (rec->offer[choice] == MdV_None) || md_has(rec, rec->offer[choice]))
    {
        SYNCLOG("Misdeeds: pick %d for thing %d by player %d refused", choice, (int)idx, (int)plyr_idx);
        return;
    }
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    const int count = md_vice_count(rec);
    if ((count >= MD_VICE_SLOTS) || (md_used_slots(rec) >= md_slots_for_level(cctrl->exp_level + 1)))
        return;
    rec->vices[count] = rec->offer[choice];
    md_slot_start(rec, count);
    SYNCLOG("Misdeeds: %s index %d (%s) of player %d took the vice %s (over %s); it has %d", thing_model_name(thing), (int)idx,
        creature_own_name(thing), (int)plyr_idx, md_vice_names[rec->offer[choice]],
        (rec->offer[1 - choice] != MdV_None) ? md_vice_names[rec->offer[1 - choice]] : "nothing", count + 1);
    rec->offer[0] = MdV_None;
    rec->offer[1] = MdV_None;
    md_check_offer(rec, thing, cctrl->exp_level + 1); /* a creature that jumped levels has the next slot waiting */
}

/******************************************************************************/
/* The Rap Sheet tab, the pencil, the flower's label */

static char md_name_buf[WORLD_MISDEED_NAME_MAX + 4] = "";
static int md_test_label_idx = 0;
static struct { int x, y, px; char text[CREATURE_NAME_MAX + 1]; } md_label;
/* the two choices' boxes on the Rap Sheet, as drawn last: x, y, w, h (w 0: none) */
static int md_pick_box[2][4];
static int md_pick_lesson = 0; /* Lessons: the two choice boxes offer a lesson, not a vice */
static int md_regimen_box[4]; /* Regimens: the row a click changes the regimen on (w 0: none) */
static int md_contract_box[4]; /* Contracts: the row a click changes the contract on (w 0: none) */
static int md_post_box[4]; /* Marching Order: the row a click changes the post on (w 0: none) */
/* Deadly Pairs: the vice rows that can be marked for the rack, as drawn last (w 0: none) */
static int md_mark_box[MD_VICE_SLOTS][4];
/* The sheet drawn last is the creature window's (world_creature_window.c), not the panel's page: its buttons draw
   themselves as the window's stone buttons. */
static TbBool md_window = false;

static struct Thing *md_panel_thing(void)
{
    struct Thing *thing = thing_get(get_user_state(get_displayed_user())->controlled_thing_idx);
    if (!thing_exists(thing) || (thing->class_id != TCls_Creature))
        return INVALID_THING;
    return thing;
}

/** Text in a box: align 0 left, 1 centre, 2 right; px: the line's height in pixels. */
static void md_text(int x, int y, int w, int px, int align, const char *text)
{
    const unsigned long flg_mem = RendererGetDrawFlags();
    LbTextSetFont(winfont);
    RendererClearDrawFlags(Lb_TEXT_ONE_COLOR | Lb_TEXT_HALIGN_CENTER | Lb_TEXT_HALIGN_RIGHT | Lb_TEXT_HALIGN_LEFT |
        Lb_TEXT_HALIGN_JUSTIFY);
    RendererAddDrawFlags((align == 1) ? Lb_TEXT_HALIGN_CENTER : (align == 2) ? Lb_TEXT_HALIGN_RIGHT : Lb_TEXT_HALIGN_LEFT);
    const int line = max(LbTextLineHeight(), 1);
    int units = max(36 * px / line, 4);
    int drop = 0;
    const long wide = (long)LbTextStringWidthM(text, units);
    if ((w > 8) && (wide > (long)w))
    {
        /* too long for its place: smaller letters (to three fifths at the least), kept on the same middle line */
        const int fit = max((int)((long)units * (w - 2) / wide), units * 3 / 5);
        drop = (line * units / 16 - line * fit / 16) / 2;
        units = max(fit, 3);
    }
    LbTextSetWindow(x, y + drop, w, 2 * px + 4);
    LbTextDrawResized(0, 0, units, text);
    LbTextSetWindow(0, 0, MyScreenWidth, MyScreenHeight);
    RendererSetDrawFlags(flg_mem);
}

/** Sampled time as game hours and tenths (a day and a night are 24 of them). */
static void md_hours(char *out, size_t size, uint32_t samples)
{
    const unsigned long tenths = (unsigned long)samples * MD_SAMPLE / 100;
    snprintf(out, size, "%lu.%luh", tenths / 10, tenths % 10);
}

/** An ability's name for the Rap Sheet: its code name in words ("FIREBALL" -> "Fireball"). */
static const char *md_ability_name(CrInstance inst)
{
    static char buf[4][32];
    static int n = 0;
    char *out = buf[n++ & 3];
    const char *code = creature_instance_code_name(inst);
    int i = 0;
    for (; (code[i] != 0) && (i < 31); i++)
    {
        char ch = (code[i] == '_') ? ' ' : code[i];
        if ((ch >= 'A') && (ch <= 'Z') && (i > 0) && (out[i - 1] != ' '))
            ch = (char)(ch - 'A' + 'a');
        out[i] = ch;
    }
    out[i] = 0;
    return out;
}

/** Lessons: a choice's title: k 0 learn, 1 master. */
static void md_lesson_text(const struct MdRec *rec, const struct CreatureModelConfig *crconf, int k, char *out, size_t size)
{
    const int slot = ((k == 0) ? rec->lesson_learn : rec->lesson_master) - 1;
    if (slot < 0)
    {
        out[0] = 0;
        return;
    }
    if (k == 0)
        snprintf(out, size, "Learn %s", md_ability_name(crconf->learned_instance_id[slot]));
    else
        snprintf(out, size, "Master %s (%s)", md_ability_name(crconf->learned_instance_id[slot]),
            md_roman[min(rec->mastery[slot] + 1, MD_RANKS)]);
}

void world_misdeeds_rap_background(struct GuiMenu *gmnu)
{
    gui_creature_query_background2(gmnu);
    md_window = false;
    md_pick_box[0][2] = 0;
    md_pick_box[1][2] = 0;
    for (int i = 0; i < MD_VICE_SLOTS; i++)
        md_mark_box[i][2] = 0;
    if (!world_mode_active())
        return;
    const struct Thing *thing = md_panel_thing();
    if (thing_is_invalid(thing))
        return;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    const struct MdRec *rec = md_peek(thing);
    const int upp = (gmnu->width * 16 + 140 / 2) / 140;
#define S(v) ((v) * upp / 16)
    const int x0 = gmnu->pos_x + S(5);
    const int w = S(130);
    const int row_px = max(S(6), 6);
    const int small_px = max(S(5), 5);
    int y = gmnu->pos_y + S(219);
    char text[96];
    if (rec == NULL)
    {
        md_text(x0, y, w, row_px, 1, "No record: a clean sheet.");
        return;
    }
    const long served = max(world_day_number() - (long)rec->joined_day, 0L);
    snprintf(text, sizeof(text), "Joined day %d, served %ld days", (int)rec->joined_day, served);
    md_text(x0, y, w, small_px, 1, text);
    y += S(9);
    {
        /* Defectors: the keeper it left, and why */
        const char *came = world_defectors_rap_line(thing->index);
        if (came != NULL)
        {
            md_text(x0, y, w, small_px, 1, came);
            y += S(8);
        }
    }
    /* the priors: two columns of label and number */
    static const char *const labels[MdC_COUNT] = {"Heroes slain", "Wild slain", "Rivals slain", "Hurt taken", "Night brawls",
        "Home brawls", "In lava", "In water", "Hard labour", "Farthest", "Roamed", "Outposts", "Hearts", "Survived", "Felled", ""};
    const int col_w = S(63);
    for (int i = 0; i < MdC_Spare; i++)
    {
        const int cx = x0 + (i % 2) * (col_w + S(4));
        const int cy = y + (i / 2) * S(17) / 2;
        if ((i == MdC_Lava) || (i == MdC_Water) || (i == MdC_Work))
            md_hours(text, sizeof(text), rec->c[i]);
        else if (i == MdC_HomeFights)
            snprintf(text, sizeof(text), "%u", (unsigned)((rec->c[i] + 3) / 4)); /* samples: about a brawl each four */
        else if (rec->c[i] >= 10000)
            snprintf(text, sizeof(text), "%uk", (unsigned)(rec->c[i] / 1000));
        else
            snprintf(text, sizeof(text), "%u", (unsigned)rec->c[i]);
        md_text(cx, cy, col_w, small_px, 0, labels[i]);
        md_text(cx, cy, col_w, small_px, 2, text);
    }
    y += 8 * S(17) / 2 + S(2);
    const int y_end = gmnu->pos_y + S(373);
    /* the vices */
    const int level = creature_control_invalid(cctrl) ? 1 : cctrl->exp_level + 1;
    const int count = md_vice_count(rec);
    const TbBool pending = (rec->offer[0] != MdV_None) && (count < md_slots_for_level(level));
    const TbBool mine = is_my_player_number(thing->owner);
    md_pick_lesson = 0;
    /* Breeds */
    if ((rec->breed != MdB_None) && (rec->breed < MdB_COUNT) && (y + S(8) <= y_end))
    {
        snprintf(text, sizeof(text), "%s: %s", md_breed_names[rec->breed], md_breed_does[rec->breed]);
        md_text(x0, y, w, small_px, 1, text);
        y += S(8);
    }
    /* Seeing Through Their Eyes */
    if ((rec->driven > 0) && (y + S(8) <= y_end))
    {
        snprintf(text, sizeof(text), "Driven: %u kills while you rode it", (unsigned)rec->driven);
        md_text(x0, y, w, small_px, 1, text);
        y += S(8);
    }
    /* Past Lives */
    if ((rec->past_lives > 0) && (y + S(8) <= y_end))
    {
        if (rec->foreign_inst != 0)
            snprintf(text, sizeof(text), "Past lives %d: once a %s; remembers %s", (int)rec->past_lives,
                creature_code_name(rec->past_model), md_ability_name(rec->foreign_inst));
        else
            snprintf(text, sizeof(text), "Past lives %d: once a %s", (int)rec->past_lives, creature_code_name(rec->past_model));
        md_text(x0, y, w, small_px, 1, text);
        y += S(8);
    }
    /* Marching Order: its post (the row its keeper clicks to change it) */
    md_post_box[2] = 0;
    if ((y + S(8) <= y_end) && (rec->post < MdP_COUNT))
    {
        if (rec->post == MdP_None)
            snprintf(text, sizeof(text), "Party post: none");
        else
            snprintf(text, sizeof(text), "Party post: %s, %s%s", md_post_names[rec->post], md_post_does[rec->post],
                creature_is_group_member(thing) ? "" : " (in a party)");
        if (mine)
        {
            md_post_box[0] = x0 - S(2);
            md_post_box[1] = y - S(1);
            md_post_box[2] = w + S(4);
            md_post_box[3] = S(9);
        }
        md_text(x0, y, w, small_px, 1, text);
        y += S(8);
    }
    /* Contracts: its pay, and the row its keeper clicks while a renegotiation is open */
    md_contract_box[2] = 0;
    if ((y + S(8) <= y_end) && (rec->contract < MdK_COUNT))
    {
        if (rec->contract_open)
        {
            snprintf(text, sizeof(text), "Contract: %s (renegotiating)", md_contract_names[rec->contract]);
            if (mine)
            {
                md_contract_box[0] = x0 - S(2);
                md_contract_box[1] = y - S(1);
                md_contract_box[2] = w + S(4);
                md_contract_box[3] = S(9);
            }
        } else
            snprintf(text, sizeof(text), "Contract: %s, %s", md_contract_names[rec->contract], md_contract_does[rec->contract]);
        md_text(x0, y, w, small_px, 1, text);
        y += S(8);
    }
    /* Regimens: what it trains on (its keeper clicks the row to change it), and what its regimens did */
    md_regimen_box[2] = 0;
    if (y + S(8) <= y_end)
    {
        snprintf(text, sizeof(text), "Regimen: %s", md_regimen_names[(rec->regimen < MdR_COUNT) ? rec->regimen : 0]);
        if (mine)
        {
            md_regimen_box[0] = x0 - S(2);
            md_regimen_box[1] = y - S(1);
            md_regimen_box[2] = w + S(4);
            md_regimen_box[3] = S(9);
        }
        md_text(x0, y, w, small_px, 1, text);
        y += S(8);
    }
    if ((rec->reg_levels[0] | rec->reg_levels[1] | rec->reg_levels[2] | rec->reg_levels[3]) && (y + S(8) <= y_end))
    {
        snprintf(text, sizeof(text), "Str %+d%%  Def %+d%%  Dex %+d%%  HP %+d%%", md_regimen_pct(rec, MdR_Brawn) - 100,
            md_regimen_pct(rec, MdR_Guard) - 100, md_regimen_pct(rec, MdR_Aim) - 100, md_regimen_pct(rec, MdR_Grit) - 100);
        md_text(x0, y, w, small_px, 1, text);
        y += S(8);
    }
    /* Lessons: the abilities it mastered, and the lesson waiting (shown when no vice is) */
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
    {
        char line[96] = "";
        for (int i = 0; i < LEARNED_INSTANCES_COUNT; i++)
            if ((rec->mastery[i] > 0) && (crconf->learned_instance_id[i] > 0))
            {
                const size_t n = strlen(line);
                snprintf(line + n, sizeof(line) - n, "%s%s %s", (n > 0) ? ", " : "Mastered: ",
                    md_ability_name(crconf->learned_instance_id[i]), md_roman[min((int)rec->mastery[i], MD_RANKS)]);
            }
        if (line[0] != 0)
        {
            md_text(x0, y, w, small_px, 1, line);
            y += S(8);
        }
    }
    if (!pending && (rec->lesson_learn != 0))
    {
        snprintf(text, sizeof(text), mine ? "Level %d lesson: pick one" : "Level %d lesson: not picked",
            MD_LESSON_STEP * (rec->lessons_done + 1));
        md_text(x0, y, w, small_px, 1, text);
        y += S(9);
        for (int k = 0; k < 2; k++)
        {
            const int slot = ((k == 0) ? rec->lesson_learn : rec->lesson_master) - 1;
            if (slot < 0)
                continue;
            md_pick_box[k][0] = x0 - S(2);
            md_pick_box[k][1] = y;
            md_pick_box[k][2] = mine ? w + S(4) : 0;
            md_pick_box[k][3] = S(17);
            if (!mine)
            {
                md_lesson_text(rec, crconf, k, text, sizeof(text));
                md_text(x0, y, w, row_px, 1, text);
            }
            y += S(18);
        }
        md_pick_lesson = 1;
    }
    /* Deadly Pairs: its sin, or two vices can be marked for the rack */
    if (rec->sin != 0)
    {
        char sin_text[MD_SIN_TEXT_MAX];
        snprintf(text, sizeof(text), "Sin: %s", md_sin_text(0, rec->sin, sin_text));
        md_text(x0, y, w, row_px, 1, text);
        y += S(8);
        md_text(x0, y, w, small_px, 1, md_sin_text(1, rec->sin, sin_text));
        y += S(9);
    } else if (flag_is_set(rec->flags, MdF_SinUsed))
    {
        md_text(x0, y, w, small_px, 1, "- a sin left in the grave -");
        y += S(8);
    }
    /* Virtues: each one it has, and a rest after a correction */
    for (int v = 0; v < MD_VIRTUES; v++)
        if ((rec->virtues & (1 << v)) && (y + S(8) <= y_end))
        {
            snprintf(text, sizeof(text), "Virtue: %s, %s", md_virtue_names[v], md_virtue_does[v]);
            md_text(x0, y, w, small_px, 1, text);
            y += S(8);
        }
    if (md_resting(rec) && (y + S(8) <= y_end))
    {
        snprintf(text, sizeof(text), "Cured on the rack: no work until day %d", (int)rec->rest_day);
        md_text(x0, y, w, small_px, 1, text);
        y += S(8);
    }
    const TbBool markable = mine && !pending && (count >= 2) && (rec->sin == 0) && !flag_is_set(rec->flags, MdF_SinUsed) &&
        !thing_is_creature_digger(thing); /* (diggers get no torture job: there is no rack for them) */
    for (int i = 0; i < count; i++)
    {
        if (markable)
        {
            md_mark_box[i][0] = x0 - S(2);
            md_mark_box[i][1] = y + S(2); /* (a row's glyphs sit lower than its y) */
            md_mark_box[i][2] = w + S(4);
            md_mark_box[i][3] = S(15);
        }
        /* Deeper Vices: its rank, and how far it is towards the next one */
        const int vice = rec->vices[i];
        const int rank = max(md_slot_rank(rec, i), 1);
        snprintf(text, sizeof(text), "%s %s", md_vice_short[vice], md_roman[rank]);
        md_text(x0, y, w, row_px, 1, text);
        if (rank < MD_RANKS)
        {
            /* the way to the next rank: a thin bar under the title (text there would not fit the longest name) */
            const uint32_t deeds = md_deeds(rec, vice) - min(rec->rank_base[i], md_deeds(rec, vice));
            const uint32_t from = md_rank_deeds(vice, rank), to = md_rank_deeds(vice, rank + 1);
            const unsigned pct = min((unsigned)((uint64_t)(deeds - min(deeds, from)) * 100 / max(to - from, 1u)), 99u);
            const int bar_w = w / 2, bar_x = x0 + (w - bar_w) / 2, bar_y = y + S(7), bar_h = max(S(1) / 2, 1);
            LbDrawBox(bar_x, bar_y, bar_w, bar_h, colours[3][2][1]);
            if (pct > 0)
                LbDrawBox(bar_x, bar_y, max(bar_w * (int)pct / 100, 1), bar_h, colours[15][11][3]);
        }
        y += S(8);
        if (!pending)
        {
            md_vice_does_text(vice, rank, text, sizeof(text));
            md_text(x0, y, w, small_px, 1, text);
            y += S(7);
        }
    }
    if (markable && (y + S(8) <= y_end))
    {
        const int marks = md_bits(rec->rack_marks);
        if (marks < 2)
            snprintf(text, sizeof(text), "Mark two vices for the rack (%d)", marks);
        else
            snprintf(text, sizeof(text), "Torture Chamber melts them: %d%%",
                min((int)((uint32_t)rec->rack_samples * MD_SAMPLE * 100 / MD_RACK_TURNS), 99));
        md_text(x0, y, w, small_px, 1, text);
        y += S(8);
    }
    int next = md_used_slots(rec) - (int)rec->c[MdC_Spare]; /* (a melted pair's two slots are used, not waiting) */
    if (pending)
    {
        snprintf(text, sizeof(text), mine ? "Level %d vice: pick one" : "Level %d vice: not picked",
            md_vice_levels[min(md_used_slots(rec), MD_VICE_SLOTS - 1)]);
        md_text(x0, y, w, small_px, 1, text);
        y += S(9);
        for (int k = 0; k < 2; k++)
        {
            if (rec->offer[k] == MdV_None)
                continue;
            md_pick_box[k][0] = x0 - S(2);
            md_pick_box[k][1] = y;
            md_pick_box[k][2] = mine ? w + S(4) : 0;
            md_pick_box[k][3] = S(17);
            md_pick_box[k][2] |= 0; /* (drawn by the button itself) */
            if (!mine)
            {
                md_text(x0, y, w, row_px, 1, md_vice_short[rec->offer[k]]);
                md_text(x0, y + S(8), w, small_px, 1, md_vice_does[rec->offer[k]]);
            }
            y += S(18);
        }
        next++;
    }
    if ((rec->blood != MdV_None) && (rec->blood < MdV_COUNT) && (count == 0) && !pending && (y + S(8) <= y_end))
    {
        snprintf(text, sizeof(text), "Bad Blood: %s at level 10", md_vice_short[rec->blood]);
        md_text(x0, y, w, small_px, 1, text);
        y += S(8);
        next++;
    }
    for (unsigned k = 0; (k < rec->c[MdC_Spare]) && (next < MD_VICE_SLOTS) && (y + S(8) <= y_end); k++)
    {
        md_text(x0, y, w, small_px, 1, "- a vice left in the grave -");
        y += S(8);
        next++;
    }
    for (int i = next; (i < MD_VICE_SLOTS) && (y + S(8) <= y_end); i++)
    {
        snprintf(text, sizeof(text), "- vice at level %d -", md_vice_levels[i]);
        md_text(x0, y, w, small_px, 1, text);
        y += S(8);
    }
#undef S
}

/******************************************************************************/
/* The creature window's Rap Sheet (world_creature_window.c).
 *
 * The same record as the panel's page, set out for the window's right side: in the window's lettering (the game's
 * plain font; the user, 2026-11-14: "the font for the RAP SHEET is terrible. Should be swapped with something
 * readable"), in parts under headings of one size - RAP SHEET, CONTRACTS, VICES (the user: "The Vice header should be
 * as big as 'RAP SHEET' to make it clear the below section is concerning something different", "the Contracts section
 * should also have a header as 'CONTRACTS'") - and with what a keeper can click as stone buttons, drawn by the rows'
 * own buttons.
 *
 * Its sizes are in the window's units; what a unit is on the screen the window says (upp: pixels, times 16), so the
 * sheet is the same on every screen. A line of text has a row of its own (the letters, room for what hangs below the
 * line, a little air), a button stands clear of the line above it, and its words stand clear of its frame (the user:
 * "Please add more space between these two elements", of a line that touched the button under it, and "the text is
 * touching the box. Add more space on each of those buttons").
 *
 * A sheet with much on it (five vices, a sin, a lesson to pick) is higher than the window has room for. It is
 * measured first; where it would not fit it is set closer (the same letters, less air), then at five sixths and at
 * two thirds of its size, then smaller - but its letters never smaller than the plain font itself (8 pixels): from
 * there on, and on a small screen from the start, the sheet keeps that size and scrolls (the user, 2026-11-14, of a
 * full sheet too small to read at 640 x 480: "for smaller screens just add a scroller"). The mouse wheel moves it,
 * and the bar at its right (an arrow at each end, a thumb between, as the World Boss window's list has). It scrolls
 * by whole lines: what does not lie wholly in the sheet's room is not drawn, so nothing is ever cut through. Where
 * the room is too narrow for two columns of the record's table, or for two buttons side by side, they stand in one. */

#define MW_FULL 48        /* the body at its full size, in these parts */
#define MW_HEAD 16        /* a heading's strip */
#define MW_HEAD_CAP 10    /* ... and its capitals */
#define MW_HEAD_FLOOR 11  /* ... which are never smaller than this, in pixels */
#define MW_CAP4 27        /* the body's capitals, in quarters of a unit: 6.75, three times the plain font on 2560 x 1440 */
#define MW_BAR 7          /* the scroll bar's width */
#define MW_BAR_GAP 4      /* ... and what lies between it and the sheet */
/* the body's rows, in quarters of a unit: set wide, and set close */
static const int mw_line4[2] = {40, 35};  /* a line: 1 above its capitals, the rest below for what hangs under the line */
static const int mw_row4[2] = {36, 34};   /* a row of the record's table */
static const int mw_btn4[2] = {60, 52};   /* a stone button of one line */
static const int mw_pick4[2] = {120, 104};/* a stone button of two lines (a vice or a lesson to pick) */
static const int mw_gap4[2] = {12, 8};    /* between a line and the button under it, and between buttons */

struct MdSheet {
    int x, w;           /* the sheet's column, in pixels */
    int y_top, y_end;   /* its room: what does not lie wholly in it is not drawn */
    int upp;            /* the window's scale */
    int body;           /* the body's size, in MW_FULL-ths */
    int close;          /* 1: set close */
    TbBool draw;        /* false: measured only */
    int fixed;          /* the height that does not shrink with the body (the headings), counted as it runs */
};
#define MW_U(v) ((v) * sh->upp / 16)
#define MW_B4(v) ((v) * sh->upp * sh->body / (64 * MW_FULL))
#define MW_LINES 4        /* a text is broken into this many lines at the most */

static int md_window_upp = 16;    /* the window's scale, for its buttons' drawing */
static int md_window_cap = 8;     /* ... the body's capitals, in pixels */
static int md_window_pick = 30;   /* ... and the height of a pick with one line of what it does */
static int md_test_level = 0;     /* test (KFX_WORLD_SHEETFULL): the level the window's sheet takes the creature to have */
static char md_row_text[3][96];   /* the window's clickable rows: regimen, contract, post (drawn by their buttons) */
/* the window's sheet scrolls where it is higher than its room */
static int md_sheet_scroll = 0;   /* how far it is scrolled, in pixels */
static int md_sheet_over = 0;     /* how much higher than its room it is (0: it fits, and there is no bar) */
static int md_sheet_page = 0;     /* a page: its room, less two lines */
static int md_sheet_step = 1;     /* a line */
static ThingIndex md_sheet_thing = 0; /* whose sheet it is: another creature's begins at its top */
static int md_scroll_box[4];      /* the bar's place (w 0: none) */

/** A stone button of the window: the game's stone, darker until the mouse is on it, in a frame (gold then). The
 *  darker stone is a dark box drawn opaque with the stone's own speckle left out: the see-through box the first
 *  version laid over the stone comes out lighter than the stone with the OpenGL renderer and darker with the
 *  software one. */
static void md_stone(int x, int y, int w, int h, TbBool over)
{
    const int u = max((md_window_upp + 8) / 32, 1);
    const TbPixel frame = over ? world_window_ink(WwI_Gold) : colours[8][6][3];
    if (over)
        draw_slab64k_background(x, y, w, h);
    else
        LbDrawBox(x, y, w, h, colours[2][1][0]);
    LbDrawBox(x, y, w, u, frame);
    LbDrawBox(x, y + h - u, w, u, frame);
    LbDrawBox(x, y, u, h, frame);
    LbDrawBox(x + w - u, y, u, h, frame);
}

/** Does a piece from y, h high, lie wholly in the sheet's room? (slack: what may hang below the room's end.) */
static TbBool mw_in(const struct MdSheet *sh, int y, int h, int slack)
{
    return (y >= sh->y_top) && (y + h <= sh->y_end + slack);
}

/** A heading: the game's lettering, a thin rule to each side of it. */
static int mw_heading(struct MdSheet *sh, int y, const char *text)
{
    const int cap = max(MW_U(MW_HEAD_CAP), MW_HEAD_FLOOR);
    const int h = max(MW_U(MW_HEAD), cap + MW_U(6) + 2);
    sh->fixed += h;
    if (sh->draw && mw_in(sh, y, h, 0))
    {
        const int wide = min(world_window_display_width(text, cap), sh->w);
        const int rule = (sh->w - wide) / 2 - MW_U(5);
        const int cap_y = y + h - cap - max(MW_U(3), 1);
        world_window_display(sh->x, cap_y, sh->w, cap, 1, text);
        if (rule >= MW_U(6))
        {
            const int lw = max((sh->upp + 8) / 32, 1);
            LbDrawBox(sh->x, cap_y + cap / 2, rule, lw, world_window_ink(WwI_Rule));
            LbDrawBox(sh->x + sh->w - rule, cap_y + cap / 2, rule, lw, world_window_ink(WwI_Rule));
        }
    }
    return h;
}

/** A line of the body in the room (x, w); returns the row it took. */
static int mw_line(struct MdSheet *sh, int x, int y, int w, int align, int ink, const char *text)
{
    const int h = MW_B4(mw_line4[sh->close]);
    if (sh->draw && mw_in(sh, y, h, MW_B4(8)))
        world_window_plain(x, y + MW_B4(4), w, MW_B4(MW_CAP4), align, world_window_ink(ink), text);
    return h;
}

/** Text over the whole column, broken at its words into lines. */
static int mw_text(struct MdSheet *sh, int y, int ink, const char *text)
{
    char lines[MW_LINES][WORLD_WINDOW_LINE];
    const int n = world_window_plain_wrap(text, MW_B4(MW_CAP4), sh->w, lines, MW_LINES);
    int h = 0;
    for (int i = 0; i < n; i++)
        h += mw_line(sh, sh->x, y + h, sh->w, 1, ink, lines[i]);
    return h;
}

/** A row its keeper can click: a stone button (its own button draws it, with the text kept for it); for a creature
 *  that is not this keeper's, or a row that is closed, the text alone on the same middle line. */
static int mw_row(struct MdSheet *sh, int *box, char *keep, TbBool clickable, int x, int y, int w, const char *text)
{
    const int h = MW_B4(mw_btn4[sh->close]);
    if (sh->draw && mw_in(sh, y, h, 0))
    {
        if (clickable)
        {
            box[0] = x;
            box[1] = y;
            box[2] = w;
            box[3] = h;
            snprintf(keep, 96, "%s", text);
        } else
        {
            const int cap = MW_B4(MW_CAP4);
            world_window_plain(x, y + (h - cap) / 2, w, cap, 1, world_window_ink(WwI_Bright), text);
        }
    }
    return h;
}

/** A pick's button, w wide: what it does is broken into lines (`lines`, `n` of them), the name's capitals stand
 *  name_y below the button's top, the first of those lines does_y, each next one `step` lower; returns the button's
 *  height. cap: the letters' height; base: the height of a pick with one line of what it does. Its words stand clear
 *  of its frame all round. */
static int mw_pick_layout(int w, int cap, int base, const char *does, char lines[][WORLD_WINDOW_LINE], int *n, int *name_y,
    int *does_y, int *step)
{
    const int pad = max(base / 4, 2);
    const int between = max(cap * 5 / 8, 2); /* (from a line's foot to the capitals under it) */
    *n = max(world_window_plain_wrap(does, cap, max(w - 2 * pad, 8), lines, MW_LINES), 1);
    *step = cap + between;
    *name_y = max((base - 2 * cap - between) / 2, 1);
    *does_y = *name_y + *step;
    return max(base, 2 * *name_y + 2 * cap + between) + (*n - 1) * *step;
}

/** A choice of two parts: a stone button (drawn by the pick's own button), or, for another keeper's creature, the
 *  lines alone. */
static int mw_pick(struct MdSheet *sh, int k, TbBool mine, int y, const char *name, const char *does)
{
    char lines[MW_LINES][WORLD_WINDOW_LINE];
    const int cap = MW_B4(MW_CAP4);
    int n, name_y, does_y, step;
    const int h = mw_pick_layout(sh->w, cap, MW_B4(mw_pick4[sh->close]), does, lines, &n, &name_y, &does_y, &step);
    if (sh->draw && mw_in(sh, y, h, 0))
    {
        md_pick_box[k][0] = sh->x;
        md_pick_box[k][1] = y;
        md_pick_box[k][2] = mine ? sh->w : 0;
        md_pick_box[k][3] = h;
        if (!mine)
        {
            world_window_plain(sh->x, y + name_y, sh->w, cap, 1, world_window_ink(WwI_Bright), name);
            for (int i = 0; i < n; i++)
                world_window_plain(sh->x, y + does_y + i * step, sh->w, cap, 1, world_window_ink(WwI_Dim), lines[i]);
        }
    }
    return h;
}

/** Sets the sheet out from y down: draws it, or (sh->draw false) only measures. Returns where it ended, not counting
 *  the lines that only fill what room is left (the vices still to come). */
static int md_sheet_run(struct MdSheet *sh, int y, const struct Thing *thing, const struct MdRec *rec)
{
    char text[160];
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
    const TbBool mine = is_my_player_number(thing->owner);
    const int level = (md_test_level > 0) ? md_test_level : creature_control_invalid(cctrl) ? 1 : cctrl->exp_level + 1;
    const int count = md_vice_count(rec);
    const TbBool pending = (rec->offer[0] != MdV_None) && (count < md_slots_for_level(level));
    const int cap = MW_B4(MW_CAP4), gap = MW_B4(mw_gap4[sh->close]), air = MW_B4(8);
    sh->fixed = 0;
    md_pick_lesson = 0;

    /* --- the record --- */
    y += mw_heading(sh, y, "RAP SHEET");
    {
        const long served = max(world_day_number() - (long)rec->joined_day, 0L);
        snprintf(text, sizeof(text), "Joined day %d, served %ld days", (int)rec->joined_day, served);
        y += mw_text(sh, y, WwI_Dim, text);
        /* Defectors: the keeper it left, and why (the panel's page has the line; the window is where a creature is
           looked at now) */
        const char *came = world_defectors_rap_line(thing->index);
        if (came != NULL)
            y += mw_text(sh, y, WwI_Dim, came);
    }
    {
        /* the priors: a label and its number, in two columns where the room holds two side by side */
        static const char *const labels[MdC_COUNT] = {"Heroes slain", "Wild slain", "Rivals slain", "Hurt taken", "Night brawls",
            "Home brawls", "In lava", "In water", "Hard labour", "Farthest", "Roamed", "Outposts", "Hearts", "Survived", "Felled", ""};
        char values[MdC_Spare][16];
        const int col_gap = MW_U(12), row_h = MW_B4(mw_row4[sh->close]);
        int widest = 0;
        for (int i = 0; i < MdC_Spare; i++)
        {
            if ((i == MdC_Lava) || (i == MdC_Water) || (i == MdC_Work))
                md_hours(values[i], sizeof(values[i]), rec->c[i]);
            else if (i == MdC_HomeFights)
                snprintf(values[i], sizeof(values[i]), "%u", (unsigned)((rec->c[i] + 3) / 4)); /* samples: about a brawl each four */
            else if (rec->c[i] >= 10000)
                snprintf(values[i], sizeof(values[i]), "%uk", (unsigned)(rec->c[i] / 1000));
            else
                snprintf(values[i], sizeof(values[i]), "%u", (unsigned)rec->c[i]);
            widest = max(widest, world_window_plain_width(labels[i], cap) + world_window_plain_width(values[i], cap) + MW_U(4));
        }
        const int cols = (2 * widest + col_gap <= sh->w) ? 2 : 1;
        const int col_w = (cols == 2) ? (sh->w - col_gap) / 2 : sh->w;
        for (int i = 0; (i < MdC_Spare) && sh->draw; i++)
        {
            const int cx = sh->x + (i % cols) * (col_w + col_gap), cy = y + (i / cols) * row_h;
            if (!mw_in(sh, cy, row_h, MW_B4(4)))
                continue;
            const int value_w = world_window_plain_width(values[i], cap);
            world_window_plain(cx, cy + MW_B4(4), max(col_w - value_w - MW_U(3), 8), cap, 0, world_window_ink(WwI_Dim), labels[i]);
            world_window_plain(cx, cy + MW_B4(4), col_w, cap, 2, world_window_ink(WwI_Bright), values[i]);
        }
        y += ((MdC_Spare + cols - 1) / cols) * row_h + gap;
    }
    {
        /* Marching Order and Regimens: its post in a party and what it trains on, each its keeper's to change with a
           click: side by side where both have room, else one under the other; under them what the post does and what
           its training has made of it */
        const int between = MW_U(6), half = (sh->w - between) / 2;
        const int post = (rec->post < MdP_COUNT) ? rec->post : MdP_None;
        char left[64], right[64];
        snprintf(left, sizeof(left), "Party post: %s", md_post_names[post]);
        snprintf(right, sizeof(right), "Regimen: %s", md_regimen_names[(rec->regimen < MdR_COUNT) ? rec->regimen : 0]);
        const int need = max(world_window_plain_width(left, cap), world_window_plain_width(right, cap)) + MW_B4(mw_btn4[sh->close]);
        if (need <= half)
        {
            mw_row(sh, md_post_box, md_row_text[2], mine, sh->x, y, half, left);
            y += mw_row(sh, md_regimen_box, md_row_text[0], mine, sh->x + half + between, y, sh->w - half - between, right) + air;
        } else
        {
            y += mw_row(sh, md_post_box, md_row_text[2], mine, sh->x, y, sh->w, left) + gap;
            y += mw_row(sh, md_regimen_box, md_row_text[0], mine, sh->x, y, sh->w, right) + air;
        }
        if (post != MdP_None)
        {
            snprintf(text, sizeof(text), "%s: %s%s", md_post_names[post], md_post_does[post],
                creature_is_group_member(thing) ? "" : " (in a party)");
            y += mw_text(sh, y, WwI_Dim, text);
        }
        if (rec->reg_levels[0] | rec->reg_levels[1] | rec->reg_levels[2] | rec->reg_levels[3])
        {
            snprintf(text, sizeof(text), "Str %+d%%  Def %+d%%  Dex %+d%%  HP %+d%%", md_regimen_pct(rec, MdR_Brawn) - 100,
                md_regimen_pct(rec, MdR_Guard) - 100, md_regimen_pct(rec, MdR_Aim) - 100, md_regimen_pct(rec, MdR_Grit) - 100);
            y += mw_text(sh, y, WwI_Dim, text);
        }
    }
    /* Breeds */
    if ((rec->breed != MdB_None) && (rec->breed < MdB_COUNT))
    {
        snprintf(text, sizeof(text), "%s: %s", md_breed_names[rec->breed], md_breed_does[rec->breed]);
        y += mw_text(sh, y, WwI_Dim, text);
    }
    /* Seeing Through Their Eyes */
    if (rec->driven > 0)
    {
        snprintf(text, sizeof(text), "Driven: %u kills while you rode it", (unsigned)rec->driven);
        y += mw_text(sh, y, WwI_Dim, text);
    }
    /* Past Lives */
    if (rec->past_lives > 0)
    {
        if (rec->foreign_inst != 0)
            snprintf(text, sizeof(text), "Past lives %d: once a %s; remembers %s", (int)rec->past_lives,
                creature_code_name(rec->past_model), md_ability_name(rec->foreign_inst));
        else
            snprintf(text, sizeof(text), "Past lives %d: once a %s", (int)rec->past_lives, creature_code_name(rec->past_model));
        y += mw_text(sh, y, WwI_Dim, text);
    }
    /* Lessons: the abilities it mastered */
    {
        char line[160] = "";
        for (int i = 0; i < LEARNED_INSTANCES_COUNT; i++)
            if ((rec->mastery[i] > 0) && (crconf->learned_instance_id[i] > 0))
            {
                const size_t n = strlen(line);
                snprintf(line + n, sizeof(line) - n, "%s%s %s", (n > 0) ? ", " : "Mastered: ",
                    md_ability_name(crconf->learned_instance_id[i]), md_roman[min((int)rec->mastery[i], MD_RANKS)]);
            }
        if (line[0] != 0)
            y += mw_text(sh, y, WwI_Dim, line);
    }

    /* --- Contracts: its pay; its keeper's to change with a click while a renegotiation is open --- */
    y += mw_heading(sh, y, "CONTRACTS");
    if (rec->contract < MdK_COUNT)
    {
        const TbBool open = (rec->contract_open != 0);
        snprintf(text, sizeof(text), open ? "%s (renegotiating)" : "%s", md_contract_names[rec->contract]);
        y += mw_row(sh, md_contract_box, md_row_text[1], mine && open, sh->x, y, sh->w, text) + air;
        y += mw_text(sh, y, WwI_Dim, md_contract_does[rec->contract]);
    }

    /* --- Lessons: the lesson waiting (shown when no vice is) --- */
    if (!pending && (rec->lesson_learn != 0))
    {
        y += mw_heading(sh, y, "LESSONS");
        snprintf(text, sizeof(text), mine ? "Level %d lesson: pick one" : "Level %d lesson: not picked",
            MD_LESSON_STEP * (rec->lessons_done + 1));
        y += mw_line(sh, sh->x, y, sh->w, 1, WwI_Gold, text) + gap;
        for (int k = 0; k < 2; k++)
        {
            const int slot = ((k == 0) ? rec->lesson_learn : rec->lesson_master) - 1;
            if (slot < 0)
                continue;
            md_lesson_text(rec, crconf, k, text, sizeof(text));
            y += mw_pick(sh, k, mine, y, text, (k == 0) ? "as its kind would" : "quicker, harder; the next comes 10 later") + gap;
        }
        md_pick_lesson = 1;
    }

    /* --- the vices: its sin and virtues, the vices it has, the one to pick, those to come --- */
    y += mw_heading(sh, y, "VICES");
    /* Deadly Pairs: its sin */
    if (rec->sin != 0)
    {
        char sin_text[MD_SIN_TEXT_MAX];
        snprintf(text, sizeof(text), "Sin: %s", md_sin_text(0, rec->sin, sin_text));
        y += mw_line(sh, sh->x, y, sh->w, 1, WwI_Gold, text);
        y += mw_text(sh, y, WwI_Dim, md_sin_text(1, rec->sin, sin_text)) + MW_B4(8);
    } else if (flag_is_set(rec->flags, MdF_SinUsed))
        y += mw_line(sh, sh->x, y, sh->w, 1, WwI_Dim, "- a sin left in the grave -");
    /* Virtues: each one it has, and a rest after a correction */
    for (int v = 0; v < MD_VIRTUES; v++)
        if (rec->virtues & (1 << v))
        {
            snprintf(text, sizeof(text), "Virtue: %s, %s", md_virtue_names[v], md_virtue_does[v]);
            y += mw_text(sh, y, WwI_Dim, text);
        }
    if (md_resting(rec))
    {
        snprintf(text, sizeof(text), "Cured on the rack: no work until day %d", (int)rec->rest_day);
        y += mw_text(sh, y, WwI_Dim, text);
    }
    if ((rec->virtues != 0) || md_resting(rec))
        y += MW_B4(8);
    /* the vices it has, each a little apart from the next; two can be marked for the rack (diggers get no torture job:
       there is no rack for them) */
    const TbBool markable = mine && !pending && (count >= 2) && (rec->sin == 0) && !flag_is_set(rec->flags, MdF_SinUsed) &&
        !thing_is_creature_digger(thing);
    for (int i = 0; i < count; i++)
    {
        const int vice = rec->vices[i];
        const int rank = max(md_slot_rank(rec, i), 1);
        const int y0 = y;
        /* (a vice that can be marked is a button: its lines stand clear of the frame the button draws) */
        if (markable)
            y += MW_B4(6);
        snprintf(text, sizeof(text), "%s %s", md_vice_short[vice], md_roman[rank]);
        y += mw_line(sh, sh->x, y, sh->w, 1, WwI_Bright, text);
        if (rank < MD_RANKS)
        {
            /* Deeper Vices: the way to its next rank, a bar under the name and clear of it */
            const int bar_h = max(MW_B4(6), 2);
            y += MW_B4(5);
            if (sh->draw && mw_in(sh, y, bar_h, 0))
            {
                const uint32_t deeds = md_deeds(rec, vice) - min(rec->rank_base[i], md_deeds(rec, vice));
                const uint32_t from = md_rank_deeds(vice, rank), to = md_rank_deeds(vice, rank + 1);
                const unsigned pct = min((unsigned)((uint64_t)(deeds - min(deeds, from)) * 100 / max(to - from, 1u)), 99u);
                const int bar_w = sh->w / 3, bar_x = sh->x + (sh->w - bar_w) / 2;
                LbDrawBox(bar_x, y, bar_w, bar_h, world_window_ink(WwI_Rule));
                if (pct > 0)
                    LbDrawBox(bar_x, y, max(bar_w * (int)pct / 100, 1), bar_h, world_window_ink(WwI_Gold));
            }
            y += bar_h + MW_B4(3);
        }
        if (!pending)
        {
            md_vice_does_text(vice, rank, text, sizeof(text));
            y += mw_text(sh, y, WwI_Dim, text);
        }
        if (markable)
        {
            y += MW_B4(4);
            if (sh->draw && mw_in(sh, y0, y - y0, 0))
            {
                md_mark_box[i][0] = sh->x;
                md_mark_box[i][1] = y0;
                md_mark_box[i][2] = sh->w;
                md_mark_box[i][3] = y - y0;
            }
        }
        y += MW_B4(markable ? 8 : 10);
    }
    if (markable)
    {
        const int marks = md_bits(rec->rack_marks);
        if (marks < 2)
            snprintf(text, sizeof(text), "Mark two vices for the rack (%d)", marks);
        else
            snprintf(text, sizeof(text), "Torture Chamber melts them: %d%%",
                min((int)((uint32_t)rec->rack_samples * MD_SAMPLE * 100 / MD_RACK_TURNS), 99));
        y += mw_text(sh, y, WwI_Dim, text);
    }
    int next = md_used_slots(rec) - (int)rec->c[MdC_Spare]; /* (a melted pair's two slots are used, not waiting) */
    if (pending)
    {
        snprintf(text, sizeof(text), mine ? "Level %d vice: pick one" : "Level %d vice: not picked",
            md_vice_levels[min(md_used_slots(rec), MD_VICE_SLOTS - 1)]);
        y += mw_line(sh, sh->x, y, sh->w, 1, WwI_Gold, text) + gap;
        for (int k = 0; k < 2; k++)
        {
            if (rec->offer[k] == MdV_None)
                continue;
            y += mw_pick(sh, k, mine, y, md_vice_short[rec->offer[k]], md_vice_does[rec->offer[k]]) + gap;
        }
        next++;
    }
    if ((rec->blood != MdV_None) && (rec->blood < MdV_COUNT) && (count == 0) && !pending)
    {
        snprintf(text, sizeof(text), "Bad Blood: %s at level 10", md_vice_short[rec->blood]);
        y += mw_text(sh, y, WwI_Dim, text);
        next++;
    }
    const int end = y;
    /* what only fills the room that is left: the vices lost with a death, and those still to come */
    if (sh->draw)
    {
        const int line = MW_B4(mw_line4[sh->close]);
        for (unsigned k = 0; (k < rec->c[MdC_Spare]) && (next < MD_VICE_SLOTS) && (y + line <= sh->y_end); k++)
        {
            y += mw_line(sh, sh->x, y, sh->w, 1, WwI_Dim, "- a vice left in the grave -");
            next++;
        }
        for (int i = next; (i < MD_VICE_SLOTS) && (y + line <= sh->y_end); i++)
        {
            snprintf(text, sizeof(text), "- vice at level %d -", md_vice_levels[i]);
            y += mw_line(sh, sh->x, y, sh->w, 1, WwI_Dim, text);
        }
    }
    return end;
}

/** The creature window's Rap Sheet, in the box (x, y, w, h); upp is the window's scale. */
void world_misdeeds_window_draw(int x, int y, int w, int h, int upp)
{
    md_window = true;
    md_window_upp = upp;
    md_pick_box[0][2] = 0;
    md_pick_box[1][2] = 0;
    md_post_box[2] = 0;
    md_contract_box[2] = 0;
    md_regimen_box[2] = 0;
    md_scroll_box[2] = 0;
    md_sheet_over = 0;
    for (int i = 0; i < MD_VICE_SLOTS; i++)
        md_mark_box[i][2] = 0;
    if (!world_mode_active())
        return;
    const struct Thing *thing = md_panel_thing();
    if (thing_is_invalid(thing))
        return;
    const struct MdRec *rec = md_peek(thing);
    struct MdSheet sheet = {x, w, y, y + h, upp, MW_FULL, 0, false, 0};
    struct MdSheet *sh = &sheet;
    if (thing->index != md_sheet_thing)
    {
        md_sheet_thing = thing->index;
        md_sheet_scroll = 0;
    }
    /* the body's letters are never smaller than the plain font itself: the smallest size the body may have */
    const int least = (MW_FULL * world_window_plain_cap() * 64 + MW_CAP4 * upp - 1) / max(MW_CAP4 * upp, 1);
    if (rec == NULL)
    {
        sh->draw = true;
        sh->body = max(MW_FULL, least);
        md_window_cap = MW_B4(MW_CAP4);
        const int y1 = y + mw_heading(sh, y, "RAP SHEET");
        mw_text(sh, y1, WwI_Dim, "No record: a clean sheet.");
        return;
    }
    /* set wide; if that is too high set close; then at five sixths and at two thirds, wide and close (on a 2560 x 1440
       screen both draw letters twice the font, the first with more air); then the largest size down to the smallest
       allowed at which it fits; and if it fits at none, the smallest allowed, set close, with the bar: it scrolls */
    static const int steps[3] = {MW_FULL, MW_FULL * 5 / 6, MW_FULL * 2 / 3};
    TbBool fits = false;
    for (int i = 0; (i < 6) && !fits; i++)
    {
        if (steps[i / 2] < least)
            break;
        sh->body = steps[i / 2];
        sh->close = i % 2;
        fits = (md_sheet_run(sh, y, thing, rec) - y <= h);
    }
    if (!fits)
    {
        sh->close = 1;
        sh->body = least;
        fits = (md_sheet_run(sh, y, thing, rec) - y <= h);
        if (fits)
        {
            /* (a line broken in two at one size may stand whole at a smaller one, so the size is looked for) */
            int low = least, high = min(steps[2] - 1, MW_FULL * 4);
            while (low < high)
            {
                sh->body = (low + high + 1) / 2;
                if (md_sheet_run(sh, y, thing, rec) - y <= h)
                    low = sh->body;
                else
                    high = sh->body - 1;
            }
            sh->body = low;
        }
    }
    md_sheet_step = max(MW_B4(mw_line4[sh->close]), 1);
    md_sheet_page = max(h - 2 * md_sheet_step, md_sheet_step);
    if (!fits)
    {
        /* the bar takes its strip off the sheet's right; the sheet is measured again in what is left */
        const int bar_w = max(MW_U(MW_BAR), 9); /* (wide enough for its arrows on the smallest screen) */
        sh->w = w - bar_w - MW_U(MW_BAR_GAP);
        md_sheet_over = max(md_sheet_run(sh, y, thing, rec) - y - h, 0);
        if (md_sheet_over > 0)
        {
            md_scroll_box[0] = x + w - bar_w;
            md_scroll_box[1] = y;
            md_scroll_box[2] = bar_w;
            md_scroll_box[3] = h;
        }
    }
    md_sheet_scroll = max(0, min(md_sheet_scroll, md_sheet_over));
    sh->draw = true;
    md_window_cap = MW_B4(MW_CAP4);
    md_window_pick = MW_B4(mw_pick4[sh->close]);
    md_sheet_run(sh, y - md_sheet_scroll, thing, rec);
}

static void md_place_button(struct GuiButton *gbtn, const int *box);

/* The sheet's scroll bar (the window's own button): an arrow at each end (a line up, a line down), between them a
   thumb that shows where the room is in the sheet (a click above or below it: a page); the mouse wheel moves the
   sheet three lines. Not there while the whole sheet fits. */
static void md_scroll_by(int pixels)
{
    md_sheet_scroll = max(0, min(md_sheet_scroll + pixels, md_sheet_over));
}

/** The thumb's place in the bar: its top (from the bar's top) and its height. */
static void md_thumb(const struct GuiButton *gbtn, int *top, int *height)
{
    const int a = gbtn->width, track = max(gbtn->height - 2 * a, 1);
    const int room = max(gbtn->height, 1);
    *height = max(track * room / max(room + md_sheet_over, 1), min(track, 6));
    *top = a + (track - *height) * md_sheet_scroll / max(md_sheet_over, 1);
}

static void md_arrow(int x, int y, int a, TbBool up, TbBool lit)
{
    const TbPixel col = lit ? world_window_ink(WwI_Gold) : colours[6][5][3];
    const int rows = max(a / 3, 2);
    const int top = y + (a - rows) / 2;
    for (int i = 0; i < rows; i++)
    {
        const int half = (up ? i : rows - 1 - i) * max(a / 2 - 2, 1) / max(rows - 1, 1) + 1;
        LbDrawBox(x + a / 2 - half, top + i, 2 * half, 1, col);
    }
}

void world_misdeeds_scroll_maintain(struct GuiButton *gbtn)
{
    md_place_button(gbtn, md_scroll_box);
    if (!md_window || (md_sheet_over <= 0) || (!wheel_scrolled_up && !wheel_scrolled_down))
        return;
    /* the wheel, while the sheet scrolls: the sheet's, not the camera's zoom */
    md_scroll_by((wheel_scrolled_down ? 3 : -3) * md_sheet_step);
    wheel_scrolled_up = false;
    wheel_scrolled_down = false;
    clear_key_pressed(KC_MOUSEWHEEL_UP);
    clear_key_pressed(KC_MOUSEWHEEL_DOWN);
}

void world_misdeeds_scroll_draw(struct GuiButton *gbtn)
{
    if ((md_sheet_over <= 0) || (md_scroll_box[2] <= 0))
        return;
    const int u = max((md_window_upp + 8) / 32, 1);
    const TbBool over = flag_is_set(gbtn->flags, LbBtnF_MouseOver);
    const int x = gbtn->scr_pos_x, y = gbtn->scr_pos_y, w = gbtn->width, h = gbtn->height;
    int top, height;
    md_thumb(gbtn, &top, &height);
    LbDrawBox(x, y, w, h, colours[2][1][0]);
    LbDrawBox(x, y, u, h, colours[8][6][3]);
    LbDrawBox(x + w - u, y, u, h, colours[8][6][3]);
    LbDrawBox(x, y, w, u, colours[8][6][3]);
    LbDrawBox(x, y + h - u, w, u, colours[8][6][3]);
    LbDrawBox(x + 2 * u, y + top, max(w - 4 * u, 1), height, over ? world_window_ink(WwI_Gold) : colours[10][7][3]);
    md_arrow(x, y, w, true, md_sheet_scroll > 0);
    md_arrow(x, y + h - w, w, false, md_sheet_scroll < md_sheet_over);
}

void world_misdeeds_scroll_click(struct GuiButton *gbtn)
{
    if (md_sheet_over <= 0)
        return;
    const int at = (int)GetMouseY() - gbtn->pos_y; /* (the mouse, from the bar's top) */
    int top, height;
    md_thumb(gbtn, &top, &height);
    if (at < gbtn->width)
        md_scroll_by(-md_sheet_step);
    else if (at >= gbtn->height - gbtn->width)
        md_scroll_by(md_sheet_step);
    else if (at < top)
        md_scroll_by(-md_sheet_page);
    else if (at >= top + height)
        md_scroll_by(md_sheet_page);
}

static void md_outline(int x, int y, int w, int h, TbPixel col)
{
    LbDrawBox(x, y, w, 1, col);
    LbDrawBox(x, y + h - 1, w, 1, col);
    LbDrawBox(x, y, 1, h, col);
    LbDrawBox(x + w - 1, y, 1, h, col);
}

/** A Rap Sheet button goes where its row is drawn. A button has two places: where it is drawn (scr_pos) and where the
 *  mouse is tested against it (pos, check_if_pos_is_over_button). Only the first was set, so the click area of every
 *  row stayed where the panel's table has its button (the three rows of pay, training and post all at one place, the
 *  two picks at two fixed places) while the rows are drawn where the text above them ends: a click on a row landed on
 *  nothing, or on another row's button. No test saw it, because the tests sent the click's packet. (The World Boss
 *  button had the same fault; found by reading on 2026-11-14 and then shown with a played mouse.)
 *  Test switch KFX_WORLD_RAPOLDCLICK=1: the old way, to see the difference. */
static void md_place_button(struct GuiButton *gbtn, const int *box)
{
    static int old_way = -1;
    if (old_way < 0)
        old_way = (getenv("KFX_WORLD_RAPOLDCLICK") != NULL) ? 1 : 0;
    if (box[2] > 0)
    {
        gbtn->scr_pos_x = box[0];
        gbtn->scr_pos_y = box[1];
        if (!old_way)
        {
            gbtn->pos_x = box[0];
            gbtn->pos_y = box[1];
        }
        gbtn->width = box[2];
        gbtn->height = box[3];
        gbtn->flags |= LbBtnF_Visible | LbBtnF_Enabled;
    } else
    {
        gbtn->scr_pos_x = -4000;
        if (!old_way)
            gbtn->pos_x = -4000;
        gbtn->flags &= ~(LbBtnF_Visible | LbBtnF_Enabled);
    }
}

void world_misdeeds_mark_maintain(struct GuiButton *gbtn)
{
    md_place_button(gbtn, md_mark_box[(int)gbtn->content.lval % MD_VICE_SLOTS]);
}

void world_misdeeds_mark_draw(struct GuiButton *gbtn)
{
    const int k = (int)gbtn->content.lval % MD_VICE_SLOTS;
    const struct Thing *thing = md_panel_thing();
    const struct MdRec *rec = thing_is_invalid(thing) ? NULL : md_peek(thing);
    if ((rec == NULL) || (md_mark_box[k][2] <= 0))
        return;
    const TbBool marked = (rec->rack_marks & (1 << k)) != 0;
    const TbBool over = flag_is_set(gbtn->flags, LbBtnF_MouseOver);
    if (!marked && !over)
        return;
    /* (an outline only: the row's text lies under the button) */
    if (md_window)
    {
        /* the window's: a frame as thick as its stone buttons', gold under the mouse, red on a marked vice */
        const int u = max((md_window_upp + 8) / 32, 1);
        const TbPixel frame = marked ? colours[15][4][2] : world_window_ink(WwI_Gold);
        const int x = gbtn->scr_pos_x, y = gbtn->scr_pos_y, w = gbtn->width, h = gbtn->height;
        LbDrawBox(x, y, w, u, frame);
        LbDrawBox(x, y + h - u, w, u, frame);
        LbDrawBox(x, y, u, h, frame);
        LbDrawBox(x + w - u, y, u, h, frame);
        if (marked)
        {
            const int s = max(h / 5, 3);
            LbDrawBox(x + 3 * u, y + (h - s) / 2, s, s, frame);
        }
        return;
    }
    const TbPixel col = marked ? colours[15][4][2] : colours[6][4][2];
    md_outline(gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, gbtn->height, col);
    if (marked)
    {
        const int s = max(gbtn->height / 4, 3);
        LbDrawBox(gbtn->scr_pos_x + 2, gbtn->scr_pos_y + (gbtn->height - s) / 2, s, s, col);
    }
}

void world_misdeeds_mark_click(struct GuiButton *gbtn)
{
    const struct Thing *thing = md_panel_thing();
    if (thing_is_invalid(thing) || !is_my_player_number(thing->owner))
        return;
    set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, thing->index,
        (unsigned long)(2 + (gbtn->content.lval % MD_VICE_SLOTS)), 0, 0);
}

void world_misdeeds_regimen_maintain(struct GuiButton *gbtn)
{
    md_place_button(gbtn, (gbtn->content.lval == 2) ? md_post_box : (gbtn->content.lval == 1) ? md_contract_box : md_regimen_box);
}

void world_misdeeds_regimen_draw(struct GuiButton *gbtn)
{
    const int row = (gbtn->content.lval == 2) ? 2 : (gbtn->content.lval == 1) ? 1 : 0;
    const int *box = (row == 2) ? md_post_box : (row == 1) ? md_contract_box : md_regimen_box;
    if (box[2] <= 0)
        return;
    const TbBool over = flag_is_set(gbtn->flags, LbBtnF_MouseOver);
    if (md_window)
    {
        /* the window's: a stone button, gold-framed under the mouse, the row's words on its middle and clear of its frame */
        const int x = gbtn->scr_pos_x, y = gbtn->scr_pos_y, w = gbtn->width, h = gbtn->height;
        const int cap = md_window_cap, pad = max(h / 2, 2);
        md_stone(x, y, w, h, over);
        world_window_plain(x + pad, y + (h - cap) / 2, w - 2 * pad, cap, 1, world_window_ink(over ? WwI_Gold : WwI_Bright),
            md_row_text[row]);
        return;
    }
    if (over)
        md_outline(gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, gbtn->height, colours[6][4][2]);
}

void world_misdeeds_regimen_click(struct GuiButton *gbtn)
{
    const struct Thing *thing = md_panel_thing();
    const struct MdRec *rec = thing_is_invalid(thing) ? NULL : md_peek(thing);
    if ((rec == NULL) || !is_my_player_number(thing->owner))
        return;
    if (gbtn->content.lval == 2)
    {
        const int next = (rec->post + 1) % MdP_COUNT;
        set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, thing->index, (unsigned long)(60 + next), 0, 0);
        return;
    }
    if (gbtn->content.lval == 1)
    {
        const int next = (rec->contract + 1) % MdK_COUNT;
        set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, thing->index, (unsigned long)(50 + next), 0, 0);
        return;
    }
    const int next = (rec->regimen + 1) % MdR_COUNT;
    set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, thing->index, (unsigned long)(30 + next), 0, 0);
}

void world_misdeeds_pick_maintain(struct GuiButton *gbtn)
{
    md_place_button(gbtn, md_pick_box[(int)gbtn->content.lval & 1]);
}

void world_misdeeds_pick_draw(struct GuiButton *gbtn)
{
    const int k = (int)gbtn->content.lval & 1;
    const struct Thing *thing = md_panel_thing();
    const struct MdRec *rec = thing_is_invalid(thing) ? NULL : md_peek(thing);
    if (md_window)
    {
        /* the window's: a stone button, gold-framed under the mouse; what it is and what it does on its middle, clear
           of its frame */
        const TbBool lesson = (rec != NULL) && md_pick_lesson;
        if ((rec == NULL) || (md_pick_box[k][2] <= 0) || (!lesson && (rec->offer[k] == MdV_None)))
            return;
        char name[96];
        const char *does;
        if (lesson)
        {
            md_lesson_text(rec, creature_stats_get_from_thing(thing), k, name, sizeof(name));
            does = (k == 0) ? "as its kind would" : "quicker, harder; the next comes 10 later";
        } else
        {
            snprintf(name, sizeof(name), "%s", md_vice_short[rec->offer[k]]);
            does = md_vice_does[rec->offer[k]];
        }
        const TbBool over = flag_is_set(gbtn->flags, LbBtnF_MouseOver);
        const int x = gbtn->scr_pos_x, y = gbtn->scr_pos_y, w = gbtn->width, h = gbtn->height;
        const int cap = md_window_cap, pad = max(md_window_pick / 4, 2);
        char lines[MW_LINES][WORLD_WINDOW_LINE];
        int n, name_y, does_y, step;
        mw_pick_layout(w, cap, md_window_pick, does, lines, &n, &name_y, &does_y, &step);
        md_stone(x, y, w, h, over);
        world_window_plain(x + pad, y + name_y, w - 2 * pad, cap, 1, world_window_ink(over ? WwI_Gold : WwI_Bright), name);
        for (int i = 0; i < n; i++)
            world_window_plain(x + pad, y + does_y + i * step, w - 2 * pad, cap, 1, world_window_ink(WwI_Dim), lines[i]);
        return;
    }
    if ((rec != NULL) && md_pick_lesson && (md_pick_box[k][2] > 0))
    {
        const TbBool over = flag_is_set(gbtn->flags, LbBtnF_MouseOver);
        LbDrawBox(gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, gbtn->height, over ? colours[9][6][2] : colours[4][3][1]);
        LbDrawBox(gbtn->scr_pos_x + 1, gbtn->scr_pos_y + 1, gbtn->width - 2, gbtn->height - 2, over ? colours[5][3][1] : colours[2][1][0]);
        char text[96];
        md_lesson_text(rec, creature_stats_get_from_thing(thing), k, text, sizeof(text));
        md_text(gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, max(gbtn->height * 6 / 17, 6), 1, text);
        md_text(gbtn->scr_pos_x, gbtn->scr_pos_y + gbtn->height * 8 / 17, gbtn->width, max(gbtn->height * 5 / 17, 5), 1,
            (k == 0) ? "as its kind would" : "quicker, harder; the next comes 10 later");
        return;
    }
    if ((rec == NULL) || (rec->offer[k] == MdV_None) || (md_pick_box[k][2] <= 0))
        return;
    const TbBool over = flag_is_set(gbtn->flags, LbBtnF_MouseOver);
    LbDrawBox(gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, gbtn->height, over ? colours[9][6][2] : colours[4][3][1]);
    LbDrawBox(gbtn->scr_pos_x + 1, gbtn->scr_pos_y + 1, gbtn->width - 2, gbtn->height - 2, over ? colours[5][3][1] : colours[2][1][0]);
    const int name_px = max(gbtn->height * 6 / 17, 6);
    const int does_px = max(gbtn->height * 5 / 17, 5);
    md_text(gbtn->scr_pos_x, gbtn->scr_pos_y, gbtn->width, name_px, 1, md_vice_short[rec->offer[k]]);
    md_text(gbtn->scr_pos_x, gbtn->scr_pos_y + gbtn->height * 8 / 17, gbtn->width, does_px, 1, md_vice_does[rec->offer[k]]);
}

void world_misdeeds_pick_click(struct GuiButton *gbtn)
{
    const struct Thing *thing = md_panel_thing();
    if (thing_is_invalid(thing) || !is_my_player_number(thing->owner))
        return;
    set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, thing->index,
        (unsigned long)((md_pick_lesson ? 20 : 0) + (gbtn->content.lval & 1)), 0, 0);
}

void world_misdeeds_pencil_maintain(struct GuiButton *gbtn)
{
    const struct Thing *thing = md_panel_thing();
    gbtn->content.str = md_name_buf;
    gbtn->maxval = WORLD_MISDEED_NAME_MAX;
    if (world_mode_active() && !thing_is_invalid(thing) && is_my_player_number(thing->owner) &&
        (md_rename_refusal(thing->owner, thing) == NULL))
    {
        gbtn->flags |= LbBtnF_Visible | LbBtnF_Enabled;
    } else
    {
        gbtn->flags &= ~(LbBtnF_Visible | LbBtnF_Enabled);
        if (gbtn == input_button)
            kill_button_area_input();
    }
}

void world_misdeeds_pencil_draw(struct GuiButton *gbtn)
{
    const int x = gbtn->scr_pos_x, y = gbtn->scr_pos_y, s = max(gbtn->height, 8);
    if (gbtn == input_button)
    {
        /* the name being typed, over the name box */
        const int bw = s * 126 / 12, bx = x + gbtn->width - bw;
        char text[WORLD_MISDEED_NAME_MAX + 8];
        LbDrawBox(bx, y - 1, bw, s + 2, colours[15][13][6]);
        LbDrawBox(bx + 1, y, bw - 2, s, colours[1][1][0]);
        snprintf(text, sizeof(text), "%s%s", md_name_buf, ((LbTimerClock() / 250) & 1) ? "_" : " ");
        md_text(bx + 2, y + s / 8, bw - 4, s * 5 / 8, 1, text);
        return;
    }
    /* a pencil, drawn from boxes: tip at the bottom left, rubber at the top right */
    const int u = max(s / 8, 1);
    LbDrawBox(x, y, s, s, flag_is_set(gbtn->flags, LbBtnF_MouseOver) ? colours[7][5][2] : colours[3][2][1]);
    for (int i = 0; i < 6; i++)
    {
        const TbPixel col = (i == 0) ? colours[2][2][2] : (i == 1) ? colours[14][11][8] : (i == 5) ? colours[15][7][8] :
            colours[15][13][2];
        LbDrawBox(x + u + i * u, y + s - 2 * u - (i + 1) * u, 2 * u, 2 * u, col);
    }
}

void world_misdeeds_rename_done(struct GuiButton *gbtn)
{
    const struct Thing *thing = md_panel_thing();
    char name[WORLD_MISDEED_NAME_MAX + 1];
    const int len = md_clean_name(md_name_buf, name);
    md_name_buf[0] = 0;
    if (thing_is_invalid(thing) || (len <= 0) || !is_my_player_number(thing->owner))
        return;
    uint32_t p1, p2;
    md_pack_name(name, thing->index, &p1, &p2);
    set_players_packet_action(get_my_player(), PckA_WorldMisdeedName, p1, p2, 0, 0);
}

void world_misdeeds_draw_label(const struct Thing *thing, long scr_x, long top_y, int flower_h, TbBool under_hand)
{
    if (!world_mode_active() || (!under_hand && (thing->index != md_test_label_idx)))
        return;
    const char *name = world_misdeeds_shown_name(thing); /* (only a creature its keeper renamed shows a name) */
    if ((name == NULL) || (name[0] == 0))
        return;
    /* (text can't be drawn among the world's sprites: it is kept for the overlay pass of this frame) */
    md_label.px = min(max(flower_h / 4, 8), 14);
    md_label.x = (int)scr_x + local_state.engine_window_x; /* (the world is drawn in the view's own window) */
    md_label.y = (int)top_y + local_state.engine_window_y - md_label.px * 2;
    snprintf(md_label.text, sizeof(md_label.text), "%s", name);
}

void world_misdeeds_draw_overlay(void)
{
    if (md_label.text[0] == 0)
        return;
    const int w = md_label.px * 14;
    md_text(md_label.x - w / 2, max(md_label.y, 0), w, md_label.px, 1, md_label.text);
    md_label.text[0] = 0;
}

/******************************************************************************/
/* The test hook */

static struct Thing *md_test_creature(void)
{
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        if (thing_exists(thing) && (thing->class_id == TCls_Creature) && (thing->owner == my_player_number) &&
            !thing_is_creature_digger(thing) && (thing->health >= 0))
            return thing;
    }
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart))
        return INVALID_THING;
    struct Coord3d pos = heart->mappos;
    pos.x.val += 3 * COORD_PER_STL;
    pos.y.val += 6 * COORD_PER_STL;
    return create_creature(&pos, (ThingModel)get_id(creature_desc, "ORC"), my_player_number);
}

static void md_test_send_rename(const struct Thing *thing, const char *raw)
{
    char name[WORLD_MISDEED_NAME_MAX + 1];
    uint32_t p1, p2;
    md_clean_name(raw, name);
    md_pack_name(name, thing->index, &p1, &p2);
    SYNCLOG("Misdeeds test: rename packet '%s' for thing %d", name, (int)thing->index);
    set_players_packet_action(get_my_player(), PckA_WorldMisdeedName, p1, p2, 0, 0);
}

/** KFX_WORLD_MISDEEDTEST="turn,level,name[,tab]": drives the whole system on the local player's first creature that
 *  isn't an imp (an Orc is made when there is none). tab 0 leaves the panel on the Rap Sheet, 3 on the stats tab. */
/** The server's part of the Rap Sheet on thin clients: every fourth turn, the record of the creature each player's
 *  query panel shows goes into the game state, which the stream carries to the clients. */
static void md_fill_views(void)
{
    if (!netstate_is_server() || ((game.play_gameturn % 4) != 2))
        return;
    for (NetUserId u = 0; u < MAX_NET_USERS; u++)
    {
        const PlayerNumber p = get_net_user_player_number(u);
        if ((p < 0) || (p >= PLAYERS_MAX) || (p >= PLAYERS_COUNT))
            continue;
        const struct UserState *ustate = get_user_state(u);
        struct Thing *thing = (ustate != NULL) ? thing_get(ustate->controlled_thing_idx) : INVALID_THING;
        const struct MdRec *rec = (thing_exists(thing) && (thing->class_id == TCls_Creature)) ? md_peek(thing) : NULL;
        if (rec != NULL)
        {
            struct MdView view;
            md_view_pack(rec, &view);
            if ((game.world_misdeed_view_thing[p] != thing->index) ||
                (memcmp(game.world_misdeed_view[p], &view, sizeof(view)) != 0))
            {
                memset(game.world_misdeed_view[p], 0, sizeof(game.world_misdeed_view[p]));
                memcpy(game.world_misdeed_view[p], &view, sizeof(view));
                game.world_misdeed_view_thing[p] = thing->index;
            }
        } else if (game.world_misdeed_view_thing[p] != 0)
        {
            game.world_misdeed_view_thing[p] = 0;
            memset(game.world_misdeed_view[p], 0, sizeof(game.world_misdeed_view[p]));
        }
    }
}

/** Test hook KFX_WORLD_TESTUNITS="n,level": at turn 60 the local keeper gets n creatures of that level beside the
 *  heart (Orcs, Trolls, Demon Spawn, Warlocks, Mistresses, Dragons in turn), each with a few misdeeds on its record
 *  so that its vices are on offer. Nothing else is done for the player: the Rap Sheet, the pencil and the picks are
 *  theirs to try. Single player or the server's own seat only. */
static void md_testunits_hook(void)
{
    static int count = -1, level = 10;
    if (count < 0)
    {
        const char *v = getenv("KFX_WORLD_TESTUNITS");
        count = 0;
        if (v != NULL)
            sscanf(v, "%d,%d", &count, &level);
        count = max(0, min(count, 12));
        level = max(1, min(level, 100));
    }
    if ((count == 0) || (game.play_gameturn != 60))
        return;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart))
        return;
    static const char *const kinds[] = {"ORC", "TROLL", "DEMONSPAWN", "SORCEROR", "DARK_MISTRESS", "DRAGON"};
    int made = 0;
    for (int i = 0; i < count; i++)
    {
        const long model = get_id(creature_desc, kinds[i % 6]);
        if (model <= 0)
            continue;
        struct Coord3d pos = heart->mappos;
        pos.x.val += (2 + (i % 4)) * COORD_PER_STL;
        pos.y.val += (5 + (i / 4)) * COORD_PER_STL;
        struct Thing *thing = create_creature(&pos, (ThingModel)model, my_player_number);
        if (thing_is_invalid(thing))
            continue;
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        set_creature_level(thing, level - 1);
        struct MdRec *rec = md_get(thing, cctrl);
        if (rec != NULL)
        {
            /* different pasts, so different vices are offered */
            rec->c[MdC_KillsHero] += 3 + 2 * (i % 3);
            rec->c[MdC_NightFights] += 1 + 3 * ((i + 1) % 3);
            rec->c[MdC_Lava] += 10 * ((i + 2) % 3);
            rec->c[MdC_HomeFights] += 20 * (i % 2);
            rec->c[MdC_Soaked] += 500 + 400 * i;
            rec->c[MdC_Work] += 40 * i;
            md_check_offer(rec, thing, level);
        }
        made++;
    }
    SYNCLOG("Misdeeds test: %d creatures of level %d for player %d", made, level, (int)my_player_number);
}

/** Test hook KFX_WORLD_BODYTEST="T": at T a Graveyard east of the local keeper's heart and an Orc of level 30 with two
 *  vices, killed on the spot (its body should be carried in by the imps and the Orc come back with one vice). At
 *  T+1500 the Orc that came back is killed again (one vice: no body, lost for good), a new Orc arrives as if through
 *  the portal and is raised to level 10 (it should inherit the lost one's vice). */
static void md_bodytest_hook(void)
{
    static long base = -2;
    static int ranked = 0; /* "T,1": Deeper Vices: the Orc's Do-Gooder's Bane is at rank III, so it comes back a rank poorer */
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_BODYTEST");
        base = -1;
        if ((v != NULL) && (sscanf(v, "%ld,%d", &base, &ranked) < 1))
            base = -1;
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    if (t == 40)
    {
        for (int i = 0; i < MD_BODY_MAX; i++)
            if (md_body[i].rec != 0)
            {
                const struct Thing *deadtng = thing_get(md_body[i].thing);
                const PlayerNumber other = (PlayerNumber)((my_player_number == 1) ? 2 : 1);
                SYNCLOG("Misdeeds body test: the waiting body: its own keeper's imps may take it: %s; player %d's imps may take it: %s",
                    world_misdeeds_body_not_for(deadtng, my_player_number) ? "NO" : "yes", (int)other,
                    world_misdeeds_body_not_for(deadtng, other) ? "no" : "YES");
            }
        return;
    }
    if ((t != 0) && (t != 1500) && (t != 1600))
        return;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    const long orc = get_id(creature_desc, "ORC");
    if (!thing_exists(heart) || (orc <= 0))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    struct Coord3d pos = heart->mappos;
    pos.x.val += 4 * COORD_PER_STL;
    pos.y.val += 6 * COORD_PER_STL;
    if (t == 0)
    {
        /* a claimed path south from where the Orc falls, and the Graveyard at its end */
        for (int dy = 3; dy <= 4; dy++)
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx + 1), slab_subtile_center(hy + dy), my_player_number, 0);
        place_slab_type_on_map(SlbT_GRAVEYARD, slab_subtile_center(hx + 1), slab_subtile_center(hy + 5), my_player_number, 0);
        struct Room *room = create_room(my_player_number, (RoomKind)get_id(room_desc, "GRAVEYARD"), slab_subtile_center(hx + 1),
            slab_subtile_center(hy + 5));
        if (!room_is_invalid(room))
            do_room_integration(room);
        struct Thing *thing = create_creature(&pos, (ThingModel)orc, my_player_number);
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        struct MdRec *rec = (thing_is_invalid(thing) || creature_control_invalid(cctrl)) ? NULL : md_get(thing, cctrl);
        if (rec == NULL)
        {
            SYNCLOG("Misdeeds body test: no Orc");
            return;
        }
        set_creature_level(thing, 29);
        rec->vices[0] = MdV_Bane;
        rec->vices[1] = MdV_Territorial;
        md_slot_start(rec, 0);
        md_slot_start(rec, 1);
        if (ranked)
            rec->c[MdC_KillsHero] += md_rank_deeds(MdV_Bane, 3);
        SYNCLOG("Misdeeds body test: Graveyard %s, %s index %d of level 30 with two vices is killed", room_is_invalid(room) ? "MISSING" : "built",
            creature_own_name(thing), (int)thing->index);
        kill_creature(thing, INVALID_THING, -1, CrDed_NoUnconscious);
        return;
    }
    if (t == 1500)
    {
        int found = 0;
        for (long i = 1; i < THINGS_COUNT; i++)
        {
            struct Thing *thing = thing_get(i);
            if (!thing_exists(thing) || (thing->class_id != TCls_Creature) || (thing->model != orc) || (thing->owner != my_player_number))
                continue;
            const struct MdRec *rec = md_peek(thing);
            if (ranked && (rec != NULL) && (md_vice_count(rec) == 2))
            {
                md_log_ranks("the Orc that came back", rec);
                base = -1; /* (the ranked variant stops here) */
                return;
            }
            if ((rec == NULL) || (rec->c[MdC_Spare] == 0))
                continue;
            found++;
            SYNCLOG("Misdeeds body test: %s index %d is back: level %d, %d vices (%s), %u lost; killed again", creature_own_name(thing),
                (int)thing->index, creature_control_get_from_thing(thing)->exp_level + 1, md_vice_count(rec),
                md_vice_names[rec->vices[0]], (unsigned)rec->c[MdC_Spare]);
            kill_creature(thing, INVALID_THING, -1, CrDed_NoUnconscious);
            break;
        }
        if (found == 0)
        {
            SYNCLOG("Misdeeds body test: the Orc did NOT come back (%d bodies waiting)", md_body_count);
            for (int i = 0; i < MD_BODY_MAX; i++)
            {
                if (md_body[i].rec == 0)
                    continue;
                const struct Thing *deadtng = thing_get(md_body[i].thing);
                const struct Room *room = find_room_of_role_with_spare_capacity(my_player_number, RoRoF_DeadStorage, 1);
                const struct Dungeon *dungeon = get_dungeon(my_player_number);
                SYNCLOG("Misdeeds body test: body thing %d exists %d class %d state %d laid %d dragged %d ready %d revealed %d at %d,%d; "
                    "Graveyard with room %s (%d/%d); imps %d", (int)md_body[i].thing, (int)thing_exists(deadtng), (int)deadtng->class_id,
                    (int)deadtng->active_state, (int)deadtng->corpse.laid_to_rest, (int)thing_is_dragged_or_pulled(deadtng),
                    (int)corpse_ready_for_collection(deadtng), (int)thing_revealed(deadtng, my_player_number),
                    (int)deadtng->mappos.x.stl.num, (int)deadtng->mappos.y.stl.num, room_is_invalid(room) ? "none" : "yes",
                    room_is_invalid(room) ? 0 : (int)room->used_capacity, room_is_invalid(room) ? 0 : (int)room->total_capacity,
                    (int)dungeon->num_active_diggers);
                for (long n = 1; n < THINGS_COUNT; n++)
                {
                    struct Thing *imp = thing_get(n);
                    if (!thing_exists(imp) || (imp->class_id != TCls_Creature) || (imp->owner != my_player_number) ||
                        !thing_is_creature_digger(imp))
                        continue;
                    struct Coord3d rpos = imp->mappos, bpos = deadtng->mappos;
                    const TbBool has_pos = !room_is_invalid(room) && find_first_valid_position_for_thing_anywhere_in_room(imp, (struct Room *)room, &rpos);
                    const struct Room *pick = find_nearest_room_of_role_for_thing_with_spare_capacity(imp, imp->owner, RoRoF_DeadStorage, NavRtF_Default, 1);
                    SYNCLOG("Misdeeds body test: imp %d state %d: walk to the body %ld, to the Graveyard %ld (spot found %d), room search %s",
                        (int)imp->index, (int)imp->active_state, (long)get_thing_navigation_distance(imp, &bpos, 0),
                        (long)get_thing_navigation_distance(imp, &rpos, 0), (int)has_pos, room_is_invalid(pick) ? "NONE" : "found");
                    break;
                }
            }
        }
        return;
    }
    struct Thing *thing = create_creature(&pos, (ThingModel)orc, my_player_number);
    if (thing_is_invalid(thing))
        return;
    world_misdeeds_attracted(thing);
    set_creature_level(thing, 9);
    const struct MdRec *rec = md_peek(thing);
    SYNCLOG("Misdeeds body test: a new Orc index %d at level 10: waiting inheritance %s", (int)thing->index,
        (rec != NULL) ? md_vice_names[rec->blood] : "none (no record)");
}

static void md_log_ranks(const char *when, const struct MdRec *rec)
{
    char line[400] = "";
    for (int i = 0; i < MD_VICE_SLOTS; i++)
    {
        if (rec->vices[i] == MdV_None)
            continue;
        char does[64];
        const int r = md_slot_rank(rec, i);
        md_vice_does_text(rec->vices[i], r, does, sizeof(does));
        const size_t n = strlen(line);
        snprintf(line + n, sizeof(line) - n, "%s%s %s [%s]", (n > 0) ? "; " : "", md_vice_short[rec->vices[i]], md_roman[r], does);
    }
    SYNCLOG("Deeper Vices test: %s: %s", when, line);
}

/** Test hook KFX_WORLD_VICERANKTEST="T": at T the local keeper's first creature that isn't an imp (an Orc is made when
 *  there is none) goes to level 100 with all five vices, and deeds enough for ranks V, I, III, II and IV; the next
 *  sample shows the rank-ups. T+100 logs every rank and effect; T+120 puts a hero beside it (Do-Gooder's Bane V in the
 *  hits); T+300 drops ranks as Unfinished Business does, on a copy; T+400 saves and loads the records, and loads them
 *  once more as a save from before Deeper Vices. */
static void md_viceranktest_hook(void)
{
    static long base = -2;
    static ThingIndex idx = 0;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_VICERANKTEST");
        base = (v != NULL) ? atol(v) : -1;
        if (base >= 0)
            SYNCLOG("Deeper Vices test: KFX_WORLD_VICERANKTEST at turn %ld", base);
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    if (t == 0)
    {
        struct Thing *thing = md_test_creature();
        struct CreatureControl *cctrl = thing_is_invalid(thing) ? NULL : creature_control_get_from_thing(thing);
        struct MdRec *rec = ((cctrl == NULL) || creature_control_invalid(cctrl)) ? NULL : md_get(thing, cctrl);
        if (rec == NULL)
        {
            SYNCLOG("Deeper Vices test: no creature");
            base = -1;
            return;
        }
        idx = thing->index;
        set_creature_level(thing, 99);
        rec->offer[0] = rec->offer[1] = MdV_None;
        static const uint8_t vices[MD_VICE_SLOTS] = {MdV_Bane, MdV_Territorial, MdV_Night, MdV_Asbestos, MdV_Shortcut};
        static const uint32_t deeds[MD_VICE_SLOTS] = {1500, 0, 150, 1000, 14000}; /* ranks V, I, III, II, IV */
        for (int i = 0; i < MD_VICE_SLOTS; i++)
        {
            rec->vices[i] = vices[i];
            md_slot_start(rec, i);
        }
        for (int i = 0; i < MD_VICE_SLOTS; i++)
            rec->c[md_vice_source[vices[i]]] += deeds[i];
        SYNCLOG("Deeper Vices test: %s index %d at level 100 with five vices and their deeds", thing_model_name(thing), (int)idx);
        return;
    }
    struct Thing *thing = thing_get(idx);
    const struct MdRec *rec = thing_exists(thing) ? md_peek(thing) : NULL;
    if (rec == NULL)
    {
        if ((t == 100) || (t == 300) || (t == 400))
            SYNCLOG("Deeper Vices test: the creature or its record is gone at T+%ld", t);
        return;
    }
    if (t == 100)
        md_log_ranks("ranks", rec);
    if (t == 120)
    {
        struct Coord3d pos = thing->mappos;
        pos.x.val += COORD_PER_STL;
        struct Thing *hero = create_creature(&pos, (ThingModel)get_id(creature_desc, "ARCHER"), PLAYER_GOOD);
        SYNCLOG("Deeper Vices test: hero %s beside it", thing_is_invalid(hero) ? "none" : thing_model_name(hero));
    }
    if (t == 300)
    {
        struct MdRec copy = *rec;
        for (int k = 0; k < 4; k++)
        {
            const int slot = md_lose_rank(&copy);
            SYNCLOG("Deeper Vices test: rank lost %d: slot %d (%s), now rank %s", k + 1, slot,
                (slot >= 0) ? md_vice_short[copy.vices[slot]] : "-", (slot >= 0) ? md_roman[md_slot_rank(&copy, slot)] : "-");
        }
        struct MdRec plain = *rec;
        for (int i = 0; i < MD_VICE_SLOTS; i++)
            md_slot_start(&plain, i);
        SYNCLOG("Deeper Vices test: with every vice at rank I a rank loss gives %d (expected -1: a whole vice goes)",
            md_lose_rank(&plain));
    }
    if (t == 400)
    {
        const uint16_t id = creature_control_get_from_thing(thing)->misdeed_id;
        const struct MdRec before = *rec;
        const size_t size = world_misdeeds_save_size();
        uint8_t *buf = (uint8_t *)malloc(size + 64);
        if (buf == NULL)
            return;
        world_misdeeds_save(buf);
        const TbBool loaded = world_misdeeds_load(buf, size);
        const struct MdRec *after = md_rec(id);
        SYNCLOG("Deeper Vices test: save %d bytes, loaded %d, record %s", (int)size, (int)loaded,
            ((after != NULL) && (memcmp(after, &before, sizeof(before)) == 0)) ? "IDENTICAL" : "DIFFERENT");
        if (after != NULL)
            md_log_ranks("after the load", after);
        /* the same records as a save from before Deeper Vices: each record cut to its old size */
        const uint32_t top = ((const uint32_t *)buf)[2];
        uint8_t *old = (uint8_t *)malloc(12 + (size_t)top * MD_REC_SIZE_BEFORE_RANKS + 4);
        if (old != NULL)
        {
            const uint32_t head[3] = {MD_SAVE_MAGIC, MD_REC_SIZE_BEFORE_RANKS, top};
            memcpy(old, head, 12);
            for (uint32_t i = 0; i < top; i++)
                memcpy(old + 12 + (size_t)i * MD_REC_SIZE_BEFORE_RANKS, buf + 12 + (size_t)i * sizeof(struct MdRec),
                    MD_REC_SIZE_BEFORE_RANKS);
            world_misdeeds_load(old, 12 + (size_t)top * MD_REC_SIZE_BEFORE_RANKS);
            if (md_rec(id) != NULL)
                md_log_ranks("loaded as an old save", md_rec(id));
            free(old);
        }
        world_misdeeds_load(buf, size); /* back as it was */
        free(buf);
    }
}

/** Test hook KFX_WORLD_SINTEST="T[,a,b]": at T the local keeper's first creature that isn't an imp (an Orc is made when
 *  there is none) goes to level 30 with vices a and b (numbers 1-5 as on the Rap Sheet's order of the vices; default
 *  1,3), and a one-slab Torture Chamber is built beside the heart. T+10 and T+12 mark both slots for the rack through
 *  the packets, T+14 tries a third mark (refused), T+20 puts it on the rack. After the melt (about 400 turns) it is
 *  taken off the rack; T+600 logs its sin and does a records save round trip; T+620 puts a hero beside it. */
static void md_sintest_hook(void)
{
    static long base = -2;
    static int va = MdV_Bane, vb = MdV_Night, kind = 0; /* kind: 0 the first creature (or an Orc), 1 a Dark Mistress, 2 an imp */
    static ThingIndex idx = 0;
    static TbBool off_rack = false;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_SINTEST");
        base = -1;
        if ((v != NULL) && (sscanf(v, "%ld,%d,%d,%d", &base, &va, &vb, &kind) >= 1))
            SYNCLOG("Deadly Pairs test: KFX_WORLD_SINTEST at turn %ld", base); /* (no vice numbers: with the sin's they give a pair away) */
        else
            base = -1;
        va = max(1, min(va, MdV_COUNT - 1));
        vb = max(1, min(vb, MdV_COUNT - 1));
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (t == 0)
    {
        struct Thing *thing = INVALID_THING;
        if ((kind != 0) && thing_exists(heart))
        {
            struct Coord3d pos = heart->mappos;
            pos.x.val += 3 * COORD_PER_STL;
            pos.y.val += 6 * COORD_PER_STL;
            thing = create_creature(&pos, (ThingModel)get_id(creature_desc, (kind == 1) ? "DARK_MISTRESS" : "IMP"), my_player_number);
        } else
            thing = md_test_creature();
        struct CreatureControl *cctrl = thing_is_invalid(thing) ? NULL : creature_control_get_from_thing(thing);
        struct MdRec *rec = ((cctrl == NULL) || creature_control_invalid(cctrl)) ? NULL : md_get(thing, cctrl);
        if ((rec == NULL) || !thing_exists(heart))
        {
            SYNCLOG("Deadly Pairs test: no creature or heart");
            base = -1;
            return;
        }
        idx = thing->index;
        set_creature_level(thing, 29);
        memset(rec->vices, 0, sizeof(rec->vices));
        rec->offer[0] = rec->offer[1] = MdV_None;
        rec->vices[0] = (uint8_t)va;
        rec->vices[1] = (uint8_t)vb;
        md_slot_start(rec, 0);
        md_slot_start(rec, 1);
        const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
        for (int dy = 3; dy <= 4; dy++)
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx - 1), slab_subtile_center(hy + dy), my_player_number, 0);
        place_slab_type_on_map(SlbT_TORTURE, slab_subtile_center(hx - 1), slab_subtile_center(hy + 5), my_player_number, 0);
        struct Room *room = create_room(my_player_number, (RoomKind)get_id(room_desc, "TORTURE"), slab_subtile_center(hx - 1),
            slab_subtile_center(hy + 5));
        if (!room_is_invalid(room))
            do_room_integration(room);
        SYNCLOG("Deadly Pairs test: %s index %d at level 30 with two vices; Torture Chamber %s", thing_model_name(thing),
            (int)idx, room_is_invalid(room) ? "MISSING" : "built");
        return;
    }
    struct Thing *thing = thing_get(idx);
    const struct MdRec *rec = thing_exists(thing) ? md_peek(thing) : NULL;
    if ((rec == NULL) || !thing_exists(heart))
    {
        if ((t == 20) || (t == 600))
            SYNCLOG("Deadly Pairs test: the creature or its record is gone at T+%ld", t);
        return;
    }
    if ((t == 10) || (t == 12) || (t == 14))
        set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, idx, (unsigned long)(2 + (t - 10) / 2), 0, 0);
    if (t == 20)
    {
        const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(hx - 1));
        pos.y.val = subtile_coord_center(slab_subtile_center(hy + 5));
        pos.z.val = get_floor_height_at(&pos);
        move_thing_in_map(thing, &pos);
        initialise_thing_state(thing, CrSt_CreatureBeingDropped); /* as the hand drops it: stock picks the torture job */
        reveal_map_area(my_player_number, slab_subtile(hx - 1, 0), slab_subtile(hx - 1, 2), slab_subtile(hy + 3, 0),
            slab_subtile(hy + 5, 2));
    }
    if (t == 60)
        SYNCLOG("Deadly Pairs test: dropped in the Torture Chamber: state %d (tortured %d), marks %d",
            (int)get_creature_state_besides_interruptions(thing), (int)creature_is_being_tortured_including_kinky(thing),
            md_bits(rec->rack_marks));
    if ((t > 20) && (t < 600) && !off_rack && (rec->sin != 0))
    {
        off_rack = true;
        SYNCLOG("Deadly Pairs test: melted at T+%ld (health %d, anger %ld); still tortured: %d", t, (int)thing->health,
            (long)creature_control_get_from_thing(thing)->annoyance_level[AngR_Other],
            (int)creature_is_being_tortured_including_kinky(thing));
    }
    if (t == 600)
    {
        char n0[MD_SIN_TEXT_MAX], n1[MD_SIN_TEXT_MAX];
        SYNCLOG("Deadly Pairs test: sin %d, %d vices left, slots used %d, lava-proof %d, marks %d; its texts decode to %d and %d "
            "characters", (int)rec->sin, md_vice_count(rec), md_used_slots(rec), (int)world_misdeeds_lava_proof(thing),
            md_bits(rec->rack_marks), (int)strlen(md_sin_text(0, rec->sin, n0)), (int)strlen(md_sin_text(1, rec->sin, n1)));
        const uint16_t id = creature_control_get_from_thing(thing)->misdeed_id;
        const struct MdRec before = *rec;
        const size_t size = world_misdeeds_save_size();
        uint8_t *buf = (uint8_t *)malloc(size + 64);
        if (buf != NULL)
        {
            world_misdeeds_save(buf);
            world_misdeeds_load(buf, size);
            free(buf);
            const struct MdRec *after = md_rec(id);
            SYNCLOG("Deadly Pairs test: records save round trip: %s", ((after != NULL) && (memcmp(after, &before, sizeof(before)) == 0)) ?
                "IDENTICAL" : "DIFFERENT");
        }
    }
    if (t == 620)
    {
        struct Coord3d pos = thing->mappos;
        pos.x.val += COORD_PER_STL;
        struct Thing *hero = create_creature(&pos, (ThingModel)get_id(creature_desc, "ARCHER"), PLAYER_GOOD);
        SYNCLOG("Deadly Pairs test: hero %s beside it (day: %d)", thing_is_invalid(hero) ? "none" : thing_model_name(hero),
            (int)world_is_day());
    }
}

/** Test hook KFX_WORLD_VIRTUETEST="T": at T the local keeper's first creature that isn't an imp (an Orc is made when
 *  there is none) goes to level 30 with the habits of all three virtues on its record (they arrive one a sample);
 *  T+100 logs them and their effects (the fear threshold, training, a blow on a knocked-out hero); T+140 puts it on the
 *  rack of a one-slab Torture Chamber beside the heart; T+200 its keeper casts Speed on it (the correction); T+260 logs
 *  the rest and does a records save round trip. */
static void md_virtuetest_hook(void)
{
    static long base = -2;
    static ThingIndex idx = 0;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_VIRTUETEST");
        base = (v != NULL) ? atol(v) : -1;
        if (base >= 0)
            SYNCLOG("Virtues test: KFX_WORLD_VIRTUETEST at turn %ld", base);
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (t == 0)
    {
        struct Thing *thing = md_test_creature();
        struct CreatureControl *cctrl = thing_is_invalid(thing) ? NULL : creature_control_get_from_thing(thing);
        struct MdRec *rec = ((cctrl == NULL) || creature_control_invalid(cctrl)) ? NULL : md_get(thing, cctrl);
        if ((rec == NULL) || !thing_exists(heart))
        {
            SYNCLOG("Virtues test: no creature or heart");
            base = -1;
            return;
        }
        idx = thing->index;
        set_creature_level(thing, 29);
        rec->flights += MD_PRUDENCE_FLIGHTS;
        rec->slept_raids += MD_TEMPERANCE_SLEPT;
        rec->knockouts += MD_MERCY_KNOCKOUTS;
        SYNCLOG("Virtues test: %s index %d at level 30 with the habits of three virtues", thing_model_name(thing), (int)idx);
        return;
    }
    struct Thing *thing = thing_get(idx);
    struct MdRec *rec = thing_exists(thing) ? md_peek(thing) : NULL;
    if ((rec == NULL) || !thing_exists(heart))
    {
        if ((t == 100) || (t == 200) || (t == 260))
            SYNCLOG("Virtues test: the creature or its record is gone at T+%ld", t);
        return;
    }
    if (t == 100)
    {
        struct Coord3d pos = thing->mappos;
        pos.x.val += COORD_PER_STL;
        struct Thing *hero = create_creature(&pos, (ThingModel)get_id(creature_desc, "ARCHER"), PLAYER_GOOD);
        HitPoints blow = -1;
        if (!thing_is_invalid(hero))
        {
            make_creature_unconscious(hero);
            blow = world_misdeeds_adjust_damage(hero, thing, 100);
        }
        SYNCLOG("Virtues test: virtues %02x; fear threshold 0 -> %ld; training %+d%%; a blow of 100 on a knocked-out hero does %d",
            (unsigned)rec->virtues, world_misdeeds_fear(thing, 0), world_misdeeds_train_bonus(thing), (int)blow);
    }
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    if (t == 140)
    {
        for (int dy = 3; dy <= 4; dy++)
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx - 1), slab_subtile_center(hy + dy), my_player_number, 0);
        place_slab_type_on_map(SlbT_TORTURE, slab_subtile_center(hx - 1), slab_subtile_center(hy + 5), my_player_number, 0);
        struct Room *room = create_room(my_player_number, (RoomKind)get_id(room_desc, "TORTURE"), slab_subtile_center(hx - 1),
            slab_subtile_center(hy + 5));
        if (!room_is_invalid(room))
            do_room_integration(room);
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(hx - 1));
        pos.y.val = subtile_coord_center(slab_subtile_center(hy + 5));
        pos.z.val = get_floor_height_at(&pos);
        move_thing_in_map(thing, &pos);
        initialise_thing_state(thing, CrSt_AtTortureRoom);
        reveal_map_area(my_player_number, slab_subtile(hx - 1, 0), slab_subtile(hx - 1, 2), slab_subtile(hy + 3, 0),
            slab_subtile(hy + 5, 2)); /* (a keeper's own Torture Chamber is always in sight; this one was built by the test) */
        SYNCLOG("Virtues test: Torture Chamber %s; on the rack", room_is_invalid(room) ? "MISSING" : "built");
    }
    if (t == 200)
    {
        SYNCLOG("Virtues test: Speed on it: on the subtile %d, on the thing %d, revealed %d", (int)can_cast_power_at_xy(my_player_number,
            PwrK_SPEEDCRTR, thing->mappos.x.stl.num, thing->mappos.y.stl.num, 0), (int)can_cast_power_on_thing(my_player_number, thing,
            PwrK_SPEEDCRTR), (int)thing_revealed(thing, my_player_number));
        {
            /* a slap on the rack goes through the same function, and must not cure (audit 2026-11-13) */
            const uint8_t before = rec->virtues;
            magic_use_power_on_thing(my_player_number, PwrK_SLAP, 0, 0, 0, thing, PwMod_CastForFree);
            SYNCLOG("Virtues test: a slap on the rack: virtues %02x -> %02x", (unsigned)before, (unsigned)rec->virtues);
        }
        const TbResult ret = magic_use_power_on_thing(my_player_number, PwrK_SPEEDCRTR, 1, 0, 0, thing, PwMod_CastForFree);
        SYNCLOG("Virtues test: Speed cast on it on the rack: %d (state %d); virtues %02x, rest until day %d, work %d%%, training %+d%%",
            (int)ret, (int)get_creature_state_besides_interruptions(thing), (unsigned)rec->virtues, (int)rec->rest_day,
            world_misdeeds_work_percent(thing), world_misdeeds_train_bonus(thing));
        SYNCLOG("Virtues test: resting %d, so it may be trained: %d", (int)world_misdeeds_resting(thing), (int)creature_can_be_trained(thing));
        struct Coord3d pos = heart->mappos;
        pos.x.val += 3 * COORD_PER_STL;
        pos.y.val += 3 * COORD_PER_STL;
        move_thing_in_map(thing, &pos);
        set_start_state(thing);
    }
    if (t == 260)
    {
        const uint16_t id = creature_control_get_from_thing(thing)->misdeed_id;
        const struct MdRec before = *rec;
        const size_t size = world_misdeeds_save_size();
        uint8_t *buf = (uint8_t *)malloc(size + 64);
        if (buf != NULL)
        {
            world_misdeeds_save(buf);
            world_misdeeds_load(buf, size);
            free(buf);
            const struct MdRec *after = md_rec(id);
            SYNCLOG("Virtues test: records save round trip: %s; day %ld", ((after != NULL) && (memcmp(after, &before, sizeof(before)) == 0)) ?
                "IDENTICAL" : "DIFFERENT", world_day_number());
        }
    }
}

static void md_log_abilities(const char *when, const struct Thing *thing, const struct MdRec *rec)
{
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    char line[512] = "";
    for (int i = 0; i < LEARNED_INSTANCES_COUNT; i++)
    {
        const int k = crconf->learned_instance_id[i];
        if (k <= 0)
            continue;
        const size_t n = strlen(line);
        snprintf(line + n, sizeof(line) - n, "%s%s@%d->%d%s%s", (n > 0) ? ", " : "", creature_instance_code_name(k),
            (int)crconf->learned_instance_level[i], md_learn_level(rec, i, crconf->learned_instance_level[i]),
            cctrl->instance_available[k] ? " known" : "", (rec->mastery[i] > 0) ? " MASTERED" : "");
    }
    SYNCLOG("Lessons test: %s (level %d, decided %d, offer learn %d master %d): %s", when, cctrl->exp_level + 1,
        (int)rec->lessons_done, (int)rec->lesson_learn - 1, (int)rec->lesson_master - 1, line);
}

/** Test hook KFX_WORLD_LESSONTEST="T": at T a Warlock beside the local keeper's heart is named and raised to level 10
 *  (its level-10 ability waits for the lesson); T+40 the keeper masters (through the packet); T+80 it goes to level 20
 *  and T+120 learns; T+160 logs a mastered reset time and puts a hero beside it; T+400 a records round trip. */
static void md_lessontest_hook(void)
{
    static long base = -2;
    static ThingIndex idx = 0;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_LESSONTEST");
        base = (v != NULL) ? atol(v) : -1;
        if (base >= 0)
            SYNCLOG("Lessons test: KFX_WORLD_LESSONTEST at turn %ld", base);
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    if (t == 0)
    {
        const struct Thing *heart = get_player_soul_container(my_player_number);
        if (!thing_exists(heart))
            return;
        struct Coord3d pos = heart->mappos;
        pos.x.val += 3 * COORD_PER_STL;
        pos.y.val += 5 * COORD_PER_STL;
        struct Thing *thing = create_creature(&pos, (ThingModel)get_id(creature_desc, "SORCEROR"), my_player_number);
        struct CreatureControl *cctrl = thing_is_invalid(thing) ? NULL : creature_control_get_from_thing(thing);
        struct MdRec *rec = ((cctrl == NULL) || creature_control_invalid(cctrl)) ? NULL : md_get(thing, cctrl);
        if (rec == NULL)
        {
            base = -1;
            return;
        }
        idx = thing->index;
        rec->flags |= MdF_Renamed; /* (as a rename does; its lessons start at level 1) */
        rec->lessons_done = 0;
        set_creature_level(thing, 9);
        md_log_abilities("named, level 10", thing, rec);
        return;
    }
    struct Thing *thing = thing_get(idx);
    struct MdRec *rec = thing_exists(thing) ? md_peek(thing) : NULL;
    if (rec == NULL)
        return;
    if ((t == 40) || (t == 120))
    {
        md_log_abilities((t == 40) ? "before mastering" : "before learning", thing, rec);
        set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, idx, (t == 40) ? 21 : 20, 0, 0);
    }
    if (t == 42)
        md_log_abilities("after mastering", thing, rec);
    if (t == 80)
    {
        set_creature_level(thing, 19);
        md_log_abilities("level 20", thing, rec);
    }
    if (t == 122)
        md_log_abilities("after learning", thing, rec);
    if (t == 160)
    {
        const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
        for (int i = 0; i < LEARNED_INSTANCES_COUNT; i++)
            if (rec->mastery[i] > 0)
            {
                const struct InstanceInfo *inst_inf = creature_instance_info_get(crconf->learned_instance_id[i]);
                int32_t ritime = inst_inf->reset_time;
                world_misdeeds_reset_time(thing, crconf->learned_instance_id[i], &ritime);
                SYNCLOG("Lessons test: %s mastered %d: reset time %d -> %d; range %d -> %ld; its shot model %d", creature_instance_code_name(
                    crconf->learned_instance_id[i]), (int)rec->mastery[i], (int)inst_inf->reset_time, (int)ritime,
                    (int)inst_inf->range_max, world_misdeeds_inst_range(thing, crconf->learned_instance_id[i], inst_inf->range_max),
                    (int)md_instance_shot(crconf->learned_instance_id[i]));
            }
        struct Coord3d pos = thing->mappos;
        pos.x.val += 2 * COORD_PER_STL;
        create_creature(&pos, (ThingModel)get_id(creature_desc, "KNIGHT"), PLAYER_GOOD);
    }
    if (t == 400)
    {
        const uint16_t id = creature_control_get_from_thing(thing)->misdeed_id;
        const struct MdRec before = *rec;
        const size_t size = world_misdeeds_save_size();
        uint8_t *buf = (uint8_t *)malloc(size + 64);
        if (buf != NULL)
        {
            world_misdeeds_save(buf);
            world_misdeeds_load(buf, size);
            free(buf);
            const struct MdRec *after = md_rec(id);
            SYNCLOG("Lessons test: records save round trip: %s", ((after != NULL) && (memcmp(after, &before, sizeof(before)) == 0)) ?
                "IDENTICAL" : "DIFFERENT");
            /* the thin client's view of it, packed and unpacked */
            struct MdView view;
            struct MdRec back;
            md_view_pack(&before, &view);
            md_view_unpack(&view, &back);
            int same = (memcmp(back.mastery, before.mastery, sizeof(back.mastery)) == 0) && (back.lessons_done == before.lessons_done);
            for (int i = 0; i < MD_VICE_SLOTS; i++)
                same = same && (md_slot_rank(&back, i) == md_slot_rank(&before, i)) && (md_rank_pct(&back, i) == md_rank_pct(&before, i));
            SYNCLOG("Lessons test: thin-client view %d bytes, what the Rap Sheet draws %s", (int)sizeof(view), same ? "SAME" : "DIFFERS");
        }
    }
}

static void md_log_stats(const char *when, struct Thing *thing)
{
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    SYNCLOG("Regimens test: %s: level %d, strength %ld, defence %ld, dexterity %ld, max health %ld, state %d", when,
        cctrl->exp_level + 1, calculate_correct_creature_strength(thing), calculate_correct_creature_defense(thing),
        calculate_correct_creature_dexterity(thing), (long)calculate_correct_creature_max_health(thing),
        (int)get_creature_state_besides_interruptions(thing));
}

/** Test hook KFX_WORLD_REGIMENTEST="T": at T an Orc of level 10 beside the local keeper's heart is put on Brawn
 *  (through the packet) and in a Training Room built beside it; T+40 it gains five levels there; T+100 it goes on Guard
 *  and T+110 gains five more; T+200 logs its stats, the stats of the same Orc outside a world's regimens, and a records
 *  and thin-view round trip. T+240 it gains ten more on Guard: five fill the fifteen levels it keeps, five take the
 *  place of its Brawn levels (T+300 logs the levels and the stats). */
static void md_regimentest_hook(void)
{
    static long base = -2;
    static ThingIndex idx = 0;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_REGIMENTEST");
        base = (v != NULL) ? atol(v) : -1;
        if (base >= 0)
            SYNCLOG("Regimens test: KFX_WORLD_REGIMENTEST at turn %ld", base);
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    if (t == 0)
    {
        for (int dy = 3; dy <= 4; dy++)
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx + 1), slab_subtile_center(hy + dy), my_player_number, 0);
        place_slab_type_on_map(SlbT_TRAINING, slab_subtile_center(hx + 1), slab_subtile_center(hy + 5), my_player_number, 0);
        struct Room *room = create_room(my_player_number, (RoomKind)get_id(room_desc, "TRAINING"), slab_subtile_center(hx + 1),
            slab_subtile_center(hy + 5));
        if (!room_is_invalid(room))
            do_room_integration(room);
        reveal_map_area(my_player_number, slab_subtile(hx + 1, 0), slab_subtile(hx + 2, 2), slab_subtile(hy + 3, 0), slab_subtile(hy + 5, 2));
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(hx + 1));
        pos.y.val = subtile_coord_center(slab_subtile_center(hy + 5));
        pos.z.val = get_floor_height_at(&pos);
        struct Thing *thing = create_creature(&pos, (ThingModel)get_id(creature_desc, "ORC"), my_player_number);
        struct CreatureControl *cctrl = thing_is_invalid(thing) ? NULL : creature_control_get_from_thing(thing);
        if ((cctrl == NULL) || creature_control_invalid(cctrl) || (md_get(thing, cctrl) == NULL))
        {
            base = -1;
            return;
        }
        idx = thing->index;
        set_creature_level(thing, 9);
        SYNCLOG("Regimens test: Training Room %s", room_is_invalid(room) ? "MISSING" : "built");
        md_log_stats("start", thing);
        set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, idx, 30 + MdR_Brawn, 0, 0);
        return;
    }
    struct Thing *thing = thing_get(idx);
    struct MdRec *rec = thing_exists(thing) ? md_peek(thing) : NULL;
    if (rec == NULL)
        return;
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if ((t == 10) || (t == 105) || (t == 205))
    {
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(hx + 1));
        pos.y.val = subtile_coord_center(slab_subtile_center(hy + 5));
        pos.z.val = get_floor_height_at(&pos);
        move_thing_in_map(thing, &pos);
        initialise_thing_state(thing, CrSt_AtTrainingRoom);
    }
    if ((t == 40) || (t == 140))
    {
        md_log_stats("training, before five levels", thing);
        set_creature_level(thing, cctrl->exp_level + 5);
    }
    if (t == 240)
    {
        md_log_stats("training, before ten more levels on Guard", thing);
        set_creature_level(thing, cctrl->exp_level + 10);
    }
    if (t == 300)
    {
        md_log_stats("after ten more on Guard", thing);
        SYNCLOG("Regimens test: its regimen levels: Brawn %d, Guard %d, Aim %d, Grit %d (it keeps %d in all): Str %+d%%, Def %+d%%, "
            "Dex %+d%%, HP %+d%%", (int)rec->reg_levels[0], (int)rec->reg_levels[1], (int)rec->reg_levels[2], (int)rec->reg_levels[3],
            MD_REG_LEVELS, md_regimen_pct(rec, MdR_Brawn) - 100, md_regimen_pct(rec, MdR_Guard) - 100,
            md_regimen_pct(rec, MdR_Aim) - 100, md_regimen_pct(rec, MdR_Grit) - 100);
    }
    if (t == 100)
    {
        md_log_stats("after Brawn", thing);
        set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, idx, 30 + MdR_Guard, 0, 0);
    }
    if (t == 200)
    {
        md_log_stats("after Guard", thing);
        const uint8_t kept[4] = {rec->reg_levels[0], rec->reg_levels[1], rec->reg_levels[2], rec->reg_levels[3]};
        memset(rec->reg_levels, 0, sizeof(rec->reg_levels));
        md_log_stats("the same Orc without its regimens", thing);
        memcpy(rec->reg_levels, kept, sizeof(kept));
        const uint16_t id = cctrl->misdeed_id;
        const struct MdRec before = *rec;
        const size_t size = world_misdeeds_save_size();
        uint8_t *buf = (uint8_t *)malloc(size + 64);
        if (buf != NULL)
        {
            world_misdeeds_save(buf);
            world_misdeeds_load(buf, size);
            free(buf);
            const struct MdRec *after = md_rec(id);
            struct MdView view;
            struct MdRec back;
            md_view_pack(&before, &view);
            md_view_unpack(&view, &back);
            SYNCLOG("Regimens test: records round trip %s; thin view %d bytes, regimen %s", ((after != NULL) &&
                (memcmp(after, &before, sizeof(before)) == 0)) ? "IDENTICAL" : "DIFFERENT", (int)sizeof(view),
                ((back.regimen == before.regimen) && (memcmp(back.reg_levels, before.reg_levels, 4) == 0)) ? "SAME" : "DIFFERS");
        }
    }
}

/** Test hook KFX_WORLD_CONTRACTTEST="T": at T a named Orc and an unnamed Bile Demon of level 9 beside the local
 *  keeper's heart; T+40 both reach level 10 (the Orc renegotiates, the Bile Demon takes its kind's contract); T+80 the
 *  Orc signs Plunder (through the packet) and T+90 a hero comes beside it; T+300 it signs Glory; T+340 it reaches level
 *  11 and T+360 tries to sign Gold (refused: the renegotiation closed). Short days show Glory's fury. T+600: a records
 *  round trip. A 3x3 Lair is built east of the heart at T (Bed and Board's bigger lair): T+700 and T+1400 log the
 *  Bile Demon's lair totem and its rest. */
static void md_contracttest_hook(void)
{
    static long base = -2;
    static ThingIndex orc = 0, bile = 0;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_CONTRACTTEST");
        base = (v != NULL) ? atol(v) : -1;
        if (base >= 0)
            SYNCLOG("Contracts test: KFX_WORLD_CONTRACTTEST at turn %ld", base);
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart))
        return;
    if ((t > 0) && ((t % 500) == 0))
    {
        /* what the two are doing, so that a run past a payday (every 10,000 turns) shows whether a creature without a
           wage goes on working (audit 2026-11-13: it stood at the Treasury for good) */
        const ThingIndex both[2] = {orc, bile};
        for (int k = 0; k < 2; k++)
        {
            const struct Thing *c = thing_get(both[k]);
            if (!thing_exists(c) || (c->class_id != TCls_Creature))
                continue;
            const struct CreatureControl *cc = creature_control_get_from_thing(c);
            const struct MdRec *crec = md_peek(c);
            SYNCLOG("Contracts test: T+%ld: the %s on %s: %s then %s, paydays owed %d, wage %ld", t, thing_model_name(c),
                (crec != NULL) ? md_contract_names[crec->contract] : "?", creature_state_code_name(c->active_state),
                creature_state_code_name(c->continue_state), (int)cc->paydays_owed, (long)calculate_correct_creature_pay(c));
        }
    }
    if (t == 0)
    {
        {
            /* a Treasury west of the heart, so that paydays are paid at a room */
            const MapSlabCoord tx = subtile_slab(heart->mappos.x.stl.num) - 6, ty = subtile_slab(heart->mappos.y.stl.num) - 1;
            int made = 0;
            for (int dy = 0; dy < 3; dy++)
                for (int dx = 0; dx < 3; dx++)
                {
                    place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(tx + dx), slab_subtile_center(ty + dy), my_player_number, 0);
                    if (!room_is_invalid(place_room(my_player_number, (RoomKind)get_id(room_desc, "TREASURE"), slab_subtile_center(tx + dx),
                        slab_subtile_center(ty + dy))))
                        made++;
                }
            SYNCLOG("Contracts test: a Treasury of %d slabs west of the heart", made);
        }
        for (int k = 0; k < 2; k++)
        {
            struct Coord3d pos = heart->mappos;
            pos.x.val += (3 + k) * COORD_PER_STL;
            pos.y.val += 5 * COORD_PER_STL;
            struct Thing *thing = create_creature(&pos, (ThingModel)get_id(creature_desc, k ? "BILE_DEMON" : "ORC"), my_player_number);
            struct CreatureControl *cctrl = thing_is_invalid(thing) ? NULL : creature_control_get_from_thing(thing);
            struct MdRec *rec = ((cctrl == NULL) || creature_control_invalid(cctrl)) ? NULL : md_get(thing, cctrl);
            if (rec == NULL)
                continue;
            set_creature_level(thing, 8);
            if (k == 0)
            {
                rec->flags |= MdF_Renamed;
                orc = thing->index;
            } else
                bile = thing->index;
            SYNCLOG("Contracts test: %s index %d, wage %ld", thing_model_name(thing), (int)thing->index,
                (long)calculate_correct_creature_pay(thing));
        }
        /* a Lair east of the heart, and the way to it */
        const MapSlabCoord lx = subtile_slab(heart->mappos.x.stl.num) + 4, ly = subtile_slab(heart->mappos.y.stl.num) - 1;
        place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(lx - 1), slab_subtile_center(ly + 1), my_player_number, 0);
        struct Room *room = INVALID_ROOM;
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
            {
                /* (claimed ground first, then the stock room building, as the keeper's own building does) */
                place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(lx + dx), slab_subtile_center(ly + dy), my_player_number, 0);
                struct Room *r = place_room(my_player_number, (RoomKind)get_id(room_desc, "LAIR"), slab_subtile_center(lx + dx),
                    slab_subtile_center(ly + dy));
                if (!room_is_invalid(r))
                    room = r;
            }
        SYNCLOG("Contracts test: a Lair %s (%d slabs)", room_is_invalid(room) ? "NOT built" : "built", room_is_invalid(room) ? 0 : (int)room->slabs_count);
        return;
    }
    if ((t == 700) || (t == 1400))
    {
        struct Thing *dm = thing_get(bile);
        const struct CreatureControl *dctrl = thing_exists(dm) ? creature_control_get_from_thing(dm) : NULL;
        const struct Thing *lair = ((dctrl != NULL) && (dctrl->lairtng_idx > 0)) ? thing_get(dctrl->lairtng_idx) : INVALID_THING;
        const struct MdRec *brec = thing_exists(dm) ? md_peek(dm) : NULL;
        if (!thing_exists(lair))
            SYNCLOG("Contracts test: T+%ld: the Bile Demon (%s) has no lair yet", t, (brec != NULL) ? md_contract_names[brec->contract] : "?");
        else
            SYNCLOG("Contracts test: T+%ld: the Bile Demon (%s): its lair totem's size %d (its kind's %d); asleep %d, health %d of %d", t,
                (brec != NULL) ? md_contract_names[brec->contract] : "?", (int)lair->sprite_size,
                (int)get_object_model_stats(lair->model)->sprite_size_max,
                (int)(get_creature_state_besides_interruptions(dm) == CrSt_CreatureSleep), (int)dm->health, (int)dctrl->max_health);
    }
    if ((t == 700) || (t == 1400) || (t == 1500) || (t == 2300) || (t == 3100))
    {
        /* the room the two lairs take: Bed and Board's is twice its kind's. At T+1500 the Orc is put on Bed and Board
           and at T+2300 back on Glory (straight through the contract's own setter): each time it gives its lair up
           and makes a new one, and the Lair's count follows. */
        const ThingIndex both[2] = {orc, bile};
        for (int k = 0; k < 2; k++)
        {
            struct Thing *c = thing_get(both[k]);
            struct MdRec *crec = (thing_exists(c) && (c->class_id == TCls_Creature)) ? md_peek(c) : NULL;
            if (crec == NULL)
                continue;
            if ((k == 0) && ((t == 1500) || (t == 2300)))
                md_contract_set(c, crec, (t == 1500) ? MdK_Board : MdK_Glory);
            const struct CreatureControl *cc = creature_control_get_from_thing(c);
            const struct Room *lroom = (cc->lair_room_id > 0) ? room_get(cc->lair_room_id) : INVALID_ROOM;
            const int kind_places = creature_stats_get_from_thing(c)->lair_size;
            if (room_is_invalid(lroom))
                SYNCLOG("Contracts test: T+%ld: the %s on %s has no lair now (it would take %d places, its kind's %d)", t,
                    thing_model_name(c), md_contract_names[crec->contract], world_misdeeds_lair_places(c, kind_places), kind_places);
            else
                SYNCLOG("Contracts test: T+%ld: the %s on %s: its lair takes %d places (its kind's %d); the Lair has %d of %d used", t,
                    thing_model_name(c), md_contract_names[crec->contract], world_misdeeds_lair_places(c, kind_places), kind_places,
                    (int)lroom->used_capacity, (int)lroom->total_capacity);
        }
    }
    struct Thing *thing = thing_get(orc);
    struct MdRec *rec = thing_exists(thing) ? md_peek(thing) : NULL;
    struct Thing *demon = thing_get(bile);
    const struct MdRec *drec = thing_exists(demon) ? md_peek(demon) : NULL;
    if (rec == NULL)
        return;
    if (t == 40)
    {
        set_creature_level(thing, 9);
        if (drec != NULL)
            set_creature_level(demon, 9);
    }
    if (t == 78)
        SYNCLOG("Contracts test: the Orc: contract %s, open %d; the Bile Demon: contract %s, wage %ld, meals x%ld",
            md_contract_names[rec->contract], (int)rec->contract_open, (drec != NULL) ? md_contract_names[drec->contract] : "?",
            (drec != NULL) ? (long)calculate_correct_creature_pay(demon) : -1L, (drec != NULL) ? world_misdeeds_hunger(demon, 1) : -1L);
    if ((t == 80) || (t == 300) || (t == 360))
        set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, orc, 50 + ((t == 80) ? MdK_Plunder : (t == 300) ? MdK_Glory : MdK_Gold), 0, 0);
    if (t == 82)
        SYNCLOG("Contracts test: the Orc on %s: wage %ld, treasury %ld", md_contract_names[rec->contract],
            (long)calculate_correct_creature_pay(thing), (long)get_dungeon(my_player_number)->total_money_owned);
    if (t == 90)
    {
        struct Coord3d pos = thing->mappos;
        pos.x.val += COORD_PER_STL;
        struct Thing *hero = create_creature(&pos, (ThingModel)get_id(creature_desc, "ARCHER"), PLAYER_GOOD);
        if (!thing_is_invalid(hero))
            kill_creature(hero, thing, my_player_number, CrDed_NoUnconscious); /* (by the Orc's hand: its bounty) */
        SYNCLOG("Contracts test: treasury after the kill %ld", (long)get_dungeon(my_player_number)->total_money_owned);
    }
    if (t == 340)
        set_creature_level(thing, 10);
    if (t == 355)
        SYNCLOG("Contracts test: the Orc at level %d: renegotiated %d tenth levels, open %d", creature_control_get_from_thing(thing)->exp_level + 1,
            (int)rec->contract_done, (int)rec->contract_open);
    if (t == 600)
    {
        const uint16_t id = creature_control_get_from_thing(thing)->misdeed_id;
        const struct MdRec before = *rec;
        const size_t size = world_misdeeds_save_size();
        uint8_t *buf = (uint8_t *)malloc(size + 64);
        if (buf != NULL)
        {
            world_misdeeds_save(buf);
            world_misdeeds_load(buf, size);
            free(buf);
            const struct MdRec *after = md_rec(id);
            SYNCLOG("Contracts test: the Orc on %s; records round trip %s", md_contract_names[before.contract],
                ((after != NULL) && (memcmp(after, &before, sizeof(before)) == 0)) ? "IDENTICAL" : "DIFFERENT");
        }
    }
}

/** Test hook KFX_WORLD_SOULTEST="T": at T a named Warlock of level 40 with two vices beside the local keeper's heart
 *  is sacrificed (the Temple's hook, then the stock removal); T+200 an Orc arrives as if through the portal and takes
 *  the soul; T+260 logs it, a records round trip and a thin-view round trip. "T,1": a 3x3 Temple is built south of
 *  the heart at T and the Warlock is put in the keeper's hand and dropped on the Temple's pool at T+10 (the hand's own
 *  pickup and drop; the stock Temple sacrifices it). */
static void md_soultest_hook(void)
{
    static long base = -2;
    static ThingIndex orc = 0, warlock = 0;
    static int by_hand = 0;
    static MapSlabCoord tx, ty;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_SOULTEST");
        base = -1;
        if ((v != NULL) && (sscanf(v, "%ld,%d", &base, &by_hand) >= 1))
            SYNCLOG("Past Lives test: KFX_WORLD_SOULTEST at turn %ld%s", base, by_hand ? ", dropped in a Temple by hand" : "");
        else
            base = -1;
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart))
        return;
    struct Coord3d pos = heart->mappos;
    pos.x.val += 3 * COORD_PER_STL;
    pos.y.val += 5 * COORD_PER_STL;
    if (t == 0)
    {
        struct Thing *w = create_creature(&pos, (ThingModel)get_id(creature_desc, "SORCEROR"), my_player_number);
        struct CreatureControl *cctrl = thing_is_invalid(w) ? NULL : creature_control_get_from_thing(w);
        struct MdRec *rec = ((cctrl == NULL) || creature_control_invalid(cctrl)) ? NULL : md_get(w, cctrl);
        if (rec == NULL)
            return;
        set_creature_level(w, 39);
        snprintf(cctrl->creature_name, sizeof(cctrl->creature_name), "Grimwald");
        rec->flags |= MdF_Renamed;
        rec->vices[0] = MdV_Bane;
        rec->vices[1] = MdV_Night;
        md_slot_start(rec, 0);
        md_slot_start(rec, 1);
        if (by_hand)
        {
            tx = subtile_slab(heart->mappos.x.stl.num) - 1;
            ty = subtile_slab(heart->mappos.y.stl.num) + 6;
            struct Room *room = INVALID_ROOM;
            for (int dy = 0; dy < 3; dy++)
                for (int dx = 0; dx < 3; dx++)
                {
                    /* (claimed ground first, then the stock room building, as the keeper's own building does) */
                    place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(tx + dx), slab_subtile_center(ty + dy), my_player_number, 0);
                    struct Room *r = place_room(my_player_number, (RoomKind)get_id(room_desc, "TEMPLE"), slab_subtile_center(tx + dx),
                        slab_subtile_center(ty + dy));
                    if (!room_is_invalid(r))
                        room = r;
                }
            for (int dy = 3; dy <= 5; dy++) /* (a path from the heart's edge: the heart takes 2 slabs round its middle) */
                place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(tx + 1), slab_subtile_center(subtile_slab(heart->mappos.y.stl.num) + dy),
                    my_player_number, 0);
            reveal_map_area(my_player_number, slab_subtile(tx - 1, 0), slab_subtile(tx + 4, 0), slab_subtile(ty - 1, 0), slab_subtile(ty + 4, 0));
            warlock = w->index;
            SYNCLOG("Past Lives test: a Temple %s at (%d,%d); the pool in its middle: %d", room_is_invalid(room) ? "NOT built" : "built",
                (int)tx, (int)ty, (int)subtile_has_sacrificial_on_top(slab_subtile_center(tx + 1), slab_subtile_center(ty + 1)));
            return;
        }
        world_misdeeds_sacrificed(w, my_player_number);
        kill_creature(w, INVALID_THING, -1, CrDed_NoEffects | CrDed_NotReallyDying); /* (as the Temple does) */
        return;
    }
    if (by_hand && (t == 10))
    {
        struct Thing *w = thing_get(warlock);
        if (!thing_exists(w) || !place_thing_in_power_hand(w, my_player_number))
        {
            SYNCLOG("Past Lives test: the Warlock can't be taken in the hand");
            return;
        }
        get_player_primary_user_state(get_player(my_player_number))->thing_under_hand = 0;
        const short res = dump_first_held_thing_on_map(my_player_number, slab_subtile_center(tx + 1), slab_subtile_center(ty + 1), true);
        SYNCLOG("Past Lives test: the Warlock dropped on the Temple's pool: %d (state %d)", (int)res,
            thing_exists(w) ? (int)get_creature_state_besides_interruptions(w) : -1);
        return;
    }
    if (by_hand && (t == 150))
    {
        const struct Thing *w = thing_get(warlock);
        SYNCLOG("Past Lives test: the Warlock is %s", (thing_exists(w) && (w->class_id == TCls_Creature) && (w->model == (ThingModel)get_id(creature_desc, "SORCEROR"))) ?
            "STILL there" : "gone (sacrificed)");
    }
    if (t == 200)
    {
        struct Thing *o = create_creature(&pos, (ThingModel)get_id(creature_desc, "ORC"), my_player_number);
        if (thing_is_invalid(o))
            return;
        orc = o->index;
        world_misdeeds_attracted(o);
    }
    if (t == 260)
    {
        struct Thing *o = thing_get(orc);
        const struct MdRec *rec = thing_exists(o) ? md_peek(o) : NULL;
        if (rec == NULL)
        {
            SYNCLOG("Past Lives test: no Orc or no record");
            return;
        }
        const struct CreatureControl *cctrl = creature_control_get_from_thing(o);
        SYNCLOG("Past Lives test: the Orc '%s': past lives %d, once a %s, vice %s, remembers %s (available %d), named %d",
            creature_own_name(o), (int)rec->past_lives, creature_code_name(rec->past_model), md_vice_names[rec->vices[0]],
            (rec->foreign_inst != 0) ? creature_instance_code_name(rec->foreign_inst) : "nothing",
            (rec->foreign_inst != 0) ? (int)cctrl->instance_available[rec->foreign_inst] : -1, (int)flag_is_set(rec->flags, MdF_Renamed));
        const uint16_t id = cctrl->misdeed_id;
        const struct MdRec before = *rec;
        const size_t size = world_misdeeds_save_size();
        uint8_t *buf = (uint8_t *)malloc(size + 64);
        if (buf != NULL)
        {
            world_misdeeds_save(buf);
            world_misdeeds_load(buf, size);
            free(buf);
            const struct MdRec *after = md_rec(id);
            struct MdView view;
            struct MdRec back;
            md_view_pack(&before, &view);
            md_view_unpack(&view, &back);
            SYNCLOG("Past Lives test: records round trip %s; thin view %d bytes: %s", ((after != NULL) &&
                (memcmp(after, &before, sizeof(before)) == 0)) ? "IDENTICAL" : "DIFFERENT", (int)sizeof(view),
                ((back.past_lives == before.past_lives) && (back.foreign_inst == before.foreign_inst)) ? "SAME" : "DIFFERS");
        }
    }
}

/** Test hook KFX_WORLD_EYESTEST="T": the local keeper takes over a Fly beside an other keeper's hidden trap (T, logged
 *  at T+40), then a Warlock (its blow on a hero, then a kill: Driven), a Vampire (it reads a hero) and an Imp (its dig
 *  damage), each through the stock possession and released after. */
static void md_eyestest_hook(void)
{
    static long base = -2;
    static ThingIndex fly = 0, trap = 0;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_EYESTEST");
        base = (v != NULL) ? atol(v) : -1;
        if (base >= 0)
            SYNCLOG("Eyes test: KFX_WORLD_EYESTEST at turn %ld", base);
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart))
        return;
    const NetUserId user = get_local_user();
    struct Coord3d pos = heart->mappos;
    pos.x.val += 3 * COORD_PER_STL;
    pos.y.val += 5 * COORD_PER_STL;
    if (t == 0)
    {
        struct Thing *f = create_creature(&pos, md_kind("FLY"), my_player_number);
        struct Coord3d tpos = pos;
        tpos.x.val += 3 * COORD_PER_STL;
        struct Thing *tr = create_trap(&tpos, (ThingModel)get_id(trap_desc, "BOULDER"), PLAYER_GOOD);
        if (thing_is_invalid(f) || thing_is_invalid(tr))
        {
            SYNCLOG("Eyes test: no Fly or no trap");
            base = -1;
            return;
        }
        fly = f->index;
        trap = tr->index;
        tr->trap.revealed = 0;
        SYNCLOG("Eyes test: a Fly and a hidden trap of the heroes 3 slabs off; taking over the Fly: %d",
            (int)control_creature_as_controller(user, f));
    }
    if (t == 40)
    {
        struct Thing *tr = thing_get(trap);
        SYNCLOG("Eyes test: the trap is revealed: %d", thing_exists(tr) ? (int)tr->trap.revealed : -1);
        struct Thing *f = thing_get(fly);
        if (thing_exists(f))
            leave_creature_as_controller(user, f);
    }
    if ((t == 60) || (t == 100) || (t == 140))
    {
        const char *kind = (t == 60) ? "SORCEROR" : (t == 100) ? "VAMPIRE" : "IMP";
        struct Thing *c = create_creature(&pos, md_kind(kind), my_player_number);
        if (thing_is_invalid(c))
            return;
        struct CreatureControl *cctrl = creature_control_get_from_thing(c);
        md_get(c, cctrl);
        struct Coord3d hpos = pos;
        hpos.x.val += COORD_PER_STL;
        struct Thing *hero = create_creature(&hpos, md_kind("ARCHER"), PLAYER_GOOD);
        const HitPoints before = thing_is_invalid(hero) ? -1 : world_misdeeds_adjust_damage(hero, c, 100);
        const struct SlabMap *slb = get_slabmap_block(subtile_slab(c->mappos.x.stl.num) + 2, subtile_slab(c->mappos.y.stl.num));
        const long dig_before = calculate_damage_did_to_slab_with_single_hit(c, slb);
        const TbBool rode = control_creature_as_controller(user, c);
        const HitPoints after = thing_is_invalid(hero) ? -1 : world_misdeeds_adjust_damage(hero, c, 100);
        const long dig_after = calculate_damage_did_to_slab_with_single_hit(c, slb);
        if ((t == 60) && !thing_is_invalid(hero))
            kill_creature(hero, c, my_player_number, CrDed_NoUnconscious);
        const struct MdRec *rec = md_peek(c);
        SYNCLOG("Eyes test: %s taken over %d: a blow of 100 on a hero does %d, then %d; dig %ld, then %ld; Driven %u", kind, (int)rode,
            (int)before, (int)after, dig_before, dig_after, (rec != NULL) ? (unsigned)rec->driven : 0);
        leave_creature_as_controller(user, c);
    }
    static ThingIndex dm = 0, porc = 0;
    if (t == 200)
    {
        struct Thing *m = create_creature(&pos, md_kind("DARK_MISTRESS"), my_player_number);
        struct Coord3d opos = pos;
        opos.x.val += COORD_PER_STL;
        struct Thing *o = create_creature(&opos, md_kind("ORC"), my_player_number);
        if (thing_is_invalid(m) || thing_is_invalid(o))
            return;
        md_get(m, creature_control_get_from_thing(m));
        md_get(o, creature_control_get_from_thing(o));
        add_creature_to_group(o, m);
        dm = m->index;
        porc = o->index;
        const long bolt = get_id(instance_desc, "LIGHTNING");
        SYNCLOG("Eyes test: a Mistress and an Orc in a party (%d); the Orc has Lightning: %d; taking over the Mistress: %d",
            (int)get_no_creatures_in_group(m), (int)creature_control_get_from_thing(o)->instance_available[bolt],
            (int)control_creature_as_controller(user, m));
    }
    static ThingIndex bimp = 0, bbook = 0;
    static struct Coord3d lib_pos;
    if (t == 400)
    {
        const MapSlabCoord lx = subtile_slab(pos.x.stl.num) + 5, ly = subtile_slab(pos.y.stl.num);
        struct Room *lib = INVALID_ROOM;
        for (int dy = 0; dy < 3; dy++)
            for (int dx = 0; dx < 3; dx++)
            {
                place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(lx + dx), slab_subtile_center(ly + dy), my_player_number, 0);
                struct Room *r = place_room(my_player_number, (RoomKind)get_id(room_desc, "RESEARCH"), slab_subtile_center(lx + dx),
                    slab_subtile_center(ly + dy));
                if (!room_is_invalid(r))
                    lib = r;
            }
        lib_pos.x.val = subtile_coord_center(slab_subtile_center(lx + 1));
        lib_pos.y.val = subtile_coord_center(slab_subtile_center(ly + 1));
        lib_pos.z.val = 0;
        struct Thing *imp = create_creature(&pos, md_kind("IMP"), my_player_number);
        if (thing_is_invalid(imp))
        {
            SYNCLOG("Eyes test: no Imp");
            return;
        }
        md_get(imp, creature_control_get_from_thing(imp));
        bimp = imp->index;
        SYNCLOG("Eyes test: an Imp, a Library %s (capacity %d); taking over the Imp: %d", room_is_invalid(lib) ? "NOT built" : "built",
            room_is_invalid(lib) ? 0 : (int)lib->total_capacity, (int)control_creature_as_controller(user, imp));
    }
    if (t == 410)
    {
        /* a book at the Imp's feet, once it has settled */
        struct Thing *imp = thing_get(bimp);
        if (!thing_exists(imp))
            return;
        struct Thing *book = create_object(&imp->mappos, (ThingModel)get_id(object_desc, "RESEARCH_BOOK"), game.neutral_player_num, -1);
        if (thing_is_invalid(book))
            return;
        book->custom_box.box_kind = WORLD_OUTPOST_BOOK_KIND; /* (a heroes' spellbook, as an outpost leaves) */
        bbook = book->index;
        SYNCLOG("Eyes test: a %s at the Imp's feet (pickable %d)", thing_model_name(book),
            (int)thing_can_be_picked_to_place_in_player_room_of_role(book, my_player_number, RoRoF_PowersStorage, TngFRPickF_Default));
    }
    if ((t == 450) || (t == 520))
    {
        struct Thing *imp = thing_get(bimp);
        struct Thing *book = thing_get(bbook);
        const struct CreatureControl *ictrl = thing_exists(imp) ? creature_control_get_from_thing(imp) : NULL;
        SYNCLOG("Eyes test: T+%ld: the Imp drags %d (the book is %d); the book %s, %ld subtiles from the Imp", t,
            (ictrl != NULL) ? (int)ictrl->dragtng_idx : -1, (int)bbook, thing_exists(book) ? "exists" : "is gone",
            (thing_exists(book) && thing_exists(imp)) ? (long)(get_chessboard_distance(&book->mappos, &imp->mappos) / COORD_PER_STL) : -1L);
        if ((t == 450) && thing_exists(imp))
        {
            lib_pos.z.val = get_floor_height_at(&lib_pos);
            move_thing_in_map(imp, &lib_pos); /* (as the keeper walks it there) */
        }
        if ((t == 520) && thing_exists(imp))
            leave_creature_as_controller(user, imp);
    }
    static ThingIndex book2 = 0;
    static GameTurn book2_born = 0;
    if (t == 560)
    {
        /* a second book by the heart, for the keeper's own imps to carry to the Library on their stock job */
        struct Coord3d b2 = heart->mappos;
        b2.x.val += 3 * COORD_PER_STL;
        struct Thing *b = create_object(&b2, (ThingModel)get_id(object_desc, "RESEARCH_BOOK"), game.neutral_player_num, -1);
        if (!thing_is_invalid(b))
        {
            b->custom_box.box_kind = WORLD_OUTPOST_BOOK_KIND;
            book2 = b->index;
            book2_born = b->creation_turn;
        }
        SYNCLOG("Eyes test: a second book by the heart for the stock imps: %s", thing_is_invalid(b) ? "NOT made" : "made");
    }
    if ((t == 1200) && (book2 != 0))
    {
        const struct Thing *b = thing_get(book2);
        SYNCLOG("Eyes test: T+1200: the second book is %s", (thing_exists(b) && (b->class_id == TCls_Object) && (b->creation_turn == book2_born)) ?
            "still on the map (not shelved)" : "gone (shelved)");
    }
    if ((t == 260) || (t == 340))
    {
        struct Thing *o = thing_get(porc);
        if (thing_exists(o))
            SYNCLOG("Eyes test: T+%ld: the Orc has Lightning: %d", t,
                (int)creature_control_get_from_thing(o)->instance_available[get_id(instance_desc, "LIGHTNING")]);
        struct Thing *m = thing_get(dm);
        if ((t == 260) && thing_exists(m))
            leave_creature_as_controller(user, m);
    }
}

/** Test hook KFX_WORLD_MARCHTEST="T": at T an Orc (the party's leader), a Warlock, a Dark Mistress and a Bile Demon
 *  beside the local keeper's heart are made one party (the stock group, as the Barracks makes it), and a lone Orc
 *  outside it; posts are set through the packets: Orc Standard, Bile Demon Van, Mistress Flank, Warlock Rear, the lone
 *  Orc Van. T+40 logs each bonus; T+60 the Mistress carries the standard instead (Orc to Flank) and the Bile Demon is
 *  wounded; T+100 the Warlock carries it; T+140 logs both gifts. */
static void md_marchtest_hook(void)
{
    static long base = -2;
    static ThingIndex m[5];
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_MARCHTEST");
        base = (v != NULL) ? atol(v) : -1;
        if (base >= 0)
            SYNCLOG("Marching Order test: KFX_WORLD_MARCHTEST at turn %ld", base);
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart))
        return;
    static const char *const kinds[5] = {"ORC", "SORCEROR", "DARK_MISTRESS", "BILE_DEMON", "ORC"};
    if (t == 0)
    {
        for (int k = 0; k < 5; k++)
        {
            struct Coord3d pos = heart->mappos;
            pos.x.val += (2 + k) * COORD_PER_STL;
            pos.y.val += ((k == 4) ? 9 : 5) * COORD_PER_STL;
            struct Thing *c = create_creature(&pos, md_kind(kinds[k]), my_player_number);
            if (thing_is_invalid(c))
                continue;
            set_creature_level(c, 9);
            md_get(c, creature_control_get_from_thing(c));
            m[k] = c->index;
            if ((k > 0) && (k < 4) && (m[0] != 0))
                add_creature_to_group(c, thing_get(m[0]));
        }
        return;
    }
    if ((t >= 1) && (t <= 5))
    {
        /* one packet a turn: a player sends one action per turn */
        static const int posts[5] = {MdP_Standard, MdP_Rear, MdP_Flank, MdP_Van, MdP_Van};
        if (m[t - 1] != 0)
            set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, m[t - 1], 60 + posts[t - 1], 0, 0);
        return;
    }
    struct Thing *th[5];
    for (int k = 0; k < 5; k++)
        th[k] = thing_get(m[k]);
    for (int k = 0; k < 5; k++)
        if ((m[k] == 0) || !thing_exists(th[k]))
            return;
    if ((t == 40) || (t == 140) || (t == 240))
    {
        struct Coord3d hpos = heart->mappos;
        hpos.y.val += 12 * COORD_PER_STL;
        struct Thing *hero = create_creature(&hpos, md_kind("ARCHER"), PLAYER_GOOD);
        if (thing_is_invalid(hero))
            return;
        int32_t ritime = 100;
        world_misdeeds_reset_time(th[1], CrInst_NULL + 1, &ritime);
        SYNCLOG("Marching Order test: T+%ld: in a party %d%d%d%d / lone %d; a hero's 100 on the Van does %d, on the lone Orc (Van, no "
            "party) %d; the Warlock's 100 on a hero does %d; its reach of 1000 is %ld; speeds: Mistress %ld, Bile Demon %ld; "
            "the Warlock's reset 100 -> %d", t, (int)creature_is_group_member(th[0]), (int)creature_is_group_member(th[1]),
            (int)creature_is_group_member(th[2]), (int)creature_is_group_member(th[3]), (int)creature_is_group_member(th[4]),
            (int)world_misdeeds_adjust_damage(th[3], hero, 100), (int)world_misdeeds_adjust_damage(th[4], hero, 100),
            (int)world_misdeeds_adjust_damage(hero, th[1], 100), world_misdeeds_range(th[1], 1000),
            (long)creature_control_get_from_thing(th[2])->max_speed, (long)creature_control_get_from_thing(th[3])->max_speed, (int)ritime);
        delete_thing_structure(hero, 0);
    }
    if (t == 60)
    {
        set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, m[2], 60 + MdP_Standard, 0, 0);
        th[3]->health = creature_control_get_from_thing(th[3])->max_health / 3;
        SYNCLOG("Marching Order test: the Mistress takes the standard; the Bile Demon is wounded");
    }
    if (t == 64)
        set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, m[0], 60 + MdP_Flank, 0, 0);
    if (t == 163) /* (two samples after the Mistress took the standard: its gift is on the leader, and read) */
    {
        const struct MdRec *drec = md_peek(th[3]);
        SYNCLOG("Marching Order test: before the Mistress leaves the standard: the Bile Demon's party gift %d (wounded is %d), health %ld of %ld, its walking bonus %d%%, max speed %ld",
            md_party_gift(th[3]), (int)MdG_Wounded, (long)th[3]->health, (long)creature_control_get_from_thing(th[3])->max_health,
            (drec != NULL) ? (int)drec->march_speed : -1, (long)creature_control_get_from_thing(th[3])->max_speed);
    }
    if (t == 164)
    {
        SYNCLOG("Marching Order test: speeds with the Mistress's standard: Bile Demon (wounded, Van) %ld, Orc (Flank) %ld",
            (long)creature_control_get_from_thing(th[3])->max_speed, (long)creature_control_get_from_thing(th[0])->max_speed);
        set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, m[2], 60 + MdP_Flank, 0, 0);
    }
    if (t == 168)
        set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, m[1], 60 + MdP_Standard, 0, 0);
}

/** Test hook KFX_WORLD_ALUMNITEST="T": at T twelve creatures arrive for the local keeper through the portal's own hook;
 *  six are raised to level 50 and six to 25 (renown 30). T+300 (after a renown pass) logs the renown, the newcomer
 *  level and the wait between arrivals, and one more creature arrives through the hook. */
static void md_alumnitest_hook(void)
{
    static long base = -2;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_ALUMNITEST");
        base = (v != NULL) ? atol(v) : -1;
        if (base >= 0)
            SYNCLOG("Alumni test: KFX_WORLD_ALUMNITEST at turn %ld", base);
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart) || ((t != 0) && (t != 300)))
        return;
    struct Coord3d pos = heart->mappos;
    pos.y.val += 5 * COORD_PER_STL;
    if (t == 0)
    {
        for (int k = 0; k < 12; k++)
        {
            pos.x.val = heart->mappos.x.val + (k - 6) * COORD_PER_STL;
            struct Thing *c = create_creature(&pos, md_kind((k % 2) ? "ORC" : "TROLL"), my_player_number);
            if (thing_is_invalid(c))
                continue;
            world_misdeeds_attracted(c);
            set_creature_level(c, (k < 6) ? 49 : 24);
        }
        return;
    }
    const long wait = world_misdeeds_entrance_wait(my_player_number, 400);
    pos.x.val = heart->mappos.x.val;
    struct Thing *c = create_creature(&pos, md_kind("ORC"), my_player_number);
    if (!thing_is_invalid(c))
        world_misdeeds_attracted(c);
    SYNCLOG("Alumni test: renown %d, best level %d, newcomers at level %d; a wait of 400 is %ld; the newcomer is level %d",
        md_renown[my_player_number], md_best_level[my_player_number], md_newcomer_level(my_player_number), wait,
        thing_is_invalid(c) ? -1 : creature_control_get_from_thing(c)->exp_level + 1);
}

/** Test hook KFX_WORLD_BREEDTEST="T": for each breed, a creature of its kind arrives (the portal's own hook) on a
 *  loaded slab of its biome near the local keeper's heart; where there is none near, the breed is set by hand and the
 *  log says so. T+40 logs each breed's effect. */
static void md_breedtest_hook(void)
{
    static long base = -2;
    static ThingIndex made[MdB_COUNT];
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_BREEDTEST");
        base = (v != NULL) ? atol(v) : -1;
        if (base >= 0)
            SYNCLOG("Breeds test: KFX_WORLD_BREEDTEST at turn %ld", base);
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart) || ((t != 0) && (t != 40)))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    static const char *const kinds[MdB_COUNT] = {"", "SPIDER", "SPIDER", "BUG", "FLY"};
    if (t == 0)
    {
        for (int b = 1; b < MdB_COUNT; b++)
        {
            MapSlabCoord fx = -1, fy = -1;
            for (int r = 0; (r <= 40) && (fx < 0); r += 2)
                for (int dy = -r; (dy <= r) && (fx < 0); dy += 2)
                    for (int dx = -r; (dx <= r) && (fx < 0); dx += 2)
                    {
                        if ((abs(dx) != r) && (abs(dy) != r))
                            continue;
                        if (md_breed_for(md_kind(kinds[b]), world_biome_at_engine_slab(hx + dx, hy + dy)) == b)
                        {
                            fx = hx + dx;
                            fy = hy + dy;
                        }
                    }
            struct Coord3d pos = heart->mappos;
            if (fx >= 0)
            {
                pos.x.val = subtile_coord_center(slab_subtile_center(fx));
                pos.y.val = subtile_coord_center(slab_subtile_center(fy));
            } else
                pos.x.val += (2 + b) * COORD_PER_STL;
            struct Thing *c = create_creature(&pos, md_kind(kinds[b]), my_player_number);
            if (thing_is_invalid(c))
                continue;
            move_thing_in_map(c, &pos); /* (on the slab itself: the biome is read where it stands) */
            world_misdeeds_attracted(c);
            struct CreatureControl *cctrl = creature_control_get_from_thing(c);
            struct MdRec *rec = md_get(c, cctrl);
            if ((fx < 0) && (rec != NULL))
            {
                rec->breed = (uint8_t)b;
                md_breed_apply(c, cctrl, rec);
            }
            made[b] = c->index;
            SYNCLOG("Breeds test: %s: %s; breed %s", md_breed_names[b], (fx >= 0) ? "its biome found near, through the portal's hook" :
                "no such biome near: set by hand", (rec != NULL) ? md_breed_names[rec->breed] : "?");
        }
        return;
    }
    struct Coord3d hpos = heart->mappos;
    hpos.y.val += 12 * COORD_PER_STL;
    struct Thing *hero = create_creature(&hpos, md_kind("ARCHER"), PLAYER_GOOD);
    if (thing_is_invalid(hero))
        return;
    struct Thing *frost = thing_get(made[MdB_FrostSpider]), *tomb = thing_get(made[MdB_TombSpider]);
    struct Thing *beetle = thing_get(made[MdB_FireBeetle]), *fly = thing_get(made[MdB_GrottoFly]);
    const long freeze = get_id(instance_desc, "FREEZE");
    if (thing_exists(frost))
        SYNCLOG("Breeds test: the Frost Spider (level %d) has Freeze: %d", creature_control_get_from_thing(frost)->exp_level + 1,
            (freeze > 0) ? (int)creature_control_get_from_thing(frost)->instance_available[freeze] : -1);
    if (thing_exists(tomb))
        SYNCLOG("Breeds test: the Tomb Spider's 100 on a hero does %d", (int)world_misdeeds_adjust_damage(hero, tomb, 100));
    if (thing_exists(beetle))
    {
        struct Thing *shot = create_shot(&hpos, (ThingModel)get_id(shot_desc, "SHOT_FIREBALL"), PLAYER_GOOD);
        if (!thing_is_invalid(shot))
        {
            SYNCLOG("Breeds test: a fireball's 100 on the Volcanic Beetle does %d", (int)world_misdeeds_shot_damage(shot, hero, beetle, 100));
            delete_thing_structure(shot, 0);
        }
    }
    if (thing_exists(fly))
    {
        const HitPoints before = fly->health = max(fly->health / 2, (HitPoints)1);
        world_misdeeds_adjust_damage(hero, fly, 100);
        SYNCLOG("Breeds test: the Grotto Fly's 100 on a hero: its health %d -> %d", (int)before, (int)fly->health);
    }
    delete_thing_structure(hero, 0);
}

/******************************************************************************/
/* Strays: a wild creature fed a chicken at night stays at dawn, as a neutral creature; after three nights of feeding
   the next chicken leads it home, at half its wild level. The keeper who fed it first is the only one it eats from.
   Zombies can't be tamed. A stray not fed for STRAY_FORGET nights goes wild again (and fades at the next dawn).
   The night it first eats it is as wild as before (it fights on, and can be killed): only dawn makes it a stray. */
#define STRAY_NIGHTS 3
#define STRAY_FORGET 3
#define STRAY_PERIOD 256

static TbBool md_stray_kind_ok(ThingModel model)
{
    return (model != md_kind("ZOMBIE"));
}

/** Whether a keeper has room for one more creature: below their creature limit, with a free place in a Lair. */
static TbBool md_stray_room(PlayerNumber plyr_idx)
{
    struct Dungeon *dungeon = get_players_num_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon) || ((long)dungeon->num_active_creatrs >= (long)dungeon->max_creatures_attracted))
        return false;
    return (calculate_free_lair_space(dungeon) > 0);
}

TbBool world_misdeeds_stray_kept(const struct Thing *thing)
{
    if ((md_top == 0) || !world_mode_active() || !is_neutral_thing(thing))
        return false;
    const struct MdRec *rec = md_peek(thing);
    return (rec != NULL) && (rec->stray_by != 0);
}

/** Strays: a wild creature that ate a keeper's chicken tonight (it is wild until dawn). */
static struct MdRec *md_stray_fed_rec(const struct Thing *thing)
{
    if ((md_top == 0) || !world_wild_creature(thing))
        return NULL;
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    struct MdRec *rec = (creature_control_invalid(cctrl) || (cctrl->misdeed_id == 0)) ? NULL : md_rec(cctrl->misdeed_id);
    return ((rec != NULL) && (rec->stray_by != 0)) ? rec : NULL;
}

TbBool world_misdeeds_stray_fed(const struct Thing *thing)
{
    return (md_stray_fed_rec(thing) != NULL);
}

void world_misdeeds_stray_dawn(struct Thing *thing)
{
    const struct MdRec *rec = md_stray_fed_rec(thing);
    if (rec == NULL)
        return;
    const PlayerNumber by = (PlayerNumber)(rec->stray_by - 1);
    change_creature_owner(thing, game.neutral_player_num);
    SYNCLOG("Misdeeds: Strays: dawn: the wild %s player %d fed stays, as a neutral creature", creature_code_name(thing->model), (int)by);
    char text[128];
    snprintf(text, sizeof(text), "The %s you fed has stayed. Feed it again tonight.", get_string(creature_stats_get(thing->model)->namestr_idx));
    md_tell(by, text);
}

int world_misdeeds_stray_feed(PlayerNumber plyr_idx, struct Thing *food, struct Thing *creatng)
{
    if (!world_mode_active() || !md_soul_owner_ok(plyr_idx) || !world_player_is_human(plyr_idx) || !thing_exists(food) ||
        !thing_is_object(food) || !object_is_mature_food(food) || !thing_exists(creatng) || !thing_is_creature(creatng) ||
        creature_is_dying(creatng) || !md_stray_kind_ok(creatng->model))
        return 0;
    if (!map_block_revealed(get_map_block_at(creatng->mappos.x.stl.num, creatng->mappos.y.stl.num), plyr_idx))
        return 0;
    const TbBool wild = world_wild_creature(creatng) && !creature_is_being_unconscious(creatng) &&
        !creature_is_kept_in_custody(creatng);
    const TbBool stray = world_misdeeds_stray_kept(creatng);
    if (!wild && !stray)
        return 0;
    struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    if (creature_control_invalid(cctrl))
        return 0;
    struct MdRec *rec = md_get(creatng, cctrl);
    if (rec == NULL)
        return 0;
    const long night = world_day_number();
    char text[160];
    const char *kind = get_string(creature_stats_get(creatng->model)->namestr_idx); /* (the game's own name for the kind) */
    if (wild)
    {
        if (world_is_day())
            return 0; /* (a wild creature in daylight is fading) */
        if (rec->stray_by != 0)
        {
            /* fed already: it is wild until dawn */
            md_tell(plyr_idx, (rec->stray_by == plyr_idx + 1) ? "It has eaten tonight. It is wild until dawn; then it stays." :
                "That one eats only from the keeper who fed it first.");
            return -1;
        }
        /* It eats, and that is all for tonight: it stays as wild as it was (it goes on fighting, and can be killed).
           Dawn makes it a stray (world_misdeeds_stray_dawn). Audit 2026-11-13: it turned neutral the moment it ate, so
           a hand of chickens took a night pack's attackers out of a fight at once. */
        food_eaten_by_creature(food, creatng);
        rec->stray_by = (uint8_t)(plyr_idx + 1);
        rec->stray_nights = 1;
        rec->stray_night = (uint16_t)night;
        rec->stray_level = (uint8_t)(cctrl->exp_level + 1);
        SYNCLOG("Misdeeds: Strays: player %d feeds a wild %s (level %d): wild until dawn, then it stays; night 1 of %d", (int)plyr_idx,
            kind, (int)rec->stray_level, STRAY_NIGHTS);
        snprintf(text, sizeof(text), "The %s eats your chicken. It is wild until dawn; then it stays. Feed it %d more nights.", kind,
            STRAY_NIGHTS - 1);
        md_tell(plyr_idx, text);
        return 1;
    }
    if (rec->stray_by != plyr_idx + 1)
    {
        md_tell(plyr_idx, "That stray eats only from the keeper who fed it first.");
        return -1;
    }
    if (rec->stray_nights >= STRAY_NIGHTS)
    {
        if (!md_stray_room(plyr_idx))
        {
            md_tell(plyr_idx, "The stray would follow you, but you have no room for one more creature.");
            SYNCLOG("Misdeeds: Strays: player %d has no room for the %s (creatures %d of %d, free lair space %ld)", (int)plyr_idx, kind,
                (int)get_players_num_dungeon(plyr_idx)->num_active_creatrs, (int)get_players_num_dungeon(plyr_idx)->max_creatures_attracted,
                (long)calculate_free_lair_space(get_players_num_dungeon(plyr_idx)));
            return -1;
        }
        const int level = max(1, (int)rec->stray_level / 2);
        change_creature_owner(creatng, plyr_idx);
        mark_creature_joined_dungeon(creatng);
        set_creature_level(creatng, (CrtrExpLevel)(level - 1));
        food_eaten_by_creature(food, creatng);
        rec->stray_by = 0;
        SYNCLOG("Misdeeds: Strays: a %s follows player %d home at level %d (level %d in the wild)", kind, (int)plyr_idx,
            level, (int)rec->stray_level);
        snprintf(text, sizeof(text), "The %s follows your chicken home. It joins you at level %d.", kind, level);
        md_tell(plyr_idx, text);
        return 1;
    }
    if (world_is_day() || (rec->stray_night == (uint16_t)night))
    {
        md_tell(plyr_idx, world_is_day() ? "Strays eat only at night." : "The stray has eaten tonight. Come back tomorrow night.");
        SYNCLOG("Misdeeds: Strays: the %s refuses player %d's chicken (%s)", kind, (int)plyr_idx, world_is_day() ? "day" : "fed tonight");
        return -1;
    }
    food_eaten_by_creature(food, creatng);
    rec->stray_nights++;
    rec->stray_night = (uint16_t)night;
    SYNCLOG("Misdeeds: Strays: player %d feeds a stray %s: night %d of %d", (int)plyr_idx, kind, (int)rec->stray_nights,
        STRAY_NIGHTS);
    if (rec->stray_nights >= STRAY_NIGHTS)
        snprintf(text, sizeof(text), "The %s is ready to come home: hold out one more chicken.", kind);
    else
        snprintf(text, sizeof(text), "The %s eats: night %d of %d.", kind, (int)rec->stray_nights, STRAY_NIGHTS);
    md_tell(plyr_idx, text);
    return 1;
}

/** Every 256 turns: strays not fed for STRAY_FORGET nights go wild again. */
static void md_strays_pass(void)
{
    if ((game.play_gameturn % STRAY_PERIOD) != 9)
        return;
    const long night = world_day_number();
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    long k = slist->index;
    for (int n = 0; (k > 0) && (n < THINGS_COUNT); n++)
    {
        struct Thing *thing = thing_get(k);
        if (thing_is_invalid(thing))
            break;
        k = thing->next_of_class;
        if (!is_neutral_thing(thing))
            continue;
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        struct MdRec *rec = (creature_control_invalid(cctrl) || (cctrl->misdeed_id == 0)) ? NULL : md_rec(cctrl->misdeed_id);
        if ((rec == NULL) || (rec->stray_by == 0))
            continue;
        if (world_caretaker_active((PlayerNumber)(rec->stray_by - 1)))
        {
            /* its keeper is away: the stray waits, as the dungeon does (the nights away don't count) */
            if (rec->stray_night < (uint16_t)night)
                rec->stray_night = (uint16_t)(night - 1);
            continue;
        }
        if (night - (long)rec->stray_night < STRAY_FORGET)
            continue;
        const PlayerNumber by = (PlayerNumber)(rec->stray_by - 1);
        rec->stray_by = 0;
        change_creature_owner(thing, PLAYER_GOOD);
        cctrl->party.objective = CHeroTsk_DefendSpawn;
        cctrl->party.original_objective = CHeroTsk_DefendSpawn; /* wild again */
        cctrl->party.target_plyr_idx = -1;
        SYNCLOG("Misdeeds: Strays: a %s of player %d's goes wild again (unfed since night %d)", creature_code_name(thing->model),
            (int)by, (int)rec->stray_night);
        char text[128];
        snprintf(text, sizeof(text), "A stray %s you fed has gone wild again.", get_string(creature_stats_get(thing->model)->namestr_idx));
        md_tell(by, text);
    }
}

/** A seat gets a new dungeon (Start again, a new player): the strays its old keeper was feeding go wild again, so they
 *  don't wait for the next keeper's chickens (audit 2026-11-13). */
void world_misdeeds_strays_seat_reset(PlayerNumber plyr_idx)
{
    if ((md_recs == NULL) || (plyr_idx < 0) || (plyr_idx >= PLAYERS_COUNT))
        return;
    int n = 0;
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    long k = slist->index;
    for (int guard = 0; (k > 0) && (guard < THINGS_COUNT); guard++)
    {
        struct Thing *thing = thing_get(k);
        if (thing_is_invalid(thing))
            break;
        k = thing->next_of_class;
        const TbBool fed_wild = world_wild_creature(thing); /* (fed tonight, wild until dawn: only its mark goes) */
        if (!is_neutral_thing(thing) && !fed_wild)
            continue;
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        struct MdRec *rec = (creature_control_invalid(cctrl) || (cctrl->misdeed_id == 0)) ? NULL : md_rec(cctrl->misdeed_id);
        if ((rec == NULL) || (rec->stray_by != plyr_idx + 1))
            continue;
        rec->stray_by = 0;
        n++;
        if (fed_wild)
            continue;
        change_creature_owner(thing, PLAYER_GOOD);
        cctrl->party.objective = CHeroTsk_DefendSpawn;
        cctrl->party.original_objective = CHeroTsk_DefendSpawn; /* wild again */
        cctrl->party.target_plyr_idx = -1;
    }
    if (n > 0)
        SYNCLOG("Misdeeds: Strays: player %d's seat starts again: %d stray%s go wild again", (int)plyr_idx, n, (n == 1) ? "" : "s");
}

/** Test hook KFX_WORLD_STRAYTEST=1, with KFX_WORLD_DAYNIGHT=600,600 (night d: turns (d-1)*1200+600 to d*1200):
 *  wild Spiders A (level 6, near the heart) and B (level 4, 7 chunks east) and a wild Zombie; chickens are put in the
 *  local keeper's hand (the hand's own pickup) and dropped on them (the hand's own drop). A is fed nights 1-3 and
 *  joins; B is fed night 1 only, frozen in its chunk through day 2 and thawed, then goes wild on night 4. */
static ThingIndex md_st_a, md_st_b, md_st_z, md_st_c;
static GameTurn md_st_c_born;
static MapSlabCoord md_st_bx, md_st_by;

static struct Thing *md_st_wild(const char *kind, struct Coord3d *pos, int level)
{
    if (md_kind(kind) <= 0)
        return INVALID_THING;
    struct Thing *c = create_thing_at_position_then_move_to_valid_and_add_light(pos, TCls_Creature, md_kind(kind), PLAYER_GOOD);
    if (thing_is_invalid(c))
        return INVALID_THING;
    init_creature_level(c, (CrtrExpLevel)(level - 1));
    struct CreatureControl *cctrl = creature_control_get_from_thing(c);
    cctrl->party.objective = CHeroTsk_DefendSpawn;
    cctrl->party.original_objective = CHeroTsk_DefendSpawn; /* (wild_spawn's mark) */
    cctrl->party.target_plyr_idx = -1;
    reveal_map_area(my_player_number, c->mappos.x.stl.num - 3, c->mappos.x.stl.num + 3, c->mappos.y.stl.num - 3,
        c->mappos.y.stl.num + 3);
    return c;
}

static void md_st_chicken(const char *who, ThingIndex idx)
{
    struct Thing *c = thing_get(idx);
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(c) || !thing_exists(heart))
    {
        SYNCLOG("Strays test: %s is gone", who);
        return;
    }
    struct Coord3d pos = heart->mappos;
    pos.x.val += 2 * COORD_PER_STL;
    struct Thing *food = create_object(&pos, ObjMdl_ChickenMature, my_player_number, -1);
    if (thing_is_invalid(food) || !place_thing_in_power_hand(food, my_player_number))
    {
        SYNCLOG("Strays test: no chicken in the hand");
        return;
    }
    struct PlayerInfo *player = get_player(my_player_number);
    get_player_primary_user_state(player)->thing_under_hand = (short)c->index;
    const short res = dump_first_held_thing_on_map(my_player_number, c->mappos.x.stl.num, c->mappos.y.stl.num, true);
    const struct Dungeon *dungeon = get_players_num_dungeon(my_player_number);
    if (dungeon->num_things_in_hand > 0)
    {
        struct Thing *left = thing_get(dungeon->things_in_hand[0]);
        remove_first_thing_from_power_hand_list(my_player_number);
        if (thing_exists(left))
            delete_thing_structure(left, 0);
    }
    const struct MdRec *rec = md_peek(c);
    SYNCLOG("Strays test: night %ld %s: chicken for %s (owner %d, level %d): %s; stray of %d, nights %d", world_day_number(),
        world_is_day() ? "(day)" : "(night)", who, (int)c->owner, (int)creature_control_get_from_thing(c)->exp_level + 1,
        res ? "eaten" : "refused", (rec != NULL) ? (int)rec->stray_by - 1 : -1, (rec != NULL) ? (int)rec->stray_nights : 0);
}

static void md_st_log(const char *who, ThingIndex idx)
{
    struct Thing *c = thing_get(idx);
    if (!thing_exists(c) || (c->class_id != TCls_Creature))
    {
        SYNCLOG("Strays test: turn %ld: %s is gone", (long)game.play_gameturn, who);
        return;
    }
    const struct MdRec *rec = md_peek(c);
    SYNCLOG("Strays test: turn %ld (%s): %s: owner %d, wild %d, stray of %d, nights %d", (long)game.play_gameturn,
        world_is_day() ? "day" : "night", who, (int)c->owner, (int)world_wild_creature(c), (rec != NULL) ? (int)rec->stray_by - 1 : -1,
        (rec != NULL) ? (int)rec->stray_nights : 0);
}

/** B after its thaw: a new thing; found as the neutral Spider with a stray record. */
static void md_st_find_b(void)
{
    md_st_b = 0;
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *c = thing_get(i);
        if (thing_exists(c) && (c->class_id == TCls_Creature) && (c->model == md_kind("SPIDER")) &&
            (world_misdeeds_stray_kept(c) || world_misdeeds_stray_fed(c)) && (i != md_st_a) && (i != md_st_c))
            md_st_b = (ThingIndex)i;
    }
    SYNCLOG("Strays test: after the thaw: B is %s", md_st_b ? "found, with its stray record" : "NOT found");
    for (long i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *c = thing_get(i);
        if (thing_exists(c) && (c->class_id == TCls_Creature) && (c->model == md_kind("SPIDER")))
            SYNCLOG("Strays test: a Spider %ld owner %d at (%d,%d) record %d", i, (int)c->owner, (int)subtile_slab(c->mappos.x.stl.num),
                (int)subtile_slab(c->mappos.y.stl.num), (int)creature_control_get_from_thing(c)->misdeed_id);
    }
}

static void md_straytest_hook(void)
{
    static int on = -1;
    if (on < 0)
    {
        on = (getenv("KFX_WORLD_STRAYTEST") != NULL);
        if (on)
            SYNCLOG("Strays test: KFX_WORLD_STRAYTEST");
    }
    if (!on)
        return;
    const long t = (long)game.play_gameturn;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart))
        return;
    if (t == 650)
    {
        struct Coord3d pos = heart->mappos;
        pos.y.val += 8 * COORD_PER_STL * 3;
        struct Thing *a = md_st_wild("SPIDER", &pos, 6);
        md_st_a = thing_is_invalid(a) ? 0 : a->index;
        if (world_test_stray_spot(&md_st_bx, &md_st_by))
        {
            struct Coord3d bpos;
            bpos.x.val = subtile_coord_center(slab_subtile_center(md_st_bx));
            bpos.y.val = subtile_coord_center(slab_subtile_center(md_st_by));
            bpos.z.val = 0;
            struct Thing *b = md_st_wild("SPIDER", &bpos, 4);
            md_st_b = thing_is_invalid(b) ? 0 : b->index;
        }
        struct Coord3d cpos = heart->mappos;
        cpos.y.val -= 8 * COORD_PER_STL * 3;
        struct Thing *cc = md_st_wild("SPIDER", &cpos, 5);
        md_st_c = thing_is_invalid(cc) ? 0 : cc->index;
        md_st_c_born = thing_is_invalid(cc) ? 0 : cc->creation_turn;
        pos.x.val += 3 * COORD_PER_STL;
        struct Thing *z = md_st_wild("ZOMBIE", &pos, 5);
        md_st_z = thing_is_invalid(z) ? 0 : z->index;
        SYNCLOG("Strays test: A %d, B %d at (%d,%d), Zombie %d", (int)md_st_a, (int)md_st_b, (int)md_st_bx, (int)md_st_by, (int)md_st_z);
    }
    if (t == 651) md_st_chicken("A", md_st_a);
    if (t == 652)
    {
        md_st_chicken("B", md_st_b);
        struct Thing *b = thing_get(md_st_b);
        if (thing_exists(b))
        {
            md_st_bx = subtile_slab(b->mappos.x.stl.num); /* (where it stands: placed on the nearest valid spot) */
            md_st_by = subtile_slab(b->mappos.y.stl.num);
        }
    }
    if ((t == 653) && md_st_z) md_st_chicken("the Zombie", md_st_z);
    if (t == 654) md_st_chicken("A again the same night", md_st_a);
    if (t == 655) md_st_chicken("C", md_st_c);
    if ((t == 3650) || (t == 3900) || (t == 4300))
    {
        const struct Thing *c = thing_get(md_st_c);
        if (thing_exists(c) && (c->creation_turn == md_st_c_born))
            md_st_log("C (fed night 1 only)", md_st_c);
        else
            SYNCLOG("Strays test: turn %ld: C (fed night 1 only) is gone", t);
    }
    if (t == 1250) { md_st_log("A", md_st_a); md_st_log("B", md_st_b); md_st_log("the Zombie", md_st_z); }
    if (t == 1251) md_st_chicken("A by day", md_st_a);
    if (t == 700) { world_test_stray_freeze(md_st_bx, md_st_by); md_st_log("B after its chunk froze", md_st_b); }
    if (t == 1850) md_st_chicken("A", md_st_a);
    if (t == 2000) SYNCLOG("Strays test: B's chunk at (%d,%d) loads again: %d chunks", (int)md_st_bx, (int)md_st_by,
        world_load_chunks_round(md_st_bx, md_st_by, 0));
    if (t == 2060) { md_st_find_b(); md_st_log("B", md_st_b); }
    if (t == 3050) md_st_chicken("A", md_st_a);
    if (t == 3060) md_st_chicken("A, ready", md_st_a);
    if (t == 3100) md_st_log("A", md_st_a);
    if (t == 3650) md_st_log("B (night 4, before the pass)", md_st_b);
    if (t == 3900) md_st_log("B", md_st_b);
    if (t == 4300) md_st_log("B", md_st_b);
    if (t == 4700) md_st_log("B", md_st_b);
}

/** Test hook KFX_WORLD_COUNTTEST=T (the counts that could be had for nothing): at T an Orc of the local keeper is put
 *  in a pool of water made beside the heart and held there to T+5200 (its water count stops at MD_STAND_DAY); T+5300
 *  it is put down, as the Hand puts a creature down, six chunks east (its farthest distance must not jump), then moved
 *  ten slabs on in walking steps (it rises by those); T+6000 on, back home, four fights with heroes: the first counts,
 *  the second began too soon after it, the third is ended by the Hand, the fourth counts. */
static void md_counttest_hook(void)
{
    static long base = -2;
    static ThingIndex orc = 0, hero = 0;
    static MapSlabCoord fx, fy;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_COUNTTEST");
        base = (v != NULL) ? atol(v) : -1;
        if (base >= 0)
            SYNCLOG("Counts test: KFX_WORLD_COUNTTEST at turn %ld", base);
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart) || (t < 0))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    const MapSlabCoord px = hx + 6, py = hy + 1; /* the pool's middle */
    if (t == 0)
    {
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                place_slab_type_on_map(SlbT_WATER, slab_subtile_center(px + dx), slab_subtile_center(py + dy), game.neutral_player_num, 0);
        struct Coord3d pos = heart->mappos;
        pos.x.val += 4 * COORD_PER_STL;
        struct Thing *c = create_creature(&pos, (ThingModel)get_id(creature_desc, "ORC"), my_player_number);
        struct CreatureControl *cctrl = thing_is_invalid(c) ? NULL : creature_control_get_from_thing(c);
        if ((cctrl == NULL) || creature_control_invalid(cctrl) || (md_get(c, cctrl) == NULL))
        {
            base = -1;
            return;
        }
        set_creature_level(c, 9);
        orc = c->index;
        return;
    }
    struct Thing *c = thing_get(orc);
    struct MdRec *rec = (thing_exists(c) && (c->class_id == TCls_Creature)) ? md_peek(c) : NULL;
    if (rec == NULL)
        return;
    struct CreatureControl *cctrl = creature_control_get_from_thing(c);
    struct Coord3d pos;
    pos.z.val = 0;
    if ((t >= 10) && (t <= 5200) && ((t % 8) == 0))
    {
        pos.x.val = subtile_coord_center(slab_subtile_center(px));
        pos.y.val = subtile_coord_center(slab_subtile_center(py));
        pos.z.val = get_floor_height_at(&pos);
        move_thing_in_map(c, &pos);
    }
    if ((t == 2500) || (t == 5200))
        SYNCLOG("Counts test: T+%ld: in the pool since T+10: water count %u (%d of them today; %d a day count)", t,
            (unsigned)rec->c[MdC_Water], (int)rec->stand_water, MD_STAND_DAY);
    if (t == 5290)
        SYNCLOG("Counts test: T+%ld: at home: farthest from home %u, put down at %d", t, (unsigned)rec->c[MdC_FarDist], (int)rec->far_base);
    if ((t == 5300) && world_test_stray_spot(&fx, &fy))
    {
        world_load_chunks_round(fx + 8, fy, 1);
        pos.x.val = subtile_coord_center(slab_subtile_center(fx));
        pos.y.val = subtile_coord_center(slab_subtile_center(fy));
        pos.z.val = get_floor_height_at(&pos);
        move_thing_in_map(c, &pos);
        cctrl->dropped_turn = game.play_gameturn; /* (as the Hand's drop marks it) */
        SYNCLOG("Counts test: T+%ld: put down %ld slabs from home", t, md_home_distance(c));
    }
    if (t == 5400)
        SYNCLOG("Counts test: T+%ld: after the put-down: farthest from home %u, put down at %d", t, (unsigned)rec->c[MdC_FarDist],
            (int)rec->far_base);
    if ((t > 5400) && (t <= 5400 + 5 * MD_SAMPLE) && (((t - 5400) % MD_SAMPLE) == 0))
    {
        /* five walking steps of two slabs, one a sample */
        pos.x.val = subtile_coord_center(slab_subtile_center(fx + 2 * (MapSlabCoord)((t - 5400) / MD_SAMPLE)));
        pos.y.val = subtile_coord_center(slab_subtile_center(fy));
        pos.z.val = get_floor_height_at(&pos);
        move_thing_in_map(c, &pos);
    }
    if (t == 5700)
        SYNCLOG("Counts test: T+%ld: after ten slabs walked on: %ld slabs from home, farthest from home %u, put down at %d", t,
            md_home_distance(c), (unsigned)rec->c[MdC_FarDist], (int)rec->far_base);
    /* the fights, at home */
    if ((t == 5990) || (t == 7354))
    {
        pos = heart->mappos;
        pos.x.val += 4 * COORD_PER_STL;
        if (t == 5990)
            move_thing_in_map(c, &pos);
        else
            SYNCLOG("Counts test: T+%ld: the Hand puts it down: %d", t, (int)dump_first_held_thing_on_map(my_player_number,
                pos.x.stl.num, pos.y.stl.num, true));
    }
    if ((t == 6000) || (t == 6210) || (t == 7300) || (t == 8700))
    {
        pos = c->mappos;
        pos.x.val += COORD_PER_STL;
        struct Thing *h = create_creature(&pos, (ThingModel)get_id(creature_desc, "ARCHER"), PLAYER_GOOD);
        hero = thing_is_invalid(h) ? 0 : h->index;
    }
    if (t == 7350)
        SYNCLOG("Counts test: T+%ld: in a fight %d; the Hand picks it up: %d", t, (int)(cctrl->combat_flags != 0),
            (int)place_thing_in_power_hand(c, my_player_number));
    if ((t == 6100) || (t == 6300) || (t == 7352) || (t == 8800))
    {
        struct Thing *h = thing_get(hero);
        if (thing_exists(h) && (h->class_id == TCls_Creature) && (h->owner == PLAYER_GOOD))
            delete_thing_structure(h, 0);
        hero = 0;
    }
    if ((t == 6200) || (t == 6400) || (t == 7450) || (t == 8900))
        SYNCLOG("Counts test: T+%ld: fights it came out of standing: %u (the last that counted began at turn %u)", t,
            (unsigned)rec->c[MdC_Survived], (unsigned)rec->fight_turn);
}

/** Test hook KFX_WORLD_MOREVICETEST=T: nine Orcs of the local keeper at level 9, one per new vice, each with the
 *  misdeeds that should earn it (its counter well above the others, as play would leave it); T: each rises to level
 *  10 and gets its offer (the offer's own check); T+2..T+10: the first choice is picked through the keeper's own
 *  packet, one a turn; T+40: each vice's effect is measured; T+60: Comes Back Angry's Orc is knocked out, and gets up
 *  at T+140 (the samples see both). */
static ThingIndex md_mv_orc[MdV_COUNT];

static long md_mv_hit(struct Thing *victim, struct Thing *attacker)
{
    return (long)world_misdeeds_adjust_damage(victim, attacker, 100);
}

static void md_morevicetest_hook(void)
{
    static long base = -2;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_MOREVICETEST");
        base = (v != NULL) ? atol(v) : -1;
        if (base >= 0)
            SYNCLOG("More vices test: KFX_WORLD_MOREVICETEST at turn %ld", base);
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart) || (t < 0) || (t > 200))
        return;
    if (t == 0)
    {
        for (int v = MD_OLD_VICES + 1; v < MdV_COUNT; v++)
        {
            struct Coord3d pos = heart->mappos;
            pos.x.val += (2 + (v % 5)) * COORD_PER_STL;
            pos.y.val += (4 + (v / 5)) * COORD_PER_STL;
            struct Thing *orc = create_thing_at_position_then_move_to_valid_and_add_light(&pos, TCls_Creature, md_kind("ORC"),
                my_player_number);
            if (thing_is_invalid(orc))
                continue;
            struct CreatureControl *cctrl = creature_control_get_from_thing(orc);
            set_creature_level(orc, 8);
            struct MdRec *rec = md_get(orc, cctrl);
            if (rec == NULL)
                continue;
            /* a little of everything, and much of its own misdeed */
            rec->c[MdC_KillsHero] += 1;
            rec->c[MdC_NightFights] += 1;
            static const uint32_t much[MdV_COUNT] = {0, 0, 0, 0, 0, 0, 20, 10, 9000, 200, 300, 120, 2, 30, 15};
            rec->c[md_vice_source[v]] += much[v];
            set_creature_level(orc, 9);
            md_check_offer(rec, orc, 10);
            md_mv_orc[v] = orc->index;
            SYNCLOG("More vices test: %s's Orc %d is offered %s or %s", md_vice_names[v], (int)orc->index, md_vice_names[rec->offer[0]],
                md_vice_names[rec->offer[1]]);
        }
        return;
    }
    if ((t >= 2) && (t < 2 + MdV_COUNT - MD_OLD_VICES - 1))
    {
        const int v = MD_OLD_VICES + 1 + (int)(t - 2);
        if (md_mv_orc[v] != 0)
            set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, md_mv_orc[v], 0, 0, 0);
        return;
    }
    if ((t == 62) && (md_mv_orc[MdV_Glutton] != 0))
    {
        /* the Rap Sheet of Glutton for Punishment's Orc, for a screenshot (KFX_WORLD_SHOTS): the query panel's packet */
        set_players_packet_action(get_my_player(), PckA_PlyrQueryCreature, md_mv_orc[MdV_Glutton], 0, 0, 0);
        return;
    }
    if (t == 66)
    {
        turn_off_menu(GMnu_CREATURE_QUERY1);
        turn_off_menu(GMnu_CREATURE_QUERY2);
        turn_off_menu(GMnu_CREATURE_QUERY3);
        turn_off_menu(GMnu_CREATURE_QUERY4);
        turn_on_menu(GMnu_CREATURE_QUERY5);
        SYNCLOG("More vices test: the Rap Sheet is open: %d", (int)menu_is_active(GMnu_CREATURE_QUERY5));
        return;
    }
    if (t == 30)
    {
        for (int v = MD_OLD_VICES + 1; v < MdV_COUNT; v++)
        {
            struct Thing *orc = thing_get(md_mv_orc[v]);
            const struct MdRec *rec = thing_exists(orc) ? md_peek(orc) : NULL;
            SYNCLOG("More vices test: %s's Orc has %s (rank %d)", md_vice_names[v], (rec != NULL) ? md_vice_names[rec->vices[0]] : "?",
                (rec != NULL) ? md_vice_rank(rec, v) : -1);
        }
        return;
    }
    if (t == 40)
    {
        struct Coord3d pos = heart->mappos;
        pos.y.val += 10 * COORD_PER_STL;
        struct Thing *hero = create_creature(&pos, md_kind("ARCHER"), PLAYER_GOOD);
        struct Thing *wild = create_creature(&pos, md_kind("SPIDER"), PLAYER_GOOD);
        struct Thing *rival = create_creature(&pos, md_kind("ORC"), (PlayerNumber)((my_player_number == 1) ? 2 : 1));
        if (thing_is_invalid(hero) || thing_is_invalid(wild) || thing_is_invalid(rival))
        {
            SYNCLOG("More vices test: no targets");
            return;
        }
        struct CreatureControl *wctrl = creature_control_get_from_thing(wild);
        wctrl->party.objective = CHeroTsk_DefendSpawn;
        wctrl->party.original_objective = CHeroTsk_DefendSpawn; /* (wild) */
        struct Thing *o;
        o = thing_get(md_mv_orc[MdV_Poacher]);
        SYNCLOG("More vices test: Poacher: 100 on a wild Spider does %ld; on a hero %ld", md_mv_hit(wild, o), md_mv_hit(hero, o));
        o = thing_get(md_mv_orc[MdV_Backstabber]);
        SYNCLOG("More vices test: Backstabber: 100 on a rival's Orc does %ld; on a hero %ld", md_mv_hit(rival, o), md_mv_hit(hero, o));
        o = thing_get(md_mv_orc[MdV_Glutton]);
        {
            const HitPoints full = o->health;
            const long a = md_mv_hit(hero, o);
            o->health = creature_control_get_from_thing(o)->max_health * 2 / 5;
            SYNCLOG("More vices test: Glutton: 100 on a hero at full health %ld, at 40%% health %ld", a, md_mv_hit(hero, o));
            o->health = full;
        }
        o = thing_get(md_mv_orc[MdV_Workaholic]);
        SYNCLOG("More vices test: Workaholic: work %d%%; another Orc %d%%", world_misdeeds_work_percent(o),
            world_misdeeds_work_percent(thing_get(md_mv_orc[MdV_Poacher])));
        o = thing_get(md_mv_orc[MdV_Heartbreaker]);
        {
            const long off = md_mv_hit(hero, o);
            struct Coord3d keep = hero->mappos;
            const MapSlabCoord sx = subtile_slab(hero->mappos.x.stl.num), sy = subtile_slab(hero->mappos.y.stl.num);
            const PlayerNumber was = slabmap_owner(get_slabmap_block(sx, sy));
            set_slab_owner(sx, sy, PLAYER_GOOD);
            SYNCLOG("More vices test: Heartbreaker: 100 on a hero off its ground %ld, on its own ground %ld", off, md_mv_hit(hero, o));
            set_slab_owner(sx, sy, was);
            (void)keep;
        }
        o = thing_get(md_mv_orc[MdV_Scarred]);
        {
            const HitPoints full = o->health;
            const long a = world_misdeeds_adjust_damage(o, hero, 100);
            o->health = creature_control_get_from_thing(o)->max_health / 5;
            SYNCLOG("More vices test: Battle-Scarred: a hero's 100 on it at full health %ld, at 20%% health %ld", a,
                (long)world_misdeeds_adjust_damage(o, hero, 100));
            o->health = full;
        }
        o = thing_get(md_mv_orc[MdV_LongWay]);
        {
            const long nearby = md_mv_hit(hero, o);
            struct Coord3d keep = o->mappos;
            MapSlabCoord fx, fy;
            if (world_test_stray_spot(&fx, &fy))
            {
                struct Coord3d far = o->mappos;
                far.x.val = subtile_coord_center(slab_subtile_center(fx));
                far.y.val = subtile_coord_center(slab_subtile_center(fy));
                move_thing_in_map(o, &far);
                SYNCLOG("More vices test: Long Way From Home: 100 on a hero near home %ld, %ld slabs out %ld", nearby,
                    md_home_distance(o), md_mv_hit(hero, o));
                move_thing_in_map(o, &keep);
            }
        }
        o = thing_get(md_mv_orc[MdV_Webbed]);
        {
            const long dry = world_misdeeds_speed(o, 100);
            const MapSlabCoord sx = subtile_slab(o->mappos.x.stl.num), sy = subtile_slab(o->mappos.y.stl.num);
            struct SlabMap *slb = get_slabmap_block(sx, sy);
            const SlabKind kind = slb->kind;
            slb->kind = SlbT_WATER; /* (the kind alone: the speed reads only that) */
            SYNCLOG("More vices test: Webbed Toes: speed 100 on dry ground %ld, in water %ld", dry, world_misdeeds_speed(o, 100));
            slb->kind = kind;
        }
        delete_thing_structure(hero, 0);
        delete_thing_structure(wild, 0);
        delete_thing_structure(rival, 0);
        return;
    }
    struct Thing *angry = thing_get(md_mv_orc[MdV_Angry]);
    if (!thing_exists(angry))
        return;
    if (t == 60)
    {
        make_creature_unconscious(angry);
        SYNCLOG("More vices test: Comes Back Angry: its Orc is knocked out (unconscious %d)", (int)creature_is_being_unconscious(angry));
    }
    if (t == 140)
    {
        make_creature_conscious(angry);
        SYNCLOG("More vices test: Comes Back Angry: its Orc gets up (unconscious %d)", (int)creature_is_being_unconscious(angry));
    }
    if (t == 200)
    {
        struct Coord3d pos = heart->mappos;
        pos.y.val += 10 * COORD_PER_STL;
        struct Thing *hero = create_creature(&pos, md_kind("ARCHER"), PLAYER_GOOD);
        if (!thing_is_invalid(hero))
        {
            SYNCLOG("More vices test: Comes Back Angry: 100 on a hero after it got up %ld", md_mv_hit(hero, angry));
            delete_thing_structure(hero, 0);
        }
    }
}

/** Join test hooks. Server: KFX_WORLD_A3JOIN=T: at T, every joined player (a human with a user other than the
 *  server's) gets, by its heart, an Orc of level 10 with an offer waiting (Poacher, from wild kills: the offer's own
 *  check), and twelve creatures its portal "brought" at level 50 (renown 36: the next pass tells the player). Client:
 *  KFX_WORLD_A3JOINC=T: at T it opens its own Orc in the query panel (the panel's own packet); T+40 logs the record
 *  the server streams for it; T+41 picks the first choice (the Rap Sheet's own packet); T+200 logs the record again.
 *  The client also logs every server notice it gets. */

static void md_jointest_server(void)
{
    static long base = -2;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_A3JOIN");
        base = (v != NULL) ? atol(v) : -1;
    }
    if ((base < 0) || ((long)game.play_gameturn != base) || !netstate_is_server())
        return;
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
    {
        const NetUserId u = get_player_primary_user(get_player(p));
        const struct Thing *heart = get_player_soul_container(p);
        if (!world_player_is_human(p) || (u <= SERVER_ID) || (u >= MAX_NET_USERS) || !thing_exists(heart))
            continue;
        struct Coord3d pos = heart->mappos;
        pos.x.val += 3 * COORD_PER_STL;
        struct Thing *orc = create_thing_at_position_then_move_to_valid_and_add_light(&pos, TCls_Creature, md_kind("ORC"), p);
        if (thing_is_invalid(orc))
            continue;
        set_creature_level(orc, 9);
        struct MdRec *rec = md_get(orc, creature_control_get_from_thing(orc));
        if (rec != NULL)
        {
            rec->c[MdC_KillsWild] += 20;
            md_check_offer(rec, orc, 10);
        }
        for (int i = 0; i < 12; i++)
        {
            struct Coord3d vpos = heart->mappos;
            vpos.y.val += (3 + i % 3) * COORD_PER_STL;
            struct Thing *vet = create_thing_at_position_then_move_to_valid_and_add_light(&vpos, TCls_Creature, md_kind("TROLL"), p);
            if (thing_is_invalid(vet))
                continue;
            set_creature_level(vet, 49);
            struct MdRec *vrec = md_get(vet, creature_control_get_from_thing(vet));
            if (vrec != NULL)
                vrec->flags |= MdF_Portal;
        }
        SYNCLOG("Join test (server): player %d (user %d): an Orc index %d with an offer (%s or %s), and twelve veterans", (int)p, (int)u,
            (int)orc->index, (rec != NULL) ? md_vice_names[rec->offer[0]] : "?", (rec != NULL) ? md_vice_names[rec->offer[1]] : "?");
    }
}

void world_misdeeds_client_turn(void)
{
    static long base = -2;
    static ThingIndex orc = 0;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_A3JOINC");
        base = (v != NULL) ? atol(v) : -1;
        md_jointest_client = (base >= 0);
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    if (t == 0)
    {
        const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
        long k = slist->index;
        for (int n = 0; (k > 0) && (n < THINGS_COUNT); n++)
        {
            const struct Thing *c = thing_get(k);
            if (thing_is_invalid(c))
                break;
            k = c->next_of_class;
            if ((c->owner == my_player_number) && (c->model == md_kind("ORC")))
                orc = c->index;
        }
        SYNCLOG("Join test (client): player %d, its Orc is index %d; opening it in the query panel", (int)my_player_number, (int)orc);
        if (orc != 0)
            set_players_packet_action(get_my_player(), PckA_PlyrQueryCreature, orc, 0, 0, 0);
        return;
    }
    if ((orc == 0) || ((t != 40) && (t != 41) && (t != 200)))
        return;
    const struct Thing *c = thing_get(orc);
    if (t == 41)
    {
        set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, orc, 0, 0, 0);
        SYNCLOG("Join test (client): picks the first choice");
        return;
    }
    const struct MdRec *rec = thing_exists(c) ? md_peek(c) : NULL;
    if (rec == NULL)
    {
        SYNCLOG("Join test (client): turn %ld: no record for the Orc in the view", t);
        return;
    }
    SYNCLOG("Join test (client): turn %ld: the view: vice %s, offer %s or %s, wild kills %u", t, md_vice_names[rec->vices[0]],
        md_vice_names[rec->offer[0]], md_vice_names[rec->offer[1]], (unsigned)rec->c[MdC_KillsWild]);
}

/** Test hook KFX_WORLD_AWAYTEST=T on a dedicated server (with KFX_WORLD_DAYNIGHT=300,300, so nights start at turns
 *  300 + 600k): at T (a night) the joined player gets an Orc (level 5) and a wild Spider fed a chicken (the hand's
 *  feeding function); the Orc is hurt by 10 every 100 turns (the stock damage). The runner then drops the client: the
 *  caretaker takes the dungeon. Every 500 turns the log shows whether the player is away, the Orc's damage soaked
 *  (it must not grow while away) and the stray (it must stay one while away). */
static void md_awaytest_hook(void)
{
    static long base = -2;
    static ThingIndex orc = 0, spider = 0;
    static PlayerNumber who = -1;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_AWAYTEST");
        base = (v != NULL) ? atol(v) : -1;
    }
    if ((base < 0) || !netstate_is_server())
        return;
    const long t = (long)game.play_gameturn;
    if (t == base)
    {
        for (PlayerNumber p = 0; (p < PLAYERS_COUNT) && (who < 0); p++)
        {
            const NetUserId u = get_player_primary_user(get_player(p));
            if (world_player_is_human(p) && (u > SERVER_ID) && (u < MAX_NET_USERS) && thing_exists(get_player_soul_container(p)))
                who = p;
        }
        if (who < 0)
        {
            SYNCLOG("Away test: no joined player");
            return;
        }
        const struct Thing *heart = get_player_soul_container(who);
        struct Coord3d pos = heart->mappos;
        pos.x.val += 3 * COORD_PER_STL;
        struct Thing *o = create_thing_at_position_then_move_to_valid_and_add_light(&pos, TCls_Creature, md_kind("ORC"), who);
        if (!thing_is_invalid(o))
        {
            set_creature_level(o, 4);
            md_get(o, creature_control_get_from_thing(o));
            orc = o->index;
        }
        struct Coord3d spos = heart->mappos;
        spos.y.val += 8 * COORD_PER_STL * 3;
        struct Thing *s = create_thing_at_position_then_move_to_valid_and_add_light(&spos, TCls_Creature, md_kind("SPIDER"), PLAYER_GOOD);
        if (!thing_is_invalid(s))
        {
            init_creature_level(s, 3);
            struct CreatureControl *scctrl = creature_control_get_from_thing(s);
            scctrl->party.objective = CHeroTsk_DefendSpawn;
            scctrl->party.original_objective = CHeroTsk_DefendSpawn; /* (wild) */
            scctrl->party.target_plyr_idx = -1;
            reveal_map_area(who, s->mappos.x.stl.num - 3, s->mappos.x.stl.num + 3, s->mappos.y.stl.num - 3, s->mappos.y.stl.num + 3);
            struct Thing *food = create_object(&s->mappos, ObjMdl_ChickenMature, who, -1);
            const TbBool fed = !thing_is_invalid(food) && world_misdeeds_stray_feed(who, food, s);
            if (!fed && thing_exists(food))
                delete_thing_structure(food, 0);
            spider = s->index;
            SYNCLOG("Away test: player %d: an Orc %d, and a Spider %d fed: %d", (int)who, (int)orc, (int)spider, (int)fed);
        }
        return;
    }
    if ((who < 0) || (t < base))
        return;
    struct Thing *o = thing_get(orc);
    if (thing_exists(o) && (o->class_id == TCls_Creature) && (((t - base) % 100) == 50))
        apply_damage_to_thing_and_display_health(o, 10, PLAYER_GOOD);
    if (((t - base) % 500) != 250)
        return;
    const struct MdRec *orec = thing_exists(o) ? md_peek(o) : NULL;
    struct Thing *s = thing_get(spider);
    const struct MdRec *srec = (thing_exists(s) && (s->class_id == TCls_Creature)) ? md_peek(s) : NULL;
    SYNCLOG("Away test: turn %ld (day %ld, %s): player %d away %d; the Orc has soaked %u; the Spider: owner %d, stray of %d, nights %d",
        t, world_day_number(), world_is_day() ? "day" : "night", (int)who, (int)world_caretaker_active(who),
        (orec != NULL) ? (unsigned)orec->c[MdC_Soaked] : 0, thing_exists(s) ? (int)s->owner : -1,
        (srec != NULL) ? (int)srec->stray_by - 1 : -1, (srec != NULL) ? (int)srec->stray_nights : 0);
}

/** Test hook KFX_WORLD_RAPVIEW=T (local only): at T the local keeper's creature with something waiting on its Rap Sheet
 *  (a vice or lesson to pick, or a contract being renegotiated) is opened in the query panel (the panel's packet); at
 *  T+6 its Rap Sheet tab is shown, for a screenshot (KFX_WORLD_SHOTS). */
static void md_rapview_hook(void)
{
    static long base = -2;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_RAPVIEW");
        base = (v != NULL) ? atol(v) : -1;
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    if (t == 0)
    {
        ThingIndex pick = 0;
        const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
        long k = slist->index;
        for (int n = 0; (k > 0) && (n < THINGS_COUNT); n++)
        {
            const struct Thing *c = thing_get(k);
            if (thing_is_invalid(c))
                break;
            k = c->next_of_class;
            const struct MdRec *rec = (c->owner == my_player_number) ? md_peek(c) : NULL;
            if ((rec != NULL) && ((rec->offer[0] != MdV_None) || rec->contract_open || (rec->lesson_learn != 0) || (rec->lesson_master != 0)))
                pick = c->index;
        }
        SYNCLOG("Rap view test: creature %d has something waiting", (int)pick);
        if (pick != 0)
            set_players_packet_action(get_my_player(), PckA_PlyrQueryCreature, pick, 0, 0, 0);
    }
    if (t == 6)
    {
        turn_off_menu(GMnu_CREATURE_QUERY1);
        turn_off_menu(GMnu_CREATURE_QUERY2);
        turn_off_menu(GMnu_CREATURE_QUERY3);
        turn_off_menu(GMnu_CREATURE_QUERY4);
        turn_on_menu(GMnu_CREATURE_QUERY5);
    }
}

/** Test hook KFX_WORLD_SHEETFULL=T,mode (single player, with KFX_WORLD_RAPVIEW before it): at T the record of the
 *  creature the Information tool shows is filled, to see how the creature window sets out a full Rap Sheet (set
 *  closer, in smaller letters). Mode 1: five vices of several ranks (two can be marked), a post, a regimen's gains,
 *  a breed, kills while possessed, past lives, masteries, the three virtues and a rest. Mode 2: three vices, a sin
 *  and a lesson to pick. Mode 3: four vices and a fifth to pick, with a post and a regimen's gains. */
static void md_sheetfull_hook(void)
{
    static long base = -2;
    static int mode = 1;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_SHEETFULL");
        const char *comma = (v != NULL) ? strchr(v, ',') : NULL;
        base = (v != NULL) ? atol(v) : -1;
        if (comma != NULL)
            mode = atoi(comma + 1);
    }
    if ((base < 0) || ((long)game.play_gameturn != base))
        return;
    struct Thing *thing = md_panel_thing();
    struct MdRec *rec = thing_is_invalid(thing) ? NULL : md_peek(thing);
    if (rec == NULL)
    {
        SYNCLOG("Sheet full test: no creature with a record is shown");
        return;
    }
    const struct CreatureModelConfig *crconf = creature_stats_get_from_thing(thing);
    static const uint8_t vices[MD_VICE_SLOTS] = {MdV_Glutton, MdV_Backstabber, MdV_Night, MdV_Heartbreaker, MdV_LongWay};
    const int held = (mode == 2) ? 3 : (mode == 3) ? 4 : 5;
    for (int i = 0; i < MdC_Spare; i++)
        rec->c[i] = 1234u * (unsigned)(i + 3);
    rec->c[MdC_Spare] = 0;
    for (int i = 0; i < MD_VICE_SLOTS; i++)
    {
        rec->vices[i] = (i < held) ? vices[i] : MdV_None;
        rec->rank[i] = 0;
        /* (deeds since it was taken: none, one unit, two, three, four - ranks I, II, II, III, III) */
        const uint32_t deeds = (i < held) ? md_deeds(rec, vices[i]) : 0;
        rec->rank_base[i] = deeds - min(deeds, md_rank_unit[vices[i]] * (uint32_t)i);
    }
    rec->offer[0] = MdV_None;
    rec->offer[1] = MdV_None;
    rec->sin = 0;
    rec->lesson_learn = 0;
    rec->lesson_master = 0;
    rec->post = MdP_Standard;
    rec->regimen = MdR_Brawn;
    rec->reg_levels[0] = 3;
    rec->reg_levels[1] = 1;
    rec->reg_levels[3] = 2;
    md_test_level = 100;
    if (mode == 1)
    {
        rec->breed = MdB_FireBeetle;
        rec->driven = 17;
        rec->past_lives = 2;
        rec->past_model = thing->model;
        rec->foreign_inst = (uint16_t)crconf->learned_instance_id[0];
        rec->mastery[0] = 2;
        rec->mastery[1] = 1;
        rec->virtues = MdT_ALL;
        rec->rest_day = (uint16_t)(world_day_number() + 1);
        rec->rack_marks = 0x03;
        rec->rack_samples = 3;
    } else if (mode == 2)
    {
        rec->sin = 1;
        rec->lesson_learn = 1;
        rec->lesson_master = 2;
        rec->mastery[1] = 1;
    } else
    {
        rec->offer[0] = MdV_LongWay;
        rec->offer[1] = MdV_Bane;
    }
    SYNCLOG("Sheet full test: creature %d's record is filled (mode %d)", (int)thing->index, mode);
}

/** Test hook KFX_WORLD_SHEETSCROLL=T (local, at normal speed, with KFX_WORLD_RAPVIEW before it, on a screen or with
 *  a sheet that makes the creature window's sheet scroll): the game turns the wheel and plays a mouse on the bar, as
 *  a player does. At T it says how much higher than its room the sheet is; at T+4 and T+8 the wheel is turned down a
 *  notch; at T+20 the mouse clicks the bar's lower arrow; at T+40 the bar under its thumb (a page down); at T+60 the
 *  bar above its thumb (a page up); at T+80 its upper arrow. Ten turns after each it says where the sheet stands. */
static void md_scrolltest_hook(void)
{
    static long base = -2;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_SHEETSCROLL");
        base = (v != NULL) ? atol(v) : -1;
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    if ((t < 0) || (t > 100))
        return;
    struct TbPoint none = {0, 0};
    if (t == 0)
        SYNCLOG("Sheet scroll test: the sheet is %d pixels higher than its room (a line %d, a page %d), bar %d,%d %dx%d",
            md_sheet_over, md_sheet_step, md_sheet_page, md_scroll_box[0], md_scroll_box[1], md_scroll_box[2], md_scroll_box[3]);
    if ((t == 4) || (t == 8))
        mouseControl(MActn_WHEELMOVEDOWN, &none);
    if ((t == 20) || (t == 40) || (t == 60) || (t == 80))
    {
        if (md_scroll_box[2] <= 0)
        {
            SYNCLOG("Sheet scroll test: no bar (the sheet fits)");
            return;
        }
        struct GuiButton fake;
        memset(&fake, 0, sizeof(fake));
        fake.width = md_scroll_box[2];
        fake.height = md_scroll_box[3];
        int top, height;
        md_thumb(&fake, &top, &height);
        const int x = md_scroll_box[0] + md_scroll_box[2] / 2;
        const int y = md_scroll_box[1] + ((t == 20) ? md_scroll_box[3] - md_scroll_box[2] / 2 :
            (t == 40) ? (top + height + md_scroll_box[3] - md_scroll_box[2]) / 2 :
            (t == 60) ? (md_scroll_box[2] + top) / 2 : md_scroll_box[2] / 2);
        LbMouseSetPositionInitial(x / max((int)pixel_size, 1), y / max((int)pixel_size, 1));
        SYNCLOG("Sheet scroll test: the mouse goes to (%d,%d) on the bar and presses", x, y);
        mouseControl(MActn_LBUTTONDOWN, &none);
    }
    if ((t == 23) || (t == 43) || (t == 63) || (t == 83))
        mouseControl(MActn_LBUTTONUP, &none);
    if ((t == 14) || (t == 30) || (t == 50) || (t == 70) || (t == 90))
        SYNCLOG("Sheet scroll test: the sheet stands at %d of %d", md_sheet_scroll, md_sheet_over);
}

/** Test hook KFX_WORLD_RAPMOUSE=T (local, at normal speed, with KFX_WORLD_RAPVIEW before it so that a Rap Sheet with
 *  an open renegotiation is on screen): the game plays a mouse itself, through its own mouse feed, as a player clicks:
 *  at T, T+40 and T+80 the mouse goes to the middle of the contract row and presses the left button (and lets go three
 *  turns later). Fourteen turns after each press the log says whether the mouse is over the row's button and what the
 *  creature is signed on. */
static void md_rapmouse_hook(void)
{
    static long base = -2;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_RAPMOUSE");
        base = (v != NULL) ? atol(v) : -1;
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    if ((t < 0) || (t > 100))
        return;
    struct TbPoint none = {0, 0};
    const long step = t % 40;
    if (step == 0)
    {
        if (md_contract_box[2] <= 0)
        {
            SYNCLOG("Rap mouse test: no contract row to click (no renegotiation open on the sheet shown)");
            return;
        }
        const int x = md_contract_box[0] + md_contract_box[2] / 2, y = md_contract_box[1] + md_contract_box[3] / 2;
        LbMouseSetPositionInitial(x / max((int)pixel_size, 1), y / max((int)pixel_size, 1));
        SYNCLOG("Rap mouse test: the mouse goes to the contract row's middle (%d,%d) and presses", x, y);
        mouseControl(MActn_LBUTTONDOWN, &none);
    }
    if (step == 3)
        mouseControl(MActn_LBUTTONUP, &none);
    if (step == 14)
    {
        const struct Thing *thing = md_panel_thing();
        const struct MdRec *rec = thing_is_invalid(thing) ? NULL : md_peek(thing);
        int over = -1;
        const int x = md_contract_box[0] + md_contract_box[2] / 2, y = md_contract_box[1] + md_contract_box[3] / 2;
        for (int i = 0; i < ACTIVE_BUTTONS_COUNT; i++)
        {
            const struct GuiButton *b = &active_buttons[i];
            if (((b->flags & LbBtnF_Active) != 0) && (b->click_event == world_misdeeds_regimen_click) && (b->content.lval == 1))
                over = (int)check_if_pos_is_over_button(b, x, y);
        }
        SYNCLOG("Rap mouse test: after the click: the mouse at the row's middle is over the row's button: %d; the creature "
            "is on %s%s", over, (rec != NULL) ? md_contract_names[rec->contract] : "?",
            ((rec != NULL) && rec->contract_open) ? " (renegotiating)" : "");
    }
}

/** Test hook KFX_WORLD_CONTRACTPLAY=T (single player, for a game somebody plays): a dungeon in which Contracts can be
 *  tried at once. At T: a Treasury west of the heart, a Lair east of it and a Hatchery south of it (three by three
 *  each); a named Orc, a named Dark Mistress and an unnamed Bile Demon of level 9 beside the heart. At T+40 the three
 *  reach level 10: the two named ones want to renegotiate (their Rap Sheets' contract row can be clicked until their
 *  next level), the Bile Demon takes its kind's Bed and Board. From T+600 on, once a minute, a Dwarf of level 1
 *  comes in beside the heart: a kill for Plunder's bounty and for Glory. Nothing else is done for the player. */
static void md_contractplay_hook(void)
{
    static long base = -2;
    static ThingIndex made[3];
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_CONTRACTPLAY");
        base = (v != NULL) ? atol(v) : -1;
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if ((t < 0) || !thing_exists(heart))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    if (t == 0)
    {
        static const char *const rooms[3] = {"TREASURE", "LAIR", "GARDEN"};
        const MapSlabCoord at[3][2] = {{(MapSlabCoord)(hx - 6), (MapSlabCoord)(hy - 1)}, {(MapSlabCoord)(hx + 4), (MapSlabCoord)(hy - 1)},
            {(MapSlabCoord)(hx - 1), (MapSlabCoord)(hy + 4)}};
        /* (the way to each room from the heart's cleared ground) */
        place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx - 3), slab_subtile_center(hy), my_player_number, 0);
        place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx + 3), slab_subtile_center(hy), my_player_number, 0);
        place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(hx), slab_subtile_center(hy + 3), my_player_number, 0);
        for (int r = 0; r < 3; r++)
        {
            int slabs = 0;
            for (int dy = 0; dy < 3; dy++)
                for (int dx = 0; dx < 3; dx++)
                {
                    place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(at[r][0] + dx), slab_subtile_center(at[r][1] + dy),
                        my_player_number, 0);
                    if (!room_is_invalid(place_room(my_player_number, (RoomKind)get_id(room_desc, rooms[r]),
                        slab_subtile_center(at[r][0] + dx), slab_subtile_center(at[r][1] + dy))))
                        slabs++;
                }
            SYNCLOG("Contracts play: a %s of %d slabs", rooms[r], slabs);
        }
        static const char *const kinds[3] = {"ORC", "DARK_MISTRESS", "BILE_DEMON"};
        for (int k = 0; k < 3; k++)
        {
            struct Coord3d pos = heart->mappos;
            pos.x.val += (2 + 2 * k) * COORD_PER_STL;
            pos.y.val += 5 * COORD_PER_STL;
            struct Thing *thing = create_creature(&pos, (ThingModel)get_id(creature_desc, kinds[k]), my_player_number);
            struct CreatureControl *cctrl = thing_is_invalid(thing) ? NULL : creature_control_get_from_thing(thing);
            struct MdRec *rec = ((cctrl == NULL) || creature_control_invalid(cctrl)) ? NULL : md_get(thing, cctrl);
            if (rec == NULL)
                continue;
            set_creature_level(thing, 8);
            if (k < 2)
                rec->flags |= MdF_Renamed; /* (as a rename does) */
            made[k] = thing->index;
            SYNCLOG("Contracts play: a %s of level 9 (%s), %s, wage %ld", thing_model_name(thing), creature_own_name(thing),
                (k < 2) ? "named" : "not named", (long)calculate_correct_creature_pay(thing));
        }
    }
    if (t == 40)
    {
        for (int k = 0; k < 3; k++)
        {
            struct Thing *thing = thing_get(made[k]);
            if (thing_exists(thing) && (thing->class_id == TCls_Creature) && (thing->owner == my_player_number))
                set_creature_level(thing, 9);
        }
        SYNCLOG("Contracts play: the three reach level 10");
    }
    if ((t >= 600) && ((t % 1200) == 600))
    {
        long model = get_id(creature_desc, "DWARFA");
        if (model <= 0)
            model = get_id(creature_desc, "ARCHER");
        struct Coord3d pos = heart->mappos;
        pos.x.val -= 6 * COORD_PER_STL;
        pos.y.val -= 6 * COORD_PER_STL;
        struct Thing *hero = (model > 0) ? create_creature(&pos, (ThingModel)model, PLAYER_GOOD) : INVALID_THING;
        if (!thing_is_invalid(hero))
            set_creature_level(hero, 0);
        SYNCLOG("Contracts play: a hero of level 1 beside the heart: %s", thing_is_invalid(hero) ? "not made" : thing_model_name(hero));
    }
    if ((t > 0) && ((t % 1200) == 0))
    {
        for (int k = 0; k < 3; k++)
        {
            const struct Thing *thing = thing_get(made[k]);
            const struct MdRec *rec = (thing_exists(thing) && (thing->class_id == TCls_Creature)) ? md_peek(thing) : NULL;
            if (rec == NULL)
                continue;
            SYNCLOG("Contracts play: the %s (level %d): contract %s%s, wage %ld", thing_model_name(thing),
                (int)creature_control_get_from_thing(thing)->exp_level + 1, md_contract_names[rec->contract],
                rec->contract_open ? " (renegotiating)" : "", (long)calculate_correct_creature_pay(thing));
        }
    }
}

/** Test hook KFX_WORLD_A3COOP=T (a dedicated server with a co-op pair): at T, for the first two allied human keepers p
 *  and q, an Orc of p's with Backstabber (set on its record: the pick itself is tested elsewhere), an Orc of q's and an
 *  Orc of a keeper allied to neither; the log shows Backstabber's 100 on each (it must not count against the ally). */
static void md_cooptest_hook(void)
{
    static long base = -2;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_A3COOP");
        base = (v != NULL) ? atol(v) : -1;
    }
    if ((base < 0) || ((long)game.play_gameturn != base))
        return;
    PlayerNumber pa = -1, pb = -1;
    for (PlayerNumber a = 0; (a < PLAYERS_COUNT) && (pa < 0); a++)
        for (PlayerNumber b = a + 1; (b < PLAYERS_COUNT) && (pa < 0); b++)
            if (world_player_is_human(a) && world_player_is_human(b) && players_are_mutual_allies(a, b) &&
                thing_exists(get_player_soul_container(a)) && thing_exists(get_player_soul_container(b)))
            {
                pa = a;
                pb = b;
            }
    PlayerNumber pc = -1;
    for (PlayerNumber c = 0; (c < PLAYERS_COUNT) && (pc < 0); c++)
        if ((c != pa) && (c != pb) && (c != PLAYER_GOOD) && (c != game.neutral_player_num) && !players_are_mutual_allies(pa, c))
            pc = c;
    if ((pa < 0) || (pc < 0))
    {
        SYNCLOG("Co-op test: no allied pair (%d, %d) or no third keeper (%d)", (int)pa, (int)pb, (int)pc);
        return;
    }
    struct Coord3d pos = get_player_soul_container(pa)->mappos;
    pos.x.val += 3 * COORD_PER_STL;
    struct Thing *o = create_creature(&pos, md_kind("ORC"), pa);
    pos.y.val += COORD_PER_STL;
    struct Thing *ally = create_creature(&pos, md_kind("ORC"), pb);
    pos.y.val += COORD_PER_STL;
    struct Thing *other = create_creature(&pos, md_kind("ORC"), pc);
    if (thing_is_invalid(o) || thing_is_invalid(ally) || thing_is_invalid(other))
    {
        SYNCLOG("Co-op test: creatures not made");
        return;
    }
    struct MdRec *rec = md_get(o, creature_control_get_from_thing(o));
    if (rec == NULL)
        return;
    rec->vices[0] = MdV_Backstabber;
    md_slot_start(rec, 0);
    SYNCLOG("Co-op test: players %d and %d allied %d; Backstabber's 100 on the ally's Orc does %ld, on player %d's Orc %ld", (int)pa,
        (int)pb, (int)players_are_mutual_allies(pa, pb), (long)world_misdeeds_adjust_damage(ally, o, 100), (int)pc,
        (long)world_misdeeds_adjust_damage(other, o, 100));
    delete_thing_structure(o, 0);
    delete_thing_structure(ally, 0);
    delete_thing_structure(other, 0);
}

void world_misdeeds_turn(void)
{
    md_cooptest_hook();
    md_jointest_server();
    md_strays_pass();
    md_renown_pass();
    md_fill_views();
    md_bodies_turn();
    md_bodytest_hook();
    md_testunits_hook();
    md_viceranktest_hook();
    md_sintest_hook();
    md_virtuetest_hook();
    md_lessontest_hook();
    md_regimentest_hook();
    md_contracttest_hook();
    md_soultest_hook();
    md_eyestest_hook();
    md_marchtest_hook();
    md_alumnitest_hook();
    md_breedtest_hook();
    md_straytest_hook();
    md_counttest_hook();
    md_morevicetest_hook();
    md_awaytest_hook();
    md_rapview_hook();
    md_sheetfull_hook();
    md_scrolltest_hook();
    md_rapmouse_hook();
    md_contractplay_hook();
    static long base = -2;
    static int level = 10, tab = 0;
    static char name[32] = "Gnasher";
    static ThingIndex idx = 0;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_MISDEEDTEST");
        base = -1;
        if (v != NULL)
        {
            char raw[32] = "";
            if (sscanf(v, "%ld,%d,%31[^,],%d", &base, &level, raw, &tab) >= 3)
                snprintf(name, sizeof(name), "%s", raw);
        }
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    if (t == -40)
    {
        struct Thing *made = md_test_creature();
        idx = thing_is_invalid(made) ? 0 : made->index;
        SYNCLOG("Misdeeds test: creature %s index %d", thing_is_invalid(made) ? "none" : thing_model_name(made), (int)idx);
        if (idx != 0)
            md_test_send_rename(made, "TooSoon"); /* refused: below level 10 */
    }
    struct Thing *thing = thing_get(idx);
    if ((idx == 0) || !thing_exists(thing))
        return;
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (t == 0)
    {
        struct MdRec *rec = md_get(thing, cctrl);
        set_creature_level(thing, level - 1);
        rec->c[MdC_KillsHero] += 7;
        rec->c[MdC_NightFights] += 4;
        rec->c[MdC_Lava] += 20;
        rec->c[MdC_Soaked] += 4321;
        rec->c[MdC_Work] += 150;
        rec->c[MdC_FarDist] = 37;
        md_check_offer(rec, thing, level);
        cctrl->force_health_flower_displayed = true;
        md_test_label_idx = idx;
        query_creature(get_local_user(), idx, true, false);
    }
    if (t == 4)
    {
        turn_off_query_menus();
        turn_on_menu((tab == 3) ? GMnu_CREATURE_QUERY3 : GMnu_CREATURE_QUERY5);
    }
    if (t == 60)
        set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, idx, 0, 0, 0);
    if (t == 100)
        md_test_send_rename(thing, name);
    if (t == 140)
        md_test_send_rename(thing, "Twice"); /* refused: once only */
    if (t == 180)
    {
        /* a hero beside it: Do-Gooder's Bane shows in the hits */
        struct Coord3d pos = thing->mappos;
        pos.x.val += COORD_PER_STL;
        struct Thing *hero = create_creature(&pos, (ThingModel)get_id(creature_desc, "ARCHER"), PLAYER_GOOD);
        SYNCLOG("Misdeeds test: hero %s index %d beside it", thing_is_invalid(hero) ? "none" : thing_model_name(hero),
            thing_is_invalid(hero) ? 0 : (int)hero->index);
    }
    if (t == 400)
    {
        /* the save round trip, and a freeze and thaw as the chunk manager does them */
        const uint16_t id = world_misdeeds_freeze(thing);
        const struct MdRec *rec = md_peek(thing);
        const struct MdRec before = (rec != NULL) ? *rec : (struct MdRec){0};
        const size_t size = world_misdeeds_save_size();
        uint8_t *buf = (uint8_t *)malloc(size + 64);
        world_misdeeds_save(buf);
        struct Coord3d pos = thing->mappos;
        const ThingModel model = thing->model;
        char kept[CREATURE_NAME_MAX];
        memcpy(kept, cctrl->creature_name, sizeof(kept));
        delete_thing_structure(thing, 0);
        const TbBool survived = (md_rec(id) != NULL);
        const TbBool loaded = world_misdeeds_load(buf, size);
        free(buf);
        struct Thing *again = create_creature(&pos, model, my_player_number);
        if (!thing_is_invalid(again))
        {
            init_creature_level(again, level - 1);
            struct CreatureControl *actrl = creature_control_get_from_thing(again);
            memcpy(actrl->creature_name, kept, sizeof(kept));
            world_misdeeds_thaw(again, id);
            idx = again->index;
            md_test_label_idx = idx;
            const struct MdRec *after = md_peek(again);
            struct MdRec cmp = (after != NULL) ? *after : (struct MdRec){0};
            cmp.flags |= MdF_Frozen; /* (the thaw cleared it) */
            SYNCLOG("Misdeeds test: freeze id %d, record kept through the delete: %d, save %d bytes loaded: %d, record after the "
                "thaw %s (heroes %u, vice %s, renamed %d, name '%s')", (int)id, (int)survived, (int)size, (int)loaded,
                ((after != NULL) && (memcmp(&cmp, &before, sizeof(cmp)) == 0)) ? "IDENTICAL" : "DIFFERENT",
                (after != NULL) ? (unsigned)after->c[MdC_KillsHero] : 0, (after != NULL) ? md_vice_names[after->vices[0]] : "?",
                (after != NULL) ? (int)flag_is_set(after->flags, MdF_Renamed) : -1, creature_own_name(again));
        }
    }
}
