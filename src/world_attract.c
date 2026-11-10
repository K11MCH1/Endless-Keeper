/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_attract.c
 *     Endless Keeper: the launcher's background (see world_attract.h).
 * @par Purpose:
 *     The director: beside the keeper's start dungeon it digs out a cave, and there it stages one battle after
 *     another - the keeper's creatures against a rival keeper's, heroes, both, two rivals, or the wild creatures of
 *     the night - with creatures of any level up to 100, dropped in as a keeper's hand drops them. The camera circles
 *     the fight slowly, moving in and out. Between battles the picture fades through black.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_attract.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "bflib_basics.h"
#include "globals.h"
#include "config_creature.h"
#include "config_rules.h"
#include "creature_control.h"
#include "creature_states_hero.h"
#include "dungeon_data.h"
#include "engine_camera.h"
#include "frontend.h"
#include "game_legacy.h"
#include "local_camera.h"
#include "map_blocks.h"
#include "map_data.h"
#include "net_game.h"
#include "player_data.h"
#include "player_computer.h"
#include "player_instances.h"
#include "player_utils.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_factory.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "post_inc.h"

static TbBool attract_on = false;
static struct AttractShared *shared = NULL;
static unsigned char *pictures = NULL;
#ifdef _WIN32
static HANDLE launcher_process = NULL;
#endif

int world_attract_active(void)
{
    return attract_on;
}

void world_attract_configure(const char *shared_name)
{
#ifdef _WIN32
    HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shared_name);
    if (map == NULL)
    {
        WARNLOG("Attract: no shared memory named %s", shared_name);
        return;
    }
    shared = (struct AttractShared *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if ((shared == NULL) || (shared->magic != ATTRACT_MAGIC) || (shared->capacity < 64 * 64 * 4))
    {
        WARNLOG("Attract: the shared memory %s isn't the launcher's", shared_name);
        shared = NULL;
        return;
    }
    pictures = (unsigned char *)(shared + 1);
    if (shared->launcher_pid != 0)
        launcher_process = OpenProcess(SYNCHRONIZE, FALSE, shared->launcher_pid);
    attract_on = true;
    SYNCLOG("Attract: this run is the launcher's background (%u bytes a picture)", (unsigned)shared->capacity);
#else
    (void)shared_name;
#endif
}

void world_attract_frame(const unsigned char *rgba, int width, int height, int bottom_up)
{
    if (!attract_on || (shared == NULL) || (rgba == NULL) || (width <= 0) || (height <= 0))
        return;
    const size_t row = (size_t)width * 4;
    if (row * (size_t)height > shared->capacity)
        return;
    const uint32_t slot = shared->frame & 1;
    unsigned char *dst = pictures + (size_t)slot * shared->capacity;
    if (bottom_up)
    {
        for (int y = 0; y < height; y++)
            memcpy(dst + (size_t)y * row, rgba + (size_t)(height - 1 - y) * row, row);
    } else
        memcpy(dst, rgba, row * (size_t)height);
    shared->width[slot] = (uint32_t)width;
    shared->height[slot] = (uint32_t)height;
    shared->frame++;
}

/******************************************************************************/
/* the director */

#define ARENA_WEST 5      /* slabs from the heart to the cave's west edge (the start dungeon is 9 x 9 with its walls) */
#define ARENA_W 22
#define ARENA_H 15
#define ATTRACT_MAX 64    /* creatures of a battle */
#define RIVAL_A 1
#define RIVAL_B 2

enum { SIDE_KEEPER, SIDE_RIVAL_A, SIDE_RIVAL_B, SIDE_HEROES, SIDE_WILD, SIDES };

struct AttractGroup {
    int side;
    PlayerNumber owner;
    int level, left;          /* its creatures' level (each a little off it), and how many are still to come */
    MapSlabCoord x, y;        /* where they are dropped */
};

static struct {
    TbBool ready;             /* the cave is dug, the rivals exist */
    MapSlabCoord hx, hy;      /* the heart */
    int scene;
    GameTurn scene_from, quiet_from, next_drop;
    int phase;                /* 0 fading in / playing, 1 fading out */
    struct AttractGroup groups[3];
    int group_count;
    ThingIndex things[ATTRACT_MAX];
    GameTurn born[ATTRACT_MAX];
    int sides[ATTRACT_MAX];
    int count;
    uint32_t rng;
    float cam_angle;
    int32_t zoom_base;
} A;

static uint32_t arand(uint32_t n)
{
    A.rng = A.rng * 1664525u + 1013904223u;
    return (n > 0) ? ((A.rng >> 8) % n) : 0;
}

static TbBool slab_ok(MapSlabCoord x, MapSlabCoord y)
{
    return (x > 1) && (y > 1) && (x < game.map_tiles_x - 2) && (y < game.map_tiles_y - 2);
}

static void put_slab(MapSlabCoord x, MapSlabCoord y, SlabKind kind, PlayerNumber owner)
{
    if (slab_ok(x, y))
        place_slab_type_on_map(kind, slab_subtile_center(x), slab_subtile_center(y), owner, 0);
}

/** The cave beside the dungeon: an oval of paths with a lake and a lava pool, the keeper's ground at its west end,
 *  reached from the dungeon by a gap in its east wall. */
static void dig_arena(void)
{
    const MapSlabCoord x0 = A.hx + ARENA_WEST, y0 = A.hy - ARENA_H / 2;
    for (int dy = 0; dy < ARENA_H; dy++)
    {
        for (int dx = 0; dx < ARENA_W; dx++)
        {
            const float nx = ((float)dx - (ARENA_W - 1) * 0.5f) / (ARENA_W * 0.5f), ny = ((float)dy - (ARENA_H - 1) * 0.5f) / (ARENA_H * 0.5f);
            if (nx * nx + ny * ny > 1.0f + ((float)arand(100) - 50.0f) / 400.0f)
                continue;
            put_slab(x0 + dx, y0 + dy, (dx < 5) ? SlbT_CLAIMED : SlbT_PATH, (dx < 5) ? PLAYER0 : game.neutral_player_num);
        }
    }
    for (int dy = -1; dy <= 1; dy++)
        put_slab(A.hx + 4, A.hy + dy, SlbT_CLAIMED, PLAYER0);
    /* (off the middle, where the fighting is) */
    for (int k = 0; k < 2; k++)
    {
        const MapSlabCoord px = x0 + 6 + (MapSlabCoord)arand(ARENA_W - 12), py = (k == 0) ? y0 + 2 : y0 + ARENA_H - 4;
        for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 3; dx++)
                put_slab(px + dx, py + dy, (k == 0) ? SlbT_WATER : SlbT_LAVA, game.neutral_player_num);
    }
}

/** A rival keeper for the battles: a computer player without a dungeon (its creatures are dropped in). */
static void make_rival(PlayerNumber p)
{
    struct PlayerInfo *player = get_player(p);
    if (player_invalid(player) || player_exists(player))
        return;
    player->id_number = p;
    player->allocflags |= PlaF_Allocated | PlaF_CompCtrl;
    game.conf.rules[p].gameplay.heart_required = 0;
    /* a computer that does nothing: no process, check or event (its creatures fight by themselves) */
    if (setup_a_computer_player(p, 0))
    {
        struct Computer2 *comp = get_computer_player(p);
        memset(&comp->processes[0], 0, sizeof(comp->processes[0]));
        comp->processes[0].flags = ComProc_ListEnd;
        memset(&comp->checks[0], 0, sizeof(comp->checks[0]));
        comp->checks[0].flags = ComChk_Unkn0002;
        memset(&comp->events[0], 0, sizeof(comp->events[0]));
    }
}

static TbBool model_fits(ThingModel m, int side)
{
    const struct CreatureModelConfig *conf = &game.conf.crtr_conf.model[m];
    const uint32_t f = conf->model_flags;
    if ((conf->name[0] == '\0') || ((f & (CMF_IsSpecDigger | CMF_IsSpectator)) != 0))
        return false;
    if (side == SIDE_WILD)
        return ((f & (CMF_IsArachnid | CMF_IsDiptera)) != 0) || (strcasecmp(conf->name, "SKELETON") == 0) || (strcasecmp(conf->name, "BUG") == 0);
    if (side == SIDE_HEROES)
        return (f & CMF_IsEvil) == 0;
    return (f & CMF_IsEvil) != 0;
}

static void drop_creature(struct AttractGroup *g)
{
    if ((A.count >= ATTRACT_MAX) || !creature_count_below_map_limit(0))
        return;
    ThingModel fits[64];
    int n = 0;
    for (ThingModel m = 1; (m < game.conf.crtr_conf.model_count) && (n < 64); m++)
        if (model_fits(m, g->side))
            fits[n++] = m;
    if (n == 0)
        return;
    struct Coord3d pos;
    pos.x.val = subtile_coord_center(slab_subtile_center(g->x + (MapSlabCoord)arand(3) - 1)) + (MapCoord)arand(257) - 128;
    pos.y.val = subtile_coord_center(slab_subtile_center(g->y + (MapSlabCoord)arand(5) - 2)) + (MapCoord)arand(257) - 128;
    pos.z.val = 0;
    struct Thing *thing = create_thing_at_position_then_move_to_valid_and_add_light(&pos, TCls_Creature, fits[arand((uint32_t)n)], g->owner);
    if (thing_is_invalid(thing))
        return;
    int level = g->level + (int)arand(21) - 10; /* (not inside max/min: they would roll it again) */
    level = max(1, min(100, level));
    init_creature_level(thing, (CrtrExpLevel)(level - 1));
    if (g->owner == PLAYER_GOOD)
    {
        struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
        cctrl->party.objective = CHeroTsk_AttackEnemies;
        cctrl->party.original_objective = CHeroTsk_AttackEnemies;
        cctrl->party.target_plyr_idx = -1;
    }
    A.things[A.count] = thing->index;
    A.born[A.count] = thing->creation_turn;
    A.sides[A.count] = g->side;
    A.count++;
    g->left--;
}

static struct Thing *attract_thing(int i)
{
    struct Thing *thing = thing_get(A.things[i]);
    if (!thing_exists(thing) || (thing->class_id != TCls_Creature) || (thing->creation_turn != A.born[i]))
        return INVALID_THING;
    return thing;
}

static void clear_battle(void)
{
    for (int i = 0; i < A.count; i++)
    {
        struct Thing *thing = attract_thing(i);
        if (!thing_is_invalid(thing))
            kill_creature(thing, INVALID_THING, -1, CrDed_NoEffects | CrDed_NotReallyDying | CrDed_NoRebirth);
    }
    A.count = 0;
    set_ally_with_player(PLAYER0, RIVAL_A, false);
    set_ally_with_player(RIVAL_A, PLAYER0, false);
}

static void add_group(int side, MapSlabCoord x, MapSlabCoord y, int level, int count)
{
    struct AttractGroup *g = &A.groups[A.group_count++];
    g->side = side;
    g->owner = (side == SIDE_KEEPER) ? PLAYER0 : (side == SIDE_RIVAL_A) ? RIVAL_A : (side == SIDE_RIVAL_B) ? RIVAL_B : PLAYER_GOOD;
    g->x = x;
    g->y = y;
    g->level = level;
    g->left = count;
}

static int near_level(int base)
{
    const int level = base + (int)arand(31) - 15;
    return max(1, min(100, level));
}

static void start_scene(void)
{
    /* the fight is in the cave's middle: the sides are dropped a few slabs apart, in sight of each other */
    const MapSlabCoord cx = A.hx + ARENA_WEST + ARENA_W / 2, cy = A.hy;
    const int base = 1 + (int)arand(100);
    #define NEAR_LEVEL near_level(base)
    A.group_count = 0;
    A.scene = (int)arand(6);
    const char *caption = "";
    switch (A.scene)
    {
    case 0:
        add_group(SIDE_KEEPER, cx - 3, cy, NEAR_LEVEL, 12);
        add_group(SIDE_RIVAL_A, cx + 3, cy, NEAR_LEVEL, 12);
        caption = "Two keepers at war";
        break;
    case 1:
        add_group(SIDE_KEEPER, cx - 3, cy, NEAR_LEVEL, 12);
        add_group(SIDE_HEROES, cx + 3, cy, NEAR_LEVEL, 12);
        caption = "Heroes raid the keeper's dungeon";
        break;
    case 2:
        add_group(SIDE_KEEPER, cx - 4, cy + 1, NEAR_LEVEL, 10);
        add_group(SIDE_RIVAL_A, cx + 4, cy + 1, NEAR_LEVEL, 10);
        add_group(SIDE_RIVAL_B, cx, cy - 3, NEAR_LEVEL, 10);
        caption = "Three keepers, one cave";
        break;
    case 3:
        add_group(SIDE_KEEPER, cx - 3, cy - 2, NEAR_LEVEL, 9);
        add_group(SIDE_RIVAL_A, cx - 3, cy + 2, NEAR_LEVEL, 9);
        add_group(SIDE_HEROES, cx + 3, cy, NEAR_LEVEL, 16);
        set_ally_with_player(PLAYER0, RIVAL_A, true);
        set_ally_with_player(RIVAL_A, PLAYER0, true);
        caption = "Allied keepers hold off the heroes";
        break;
    case 4:
        add_group(SIDE_KEEPER, cx - 4, cy + 1, NEAR_LEVEL, 10);
        add_group(SIDE_RIVAL_A, cx + 4, cy + 1, NEAR_LEVEL, 10);
        add_group(SIDE_HEROES, cx, cy - 3, NEAR_LEVEL, 10);
        caption = "Heroes storm two rival keepers";
        break;
    default:
        add_group(SIDE_KEEPER, cx - 3, cy, NEAR_LEVEL, 10);
        add_group(SIDE_WILD, cx + 3, cy, NEAR_LEVEL, 14);
        caption = "Wild creatures come for the keeper";
        break;
    }
    #undef NEAR_LEVEL
    A.scene_from = game.play_gameturn;
    A.quiet_from = 0;
    A.next_drop = game.play_gameturn;
    A.phase = 0;
    if (shared != NULL)
        snprintf(shared->caption, sizeof(shared->caption), "%s", caption);
    SYNCLOG("Attract: scene %d (%s), levels about %d", A.scene, caption, base);
}

static void move_camera(void)
{
    struct UserState *ustate = get_user_state(get_my_user());
    if (ustate == NULL)
        return;
    /* round the cave's middle, slowly; in and out */
    const float t = (float)game.play_gameturn;
    const float cx = (float)(A.hx + ARENA_WEST + ARENA_W / 2) + sinf(t * 0.0021f) * 4.0f, cy = (float)A.hy + cosf(t * 0.0033f) * 2.0f;
    struct DungeonCamera pose = ustate->dungeon_camera;
    pose.x = (MapCoord)((cx * STL_PER_SLB + 1.5f) * COORD_PER_STL);
    pose.y = (MapCoord)((cy * STL_PER_SLB + 1.5f) * COORD_PER_STL);
    A.zoom_base = 2300; /* a good part of the cave in view (the game's range is 520, far, to 12000) */
    pose.zoom[0] = (int32_t)((float)A.zoom_base * (1.0f + 0.22f * sinf(t * 0.0045f)));
    A.cam_angle += 0.35f;
    pose.yaw[0] = ((int32_t)A.cam_angle) & ANGLE_MASK;
    ustate->dungeon_camera = pose;
    sync_local_camera_pose(get_my_user(), &pose);
}

void world_attract_turn(void)
{
    if (!attract_on)
        return;
#ifdef _WIN32
    /* the launcher has gone (or started the real game and closed this one's memory): so does this */
    if ((launcher_process != NULL) && ((game.play_gameturn % 20) == 0) && (WaitForSingleObject(launcher_process, 0) == WAIT_OBJECT_0))
        TerminateProcess(GetCurrentProcess(), 0);
#endif
    if ((game.play_gameturn % 50) == 5)
        set_gui_visible(false);
    const struct Thing *heartng = find_players_dungeon_heart(PLAYER0);
    if (!thing_exists(heartng))
    {
#ifdef _WIN32
        /* the keeper fell (or its world never came): the launcher starts another */
        if (game.play_gameturn > 200)
            TerminateProcess(GetCurrentProcess(), 0);
#endif
        return;
    }
    if (!A.ready)
    {
        if (game.play_gameturn < 6)
        {
            if (shared != NULL)
                shared->fade = 255;
            return;
        }
        A.rng = (uint32_t)time(NULL) ^ (uint32_t)(heartng->mappos.x.val * 2654435761u);
        A.hx = subtile_slab(heartng->mappos.x.stl.num);
        A.hy = subtile_slab(heartng->mappos.y.stl.num);
        dig_arena();
        /* a cave has no torches: the whole scene a little lighter, so the fight can be seen */
        for (PlayerNumber p = 0; p < PLAYERS_COUNT; p++)
            game.conf.rules[p].gameplay.global_ambient_light = 38;
        /* and the ground round the cave and the dungeon is known: earth and rock, not the black of the unexplored */
        {
            const MapSlabCoord rx0 = max(2, A.hx - 16), ry0 = max(2, A.hy - 18);
            const MapSlabCoord rx1 = min(game.map_tiles_x - 3, A.hx + ARENA_WEST + ARENA_W + 14), ry1 = min(game.map_tiles_y - 3, A.hy + 18);
            reveal_map_rect(PLAYER0, slab_subtile(rx0, 0), slab_subtile(rx1, 2), slab_subtile(ry0, 0), slab_subtile(ry1, 2));
        }
        make_rival(RIVAL_A);
        make_rival(RIVAL_B);
        A.ready = true;
        start_scene();
    }
    move_camera();
    const GameTurn now = game.play_gameturn, age = now - A.scene_from;
    if (A.phase == 1)
    {
        /* fading out: 24 turns to black, then the next battle */
        const GameTurn out = now - A.quiet_from;
        if (shared != NULL)
            shared->fade = (uint32_t)min(255, (int)out * 11);
        if (out >= 28)
        {
            clear_battle();
            start_scene();
        }
        return;
    }
    if (shared != NULL)
        shared->fade = (uint32_t)max(0, 255 - (int)age * 20);
    /* the hands drop their creatures in, a few at first and then one by one */
    if (now >= A.next_drop)
    {
        for (int g = 0; g < A.group_count; g++)
        {
            const int burst = (age < 5) ? 5 : 1;
            for (int k = 0; (k < burst) && (A.groups[g].left > 0); k++)
                drop_creature(&A.groups[g]);
        }
        A.next_drop = now + 70 + (GameTurn)arand(60);
    }
    if ((now % 20) != 0)
        return;
    /* over when one side (allies as one) is left with nobody still to come, or after a good while */
    int alive[SIDES] = {0};
    for (int i = 0; i < A.count; i++)
        if (!thing_is_invalid(attract_thing(i)))
            alive[A.sides[i]]++;
    for (int g = 0; g < A.group_count; g++)
        alive[A.groups[g].side] += A.groups[g].left;
    if (A.scene == 3)
    {
        alive[SIDE_KEEPER] += alive[SIDE_RIVAL_A];
        alive[SIDE_RIVAL_A] = 0;
    }
    int standing = 0;
    for (int s = 0; s < SIDES; s++)
        standing += (alive[s] > 0) ? 1 : 0;
    if ((standing <= 1) && (A.quiet_from == 0))
        A.quiet_from = now;
    if (((A.quiet_from != 0) && (now - A.quiet_from >= 100)) || (age > 1600))
    {
        A.phase = 1;
        A.quiet_from = now;
    }
}
