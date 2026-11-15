/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_chemistry.c
 *     Endless world: Foul Chemistry - attacks react with each other, whoever made them (a creature, a trap, a spell,
 *     a hero or a keeper).
 *
 * Six pairings, each with an effect of its own:
 * - Gas and fire: a gas cloud (a Bile Demon's, a Poison Gas trap's) that a fire shot passes through, or that drifts
 *   over lava (a Lava trap's), explodes: every creature within 3 subtiles loses a quarter of its health (at least 30),
 *   once in 16 turns. (Explosion.)
 * - Ice and boulders: a Boulder that hits a frozen creature shatters the ice and does twice its damage. (Ice shards.)
 * - Lightning and water: a bolt that hits a creature standing in water hits every creature standing in that water
 *   (8 slabs round at most, 24 creatures) for the same damage; not its shooter. (Sparks on each.)
 * - Fire and ice: a frozen creature hit by fire thaws. (Steam.)
 * - Fire and water: fire does half its damage to a creature standing in water. (Steam.)
 * - Ice and water: a Freeze that hits a creature standing in water freezes every creature standing in that water;
 *   not its caster. (Ice on each.)
 * A reaction spreading from an attack (the pool's lightning and freeze, the blast of gas a keeper's fire lit) spares
 * the attacker's own creatures and its allies' (a co-op pair): no keeper harms another without both agreeing.
 * Only in endless worlds; nothing changes elsewhere.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_chemistry.h"
#include <stdlib.h>
#include <string.h>
#include "bflib_basics.h"
#include "config.h"
#include "config_creature.h"
#include "config_magic.h"
#include "config_effects.h"
#include "creature_control.h"
#include "game_legacy.h"
#include "globals.h"
#include "map_data.h"
#include "player_data.h"
#include "player_utils.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_effects.h"
#include "thing_list.h"
#include "thing_shots.h"
#include "thing_stats.h"
#include "thing_physics.h"
#include "world_stream.h"
#include "bflib_planar.h"
#include "map_blocks.h"
#include "player_instances.h"
#include "thing_navigate.h"
#include "thing_factory.h"
#include "post_inc.h"

#define CHEM_SCAN_TURNS 4          /* gas clouds are checked for fire every 4 turns */
#define CHEM_GAS_REACH (2 * COORD_PER_STL) /* a fire shot this close to a gas cloud lights it */
#define CHEM_BLAST_REACH (3 * COORD_PER_STL)
#define CHEM_BLAST_PERCENT 25      /* of a creature's health */
#define CHEM_BLAST_MIN 30
#define CHEM_BLAST_EVERY 16        /* a creature is hurt by one blast in 16 turns at most */
#define CHEM_WATER_REACH 8         /* slabs round the hit creature that the water is followed */
#define CHEM_WATER_VICTIMS 24
#define CHEM_FIRE_SHOTS 64

long shot_hit_creature_at(struct Thing *shotng, struct Thing *trgtng, struct Coord3d *pos);
static void chem_cooptest_hook(void);

static GameTurn chem_blast_turn[THINGS_COUNT];
static int chem_log_budget = 40;

/******************************************************************************/
/* Kinds */

static ThingModel chem_id(const struct NamedCommand *desc, const char *name)
{
    const long id = get_id(desc, name);
    return (id > 0) ? (ThingModel)id : 0;
}

/* The kinds by name, looked up once a game (audit 2026-11-13: these were string scans on every shot hit). A game
   starts again from a low turn, and the table is looked up again then; within a game the configuration is fixed. */
enum ChemName { CN_Fireball, CN_Firebomb, CN_FlameBreath, CN_Lightning, CN_TrapLightning, CN_GodLightning,
    CN_LightningBall, CN_Gas1, CN_Gas2, CN_Gas3, CN_COUNT };
static ThingModel chem_names[CN_COUNT];
static GameTurn chem_names_turn;
static TbBool chem_names_made = false;

static ThingModel chem_name(int which)
{
    if (!chem_names_made || (game.play_gameturn < chem_names_turn))
    {
        static const char *const shots[CN_Gas1] = {"SHOT_FIREBALL", "SHOT_FIREBOMB", "SHOT_FLAME_BREATH", "SHOT_LIGHTNING",
            "SHOT_TRAP_LIGHTNING", "SHOT_GOD_LIGHTNING", "SHOT_LIGHTNING_BALL"};
        for (int i = 0; i < CN_Gas1; i++)
            chem_names[i] = chem_id(shot_desc, shots[i]);
        chem_names[CN_Gas1] = chem_id(effect_desc, "EFFECT_GAS_1");
        chem_names[CN_Gas2] = chem_id(effect_desc, "EFFECT_GAS_2");
        chem_names[CN_Gas3] = chem_id(effect_desc, "EFFECT_GAS_3");
        chem_names_made = true;
    }
    chem_names_turn = game.play_gameturn;
    return chem_names[which];
}

static TbBool chem_is_one_of(ThingModel model, int first, int last)
{
    for (int i = first; i <= last; i++)
    {
        const ThingModel m = chem_name(i);
        if ((m > 0) && (model == m))
            return true;
    }
    return false;
}

/** A fire shot: the FIRE property, or one of the stock fire shots (the stock config marks none). */
static TbBool chem_shot_is_fire(ThingModel model)
{
    const struct ShotConfigStats *shotst = get_shot_model_stats(model);
    if (flag_is_set(shotst->model_flags, ShMF_Fire))
        return true;
    return chem_is_one_of(model, CN_Fireball, CN_FlameBreath);
}

static TbBool chem_shot_is_lightning(ThingModel model)
{
    return chem_is_one_of(model, CN_Lightning, CN_LightningBall);
}

static TbBool chem_effect_is_gas(ThingModel model)
{
    return chem_is_one_of(model, CN_Gas1, CN_Gas3);
}

static void chem_effect(const struct Coord3d *pos, const char *name, PlayerNumber owner)
{
    const ThingModel m = chem_id(effect_desc, name); /* (only when a reaction happens) */
    if (m > 0)
        create_effect(pos, m, owner);
}

/** The creatures on the map blocks of a box of subtiles, by the map's own lists (audit 2026-11-13: these walks went over
 *  every creature in the world for each bolt and blast). Calls fn for each; stops when it returns false. */
static void chem_creatures_in(MapSubtlCoord x0, MapSubtlCoord y0, MapSubtlCoord x1, MapSubtlCoord y1,
    TbBool (*fn)(struct Thing *, void *), void *data)
{
    x0 = max(x0, (MapSubtlCoord)0);
    y0 = max(y0, (MapSubtlCoord)0);
    x1 = min(x1, (MapSubtlCoord)(game.map_subtiles_x - 1));
    y1 = min(y1, (MapSubtlCoord)(game.map_subtiles_y - 1));
    for (MapSubtlCoord y = y0; y <= y1; y++)
        for (MapSubtlCoord x = x0; x <= x1; x++)
        {
            long i = get_mapwho_thing_index(get_map_block_at(x, y));
            unsigned long guard = 0;
            while ((i != 0) && (guard++ < THINGS_COUNT))
            {
                struct Thing *thing = thing_get(i);
                if (thing_is_invalid(thing))
                    break;
                i = thing->next_on_mapblk;
                if ((thing->class_id == TCls_Creature) && !fn(thing, data))
                    return;
            }
        }
}

/** A creature the reaction spreading from player `by`'s attack leaves alone: its own, and its allies'. */
static TbBool chem_spared(const struct Thing *thing, PlayerNumber by)
{
    if ((by < 0) || (by >= PLAYERS_COUNT))
        return false;
    return (thing->owner == by) || players_are_mutual_allies(by, thing->owner);
}

static TbBool chem_frozen(const struct Thing *thing)
{
    return creature_under_spell_effect(thing, CSAfF_Freeze);
}

static TbBool chem_in_water(const struct Thing *thing)
{
    const struct SlabMap *slb = get_slabmap_for_subtile(thing->mappos.x.stl.num, thing->mappos.y.stl.num);
    return !slabmap_block_invalid(slb) && (slb->kind == SlbT_WATER) && (thing->mappos.z.val <= get_floor_height_at(&thing->mappos) + COORD_PER_STL / 2);
}

/******************************************************************************/
/* The water a creature stands in: its slabs, CHEM_WATER_REACH round (a flood fill) */

#define CHEM_W (2 * CHEM_WATER_REACH + 1)
static uint8_t chem_water[CHEM_W][CHEM_W];
static MapSlabCoord chem_wx0, chem_wy0;

static void chem_fill_water(MapSlabCoord sx, MapSlabCoord sy)
{
    memset(chem_water, 0, sizeof(chem_water));
    chem_wx0 = sx - CHEM_WATER_REACH;
    chem_wy0 = sy - CHEM_WATER_REACH;
    static int16_t qx[CHEM_W * CHEM_W], qy[CHEM_W * CHEM_W];
    int head = 0, tail = 0;
    qx[tail] = CHEM_WATER_REACH;
    qy[tail++] = CHEM_WATER_REACH;
    chem_water[CHEM_WATER_REACH][CHEM_WATER_REACH] = 1;
    static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    while (head < tail)
    {
        const int x = qx[head], y = qy[head++];
        for (int d = 0; d < 4; d++)
        {
            const int nx = x + dx[d], ny = y + dy[d];
            if ((nx < 0) || (ny < 0) || (nx >= CHEM_W) || (ny >= CHEM_W) || chem_water[ny][nx])
                continue;
            const struct SlabMap *slb = get_slabmap_block(chem_wx0 + nx, chem_wy0 + ny);
            if (slabmap_block_invalid(slb) || (slb->kind != SlbT_WATER))
                continue;
            chem_water[ny][nx] = 1;
            qx[tail] = (int16_t)nx;
            qy[tail++] = (int16_t)ny;
        }
    }
}

struct ChemWaterWalk { struct Thing **out; int max, n; const struct Thing *except, *skip; PlayerNumber by; };

static TbBool chem_water_one(struct Thing *thing, void *data)
{
    struct ChemWaterWalk *w = (struct ChemWaterWalk *)data;
    if ((thing == w->except) || (thing == w->skip) || (thing->health <= 0) || thing_is_picked_up(thing) || !chem_in_water(thing) ||
        chem_spared(thing, w->by))
        return true;
    const int x = subtile_slab(thing->mappos.x.stl.num) - chem_wx0, y = subtile_slab(thing->mappos.y.stl.num) - chem_wy0;
    if ((x < 0) || (y < 0) || (x >= CHEM_W) || (y >= CHEM_W) || !chem_water[y][x])
        return true;
    w->out[w->n++] = thing;
    return (w->n < w->max);
}

/** The creatures standing in the water filled last (not `except` nor `skip`), up to `max`: only the water's own slabs
 *  are looked at. */
static int chem_water_creatures(struct Thing **out, int max, const struct Thing *except, const struct Thing *skip, PlayerNumber by)
{
    struct ChemWaterWalk w = {out, max, 0, except, skip, by};
    for (int sy = 0; (sy < CHEM_W) && (w.n < max); sy++)
        for (int sx = 0; (sx < CHEM_W) && (w.n < max); sx++)
            if (chem_water[sy][sx])
                chem_creatures_in(slab_subtile(chem_wx0 + sx, 0), slab_subtile(chem_wy0 + sy, 0),
                    slab_subtile(chem_wx0 + sx, 2), slab_subtile(chem_wy0 + sy, 2), chem_water_one, &w);
    return w.n;
}

/******************************************************************************/
/* Hits */

HitPoints world_chemistry_shot_damage(const struct Thing *shotng, struct Thing *target, HitPoints dmg)
{
    if (!world_mode_active() || !thing_exists(target) || (target->class_id != TCls_Creature) || (dmg <= 0))
        return dmg;
    const TbBool fire = chem_shot_is_fire(shotng->model);
    if (shot_is_boulder(shotng) && chem_frozen(target))
    {
        /* ice and boulders: it shatters */
        clean_spell_effect(target, CSAfF_Freeze);
        chem_effect(&target->mappos, "EFFECT_ICE_SHARD", target->owner);
        if (chem_log_budget > 0)
        {
            chem_log_budget--;
            SYNCLOG("Chemistry: a boulder shatters a frozen %s: %d damage, twice %d", thing_model_name(target), (int)(dmg * 2), (int)dmg);
        }
        return dmg * 2;
    }
    if (fire && chem_frozen(target))
    {
        /* fire and ice: it thaws */
        clean_spell_effect(target, CSAfF_Freeze);
        chem_effect(&target->mappos, "EFFECT_CLOUD", target->owner);
        if (chem_log_budget > 0)
        {
            chem_log_budget--;
            SYNCLOG("Chemistry: fire thaws a frozen %s", thing_model_name(target));
        }
    }
    if (fire && chem_in_water(target))
    {
        /* fire and water: steam */
        chem_effect(&target->mappos, "EFFECT_HARMLESS_GAS_2", target->owner);
        if (chem_log_budget > 0)
        {
            chem_log_budget--;
            SYNCLOG("Chemistry: fire on a %s in water turns to steam: %d damage of %d", thing_model_name(target), (int)(dmg / 2), (int)dmg);
        }
        return max(dmg / 2, (HitPoints)1);
    }
    return dmg;
}

void world_chemistry_shot_hit(const struct Thing *shotng, struct Thing *shooter, struct Thing *target, HitPoints dmg)
{
    if (!world_mode_active() || !thing_exists(target) || (target->class_id != TCls_Creature) || !chem_in_water(target))
        return;
    const struct ShotConfigStats *shotst = get_shot_model_stats(shotng->model);
    const TbBool bolt = chem_shot_is_lightning(shotng->model) && (dmg > 0);
    const SpellKind spell = shotst->cast_spell_kind;
    const TbBool freeze = (spell > 0) && flag_is_set(get_spell_config(spell)->spell_flags, CSAfF_Freeze);
    if (!bolt && !freeze)
        return;
    chem_fill_water(subtile_slab(target->mappos.x.stl.num), subtile_slab(target->mappos.y.stl.num));
    struct Thing *victims[CHEM_WATER_VICTIMS];
    const PlayerNumber by = thing_exists(shooter) ? shooter->owner : shotng->owner;
    const int n = chem_water_creatures(victims, CHEM_WATER_VICTIMS, target, thing_exists(shooter) ? shooter : NULL, by);
    CrtrExpLevel level = 0;
    if (thing_exists(shooter) && (shooter->class_id == TCls_Creature))
        level = creature_control_get_from_thing(shooter)->exp_level;
    for (int i = 0; i < n; i++)
    {
        if (bolt)
        {
            /* lightning and water: the whole pool */
            apply_damage_to_thing_and_display_health(victims[i], dmg, by);
            chem_effect(&victims[i]->mappos, "EFFECT_ELECTRIC_BALLS", by);
        } else
        {
            /* ice and water: the whole pool freezes */
            if (!chem_frozen(victims[i]) && !creature_is_immune_to_spell_effect(victims[i], CSAfF_Freeze))
            {
                apply_spell_effect_to_thing(victims[i], spell, level, by);
                chem_effect(&victims[i]->mappos, "EFFECT_HIT_FROZEN_UNIT", by);
            }
        }
    }
    if ((n > 0) && (chem_log_budget > 0))
    {
        chem_log_budget--;
        SYNCLOG("Chemistry: %s on a %s in water reaches %d more creatures in that water", bolt ? "lightning" : "a freeze",
            thing_model_name(target), n);
    }
}

/******************************************************************************/
/* Gas and fire */

#define CHEM_BLAST_VICTIMS 64
struct ChemBlastWalk { const struct Coord3d *pos; PlayerNumber by; struct Thing *list[CHEM_BLAST_VICTIMS]; int n; };

static TbBool chem_blast_one(struct Thing *thing, void *data)
{
    struct ChemBlastWalk *b = (struct ChemBlastWalk *)data;
    if ((thing->health <= 0) || thing_is_picked_up(thing) || (get_chessboard_distance(b->pos, &thing->mappos) > CHEM_BLAST_REACH) ||
        chem_spared(thing, b->by))
        return true;
    const GameTurn last = chem_blast_turn[thing->index];
    if ((last != 0) && (last <= game.play_gameturn) && (game.play_gameturn - last < CHEM_BLAST_EVERY))
        return true;
    for (int i = 0; i < b->n; i++)
        if (b->list[i] == thing)
            return true;
    b->list[b->n++] = thing;
    return (b->n < CHEM_BLAST_VICTIMS);
}

static void chem_blast(struct Thing *gas, PlayerNumber by, const char *why)
{
    struct Coord3d pos = gas->mappos;
    destroy_effect_thing(gas);
    chem_effect(&pos, "EFFECT_EXPLOSION_4", by);
    /* the creatures within reach, by the map's lists of the blast's box (first gathered: the damage can move things) */
    struct ChemBlastWalk b = {&pos, by, {0}, 0};
    const int r = CHEM_BLAST_REACH / COORD_PER_STL + 1;
    chem_creatures_in(pos.x.stl.num - r, pos.y.stl.num - r, pos.x.stl.num + r, pos.y.stl.num + r, chem_blast_one, &b);
    int hurt = 0;
    for (int i = 0; i < b.n; i++)
    {
        struct Thing *thing = b.list[i];
        if (!thing_exists(thing) || (thing->health <= 0))
            continue;
        chem_blast_turn[thing->index] = game.play_gameturn;
        const struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        const HitPoints full = creature_control_invalid(cctrl) ? thing->health : cctrl->max_health;
        apply_damage_to_thing_and_display_health(thing, max((HitPoints)(full * CHEM_BLAST_PERCENT / 100), (HitPoints)CHEM_BLAST_MIN), by);
        hurt++;
    }
    if (chem_log_budget > 0)
    {
        chem_log_budget--;
        SYNCLOG("Chemistry: a gas cloud at (%d,%d) explodes (%s): %d creatures hurt", (int)pos.x.stl.num, (int)pos.y.stl.num, why, hurt);
    }
}

void world_chemistry_turn(void)
{
    if (!world_mode_active())
        return;
    world_chemistry_test_hook();
    chem_cooptest_hook();
    if ((game.play_gameturn % CHEM_SCAN_TURNS) != 1)
        return;
    /* the fire shots in flight */
    struct Coord3d fire_pos[CHEM_FIRE_SHOTS];
    PlayerNumber fire_owner[CHEM_FIRE_SHOTS];
    int fires = 0;
    {
        const struct StructureList *slist = get_list_for_thing_class(TCls_Shot);
        long k = slist->index;
        for (int guard = 0; (k > 0) && (guard < THINGS_COUNT) && (fires < CHEM_FIRE_SHOTS); guard++)
        {
            const struct Thing *shot = thing_get(k);
            if (thing_is_invalid(shot))
                break;
            k = shot->next_of_class;
            if (chem_shot_is_fire(shot->model))
            {
                fire_pos[fires] = shot->mappos;
                fire_owner[fires++] = shot->owner;
            }
        }
    }
    const struct StructureList *slist = get_list_for_thing_class(TCls_Effect);
    long k = slist->index;
    for (int guard = 0; (k > 0) && (guard < THINGS_COUNT); guard++)
    {
        struct Thing *eff = thing_get(k);
        if (thing_is_invalid(eff))
            break;
        k = eff->next_of_class;
        if (!chem_effect_is_gas(eff->model))
            continue;
        int lit = -1;
        for (int f = 0; (f < fires) && (lit < 0); f++)
            if (get_chessboard_distance(&fire_pos[f], &eff->mappos) <= CHEM_GAS_REACH)
                lit = f;
        if (lit >= 0)
        {
            chem_blast(eff, fire_owner[lit], "fire");
            continue;
        }
        const struct SlabMap *slb = get_slabmap_for_subtile(eff->mappos.x.stl.num, eff->mappos.y.stl.num);
        if (!slabmap_block_invalid(slb) && (slb->kind == SlbT_LAVA))
            chem_blast(eff, -1, "lava"); /* (lava is nobody's attack: it spares nobody) */
    }
}

/******************************************************************************/
/** Test hook KFX_WORLD_A3COOP=T (a dedicated server with a co-op pair): at T a strip of water by keeper p's heart, two
 *  hero Knights and an Orc of p's ally q in it; a bolt of p's hits the first Knight (the shot's own hit). The second
 *  Knight must be hit by the pool, the ally's Orc spared. */
static void chem_cooptest_hook(void)
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
            if ((a != PLAYER_GOOD) && (b != PLAYER_GOOD) && (a != game.neutral_player_num) && (b != game.neutral_player_num) &&
                players_are_mutual_allies(a, b) && thing_exists(get_player_soul_container(a)) && thing_exists(get_player_soul_container(b)))
            {
                pa = a;
                pb = b;
            }
    if (pa < 0)
    {
        SYNCLOG("Co-op chemistry test: no allied pair");
        return;
    }
    const struct Thing *heart = get_player_soul_container(pa);
    const MapSlabCoord wx = subtile_slab(heart->mappos.x.stl.num) - 1, wy = subtile_slab(heart->mappos.y.stl.num) + 6;
    for (int i = 0; i < 3; i++)
        place_slab_type_on_map(SlbT_WATER, slab_subtile_center(wx + i), slab_subtile_center(wy), game.neutral_player_num, 0);
    struct Thing *th[3];
    const ThingModel models[3] = {chem_id(creature_desc, "KNIGHT"), chem_id(creature_desc, "KNIGHT"), chem_id(creature_desc, "ORC")};
    const PlayerNumber owners[3] = {PLAYER_GOOD, PLAYER_GOOD, pb};
    for (int i = 0; i < 3; i++)
    {
        struct Coord3d pos;
        pos.x.val = subtile_coord_center(slab_subtile_center(wx + i));
        pos.y.val = subtile_coord_center(slab_subtile_center(wy));
        pos.z.val = 0;
        th[i] = create_creature(&pos, models[i], owners[i]);
        if (thing_is_invalid(th[i]))
        {
            SYNCLOG("Co-op chemistry test: creatures not made");
            return;
        }
        pos.z.val = get_floor_height_at(&pos);
        move_thing_in_map(th[i], &pos);
    }
    const HitPoints k2 = th[1]->health, ally = th[2]->health;
    struct Thing *s = create_shot(&th[0]->mappos, chem_id(shot_desc, "SHOT_LIGHTNING"), pa);
    if (thing_is_invalid(s))
        return;
    shot_hit_creature_at(s, th[0], &th[0]->mappos);
    if (thing_exists(s))
        delete_thing_structure(s, 0);
    SYNCLOG("Co-op chemistry test: player %d's bolt on a Knight in the water (allied with %d: %d): the other Knight %d -> %d, the ally's Orc %d -> %d",
        (int)pa, (int)pb, (int)players_are_mutual_allies(pa, pb), (int)k2, (int)th[1]->health, (int)ally, (int)th[2]->health);
}

/* Test hook KFX_WORLD_CHEMTEST=T: Knights of the heroes beside the local keeper's heart (revealed), on a strip of water
   made there for the water pairings. T: gas and a fireball (gas and fire); T+20: a boulder on a frozen Knight and on
   a Knight that isn't (ice and boulders); T+40: a fireball on a frozen Knight (fire and ice); T+60: a fireball on a
   Knight in the water and one out of it (fire and water); T+80: lightning on a Knight in the water (lightning and
   water); T+100: a freeze on a Knight in the water (ice and water). The hits go through the shot's own hit. */

static struct Thing *chem_test_knight(MapSlabCoord sx, MapSlabCoord sy)
{
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(sx));
    pos.y.val = subtile_coord_center(slab_subtile_center(sy));
    pos.z.val = 0;
    struct Thing *k = create_thing_at_position_then_move_to_valid_and_add_light(&pos, TCls_Creature, chem_id(creature_desc, "KNIGHT"), PLAYER_GOOD);
    if (thing_is_invalid(k))
        return NULL;
    pos.z.val = get_floor_height_at(&pos);
    move_thing_in_map(k, &pos);
    init_creature_level(k, 4);
    return k;
}

static void chem_test_hit(const char *what, const char *shot, struct Thing *k)
{
    if (k == NULL)
        return;
    struct Thing *s = create_shot(&k->mappos, chem_id(shot_desc, shot), my_player_number);
    if (thing_is_invalid(s))
    {
        SYNCLOG("Chemistry test: no %s", shot);
        return;
    }
    if (shot_is_boulder(s))
        s->shot.damage = 200; /* (a stock boulder kills a Knight outright: the doubling would not show) */
    const HitPoints before = k->health;
    const TbBool frozen = chem_frozen(k), water = chem_in_water(k);
    shot_hit_creature_at(s, k, &k->mappos);
    SYNCLOG("Chemistry test: %s: %s (frozen %d, in water %d): health %d -> %d, frozen after %d", what, shot, (int)frozen, (int)water,
        (int)before, (int)k->health, (int)chem_frozen(k));
    if (thing_exists(s))
        delete_thing_structure(s, 0);
}

void world_chemistry_test_hook(void)
{
    static long base = -2;
    static struct Thing *kn[8];
    static MapSlabCoord wx, wy;
    static struct Coord3d spot[8];
    static struct Thing *own = NULL;
    static struct Coord3d own_spot;
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_CHEMTEST");
        base = (v != NULL) ? atol(v) : -1;
        if (base >= 0)
            SYNCLOG("Chemistry test: KFX_WORLD_CHEMTEST at turn %ld", base);
    }
    if (base < 0)
        return;
    const long t = (long)game.play_gameturn - base;
    const struct Thing *heart = get_player_soul_container(my_player_number);
    if (!thing_exists(heart) || (t < 0) || (t > 200))
        return;
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    if (t <= 1)
    {
        if (t == 0)
        {
            /* a strip of water 6 slabs south of the heart, 5 long; the Knights stand on it and beside it */
            wx = hx - 2;
            wy = hy + 6;
            for (int i = 0; i < 5; i++)
                place_slab_type_on_map(SlbT_WATER, slab_subtile_center(wx + i), slab_subtile_center(wy), game.neutral_player_num, 0);
            for (int i = 0; i < 3; i++)
                place_slab_type_on_map(SlbT_PATH, slab_subtile_center(wx + i), slab_subtile_center(wy + 2), game.neutral_player_num, 0);
            reveal_map_area(my_player_number, slab_subtile(wx - 3, 0), slab_subtile(wx + 9, 0), slab_subtile(hy - 4, 0), slab_subtile(wy + 4, 0));
            for (int i = 0; i < 4; i++)
                kn[i] = chem_test_knight(wx + i, wy);       /* in the water */
            for (int i = 0; i < 3; i++)
                kn[4 + i] = chem_test_knight(wx + i, wy + 2); /* out of it */
            for (int i = 0; i < 7; i++)
            {
                if (kn[i] == NULL)
                {
                    SYNCLOG("Chemistry test: no Knight");
                    base = -1;
                    return;
                }
                spot[i] = kn[i]->mappos;
            }
            /* the keeper's own Orc in the water (the reactions must spare it) */
            {
                struct Coord3d opos = kn[3]->mappos;
                opos.x.val += COORD_PER_STL;
                own = create_thing_at_position_then_move_to_valid_and_add_light(&opos, TCls_Creature, chem_id(creature_desc, "ORC"),
                    my_player_number);
                if (thing_is_invalid(own))
                    own = NULL;
                else
                {
                    move_thing_in_map(own, &opos);
                    own_spot = own->mappos;
                }
                SYNCLOG("Chemistry test: the keeper's Orc %s, in water %d", (own != NULL) ? "made" : "NOT made", (own != NULL) ? (int)chem_in_water(own) : 0);
            }
            /* gas and fire: a cloud by the dry Knights, a fireball through it */
            struct Coord3d gpos = kn[4]->mappos;
            gpos.z.val += COORD_PER_STL;
            struct Thing *gas = create_effect(&gpos, chem_id(effect_desc, "EFFECT_GAS_3"), PLAYER_GOOD);
            SYNCLOG("Chemistry test: Knights health %d; a gas cloud %s", (int)kn[4]->health, thing_is_invalid(gas) ? "NOT made" : "made");
            return;
        }
        if (kn[4] == NULL)
            return;
        struct Coord3d fpos = kn[4]->mappos;
        fpos.z.val += COORD_PER_STL;
        create_shot(&fpos, chem_id(shot_desc, "SHOT_FIREBALL"), my_player_number);
        return;
    }
    const SpellKind freeze = (SpellKind)get_id(spell_desc, "SPELL_FREEZE");
    if ((t == 12) || (t == 20) || (t == 40) || (t == 60) || (t == 80) || (t == 100) || (t == 120))
        for (int i = 0; i < 7; i++)
            if (thing_exists(kn[i]) && (kn[i]->class_id == TCls_Creature))
                move_thing_in_map(kn[i], &spot[i]);
    if (((t == 80) || (t == 100)) && (own != NULL) && thing_exists(own))
        move_thing_in_map(own, &own_spot);
    switch (t)
    {
    case 12:
        SYNCLOG("Chemistry test: after the blast: dry Knights health %d %d %d", (int)kn[4]->health, (int)kn[5]->health, (int)kn[6]->health);
        break;
    case 20:
        apply_spell_effect_to_thing(kn[5], freeze, 4, my_player_number);
        chem_test_hit("ice and boulders, not frozen", "SHOT_BOULDER", kn[6]);
        break;
    case 21:
        chem_test_hit("ice and boulders, frozen", "SHOT_BOULDER", kn[5]);
        break;
    case 40:
        apply_spell_effect_to_thing(kn[4], freeze, 4, my_player_number);
        chem_test_hit("fire and ice", "SHOT_FIREBALL", kn[4]);
        break;
    case 60:
        chem_test_hit("fire, out of the water", "SHOT_FIREBALL", kn[4]);
        chem_test_hit("fire and water", "SHOT_FIREBALL", kn[0]);
        break;
    case 80:
        SYNCLOG("Chemistry test: in the water: health %d %d %d; the keeper's Orc %d", (int)kn[1]->health, (int)kn[2]->health,
            (int)kn[3]->health, (own != NULL) ? (int)own->health : -1);
        chem_test_hit("lightning and water", "SHOT_LIGHTNING", kn[0]);
        SYNCLOG("Chemistry test: in the water after: health %d %d %d; out of it %d; the keeper's Orc in the water %d (in water %d)",
            (int)kn[1]->health, (int)kn[2]->health, (int)kn[3]->health, (int)kn[4]->health, (own != NULL) ? (int)own->health : -1,
            (own != NULL) ? (int)chem_in_water(own) : 0);
        break;
    case 120:
    {
        /* gas and lava: a slab of lava between two dry Knights, a gas cloud drifting over it */
        const MapSlabCoord lx = subtile_slab(kn[5]->mappos.x.stl.num), ly = subtile_slab(kn[5]->mappos.y.stl.num) + 1;
        place_slab_type_on_map(SlbT_LAVA, slab_subtile_center(lx), slab_subtile_center(ly), game.neutral_player_num, 0);
        struct Coord3d gpos;
        gpos.x.val = subtile_coord_center(slab_subtile_center(lx));
        gpos.y.val = subtile_coord_center(slab_subtile_center(ly));
        gpos.z.val = get_floor_height_at(&gpos) + COORD_PER_STL;
        struct Thing *gas = create_effect(&gpos, chem_id(effect_desc, "EFFECT_GAS_3"), PLAYER_GOOD);
        SYNCLOG("Chemistry test: a gas cloud %s over lava at (%d,%d); Knights by it: %d %d", thing_is_invalid(gas) ? "NOT made" : "made",
            (int)lx, (int)ly, (int)kn[4]->health, (int)kn[5]->health);
        break;
    }
    case 132:
        SYNCLOG("Chemistry test: after the lava: Knights by it: %d %d", (int)kn[4]->health, (int)kn[5]->health);
        break;
    case 100:
        chem_test_hit("ice and water", "SHOT_FREEZE", kn[1]);
        SYNCLOG("Chemistry test: frozen in the water: %d %d %d; out of it %d; the keeper's Orc %d", (int)chem_frozen(kn[0]),
            (int)chem_frozen(kn[2]), (int)chem_frozen(kn[3]), (int)chem_frozen(kn[4]),
            ((own != NULL) && thing_exists(own) && (own->class_id == TCls_Creature) && (own->health > 0)) ? (int)chem_frozen(own) : -1);
        break;
    }
}
