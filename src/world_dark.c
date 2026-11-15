/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_dark.c
 *     Endless world: the dark takes it back.
 * @par Purpose:
 *     See world_dark.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_dark.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "world_stream.h"
#include "world_pages.h"
#include "world_daynight.h"
#include "world_watchfire.h"
#include "world_caretaker.h"
#include "game_legacy.h"
#include "player_data.h"
#include "player_instances.h"
#include "dungeon_data.h"
#include "room_data.h"
#include "slab_data.h"
#include "map_data.h"
#include "map_blocks.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_creature.h"
#include "gui_msgs.h"
#include "net_main.h"
#include "net_state.h"
#include "keeperfx.hpp"
#include "server_admin.h"
#include "chat_box.h"
#include "thing_effects.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
struct DarkSlot {
    int64_t wx, wy;     /* world slab */
    uint32_t neglect;   /* turns without an anchor or an imp */
    uint8_t used;
    uint8_t owner;
    uint8_t warned;
    uint8_t pad;
};
static struct DarkSlot dk[DARK_SLOTS];
static uint32_t dk_next;           /* the next chunk of the pass */
static uint32_t dk_warned_day[PLAYERS_MAX];
static uint16_t dk_taken[PLAYERS_MAX]; /* slabs taken back in this chunk's pass, by keeper */

/** A word to a keeper wherever it plays: on this machine (the chat box, or the messages), or as the server's notice
 *  to its joined client (which shows it in its chat box or messages). */
static void tell_keeper(PlayerNumber p, const char *text)
{
    SYNCLOG("World: the dark, to player %d: %s", (int)p, text);
    if (is_my_player_number(p) && !netstate_dedicated())
    {
        if (chat_box_active())
            chat_box_add(-1, text);
        else
            message_add(MsgType_Blank, 0, text);
        return;
    }
    if (!network_is_active() || !netstate_is_server())
        return;
    const NetUserId u = get_player_primary_user(get_player(p));
    if ((u > SERVER_ID) && (u < MAX_NET_USERS))
        server_admin_notice(u, text);
}

static uint32_t dk_hash(int64_t wx, int64_t wy)
{
    uint64_t h = (uint64_t)wx * 0x9E3779B97F4A7C15ULL ^ (uint64_t)wy * 0xC2B2AE3D27D4EB4FULL;
    h ^= h >> 29;
    return (uint32_t)h & (DARK_SLOTS - 1);
}

static struct DarkSlot *dk_find(int64_t wx, int64_t wy, TbBool create)
{
    uint32_t i = dk_hash(wx, wy);
    struct DarkSlot *free_slot = NULL;
    for (int n = 0; n < 64; n++, i = (i + 1) & (DARK_SLOTS - 1))
    {
        struct DarkSlot *s = &dk[i];
        if (!s->used)
        {
            if (free_slot == NULL)
                free_slot = s;
            continue;
        }
        if ((s->wx == wx) && (s->wy == wy))
            return s;
    }
    if (!create || (free_slot == NULL))
        return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = 1;
    free_slot->wx = wx;
    free_slot->wy = wy;
    return free_slot;
}

/* a keeper's anchors, worked out once a turn for the keepers met in the turn's chunk */
#define DARK_ANCHORS_MAX 64
#define DARK_MARKS_PER_PASS 16 /* slabs about to go that show it, in one chunk's pass */
struct DarkAnchors {
    uint32_t turn;      /* 1 + the turn */
    int n;
    MapSlabCoord x[DARK_ANCHORS_MAX], y[DARK_ANCHORS_MAX];
    int r[DARK_ANCHORS_MAX];
};
static struct DarkAnchors dk_anchor[PLAYERS_MAX];

static void add_room_anchors(struct DarkAnchors *a, const struct Dungeon *dungeon, RoomKind kind, int reach)
{
    long guard = 0;
    for (RoomIndex i = dungeon->room_list_start[kind]; (i != 0) && (guard++ < ROOMS_COUNT) && (a->n < DARK_ANCHORS_MAX);)
    {
        const struct Room *room = room_get(i);
        if (room_is_invalid(room))
            break;
        i = room->next_of_owner;
        a->x[a->n] = subtile_slab(room->central_stl_x);
        a->y[a->n] = subtile_slab(room->central_stl_y);
        a->r[a->n++] = reach;
    }
}

static const struct DarkAnchors *anchors_of(PlayerNumber p)
{
    struct DarkAnchors *a = &dk_anchor[p];
    if (a->turn == (uint32_t)game.play_gameturn + 1)
        return a;
    a->turn = (uint32_t)game.play_gameturn + 1;
    a->n = 0;
    const struct Thing *heart = get_player_soul_container(p);
    if (thing_exists(heart))
    {
        a->x[a->n] = subtile_slab(heart->mappos.x.stl.num);
        a->y[a->n] = subtile_slab(heart->mappos.y.stl.num);
        a->r[a->n++] = DARK_REACH_HEART;
    }
    const struct Dungeon *dungeon = get_players_num_dungeon(p);
    if (!dungeon_invalid(dungeon))
    {
        add_room_anchors(a, dungeon, RoK_ENTRANCE, DARK_REACH_PORTAL);
        add_room_anchors(a, dungeon, RoK_TREASURE, DARK_REACH_ROOM);
        add_room_anchors(a, dungeon, RoK_GUARDPOST, DARK_REACH_ROOM);
    }
    return a;
}

int world_dark_anchor_list(PlayerNumber p, MapSlabCoord *x, MapSlabCoord *y, int *r, int max)
{
    if (!world_mode_active() || (p < 0) || (p >= PLAYERS_MAX))
        return 0;
    const struct DarkAnchors *a = anchors_of(p);
    int n = 0;
    for (int i = 0; (i < a->n) && (n < max); i++, n++)
    {
        x[n] = a->x[i];
        y[n] = a->y[i];
        r[n] = a->r[i];
    }
    return n;
}

TbBool world_dark_anchored(PlayerNumber p, MapSlabCoord x, MapSlabCoord y)
{
    if ((p < 0) || (p >= PLAYERS_MAX))
        return true;
    const struct DarkAnchors *a = anchors_of(p);
    for (int i = 0; i < a->n; i++)
    {
        const long dx = x - a->x[i], dy = y - a->y[i];
        if (dx * dx + dy * dy <= (long)a->r[i] * a->r[i])
            return true;
    }
    return world_watchfire_covers_for(p, x, y);
}

static TbBool imp_near(PlayerNumber p, MapSlabCoord x, MapSlabCoord y)
{
    const MapSubtlCoord x1 = slab_subtile(x - DARK_IMP_SLABS, 0), x2 = slab_subtile(x + DARK_IMP_SLABS, 2);
    const MapSubtlCoord y1 = slab_subtile(y - DARK_IMP_SLABS, 0), y2 = slab_subtile(y + DARK_IMP_SLABS, 2);
    for (MapSubtlCoord sy = y1; sy <= y2; sy++)
    {
        for (MapSubtlCoord sx = x1; sx <= x2; sx++)
        {
            if ((sx < 0) || (sy < 0) || (sx >= game.map_subtiles_x) || (sy >= game.map_subtiles_y))
                continue;
            long i = get_mapwho_thing_index(get_map_block_at(sx, sy));
            unsigned long guard = 0;
            while ((i != 0) && (guard++ < THINGS_COUNT))
            {
                const struct Thing *t = thing_get(i);
                if (thing_is_invalid(t))
                    break;
                i = t->next_on_mapblk;
                if (thing_is_creature(t) && (t->owner == p) && thing_is_creature_digger(t))
                    return true;
            }
        }
    }
    return false;
}

static TbBool is_reinforced_wall(SlabKind kind)
{
    return (kind >= SlbT_WALLDRAPE) && (kind <= SlbT_WALLPAIRSHR);
}

static TbBool keepers_floor(PlayerNumber p, MapSlabCoord x, MapSlabCoord y)
{
    const struct SlabMap *slb = get_slabmap_block(x, y);
    return !slabmap_block_invalid(slb) && (slabmap_owner(slb) == p) && (slb->kind >= SlbT_PATH) && !slab_kind_is_liquid(slb->kind)
        && !is_reinforced_wall(slb->kind) && !slab_is_wall(x, y);
}

/** The dark takes a slab back: bare path, and the keeper's walls beside it that touch no more of its floor. */
static void take_back(PlayerNumber p, MapSlabCoord x, MapSlabCoord y)
{
    place_slab_type_on_map(SlbT_PATH, slab_subtile_center(x), slab_subtile_center(y), game.neutral_player_num, 0);
    static const signed char d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    int walls = 0;
    for (int d = 0; d < 4; d++)
    {
        const MapSlabCoord wx = x + d4[d][0], wy = y + d4[d][1];
        const struct SlabMap *slb = get_slabmap_block(wx, wy);
        if (slabmap_block_invalid(slb) || (slabmap_owner(slb) != p) || !is_reinforced_wall(slb->kind))
            continue;
        TbBool touches = false;
        for (int e = 0; (e < 4) && !touches; e++)
            touches = keepers_floor(p, wx + d4[e][0], wy + d4[e][1]);
        if (touches)
            continue;
        place_slab_type_on_map(SlbT_EARTH, slab_subtile_center(wx), slab_subtile_center(wy), game.neutral_player_num, 0);
        walls++;
    }
    SYNCLOG("World: the dark takes back player %d's floor at engine slab (%d,%d) (%d wall%s with it)", (int)p, (int)x, (int)y,
        walls, (walls == 1) ? "" : "s");
}

static void scan_chunk(MapSlabCoord x0, MapSlabCoord y0, uint32_t elapsed)
{
    memset(dk_taken, 0, sizeof(dk_taken));
    const uint32_t limit = (uint32_t)(DARK_DAYS * world_day_turns());
    const uint32_t warn_at = (limit > (uint32_t)world_day_turns()) ? limit - (uint32_t)world_day_turns() : 0;
    int marks = 0;
    for (MapSlabCoord y = y0; y < y0 + WORLD_CHUNK_SLABS; y++)
    {
        for (MapSlabCoord x = x0; x < x0 + WORLD_CHUNK_SLABS; x++)
        {
            const struct SlabMap *slb = get_slabmap_block(x, y);
            if (slabmap_block_invalid(slb) || (slb->kind != SlbT_CLAIMED))
                continue;
            const PlayerNumber p = slabmap_owner(slb);
            if ((p < 0) || (p >= PLAYERS_COUNT) || (p >= PLAYERS_MAX) || !world_player_is_human(p))
                continue;
            int64_t wx, wy;
            world_slab_to_world(x, y, &wx, &wy);
            if (world_caretaker_active(p) || world_dark_anchored(p, x, y) || imp_near(p, x, y))
            {
                struct DarkSlot *s = dk_find(wx, wy, false);
                if (s != NULL)
                    s->used = 0; /* looked after */
                continue;
            }
            struct DarkSlot *s = dk_find(wx, wy, true);
            if (s == NULL)
                continue;
            if (s->owner != (uint8_t)p)
            {
                s->owner = (uint8_t)p;
                s->neglect = 0;
                s->warned = 0;
            }
            s->neglect += elapsed;
            if ((s->neglect >= warn_at) && !s->warned)
            {
                s->warned = 1;
                const uint32_t day = (uint32_t)world_day_number();
                if (dk_warned_day[p] != day)
                {
                    dk_warned_day[p] = day;
                    tell_keeper(p, "The dark will take back your untended floor within a day.");
                }
            }
            if (s->warned && (s->neglect < limit) && (marks < DARK_MARKS_PER_PASS))
            {
                /* the slab shows it is about to go: a wisp of harmless gas rises from it each pass (a thing, so a joined
                   player sees it too) */
                struct Coord3d pos;
                pos.x.val = subtile_coord_center(slab_subtile_center(x));
                pos.y.val = subtile_coord_center(slab_subtile_center(y));
                pos.z.val = get_floor_height_at(&pos);
                create_effect(&pos, TngEff_HarmlessGas1, p);
                marks++;
            }
            if (s->neglect >= limit)
            {
                s->used = 0;
                take_back(p, x, y);
                if (dk_taken[p] < 65535)
                    dk_taken[p]++;
            }
        }
    }
    {
        static TbBool marks_logged = false;
        if ((marks > 0) && !marks_logged)
        {
            marks_logged = true;
            SYNCLOG("World: the dark marks %d slab(s) about to go (first time)", marks);
        }
    }
    for (PlayerNumber p = 0; p < PLAYERS_MAX; p++)
    {
        if (dk_taken[p] == 0)
            continue;
        char text[96];
        snprintf(text, sizeof(text), "The dark took back %d slab%s of your untended floor.", (int)dk_taken[p],
            (dk_taken[p] == 1) ? "" : "s");
        tell_keeper(p, text);
    }
}

/** KFX_WORLD_DARKTEST=turn (test switch): a corridor of this machine's keeper's claimed floor is laid from 22 to 40
 *  slabs east of its Heart (with a Workshop below the Heart and a watchfire at 28 when the fire rules allow), and how
 *  much of it is left is logged every 200 turns. */
static void dark_test(void)
{
    static long at = -2;
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_DARKTEST");
        at = (v != NULL) ? atol(v) : -1;
        if (at >= 0)
            SYNCLOG("World: test switch KFX_WORLD_DARKTEST=%ld", at);
    }
    if ((at < 0) || ((long)game.play_gameturn < at))
        return;
    /* this machine's keeper, or on a dedicated server the first human keeper with a Heart (a joined player) */
    static PlayerNumber p = -1;
    if (p < 0)
    {
        p = my_player_number;
        for (PlayerNumber q = 0; netstate_dedicated() && (q < PLAYERS_COUNT) && (q < PLAYERS_MAX); q++)
            if (world_player_is_human(q) && thing_exists(get_player_soul_container(q)))
            {
                p = q;
                break;
            }
    }
    const struct Thing *heart = get_player_soul_container(p);
    if (!thing_exists(heart))
    {
        p = -1;
        return;
    }
    const MapSlabCoord hx = subtile_slab(heart->mappos.x.stl.num), hy = subtile_slab(heart->mappos.y.stl.num);
    if ((long)game.play_gameturn == at)
    {
        for (MapSlabCoord x = hx + 22; x <= hx + 40; x++)
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(x), slab_subtile_center(hy), p, 0);
        reveal_map_area(p, slab_subtile(hx + 22, 0), slab_subtile(hx + 40, 2), slab_subtile(hy, 0), slab_subtile(hy, 2));
        struct Dungeon *dungeon = get_players_num_dungeon(p);
        dungeon->total_money_owned += 1000;
        world_watchfire_place(p, slab_subtile_center(hx + 28), slab_subtile_center(hy));
        /* and the keeper's reinforced walls along the far end, above it (37, 38): they go back to earth with that floor */
        for (MapSlabCoord x = hx + 37; x <= hx + 38; x++)
            place_slab_type_on_map(SlbT_WALLDRAPE, slab_subtile_center(x), slab_subtile_center(hy - 1), p, 0);
        SYNCLOG("World: dark test: a corridor laid 22-40 slabs east of player %d's Heart, walls above 37-38", (int)p);
        return;
    }
    if (((long)game.play_gameturn - at) % 200 != 0)
        return;
    int left = 0, anchored = 0;
    for (MapSlabCoord x = hx + 22; x <= hx + 40; x++)
    {
        const struct SlabMap *slb = get_slabmap_block(x, hy);
        if (!slabmap_block_invalid(slb) && (slb->kind == SlbT_CLAIMED) && (slabmap_owner(slb) == p))
        {
            left++;
            anchored += world_dark_anchored(p, x, hy) ? 1 : 0;
        }
    }
    const struct SlabMap *w1 = get_slabmap_block(hx + 37, hy - 1), *w2 = get_slabmap_block(hx + 38, hy - 1);
    {
        /* how long the far end of the corridor has gone untended, against the limit */
        int64_t fwx, fwy;
        world_slab_to_world(hx + 37, hy, &fwx, &fwy);
        const struct DarkSlot *fs = dk_find(fwx, fwy, false);
        SYNCLOG("World: dark test: the corridor's far end untended %lu turns (warned %d), the limit %lu; an imp of the keeper within 3 slabs now %d",
            (fs != NULL) ? (unsigned long)fs->neglect : 0UL, (fs != NULL) ? (int)fs->warned : -1,
            (unsigned long)(DARK_DAYS * world_day_turns()), (int)imp_near(p, hx + 37, hy));
    }
    SYNCLOG("World: dark test: %d of 19 corridor slabs still claimed (%d anchored), day %ld; walls above 37-38: kinds %d,%d "
        "(earth %d)", left, anchored, world_day_number(), slabmap_block_invalid(w1) ? -1 : (int)w1->kind,
        slabmap_block_invalid(w2) ? -1 : (int)w2->kind, (int)SlbT_EARTH);
}

void world_dark_turn(void)
{
    if (!world_mode_active() || (network_is_active() && !netstate_is_server()))
        return;
    dark_test();
    /* one loaded chunk a turn: of the chunked world's pages, or of the window */
    if (world_is_chunked())
    {
        const int n = wp_count();
        if (n <= 0)
            return;
        int k = (int)(dk_next++ % (uint32_t)n);
        struct WorldPage *pg = wp_first();
        while ((k-- > 0) && (pg != NULL))
            pg = pg->next;
        if (pg != NULL)
            scan_chunk((MapSlabCoord)pg->cx * WORLD_CHUNK_SLABS, (MapSlabCoord)pg->cy * WORLD_CHUNK_SLABS, (uint32_t)n);
    } else
    {
        const uint32_t n = WORLD_WINDOW_CHUNKS * WORLD_WINDOW_CHUNKS;
        const uint32_t k = dk_next++ % n;
        scan_chunk(1 + (MapSlabCoord)(k % WORLD_WINDOW_CHUNKS) * WORLD_CHUNK_SLABS,
            1 + (MapSlabCoord)(k / WORLD_WINDOW_CHUNKS) * WORLD_CHUNK_SLABS, n);
    }
}

/******************************************************************************/
void world_dark_reset(void)
{
    memset(dk, 0, sizeof(dk));
    memset(dk_anchor, 0, sizeof(dk_anchor));
    memset(dk_warned_day, 0, sizeof(dk_warned_day));
    dk_next = 0;
}

size_t world_dark_save_size(void)
{
    int n = 0;
    for (int i = 0; i < DARK_SLOTS; i++)
        n += dk[i].used ? 1 : 0;
    return (n > 0) ? 4 + (size_t)n * sizeof(struct DarkSlot) : 0;
}

void world_dark_save(uint8_t *buf)
{
    uint32_t n = 0;
    size_t at = 4;
    for (int i = 0; i < DARK_SLOTS; i++)
    {
        if (!dk[i].used)
            continue;
        memcpy(buf + at, &dk[i], sizeof(struct DarkSlot));
        at += sizeof(struct DarkSlot);
        n++;
    }
    memcpy(buf, &n, 4);
}

TbBool world_dark_load(const uint8_t *buf, size_t len)
{
    world_dark_reset();
    uint32_t n = 0;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if ((size_t)n * sizeof(struct DarkSlot) + 4 > len)
        return false;
    for (uint32_t i = 0; i < n; i++)
    {
        struct DarkSlot s;
        memcpy(&s, buf + 4 + (size_t)i * sizeof(struct DarkSlot), sizeof(s));
        struct DarkSlot *d = dk_find(s.wx, s.wy, true);
        if (d != NULL)
            *d = s;
    }
    return true;
}

#ifdef __cplusplus
}
#endif
