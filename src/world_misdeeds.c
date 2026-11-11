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
#include "world_daynight.h"
#include "world_wild.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "bflib_basics.h"
#include "bflib_guibtns.h"
#include "bflib_sprfnt.h"
#include "bflib_video.h"
#include "bflib_vidraw.h"
#include "bflib_datetm.h"
#include "config.h"
#include "config_creature.h"
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
#include "post_inc.h"

#define MD_SAMPLE 32            /* a creature is sampled one turn in this many */
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

enum MdVice { MdV_None, MdV_Bane, MdV_Territorial, MdV_Night, MdV_Asbestos, MdV_Shortcut, MdV_COUNT };

enum MdFlags { MdF_Renamed = 0x01, MdF_InFight = 0x02, MdF_Down = 0x04, MdF_Frozen = 0x08, MdF_HasLast = 0x10,
    MdF_Dying = 0x20 /* it is dying now: gone without a body, it is lost for good */ };
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
};

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
struct MdNear { ThingIndex idx; uint32_t turn; };
#define MD_NEAR_MAX 48
static struct MdNear md_terr[MD_NEAR_MAX];
static struct MdNear md_short[MD_NEAR_MAX];

static const int md_vice_levels[MD_VICE_SLOTS] = {10, 25, 50, 75, 100};
static const char *const md_vice_names[MdV_COUNT] = {
    "", "Do-Gooder's Bane", "Territorial", "Afraid of Nothing in the Dark", "Asbestos Hide", "Knows a Shortcut"};
static const char *const md_vice_short[MdV_COUNT] = {
    "", "Do-Gooder's Bane", "Territorial", "No Fear in the Dark", "Asbestos Hide", "Knows a Shortcut"};
static const char *const md_vice_does[MdV_COUNT] = {
    "", "+20% damage to heroes", "Shields the weak on your land", "Swift, hard to hit at night",
    "Strolls through lava unharmed", "Its group hurries far from home"};
/* what earns each vice, and its weight when the two strongest misdeeds are chosen */
static const int md_vice_source[MdV_COUNT] = {0, MdC_KillsHero, MdC_HomeFights, MdC_NightFights, MdC_Lava, MdC_AwayDist};
static const int md_vice_weight[MdV_COUNT] = {0, 40, 4, 40, 8, 1};

static int md_log_budget = 24; /* effect log lines per run */
static void md_lost(struct MdRec *rec, PlayerNumber owner, ThingModel model, const char *name);

/******************************************************************************/
/* Records */

static struct MdRec *md_rec(int id)
{
    if ((id <= 0) || (id > md_top) || !md_recs[id].used)
        return NULL;
    return &md_recs[id];
}

_Static_assert(sizeof(struct MdRec) <= sizeof(game.world_misdeed_view[0]), "a Misdeeds record must fit its view in the game state");

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
        struct MdRec *view = (struct MdRec *)game.world_misdeed_view[me];
        return view->used ? view : NULL;
    }
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (creature_control_invalid(cctrl) || (cctrl->misdeed_id == 0))
        return NULL;
    return md_rec(cctrl->misdeed_id);
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
    if ((rec->offer[0] != MdV_None) || (md_vice_count(rec) + (int)rec->c[MdC_Spare] >= md_slots_for_level(level)))
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
        const long score = (long)rec->c[md_vice_source[v]] * md_vice_weight[v];
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

static void md_near_add(struct MdNear *list, ThingIndex idx)
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
}

/** A creature of this owner on the list, seen lately, within `slabs` of the thing (not the thing itself unless
 *  self_ok): its vice works here. */
static struct Thing *md_near_find(const struct MdNear *list, const struct Thing *thing, int slabs, TbBool self_ok)
{
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
        if ((abs(coord_slab(other->mappos.x.val) - coord_slab(thing->mappos.x.val)) <= slabs) &&
            (abs(coord_slab(other->mappos.y.val) - coord_slab(thing->mappos.y.val)) <= slabs))
            return other;
    }
    return INVALID_THING;
}

static TbBool md_on_own_land(const struct Thing *thing)
{
    const struct SlabMap *slb = get_slabmap_for_subtile(thing->mappos.x.stl.num, thing->mappos.y.stl.num);
    return !slabmap_block_invalid(slb) && (slabmap_owner(slb) == thing->owner);
}

/** Slabs from its keeper's heart; -1 when the heart isn't on the map. */
static long md_home_distance(const struct Thing *thing)
{
    const struct Thing *heart = get_player_soul_container(thing->owner);
    if (!thing_exists(heart))
        return -1;
    const long dx = labs((long)coord_slab(thing->mappos.x.val) - coord_slab(heart->mappos.x.val));
    const long dy = labs((long)coord_slab(thing->mappos.y.val) - coord_slab(heart->mappos.y.val));
    return max(dx, dy) + min(dx, dy) / 2;
}

/******************************************************************************/
/* Counting */

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
    if (thing_is_picked_up(thing))
        return;
    /* lava and water */
    if (flag_is_set(thing->movement_flags, TMvF_IsOnLava))
        rec->c[MdC_Lava]++;
    if (flag_is_set(thing->movement_flags, TMvF_IsOnWater))
        rec->c[MdC_Water]++;
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
    if (fight)
    {
        if (!flag_is_set(rec->flags, MdF_InFight) && night)
            rec->c[MdC_NightFights]++;
        if (own_land)
            rec->c[MdC_HomeFights]++;
        rec->flags |= MdF_InFight;
    } else
    {
        if (flag_is_set(rec->flags, MdF_InFight) && !down)
            rec->c[MdC_Survived]++;
        rec->flags &= ~MdF_InFight;
    }
    if (down && !flag_is_set(rec->flags, MdF_Down))
        rec->c[MdC_Falls]++;
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
        if ((uint32_t)home > rec->c[MdC_FarDist])
            rec->c[MdC_FarDist] = (uint32_t)home;
        if ((home >= MD_AWAY_SLABS) && flag_is_set(rec->flags, MdF_HasLast))
        {
            const long moved = labs((long)(wx - rec->last_wx)) + labs((long)(wy - rec->last_wy));
            if (moved < 2 * MD_SAMPLE) /* (a walk, not a drop from the hand) */
                rec->c[MdC_AwayDist] += (uint32_t)moved;
        }
    }
    rec->last_wx = (int32_t)wx;
    rec->last_wy = (int32_t)wy;
    rec->flags |= MdF_HasLast;
    /* vices */
    const int level = cctrl->exp_level + 1;
    md_check_offer(rec, thing, level);
    const TbBool away = (home >= MD_AWAY_SLABS);
    if (md_has(rec, MdV_Territorial) && own_land)
        md_near_add(md_terr, thing->index);
    if (md_has(rec, MdV_Shortcut) && away)
        md_near_add(md_short, thing->index);
    uint8_t bits = 0;
    if (night && md_has(rec, MdV_Night))
        bits |= MdS_Night;
    if (away && !thing_is_invalid(md_near_find(md_short, thing, MD_SHORTCUT_SLABS, true)))
        bits |= MdS_Shortcut;
    if (bits != rec->speed_bits)
    {
        const long before = cctrl->max_speed;
        rec->speed_bits = bits;
        cctrl->max_speed = calculate_correct_creature_maxspeed(thing);
        if (md_log_budget > 0)
        {
            md_log_budget--;
            SYNCLOG("Misdeeds: %s index %d speed %ld -> %ld (%s%s)", thing_model_name(thing), (int)thing->index, before,
                (long)cctrl->max_speed, (bits & MdS_Night) ? "night " : "", (bits & MdS_Shortcut) ? "shortcut" : "");
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

int world_misdeeds_train_bonus(const struct Thing *thing)
{
    if (!world_mode_active())
        return 0;
    const struct MdRec *rec = md_peek(thing);
    if ((rec == NULL) || (rec->influence_turn == 0) || (rec->influence_turn + 2 * MD_SAMPLE < (uint32_t)game.play_gameturn))
        return 0;
    return MD_INFLUENCE_PERCENT;
}

void world_misdeeds_on_kill(struct Thing *victim, struct Thing *killertng)
{
    if (!world_mode_active())
        return;
    if (thing_exists(killertng) && (killertng->class_id == TCls_Creature) && world_player_is_human(killertng->owner) &&
        (killertng->owner != victim->owner))
    {
        struct CreatureControl *kctrl = creature_control_get_from_thing(killertng);
        struct MdRec *rec = creature_control_invalid(kctrl) ? NULL : md_get(killertng, kctrl);
        if (rec != NULL)
        {
            const int what = world_wild_creature(victim) ? MdC_KillsWild : (victim->owner == PLAYER_GOOD) ? MdC_KillsHero :
                is_neutral_thing(victim) ? MdC_KillsWild : MdC_KillsKeeper;
            rec->c[what]++;
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

void world_misdeeds_on_damage(struct Thing *victim, HitPoints dmg)
{
    if (dmg <= 0)
        return;
    struct MdRec *rec = md_peek(victim);
    if (rec != NULL)
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
            !world_player_is_human(thing->owner) || thing_is_picked_up(thing))
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
        if ((arec != NULL) && md_has(arec, MdV_Bane))
        {
            const HitPoints ndmg = dmg * (100 + MD_BANE_PERCENT) / 100;
            if (md_log_budget > 0)
            {
                md_log_budget--;
                SYNCLOG("Misdeeds: Do-Gooder's Bane: %s index %d hits hero %s for %d instead of %d", thing_model_name(attacker),
                    (int)attacker->index, thing_model_name(victim), (int)ndmg, (int)dmg);
            }
            dmg = ndmg;
        }
    }
    const struct MdRec *vrec = md_peek(victim);
    if ((vrec != NULL) && md_has(vrec, MdV_Night) && !world_is_day() && (GAME_RANDOM(100) < MD_NIGHT_MISS))
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
        struct Thing *guard = md_near_find(md_terr, victim, MD_TERRITORIAL_SLABS, false);
        if (!thing_is_invalid(guard) && md_on_own_land(guard) && !creature_is_being_unconscious(guard))
        {
            const struct CreatureControl *gctrl = creature_control_get_from_thing(guard);
            const struct CreatureControl *vctrl = creature_control_get_from_thing(victim);
            const HitPoints share = dmg * MD_TERRITORIAL_SHARE / 100;
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

long world_misdeeds_speed(const struct Thing *thing, long speed)
{
    if (md_top == 0)
        return speed;
    const struct MdRec *rec = md_peek(thing);
    if ((rec == NULL) || (rec->speed_bits == 0) || !world_mode_active())
        return speed;
    if (rec->speed_bits & MdS_Night)
        speed = speed * (100 + MD_NIGHT_SPEED) / 100;
    if (rec->speed_bits & MdS_Shortcut)
        speed = speed * (100 + MD_SHORTCUT_SPEED) / 100;
    return speed;
}

TbBool world_misdeeds_lava_proof(const struct Thing *thing)
{
    if (md_top == 0)
        return false;
    const struct MdRec *rec = md_peek(thing);
    return (rec != NULL) && md_has(rec, MdV_Asbestos) && world_mode_active();
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
    rec->flags &= ~(MdF_HasLast | MdF_InFight);
    rec->speed_bits = 0;
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
}

/* the extra: magic, record size, count, then the records 1..count; then "MSB1", the bodies and the inheritances */
#define MD_TAIL_SIZE (4 + sizeof(md_body) + sizeof(md_blood))
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
    if ((top == 0) && !md_has_tail())
        return 0;
    return 12 + (size_t)top * sizeof(struct MdRec) + (md_has_tail() ? MD_TAIL_SIZE : 0);
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
    for (uint32_t i = 0; i < head[2]; i++)
        memcpy(&md_recs[i + 1], buf + 12 + (size_t)i * head[1], take);
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
    SYNCLOG("Misdeeds: %d records loaded, %d bodies waiting", md_top, md_body_count);
    return true;
}

/******************************************************************************/
/* Unfinished Business and Bad Blood */

static void md_tell(PlayerNumber plyr_idx, const char *text)
{
    if (is_my_player_number(plyr_idx))
        message_add(MsgType_Blank, 0, text);
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

void world_misdeeds_dying(struct Thing *thing)
{
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
    snprintf(md_dying.name, sizeof(md_dying.name), "%s", creature_own_name(thing));
}

uint16_t world_misdeeds_corpse_begin(struct Thing *thing)
{
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if (creature_control_invalid(cctrl) || (cctrl->misdeed_id == 0) || (cctrl->misdeed_id != md_dying.rec))
        return 0;
    struct MdRec *rec = md_rec(cctrl->misdeed_id);
    if ((rec == NULL) || !flag_is_set(rec->flags, MdF_Dying) || (md_vice_count(rec) < MD_BODY_VICES))
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
    if (thing_is_invalid(deadtng) || (body == NULL))
    {
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
    memcpy(cctrl->creature_name, was.name, min(sizeof(cctrl->creature_name), sizeof(was.name)));
    cctrl->creature_name[sizeof(cctrl->creature_name) - 1] = 0;
    /* one vice poorer: the last one earned, and its slot stays empty */
    int gone = MdV_None;
    for (int i = MD_VICE_SLOTS - 1; i >= 0; i--)
        if (rec->vices[i] != MdV_None)
        {
            gone = rec->vices[i];
            rec->vices[i] = MdV_None;
            break;
        }
    rec->c[MdC_Spare]++;
    rec->flags &= ~(MdF_Dying | MdF_Frozen);
    create_effect(&pos, TngEff_Explosion3, was.owner);
    delete_thing_structure(deadtng, 0);
    SYNCLOG("Misdeeds: Unfinished Business: %s (%s, level %d) of player %d is back as thing %d without %s; %d vices left",
        was.name, creature_code_name(was.model), was.level + 1, (int)was.owner, (int)thing->index, md_vice_names[gone],
        md_vice_count(rec));
    char text[160];
    snprintf(text, sizeof(text), "%s is back from the grave, but lost the vice %s.", was.name, md_vice_names[gone]);
    md_tell(was.owner, text);
    return true;
}

void world_misdeeds_attracted(struct Thing *thing)
{
    if (!world_mode_active() || !world_player_is_human(thing->owner))
        return;
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

void world_misdeeds_packet_pick(PlayerNumber plyr_idx, ThingIndex idx, int choice)
{
    if (!world_mode_active())
        return;
    struct Thing *thing = thing_get(idx);
    struct MdRec *rec = (thing_exists(thing) && (thing->owner == plyr_idx)) ? md_peek(thing) : NULL;
    if ((rec == NULL) || (choice < 0) || (choice > 1) || (rec->offer[choice] == MdV_None) || md_has(rec, rec->offer[choice]))
    {
        SYNCLOG("Misdeeds: pick %d for thing %d by player %d refused", choice, (int)idx, (int)plyr_idx);
        return;
    }
    const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    const int count = md_vice_count(rec);
    if ((count >= MD_VICE_SLOTS) || (count + (int)rec->c[MdC_Spare] >= md_slots_for_level(cctrl->exp_level + 1)))
        return;
    rec->vices[count] = rec->offer[choice];
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
    LbTextSetWindow(x, y, w, 2 * px + 4);
    const int line = max(LbTextLineHeight(), 1);
    LbTextDrawResized(0, 0, max(36 * px / line, 4), text); /* (px: about the capitals' height) */
    LbTextSetWindow(0, 0, MyScreenWidth, MyScreenHeight);
    RendererSetDrawFlags(flg_mem);
}

/** Sampled time as game hours and tenths (a day and a night are 24 of them). */
static void md_hours(char *out, size_t size, uint32_t samples)
{
    const unsigned long tenths = (unsigned long)samples * MD_SAMPLE / 100;
    snprintf(out, size, "%lu.%luh", tenths / 10, tenths % 10);
}

void world_misdeeds_rap_background(struct GuiMenu *gmnu)
{
    gui_creature_query_background2(gmnu);
    md_pick_box[0][2] = 0;
    md_pick_box[1][2] = 0;
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
    for (int i = 0; i < count; i++)
    {
        md_text(x0, y, w, row_px, 1, md_vice_short[rec->vices[i]]);
        y += S(8);
        if (!pending)
        {
            md_text(x0, y, w, small_px, 1, md_vice_does[rec->vices[i]]);
            y += S(7);
        }
    }
    int next = count;
    if (pending)
    {
        snprintf(text, sizeof(text), mine ? "Level %d vice: pick one" : "Level %d vice: not picked", md_vice_levels[count]);
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

void world_misdeeds_pick_maintain(struct GuiButton *gbtn)
{
    const int k = (int)gbtn->content.lval & 1;
    if (md_pick_box[k][2] > 0)
    {
        gbtn->scr_pos_x = md_pick_box[k][0];
        gbtn->scr_pos_y = md_pick_box[k][1];
        gbtn->width = md_pick_box[k][2];
        gbtn->height = md_pick_box[k][3];
        gbtn->flags |= LbBtnF_Visible | LbBtnF_Enabled;
    } else
    {
        gbtn->scr_pos_x = -4000;
        gbtn->flags &= ~(LbBtnF_Visible | LbBtnF_Enabled);
    }
}

void world_misdeeds_pick_draw(struct GuiButton *gbtn)
{
    const int k = (int)gbtn->content.lval & 1;
    const struct Thing *thing = md_panel_thing();
    const struct MdRec *rec = thing_is_invalid(thing) ? NULL : md_peek(thing);
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
    set_players_packet_action(get_my_player(), PckA_WorldMisdeedPick, thing->index, (unsigned long)(gbtn->content.lval & 1), 0, 0);
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
    const char *name = creature_own_name(thing);
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
            if ((game.world_misdeed_view_thing[p] != thing->index) ||
                (memcmp(game.world_misdeed_view[p], rec, sizeof(struct MdRec)) != 0))
            {
                memset(game.world_misdeed_view[p], 0, sizeof(game.world_misdeed_view[p]));
                memcpy(game.world_misdeed_view[p], rec, sizeof(struct MdRec));
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
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_BODYTEST");
        base = (v != NULL) ? atol(v) : -1;
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

void world_misdeeds_turn(void)
{
    md_fill_views();
    md_bodies_turn();
    md_bodytest_hook();
    md_testunits_hook();
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
