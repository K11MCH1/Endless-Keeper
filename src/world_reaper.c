/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_reaper.c
 *     Endless worlds: the Heart of the Pentagram's Reaper (see world_reaper.h).
 * @par Comment:
 *     Game state lives in the seat's struct WorldPentagram (reaper_day, reaper_until, reaper_thing, reaper_angry), so a
 *     thin client draws the HUD icon from what the server has. The summon is a packet; the simulating machine checks it.
 *     "Once a day": a world day, dawn to dawn (the HUD counts to the next dawn).
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_reaper.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "bflib_basics.h"
#include "bflib_math.h"
#include "bflib_mouse.h"
#include "bflib_datetm.h"
#include "kjm_input.h"
#include "game_legacy.h"
#include "game_merge.h"
#include "player_data.h"
#include "player_instances.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_creature.h"
#include "thing_effects.h"
#include "thing_stats.h"
#include "thing_navigate.h"
#include "thing_physics.h"
#include "creature_control.h"
#include "creature_states.h"
#include "config_creature.h"
#include "config.h"
#include "slab_data.h"
#include "map_data.h"
#include "packets.h"
#include "net_state.h"
#include "vidmode.h"
#include "local_camera.h"
#include "ui_paint.h"
#include "ui_molten.h"
#include "world_stream.h"
#include "world_daynight.h"
#include "world_boss.h"
#include "world_pentagram.h"
#include "config_magic.h"
#include "engine_render.h"
#include "custom_sprites.h"
#include "observer.h"
#include "gui_msgs.h"
#include "kfx/renderer/RendererBridge_UI.h"
#include "frontend.h"
#include "front_simple.h"
#include "keeperfx.hpp"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURNS_A_SECOND 20
#define WRP_RISE_TURNS (4 * TURNS_A_SECOND)    /* he comes up out of the floor, turning as the Temple's summon: 4 s (the user) */
#define WRP_LIFE_TURNS (20 * TURNS_A_SECOND)   /* his 20 seconds, once he is up */
#define WRP_SINK_TURNS (2 * TURNS_A_SECOND)    /* and he sinks back */
#define WRP_HUNT_SLABS 25
#define WRP_BLAST_SLABS 4
#define WRP_RAGE_ONE (TURNS_A_SECOND * 5 / 2)  /* in his rage: turns on one creature before he turns to another */

static ThingIndex wrp_rage_on[PLAYERS_MAX];          /* (the simulating machine: whom he fights now, since when) */
static GameTurn wrp_rage_since[PLAYERS_MAX];
#define SLAB_UNITS (COORD_PER_STL * STL_PER_SLB)

static ThingModel wrp_model(void)
{
    static ThingModel m = -1;
    if (m < 0)
        m = (ThingModel)get_id(creature_desc, "HORNY");
    return (m > 0) ? m : 0;
}

static struct Thing *wrp_thing(PlayerNumber p)
{
    struct WorldPentagram *s = world_pentagram_state(p);
    if ((s == NULL) || (s->reaper_thing == 0))
        return INVALID_THING;
    struct Thing *t = thing_get(s->reaper_thing);
    return (thing_exists(t) && thing_is_creature(t) && (t->owner == p) && (t->model == wrp_model())) ? t : INVALID_THING;
}

TbBool world_reaper_is(const struct Thing *thing)
{
    if (!world_mode_active() || !thing_exists(thing) || (thing->owner >= PLAYERS_MAX) || !thing_is_creature(thing))
        return false;
    const struct WorldPentagram *s = world_pentagram_state(thing->owner);
    return (s != NULL) && (s->reaper_thing == thing->index) && (s->reaper_until != 0);
}

TbBool world_reaper_ready(PlayerNumber p)
{
    struct WorldPentagram *s = world_pentagram_state(p);
    if (!world_mode_active() || (s == NULL) || (world_pentagram_points(p, WPG_HEART_NODE) == 0))
        return false;
    if (s->reaper_until != 0)
        return false; /* he is out */
    if (world_boss_player_away(p))
        return false; /* (not in a World Boss's arena) */
    return s->reaper_day != (uint32_t)world_day_number();
}

void world_reaper_summon_packet(PlayerNumber p, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!world_reaper_ready(p) || (wrp_model() <= 0))
    {
        SYNCLOG("World: player %d's Reaper can't be summoned now", (int)p);
        return;
    }
    const struct SlabMap *slb = get_slabmap_for_subtile(stl_x, stl_y);
    if (slabmap_block_invalid(slb) || (slabmap_owner(slb) != p) || slab_is_wall(subtile_slab(stl_x), subtile_slab(stl_y)) ||
        slab_kind_is_liquid(slb->kind))
    {
        SYNCLOG("World: player %d's Reaper: (%d,%d) is not the keeper's own floor", (int)p, (int)stl_x, (int)stl_y);
        return;
    }
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(stl_x);
    pos.y.val = subtile_coord_center(stl_y);
    pos.z.val = get_floor_height_at(&pos);
    struct Thing *t = create_creature(&pos, wrp_model(), p);
    if (!thing_exists(t))
        return;
    set_creature_level(t, (CrtrExpLevel)(CREATURE_MAX_LEVEL_LIMIT - 1)); /* level 100 */
    update_creature_health_to_max(t);
    /* he rises where he was summoned, turning, as a creature the Temple gives (its own state: he stays on the spot for
       its 48 turns, then sets about his work) */
    internal_set_thing_state(t, CrSt_CreatureBeingSummoned);
    t->movement_flags |= TMvF_BeingSacrificed;
    {
        struct CreatureControl *scc = creature_control_get_from_thing(t);
        if (!creature_control_invalid(scc))
        {
            /* (the state's first turn would set its stock 48 turns: it is begun past that, with its effect made here) */
            scc->sacrifice.animation_counter = 1;
            scc->sacrifice.animation_duration = WRP_RISE_TURNS;
            create_effect(&t->mappos, TngEff_Explosion4, p);
        }
    }
    struct WorldPentagram *s = world_pentagram_state(p);
    s->reaper_thing = t->index;
    s->reaper_day = (uint32_t)world_day_number();
    s->reaper_until = (uint32_t)game.play_gameturn + WRP_RISE_TURNS + WRP_LIFE_TURNS;
    s->reaper_angry = 0;
    create_effect(&pos, TngEff_ColouredRingOfFire, p);
    SYNCLOG("World: player %d summons the Reaper (thing %d) at (%d,%d); he goes at turn %lu", (int)p, (int)t->index,
        (int)stl_x, (int)stl_y, (unsigned long)s->reaper_until);
}

void world_reaper_slapped(struct Thing *thing)
{
    if (!world_reaper_is(thing))
        return;
    struct WorldPentagram *s = world_pentagram_state(thing->owner);
    if (!s->reaper_angry)
        SYNCLOG("World: player %d slapped the Reaper: he is angry", (int)thing->owner);
    s->reaper_angry = 1;
}

/** His effects (Word of Power, the lava's fireballs) are marked as his: a creature parent with no parent index and the
 *  Reaper's model, which the game itself never makes. They harm nothing and change no slab (thing_effects.c), even
 *  after he has gone. */
static struct Thing *wrp_effect(struct Thing *reaper, ThingModel model)
{
    struct Thing *eff = create_effect(&reaper->mappos, model, reaper->owner);
    if (!thing_is_invalid(eff))
    {
        eff->parent_idx = 0;
        eff->shot_effect.parent_class_id = TCls_Creature;
        eff->shot_effect.parent_model = reaper->model;
    }
    return eff;
}

TbBool world_reaper_effect(const struct Thing *efftng)
{
    return !thing_is_invalid(efftng) && (efftng->parent_idx == 0) && (efftng->shot_effect.parent_class_id == TCls_Creature) &&
        (wrp_model() > 0) && (efftng->shot_effect.parent_model == wrp_model());
}

/** Word of Power on his place: no harm, no slab touched; everything within WRP_BLAST_SLABS thrown back (him not). */
static void wrp_blast(struct Thing *reaper)
{
    wrp_effect(reaper, TngEff_WoPExplosion);
    const MapSubtlCoord cx = reaper->mappos.x.stl.num, cy = reaper->mappos.y.stl.num;
    const int r = WRP_BLAST_SLABS * STL_PER_SLB;
    for (MapSubtlCoord y = cy - r; y <= cy + r; y++)
        for (MapSubtlCoord x = cx - r; x <= cx + r; x++)
        {
            if ((x < 0) || (y < 0) || (x >= game.map_subtiles_x) || (y >= game.map_subtiles_y))
                continue;
            const struct Map *mapblk = get_map_block_at(x, y);
            long i = get_mapwho_thing_index(mapblk), guard = 0;
            while ((i != 0) && (guard++ < 200))
            {
                struct Thing *t = thing_get(i);
                if (thing_is_invalid(t))
                    break;
                i = t->next_on_mapblk;
                if ((t == reaper) || !thing_is_creature(t) || thing_is_picked_up(t))
                    continue;
                const long dx = t->mappos.x.val - reaper->mappos.x.val, dy = t->mappos.y.val - reaper->mappos.y.val;
                const long d = LbSqrL(dx * dx + dy * dy);
                if ((d <= 0) || (d > WRP_BLAST_SLABS * SLAB_UNITS))
                    continue;
                /* a push that carries it about 4 slabs (outward, a little up) */
                const long push = 96;
                t->veloc_push_add.x.val += (MapCoordDelta)(dx * push / d);
                t->veloc_push_add.y.val += (MapCoordDelta)(dy * push / d);
                t->veloc_push_add.z.val += 40;
                t->state_flags |= TF1_PushAdd;
            }
        }
}

/** The nearest enemy within reach (in his rage: the nearest creature at all, his keeper's too; not `skip`). */
static struct Thing *wrp_prey(struct Thing *reaper, TbBool angry, long slabs, const struct Thing *skip)
{
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    struct Thing *best = INVALID_THING;
    long best_d = slabs * SLAB_UNITS;
    int guard = 0;
    for (ThingIndex i = slist->index; (i > 0) && (guard < 20000); guard++)
    {
        struct Thing *c = thing_get(i);
        if (!thing_exists(c))
            break;
        i = c->next_of_class;
        if ((c == reaper) || (c == skip) || thing_is_picked_up(c) || creature_is_dying(c) || creature_is_being_unconscious(c))
            continue;
        if (!angry && ((c->owner == reaper->owner) || players_are_mutual_allies(reaper->owner, c->owner) ||
            (c->owner == game.neutral_player_num)))
            continue;
        const long d = get_2d_distance(&c->mappos, &reaper->mappos);
        if (d < best_d)
        {
            best = c;
            best_d = d;
        }
    }
    return best;
}

static void wrp_turn_for(PlayerNumber p)
{
    struct WorldPentagram *s = world_pentagram_state(p);
    if ((s == NULL) || (s->reaper_until == 0))
        return;
    struct Thing *t = wrp_thing(p);
    if (!thing_exists(t))
    {
        /* he fell (or was never made): today's summon is spent all the same */
        s->reaper_thing = 0;
        s->reaper_until = 0;
        s->reaper_angry = 0;
        return;
    }
    const GameTurn now = game.play_gameturn;
    const GameTurn up_at = s->reaper_until - WRP_LIFE_TURNS;
    if (now >= s->reaper_until + WRP_SINK_TURNS)
    {
        SYNCLOG("World: player %d's Reaper sinks back into the floor", (int)p);
        create_effect(&t->mappos, TngEff_ColouredRingOfFire, p);
        s->reaper_thing = 0;
        s->reaper_until = 0;
        s->reaper_angry = 0;
        kill_creature(t, INVALID_THING, -1, CrDed_NoEffects | CrDed_NotReallyDying);
        return;
    }
    struct CreatureControl *cc = creature_control_get_from_thing(t);
    if (now >= s->reaper_until)
    {
        /* sinking: he stands and turns (rising, the Temple's summon state turns him) */
        t->move_angle_xy = (t->move_angle_xy + 96) & ANGLE_MASK;
        if (!creature_control_invalid(cc))
            cc->move_speed = 0;
    }
    /* while he rises: Word of Power blasts at random short intervals and the lava's fireballs (no harm in either);
       they stop once he can move (the user, 2026-11-27) */
    if (now < up_at)
    {
        if ((int)GAME_RANDOM(100) < 7)
            wrp_blast(t);
        if ((int)GAME_RANDOM(100) < 10)
            wrp_effect(t, TngEff_Eruption);
    }
    if ((now < up_at) || (now >= s->reaper_until))
        return;
    if (creature_control_invalid(cc))
        return;
    if (s->reaper_angry)
    {
        /* his rage (the user, 2026-11-27): he fights as a creature gone mad (the game's own Mad Killing: he attacks
           anyone, his keeper's creatures too, with his own blows, and whoever is near attacks him), and he does not
           stay on one: after WRP_RAGE_ONE turns on the same creature he breaks off for the nearest other */
        if (!flag_is_set(cc->spell_flags, CSAfF_MadKilling))
        {
            set_flag(cc->spell_flags, CSAfF_MadKilling);
            external_set_thing_state(t, CrSt_MadKillingPsycho);
            wrp_rage_on[p] = 0;
        }
        const ThingIndex on = (cc->combat_flags != 0) ? cc->combat.battle_enemy_idx : 0;
        if (on != wrp_rage_on[p])
        {
            static int logs = 0;
            if ((on != 0) && (logs++ < 40))
            {
                const struct Thing *e = thing_get(on);
                SYNCLOG("World: player %d's angry Reaper fights the %s %d (player %d)", (int)p, thing_model_name(e), (int)on,
                    (int)e->owner);
            }
            wrp_rage_on[p] = on;
            wrp_rage_since[p] = now;
        }
        if ((on != 0) && ((GameTurnDelta)(now - wrp_rage_since[p]) >= WRP_RAGE_ONE))
        {
            const struct Thing *was = thing_get(on);
            struct Thing *next = wrp_prey(t, true, 8, was);
            if (thing_exists(next))
            {
                SYNCLOG("World: player %d's Reaper turns from %d to %d", (int)p, (int)on, (int)next->index);
                set_start_state(t);
                setup_person_move_to_coord(t, &next->mappos, NavRtF_Default);
                wrp_rage_on[p] = 0;
                wrp_rage_since[p] = now;
            }
        } else if ((on == 0) && ((now % 10) == (t->index % 10)))
        {
            /* no fight: to the nearest creature of any keeper */
            struct Thing *prey = wrp_prey(t, true, 12, NULL);
            if (thing_exists(prey))
                setup_person_move_to_coord(t, &prey->mappos, NavRtF_Default);
        }
        return;
    }
    /* hunting: the nearest enemy in reach */
    if ((now % 10) == (t->index % 10))
    {
        struct Thing *prey = wrp_prey(t, false, WRP_HUNT_SLABS, NULL);
        if (thing_exists(prey) && (cc->combat_flags == 0))
            setup_person_move_to_coord(t, &prey->mappos, NavRtF_Default);
    }
}

void world_reaper_turn(void)
{
    if (!world_mode_active())
        return;
    for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
        wrp_turn_for(p);
}

/******************************************************************************/
/* The HUD icon (this machine): top right, under the day dial and the World Boss strip */

static struct UmHost wrp_host;
static TbBool wrp_place_mode;
static int wrp_box[4]; /* x, y, w, h on the screen */
static unsigned char *wrp_pic;
static int wrp_pic_size;

static const unsigned char *wrp_picture(int size)
{
    if ((wrp_pic != NULL) && (wrp_pic_size == size))
        return wrp_pic;
    int w = 0, h = 0;
    const unsigned char *src = world_pentagram_icon(world_pentagram_heart_icon(), &w, &h);
    if ((src == NULL) || (size <= 0))
        return NULL;
    free(wrp_pic);
    wrp_pic = (unsigned char *)malloc((size_t)size * size * 4);
    if (wrp_pic == NULL)
        return NULL;
    wrp_pic_size = size;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
        {
            const float fx = ((float)x + 0.5f) * w / size - 0.5f, fy = ((float)y + 0.5f) * h / size - 0.5f;
            const int x0 = max(0, min(w - 1, (int)floorf(fx))), y0 = max(0, min(h - 1, (int)floorf(fy)));
            const int x1 = min(w - 1, x0 + 1), y1 = min(h - 1, y0 + 1);
            const float ax = fx - floorf(fx), ay = fy - floorf(fy);
            for (int ch = 0; ch < 4; ch++)
            {
                const float v = (1 - ay) * ((1 - ax) * src[(y0 * w + x0) * 4 + ch] + ax * src[(y0 * w + x1) * 4 + ch]) +
                    ay * ((1 - ax) * src[(y1 * w + x0) * 4 + ch] + ax * src[(y1 * w + x1) * 4 + ch]);
                wrp_pic[((size_t)y * size + x) * 4 + ch] = (unsigned char)max(0.0f, min(255.0f, v + 0.5f));
            }
        }
    return wrp_pic;
}

void world_reaper_hud_draw(void)
{
    const PlayerNumber p = my_player_number;
    wrp_box[2] = 0;
    /* only once the Heart of the Pentagram is unlocked (the user, 2026-11-27); a seat not yet taken, or a web reset,
       shows nothing and leaves no placing click behind */
    if (!world_mode_active() || netstate_dedicated() || (p < 0) || (p >= PLAYERS_MAX) ||
        (world_pentagram_points(p, WPG_HEART_NODE) == 0))
    {
        wrp_place_mode = false;
        return;
    }
    if (get_local_view_type() != PVT_DungeonTop)
        return;
    const struct WorldPentagram *s = world_pentagram_state(p);
    const float u = max(0.62f, min(1.6f, (float)MyScreenHeight / 1080.0f));
    const int size = (int)(76 * u), pad = (int)(14 * u), text_h = (int)(28 * u);
    int strip[4];
    world_boss_strip_rect(strip);
    const int x = (int)MyScreenWidth - pad - size;
    const int y = ((strip[2] > 0) ? (strip[1] + strip[3]) : (int)(170 * u)) + pad;
    wrp_box[0] = x; wrp_box[1] = y; wrp_box[2] = size; wrp_box[3] = size + text_h;
    const TbBool out = (s->reaper_until != 0), ready = world_reaper_ready(p);
    const int qx = GetMouseX(), qy = GetMouseY();
    const TbBool hover = (qx >= x) && (qy >= y) && (qx < x + size) && (qy < y + size + text_h);
    char timer[32];
    float frac = 0;
    if (out)
    {
        const long left = (long)s->reaper_until - (long)game.play_gameturn;
        const long secs = max(0L, min(20L, (left + TURNS_A_SECOND - 1) / TURNS_A_SECOND));
        snprintf(timer, sizeof(timer), "0:%02ld", secs);
        frac = (float)secs / 20.0f;
    } else if (ready)
        snprintf(timer, sizeof(timer), wrp_place_mode ? "Place him" : "Ready");
    else
    {
        /* to the next dawn */
        const long day = world_day_turns();
        const long into = (long)((game.play_gameturn - world_daylight_began()) % (GameTurn)max(day, 1L));
        const long secs = max(0L, (day - into) / TURNS_A_SECOND);
        snprintf(timer, sizeof(timer), "%ld:%02ld", secs / 60, secs % 60);
        frac = 1.0f - (float)into / (float)max(day, 1L);
    }
    char key[96];
    snprintf(key, sizeof(key), "%d %d %d %d %s %d %d %d", size, text_h, (int)out, (int)ready, timer, (int)(frac * 60), (int)hover,
        (int)wrp_place_mode);
    wrp_host.name = "reaper";
    wrp_host.slot = UmSl_Spare;
    if (um_host_begin(&wrp_host, size + 2 * pad, size + text_h + 2 * pad, key))
    {
        struct UiCanvas *c = &wrp_host.canvas;
        const TbBool flat = um_host_flat();
        const float cx = pad + size / 2.0f, cy = pad + size / 2.0f, r = size / 2.0f;
        /* the pointer over it (the user, 2026-11-27: "does not react with a mouse hover"): a brighter glow and rim */
        const TbBool glowing = hover || wrp_place_mode;
        if (!flat)
            ui_glow_rrect(c, cx - r, cy - r, cx + r, cy + r, r, (glowing ? 20 : 14) * u, out ? UIC(255, 40, 20, 140) :
                ready ? (glowing ? UIC(255, 170, 70, 190) : UIC(255, 110, 30, 110)) : (hover ? UIC(255, 214, 170, 50) : UIC(0, 0, 0, 0)));
        ui_fill_rrect(c, cx - r, cy - r, cx + r, cy + r, r, ready || out ? UIC(90, 20, 8, 255) : UIC(44, 38, 34, 255), UIC(14, 4, 3, 255), false);
        const int psize = (int)(size - 10 * u);
        const unsigned char *pic = wrp_picture(psize);
        if (pic != NULL)
        {
            if (ready || out)
                ui_image(c, (int)(cx - psize / 2.0f), (int)(cy - psize / 2.0f), psize, psize, pic, psize, psize, psize / 2.0f);
            else
                ui_image_gain(c, (int)(cx - psize / 2.0f), (int)(cy - psize / 2.0f), psize, psize, pic, psize, psize, 0.4f);
        }
        ui_stroke_rrect(c, cx - r, cy - r, cx + r, cy + r, r, max(1.5f, (glowing ? 4 : 3) * u),
            out ? UIC(255, 90, 42, 255) : ready ? (glowing ? UIC(255, 232, 170, 255) : UIC(255, 178, 74, 255)) :
            (hover ? UIC(255, 214, 170, 140) : UIC(255, 214, 170, 70)));
        /* the timer's ring: his 20 seconds, or the day to the next dawn */
        if ((out || !ready) && (frac > 0))
        {
            const int segs = 60, lit = (int)(frac * segs);
            for (int k = 0; k < lit; k++)
            {
                const float a = (float)(-M_PI / 2 + 2 * M_PI * k / segs);
                const float rx = cx + (r + 5 * u) * cosf(a), ry = cy + (r + 5 * u) * sinf(a);
                ui_fill_rrect(c, rx - 1.5f * u, ry - 1.5f * u, rx + 1.5f * u, ry + 1.5f * u, 1.5f * u,
                    out ? UIC(255, 90, 42, 255) : UIC(255, 214, 170, 110), out ? UIC(255, 90, 42, 255) : UIC(255, 214, 170, 110), false);
            }
        }
        ui_text_box(c, UiF_Num, max(12.0f, 20 * u), 0, (float)(pad + size + 4 * u), (float)(size + 2 * pad), (float)text_h, 1,
            out ? UIC(255, 138, 90, 255) : ready ? UIC(255, 208, 138, 255) : UIC(170, 156, 132, 255), 0, true, timer);
        um_host_painted(&wrp_host, LbTimerClock());
    }
    um_host_show(&wrp_host, x - pad, y - pad);
}

TbBool world_reaper_placing(void)
{
    return wrp_place_mode;
}

TbBool world_reaper_hud_click(int qx, int qy)
{
    if ((wrp_box[2] <= 0) || (qx < wrp_box[0]) || (qy < wrp_box[1]) || (qx >= wrp_box[0] + wrp_box[2]) || (qy >= wrp_box[1] + wrp_box[3]))
        return false;
    if (world_reaper_ready(my_player_number))
        wrp_place_mode = !wrp_place_mode;
    return true;
}

static TbBool wrp_in(const int *b, int qx, int qy)
{
    return (b[2] > 0) && (qx >= b[0]) && (qy >= b[1]) && (qx < b[0] + b[2]) && (qy < b[1] + b[3]);
}

TbBool world_reaper_hud_mouse(void)
{
    if (!world_mode_active() || (get_local_view_type() != PVT_DungeonTop))
        return false;
    const int qx = GetMouseX(), qy = GetMouseY();
    int strip[4];
    world_boss_strip_rect(strip);
    const TbBool icon = wrp_in(wrp_box, qx, qy), over_strip = wrp_in(strip, qx, qy);
    if (!icon && !over_strip)
        return false;
    if (icon)
    {
        /* the icon's own click: it arms the placing (or calls it off); nothing reaches the ground under it */
        if (left_button_released)
            world_reaper_hud_click(qx, qy);
        left_button_clicked = 0;
        left_button_released = 0;
        right_button_clicked = 0;
        right_button_released = 0;
    } else if (!world_boss_strip_clickable())
    {
        /* (a strip that can be clicked has its own button, which takes the click) */
        left_button_clicked = 0;
        left_button_released = 0;
    }
    return true;
}

/** The slab under the pointer, false off the map. */
static TbBool wrp_pointed(MapSlabCoord *x, MapSlabCoord *y)
{
    if ((block_pointed_at_x < 0) || (block_pointed_at_y < 0) || (block_pointed_at_x >= game.map_subtiles_x) ||
        (block_pointed_at_y >= game.map_subtiles_y))
        return false;
    *x = subtile_slab(block_pointed_at_x);
    *y = subtile_slab(block_pointed_at_y);
    return true;
}

/** Can he be summoned on this slab (as world_reaper_summon_packet checks it: the keeper's own floor)? */
static TbBool wrp_floor_ok(PlayerNumber p, MapSlabCoord x, MapSlabCoord y)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    return !slabmap_block_invalid(slb) && (slabmap_owner(slb) == p) && !slab_is_wall(x, y) && !slab_kind_is_liquid(slb->kind);
}

TbBool world_reaper_place_click(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!wrp_place_mode)
        return false;
    wrp_place_mode = false;
    struct PlayerInfo *player = get_my_player();
    if (player_invalid(player))
        return true;
    set_players_packet_action(player, PckA_WorldReaperSummon, (unsigned long)stl_x, (unsigned long)stl_y, 0, 0);
    return true;
}

/** front_input.c (the dungeon view): the placing, as Move The Heart's: the left click summons him where the box is
 *  green, the right click calls it off; the ground gets none of these clicks. True: taken. */
TbBool world_reaper_inputs(void)
{
    if (!world_mode_active() || !wrp_place_mode)
        return false;
    if ((get_local_view_type() != PVT_DungeonTop) || !world_reaper_ready(my_player_number))
    {
        wrp_place_mode = false;
        return false;
    }
    if (right_button_clicked || right_button_released || right_button_held)
    {
        if (right_button_released)
            wrp_place_mode = false;
        right_button_clicked = 0;
        right_button_released = 0;
        return true;
    }
    if (left_button_clicked || left_button_held || left_button_released)
    {
        if (left_button_released)
        {
            MapSlabCoord x, y;
            if (wrp_pointed(&x, &y))
            {
                if (wrp_floor_ok(my_player_number, x, y))
                    world_reaper_place_click(slab_subtile_center(x), slab_subtile_center(y));
                else
                    message_add(MsgType_Blank, 0, "Summon the Reaper on your own claimed floor.");
            }
        }
        left_button_clicked = 0;
        left_button_released = 0;
        return true;
    }
    return false;
}

void world_reaper_cursor_box(void)
{
    /* the game's cursor box (as Move The Heart's placing): cleared the moment the placing ends or the pointer leaves */
    static TbBool shown = false;
    MapSlabCoord x, y;
    if (!wrp_place_mode || gameplay_cursor_is_over_gui() || !wrp_pointed(&x, &y))
    {
        if (shown)
        {
            map_volume_box.visible = 0;
            shown = false;
        }
        return;
    }
    shown = true;
    map_volume_box.visible = 1;
    map_volume_box.color = wrp_floor_ok(my_player_number, x, y) ? SLC_GREEN : SLC_RED;
    map_volume_box.beg_x = subtile_coord(slab_subtile(x, 0), 0);
    map_volume_box.beg_y = subtile_coord(slab_subtile(y, 0), 0);
    map_volume_box.end_x = subtile_coord(slab_subtile(x + 1, 0), 0);
    map_volume_box.end_y = subtile_coord(slab_subtile(y + 1, 0), 0);
    map_volume_box.floor_height_z = 1;
}

static long wrp_cursor_icon = 0;

void world_reaper_register_icons(void)
{
    unsigned int len = 0;
    const unsigned char *png = world_pentagram_reaper_cursor(&len);
    const unsigned char *pngs[1] = {png};
    const unsigned int sizes[1] = {len};
    wrp_cursor_icon = custom_sprites_add_builtin_icon("ENDLESS_REAPER_CURSOR", pngs, sizes, 1);
}

TbBool world_reaper_draw_hand(long cursor_x, long cursor_y)
{
    if (!wrp_place_mode)
        return false;
    const struct TbSprite *spr = (wrp_cursor_icon > 0) ? get_new_icon_sprite((short)wrp_cursor_icon) : NULL;
    if ((spr == NULL) || (spr->SHeight <= 0))
        return false;
    /* the icon on the pointer, about the Hand's size */
    const int upp = max(8, (int)(16 * scale_ui_value(40) / spr->SHeight));
    UIRenderer_SubmitPanelSpriteRaw(cursor_x - spr->SWidth * upp / 16 / 2, cursor_y - spr->SHeight * upp / 16 / 2, upp, spr, 0);
    return true;
}

#ifdef __cplusplus
}
#endif
