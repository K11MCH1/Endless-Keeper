/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file net_state.c
 *     Thin-client network play: the server streams the game state, clients only draw it (see net_state.h).
 * @par Records:
 *     One record a turn, zlib-compressed, sent reliably and in order:
 *       header (magic, turn, flags)
 *       entries, each starting with a kind byte:
 *         MemLines   region u8, page u32, line mask u64, then per line a word mask u8 and per word a byte mask u8
 *                    followed by the new bytes (absolute values, not XOR: bytes a client keeps for itself survive)
 *         MemFull    region u8, length u32, raw bytes (keyframes)
 *         PageAdd    cx i32, cy i32, the page's payload (keyframes and new chunks)
 *         PageLines  cx i32, cy i32, page u32, line mask u64, lines as MemLines
 *         PageFree   cx i32, cy i32
 *         PageSetEnd (keyframes: pages not named are freed)
 *         Events     count u16, events
 *         End
 *     The regions are `game` and the rewindable statics (replay rewind's set). Bytes that belong to one machine are
 *     never sent: the server copies them into its shadow before diffing.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "net_state.h"

#include <string.h>
#include <stdlib.h>
#include <zlib.h>
#include <SDL3/SDL.h>

#include "bflib_basics.h"
#include "bflib_datetm.h"
#include "game_legacy.h"
#include "net_main.h"
#include "bflib_enet.h"
#include "net_game.h"
#include "net_exchange_common.h"
#include "net_exchange_gameplay.h"
#include "net_spectator.h"
#include "packets.h"
#include "player_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "map_data.h"
#include "local_camera.h"
#include "power_hand.h"
#include "frontmenu_ingame_map.h"
#include "creature_control.h"
#include "sounds.h"
#include "bflib_sound.h"
#include "gui_soundmsgs.h"
#include "room_data.h"
#include "replay_rewind.h"
#include "world_stream.h"
#include "world_pages.h"
#include "world_net.h"
#include "keeperfx.hpp"
#include "gui_topmsg.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define NS_MAGIC 0x3154534Eu /* "NST1" */
#define NS_PAGE 4096u
#define NS_LINE 64u
#define NS_LINES_PER_PAGE (NS_PAGE / NS_LINE)
#define NS_QUEUE_MAX 4096           /* turns a client may hold (over 3 minutes) before it asks for a keyframe */
#define NS_MAX_EVENTS 1024

enum NsKind {
    NsK_End = 0,
    NsK_MemLines,
    NsK_MemFull,
    NsK_PageAdd,
    NsK_PageLines,
    NsK_PageFree,
    NsK_PageSetEnd,
    NsK_Events,
    NsK_ThingLines,    /* thing u16, then lines as MemLines (one thing, as this client has it) */
};

enum NsFlags {
    NsF_Keyframe = 0x01,
};

enum NsRegion {
    NsR_Game = 0,
    NsR_Statics,
    NsR_Count,
};

#pragma pack(1)
struct NsHeader {
    uint32_t magic;
    uint32_t turn;
    uint32_t flags;
};
struct NsWire {                 /* after the message type byte */
    uint32_t raw_len;               /* the whole record */
    uint32_t comp_len;              /* the bytes after this header */
    uint32_t shared_raw;            /* 0, or: the record's first shared_raw bytes come compressed apart, in the */
    uint32_t shared_comp;           /*   first shared_comp bytes (the turn's shared part, compressed once for all) */
};
#pragma pack()

/** A byte range a machine keeps for itself: `count` ranges `stride` apart (count 1: a plain range). */
struct NsExcl {
    size_t off, len, stride;
    uint32_t count;
};

struct NsRegionDef {
    uint8_t *base;
    size_t len;
};

/******************************************************************************/
/* shared */
static struct NsRegionDef regions[NsR_Count];
static struct NsExcl excl[64];
static int excl_count;

/* server */
static uint8_t *shadow[NsR_Count];
struct NsPageShadow {
    int32_t cx, cy;
    uint32_t seen;
    struct NsPageShadow *next;
    NetUserMask known;              /* clients that have this page */
    uint32_t diff_pass, diff_off, diff_len; /* this turn's lines of it, in the shared record */
    uint8_t stale;                  /* no client had it, so it wasn't compared: data is older than the page */
    uint8_t data[];
};
#define NS_PAGE_BUCKETS 1024
static struct NsPageShadow *page_shadows[NS_PAGE_BUCKETS];
static uint32_t page_pass;
/* the record being written (the main thread's; the clients' thing lines go through an NsWriter on the workers) */
static uint8_t *rec;
static size_t rec_len, rec_cap;
static TbBool rec_failed;
static NetUserMask keyframe_wanted; /* users that need a keyframe */
static NetUserMask known_clients;   /* users that have had a keyframe since they connected */
static uint32_t next_input_seq[MAX_NET_USERS];
static struct Packet last_input[MAX_NET_USERS];
static int idle_turns[MAX_NET_USERS];
static uint8_t events_buf[NS_MAX_EVENTS * 24];
static size_t events_len;
static uint16_t events_count;
static TbBool server_started;
static uint32_t stats_bytes, stats_records;
enum NsPart { NsP_Things, NsP_Cctrl, NsP_Game, NsP_Statics, NsP_Pages, NsP_Events, NsP_Count };
static uint64_t stats_part[NsP_Count];
static int64_t stats_ns;
/* the server's time by part: the shared diff of game and statics, the chunk pages, each client's things,
   compressing and sending, and keyframes */
enum NsTime { NsT_Shared, NsT_Pages, NsT_Things, NsT_Compress, NsT_Send, NsT_Keyframes, NsT_Count };
static int64_t stats_t[NsT_Count];
static uint64_t stats_pages_compared, stats_pages_changed;
static int64_t stats_sim_ns;
static double last_sim_ms, last_stream_ms; /* the last 200 turns' (keeperserver's status line) */

void netstate_server_timing(double *sim_ms, double *stream_ms)
{
    *sim_ms = last_sim_ms;
    *stream_ms = last_stream_ms;
}

void netstate_server_sim_time(int64_t ns)
{
    stats_sim_ns += ns;
}

/* the server's view of each client: its copy of the things it was sent. Sparse: a thing without a copy is all zero
   to the client (a keyframe zeroes the things not sent; a thing leaving goes back to zero). A copy costs a Thing
   (2244 bytes); a whole array was 27.6 MB a client. */
struct NsClientState {
    uint16_t *slot;          /* per thing index: 0 none, else 1 + its copy in pool */
    struct Thing *pool;
    uint16_t *free_slots;
    int nfree, used, cap;
    ThingIndex hi;           /* no copy above this thing index */
} __attribute__((aligned(64))); /* a cache line each: the stream workers write different clients' at once */
/* no thing exists above this index (found each turn before the clients' things are compared) */
static ThingIndex live_hi = THINGS_COUNT - 1;
/* each thing's hash at the end of the last turn's stream, and whether it is the same this turn: a client's copy of a
   thing that is the same needs no compare (every copy is brought up to date each turn, or rebuilt by a keyframe) */
static uint64_t thing_hash[THINGS_COUNT];
static uint8_t thing_same[THINGS_COUNT];
/* the live things of the turn, in index order, with what deciding a client's interest needs: scanning 2244-byte things
   for every client cost a cache miss a slot */
struct NsLive {
    ThingIndex idx;
    int32_t cx, cy;
    PlayerNumber owner;
    uint8_t limbo;
};
static struct NsLive live_list[THINGS_COUNT];
static int live_count;
static uint8_t live_bits[(THINGS_COUNT + 7) / 8];

static uint64_t hash_thing(const struct Thing *t)
{
    const uint8_t *p = (const uint8_t *)t;
    uint64_t h = 0x9E3779B97F4A7C15ull ^ sizeof(struct Thing);
    size_t i = 0;
    for (; i + 8 <= sizeof(struct Thing); i += 8)
    {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < sizeof(struct Thing); i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 29);
}

/** Before the clients' things: the highest live thing, and which things are the same as at the last turn. */
static void hash_things(void)
{
    live_hi = 0;
    for (ThingIndex i = THINGS_COUNT - 1; i > 0; i--)
    {
        if ((game.things_data[i].alloc_flags & TAlF_Exists) != 0)
        {
            live_hi = i;
            break;
        }
    }
    live_count = 0;
    memset(live_bits, 0, sizeof(live_bits));
    for (ThingIndex i = 1; i < THINGS_COUNT; i++)
    {
        const struct Thing *t = &game.things_data[i];
        if ((i > live_hi) || ((t->alloc_flags & TAlF_Exists) == 0))
        {
            thing_same[i] = 0;
            thing_hash[i] = 0;
            continue;
        }
        const uint64_t h = hash_thing(t);
        thing_same[i] = (h == thing_hash[i]);
        thing_hash[i] = h;
        struct NsLive *li = &live_list[live_count++];
        li->idx = i;
        li->cx = t->mappos.x.stl.num / WP_CHUNK_STL;
        li->cy = t->mappos.y.stl.num / WP_CHUNK_STL;
        li->owner = t->owner;
        li->limbo = (t->alloc_flags & TAlF_IsInLimbo) ? 1 : 0;
        live_bits[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
}
static struct NsClientState ns_cl[MAX_NET_CONNECTIONS];

static void cl_free(NetUserId c)
{
    free(ns_cl[c].slot);
    free(ns_cl[c].pool);
    free(ns_cl[c].free_slots);
    memset(&ns_cl[c], 0, sizeof(ns_cl[c]));
}

/** A fresh, empty copy for a client (its keyframe). */
static TbBool cl_init(NetUserId c)
{
    cl_free(c);
    ns_cl[c].slot = (uint16_t *)calloc(THINGS_COUNT, sizeof(uint16_t));
    return ns_cl[c].slot != NULL;
}

static struct Thing *cl_copy(NetUserId c, ThingIndex i)
{
    const uint16_t k = ns_cl[c].slot[i];
    return (k > 0) ? &ns_cl[c].pool[k - 1] : NULL;
}

/** A copy for thing i, all zero (as the client has it); NULL when out of memory. */
static struct Thing *cl_new_copy(NetUserId c, ThingIndex i)
{
    struct NsClientState *cl = &ns_cl[c];
    int k;
    if (cl->nfree > 0)
        k = cl->free_slots[--cl->nfree];
    else
    {
        if (cl->used >= cl->cap)
        {
            const int ncap = min((cl->cap > 0) ? cl->cap * 2 : 256, (int)THINGS_COUNT);
            if (cl->used >= ncap)
                return NULL;
            struct Thing *np = (struct Thing *)realloc(cl->pool, (size_t)ncap * sizeof(struct Thing));
            if (np == NULL)
                return NULL;
            cl->pool = np;
            uint16_t *nf = (uint16_t *)realloc(cl->free_slots, (size_t)ncap * sizeof(uint16_t));
            if (nf == NULL)
                return NULL;
            cl->free_slots = nf;
            cl->cap = ncap;
        }
        k = cl->used++;
    }
    memset(&cl->pool[k], 0, sizeof(struct Thing));
    cl->slot[i] = (uint16_t)(k + 1);
    if (i > cl->hi)
        cl->hi = i;
    return &cl->pool[k];
}

static uint32_t stats_dropped_copies; /* things gone from a client's area or gone (for the statistics) */

static void cl_drop_copy(NetUserId c, ThingIndex i)
{
    struct NsClientState *cl = &ns_cl[c];
    __atomic_fetch_add(&stats_dropped_copies, 1, __ATOMIC_RELAXED); /* worker threads too */
    const uint16_t k = cl->slot[i];
    if (k == 0)
        return;
    cl->slot[i] = 0;
    cl->free_slots[cl->nfree++] = (uint16_t)(k - 1);
}
static uint8_t *crec;
static size_t crec_len, crec_cap;
static TbBool crec_failed;
#define NS_GONE_MAX 256
static struct { int32_t cx, cy; NetUserMask known; } gone_pages[NS_GONE_MAX];
static int gone_count;
#define NS_VIEW_CHUNKS 3   /* a client's area: chunks this far from its view (the view is about 2 chunks across) */
#define NS_HEART_CHUNKS 2  /* ... and from its heart */

/* client */
struct NsQueued {
    uint8_t *data;
    size_t len;
};
static struct NsQueued queue[NS_QUEUE_MAX];
static uint8_t *excl_save;  /* the client's own bytes of `game` (the exclusions), kept through what the server sends */
static int queue_head, queue_count;
static TbBool client_has_keyframe;
static TbBool client_keyframe_asked;
static struct Packet client_last_input;
static TbBool client_last_input_valid = false;
static TbClockMSec client_last_turn_ms = 0;
/* game.operation_flags comes from the server, but 'play one level, then exit' is this machine's own */
static TbBool client_single_level = false;
static uint32_t input_seq;

/******************************************************************************/
static TbBool dedicated;
static int startup_load_slot = -1;

void netstate_set_dedicated(TbBool on)
{
    dedicated = on;
}

TbBool netstate_dedicated(void)
{
    return dedicated;
}

void netstate_set_startup_load_slot(int slot)
{
    startup_load_slot = slot;
}

int netstate_take_startup_load_slot_peek(void)
{
    return startup_load_slot;
}

int netstate_take_startup_load_slot(void)
{
    const int slot = startup_load_slot;
    startup_load_slot = -1;
    return slot;
}

TbBool netstate_thin_mode(void)
{
    static int lockstep = -1;
    if (lockstep < 0)
        lockstep = (getenv("KFX_NET_LOCKSTEP") != NULL) ? 1 : 0;
    return !lockstep && network_is_active() && world_mode_active();
}

/** An endless world takes users 4-7 too (a stock network game keeps its lobby's 4). Called when the world is active. */
void netstate_take_all_users(void)
{
    if (netstate_thin_mode() && (netstate.max_users < MAX_NET_USERS))
    {
        SYNCLOG("Thin-client play: up to %d users (was %u)", MAX_NET_USERS, netstate.max_users);
        netstate.max_users = MAX_NET_USERS;
    }
}

TbBool netstate_is_server(void)
{
    return netstate_thin_mode() && network_is_host();
}

TbBool netstate_is_client(void)
{
    return netstate_thin_mode() && !network_is_host();
}

/******************************************************************************/
/* the page payload: the parts of a chunk page that are game state */
#define NS_PG_MAP (sizeof(struct Map) * WP_CHUNK_STL_CELLS)
#define NS_PG_STAT (sizeof(unsigned short) * WP_CHUNK_STL_CELLS)
#define NS_PG_LIGHT (sizeof(unsigned short) * WP_CHUNK_STL_CELLS)
#define NS_PG_SLAB (sizeof(struct SlabMap) * WP_CHUNK_SLAB_CELLS)
#define NS_PG_EXT (WP_CHUNK_SLAB_CELLS)
#define NS_PG_SIZE (NS_PG_MAP + NS_PG_STAT + NS_PG_LIGHT + NS_PG_SLAB + NS_PG_EXT + NS_PG_EXT)

static void page_gather(const struct WorldPage *pg, uint8_t *out)
{
    memcpy(out, pg->map, NS_PG_MAP); out += NS_PG_MAP;
    memcpy(out, pg->stat_light, NS_PG_STAT); out += NS_PG_STAT;
    memcpy(out, pg->lightness, NS_PG_LIGHT); out += NS_PG_LIGHT;
    memcpy(out, pg->slab, NS_PG_SLAB); out += NS_PG_SLAB;
    memcpy(out, pg->ext, NS_PG_EXT); out += NS_PG_EXT;
    memcpy(out, pg->ext_initial, NS_PG_EXT);
}

static void page_scatter(struct WorldPage *pg, const uint8_t *in)
{
    memcpy(pg->map, in, NS_PG_MAP); in += NS_PG_MAP;
    memcpy(pg->stat_light, in, NS_PG_STAT); in += NS_PG_STAT;
    memcpy(pg->lightness, in, NS_PG_LIGHT); in += NS_PG_LIGHT;
    memcpy(pg->slab, in, NS_PG_SLAB); in += NS_PG_SLAB;
    memcpy(pg->ext, in, NS_PG_EXT); in += NS_PG_EXT;
    memcpy(pg->ext_initial, in, NS_PG_EXT);
}

/******************************************************************************/
/* exclusions: the bytes of `game` that belong to one machine */
static void excl_add(size_t off, size_t len, size_t stride, uint32_t count)
{
    if ((excl_count >= (int)(sizeof(excl) / sizeof(excl[0]))) || (len == 0))
        return;
    excl[excl_count].off = off;
    excl[excl_count].len = len;
    excl[excl_count].stride = stride;
    excl[excl_count].count = count;
    excl_count++;
}
#define EXCL_FIELD(field) excl_add(offsetof(struct Game, field), sizeof(((struct Game *)0)->field), 0, 1)
#define EXCL_THING_FIELD(field) excl_add(offsetof(struct Game, things_data) + offsetof(struct Thing, field), \
    sizeof(((struct Thing *)0)->field), sizeof(struct Thing), THINGS_COUNT)

#define EXCL_CCTRL_FIELD(field) excl_add(offsetof(struct Game, cctrl_data) + offsetof(struct CreatureControl, field), \
    sizeof(((struct CreatureControl *)0)->field), sizeof(struct CreatureControl), CREATURES_COUNT)
#define EXCL_USER_FIELD(field) excl_add(offsetof(struct Game, user_states) + offsetof(struct UserState, field), \
    sizeof(((struct UserState *)0)->field), sizeof(struct UserState), MAX_NET_USERS)

/** The lists in game.thing_lists that hold things; the static and dynamic light lists come after them. */
#define NS_THING_CLASS_LISTS TngList_StaticLights

static void excl_setup(void)
{
    excl_count = 0;
    /* Every thing is streamed, the effects too: the server makes effects round every human player's view, and they
       share the light pool, the lists and the map with the synced things. A client runs no thing updates at all. */
    EXCL_FIELD(unsync_random_seed);
    EXCL_FIELD(sound_random_seed);
    /* chains of things on map blocks: each client rebuilds them from the things' in-map flags */
    EXCL_THING_FIELD(next_on_mapblk);
    EXCL_THING_FIELD(prev_on_mapblk);
    /* sound handles and drawing bookkeeping of this machine */
    EXCL_THING_FIELD(snd_emitter_id);
    EXCL_THING_FIELD(last_turn_drawn);
    EXCL_CCTRL_FIELD(thought_bubble_last_turn_drawn);
    EXCL_CCTRL_FIELD(thought_bubble_display_timer);
    EXCL_CCTRL_FIELD(footstep_variant);
    EXCL_CCTRL_FIELD(footstep_counter);
    EXCL_USER_FIELD(render_roomspace);
    EXCL_FIELD(sound_settings);  /* holds pointers */
    /* each client rebuilds the thing lists from the things it has; the light lists after them stay replicated */
    excl_add(offsetof(struct Game, thing_lists), sizeof(game.thing_lists[0]) * NS_THING_CLASS_LISTS, 0, 1);
    EXCL_FIELD(bookmark);
    EXCL_FIELD(creatures_tend_imprison);
    EXCL_FIELD(creatures_tend_flee);
    EXCL_FIELD(frame_step);
    /* this machine's input, checks and GUI */
    EXCL_FIELD(packets);
    EXCL_FIELD(host_checksums);
    EXCL_FIELD(log_snapshot);
    EXCL_FIELD(view_mode_flags);
    EXCL_FIELD(flags_gui);
    EXCL_FIELD(local_plyr_idx);
    EXCL_FIELD(music_track);
    EXCL_FIELD(music_fname);
    EXCL_FIELD(eastegg01_cntr);
    EXCL_FIELD(eastegg02_cntr);
    EXCL_FIELD(save_game_slot);
    EXCL_FIELD(small_map_state);
    EXCL_FIELD(mouse_light_pos);
    EXCL_FIELD(something_light_x);
    EXCL_FIELD(something_light_y);
    EXCL_FIELD(flash_button_index);
    EXCL_FIELD(flash_button_time);
    EXCL_FIELD(active_panel_mnu_idx);
    EXCL_FIELD(hand_over_subtile_x);
    EXCL_FIELD(hand_over_subtile_y);
    EXCL_FIELD(chosen_room_kind);
    EXCL_FIELD(chosen_room_spridx);
    EXCL_FIELD(chosen_room_tooltip);
    EXCL_FIELD(chosen_spell_type);
    EXCL_FIELD(chosen_spell_spridx);
    EXCL_FIELD(chosen_spell_tooltip);
    EXCL_FIELD(manufactr_element);
    EXCL_FIELD(manufactr_spridx);
    EXCL_FIELD(manufactr_tooltip);
    EXCL_FIELD(messages);
    EXCL_FIELD(active_messages_count);
    EXCL_FIELD(evntbox_text_buffer);
    EXCL_FIELD(evntbox_scroll_window);
    EXCL_FIELD(input_lag_turns);
    EXCL_FIELD(skip_initial_input_turns);
    /* static configuration: loaded the same on every machine, and large */
    EXCL_FIELD(conf);
    /* only the server runs these */
    EXCL_FIELD(computer);
    EXCL_FIELD(computer_task);
    EXCL_FIELD(script);
    EXCL_FIELD(navigation_map);
}

/** Copies the excluded bytes of `src` into `dst` (both laid out as `game`). */
/** The excluded bytes within [off, off+n), from src to dst: only the pages compared pay for them (the excluded
 *  fields of all things and creature controls, copied whole, cost more than the comparing). */
static void excl_copy_page(uint8_t *dst, const uint8_t *src, size_t off, size_t n)
{
    const size_t end = off + n;
    for (int i = 0; i < excl_count; i++)
    {
        const struct NsExcl *e = &excl[i];
        const size_t stride = (e->count > 1) ? e->stride : 0;
        const size_t last = e->off + (stride * (e->count - 1)) + e->len;
        if ((end <= e->off) || (off >= last))
            continue;
        /* the elements that overlap the page */
        uint32_t k0 = 0, k1 = 0;
        if (stride > 0)
        {
            k0 = (off >= e->off + e->len) ? (uint32_t)((off - e->off - e->len) / stride + 1) : 0;
            k1 = (uint32_t)min((size_t)(e->count - 1), (end - 1 - e->off) / stride);
        }
        for (uint32_t k = k0; k <= k1; k++)
        {
            const size_t a = e->off + k * stride;
            const size_t lo = max(a, off), hi = min(a + e->len, end);
            if (lo < hi)
                memcpy(dst + lo, src + lo, hi - lo);
        }
    }
}

static void excl_copy(uint8_t *dst, const uint8_t *src)
{
    for (int i = 0; i < excl_count; i++)
    {
        const struct NsExcl *e = &excl[i];
        for (uint32_t k = 0; k < e->count; k++)
        {
            const size_t off = e->off + k * e->stride;
            memcpy(dst + off, src + off, e->len);
        }
    }
}

/******************************************************************************/
/* record writing (server) */
static void rec_reserve(size_t more)
{
    if (rec_len + more <= rec_cap)
        return;
    size_t cap = (rec_cap == 0) ? (1u << 20) : rec_cap;
    while (cap < rec_len + more)
        cap *= 2;
    uint8_t *nb = (uint8_t *)realloc(rec, cap);
    if (nb == NULL)
    {
        rec_failed = true;
        return;
    }
    rec = nb;
    rec_cap = cap;
}

static void rec_put(const void *p, size_t n)
{
    rec_reserve(n);
    if (rec_failed)
        return;
    memcpy(rec + rec_len, p, n);
    rec_len += n;
}

static void rec_u8(uint8_t v) { rec_put(&v, 1); }

/* a writer of lines that isn't the main thread's record: a stream worker's thing lines */
struct NsWriter {
    uint8_t *buf;
    size_t len, cap;
    TbBool failed;
};

static void w_put(struct NsWriter *w, const void *p, size_t n)
{
    if (w->len + n > w->cap)
    {
        size_t cap = (w->cap == 0) ? (1u << 16) : w->cap;
        while (cap < w->len + n)
            cap *= 2;
        uint8_t *nb = (uint8_t *)realloc(w->buf, cap);
        if (nb == NULL)
        {
            w->failed = true;
            return;
        }
        w->buf = nb;
        w->cap = cap;
    }
    if (w->failed)
        return;
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

/** diff_page, writing to w (head(w, ctx) writes the entry head). */
static void diff_page_w(struct NsWriter *w, uint8_t *shad, const uint8_t *live, size_t len,
    void (*head)(struct NsWriter *, const void *), const void *ctx)
{
    if (memcmp(shad, live, len) == 0)
        return;
    uint64_t line_mask = 0;
    const int lines = (int)((len + NS_LINE - 1) / NS_LINE);
    for (int ln = 0; ln < lines; ln++)
    {
        const size_t o = (size_t)ln * NS_LINE;
        const size_t n = (o + NS_LINE <= len) ? NS_LINE : len - o;
        if (memcmp(shad + o, live + o, n) != 0)
            line_mask |= (uint64_t)1 << ln;
    }
    head(w, ctx);
    w_put(w, &line_mask, sizeof(line_mask));
    for (int ln = 0; ln < lines; ln++)
    {
        if (((line_mask >> ln) & 1u) == 0)
            continue;
        const size_t o = (size_t)ln * NS_LINE;
        uint8_t word_mask = 0;
        const size_t wm_at = w->len;
        const uint8_t zero = 0;
        w_put(w, &zero, 1);
        for (int k = 0; k < 8; k++)
        {
            const size_t wo = o + (size_t)k * 8;
            if (wo >= len)
                break;
            const size_t wn = (wo + 8 <= len) ? 8 : len - wo;
            if (memcmp(shad + wo, live + wo, wn) == 0)
                continue;
            word_mask |= (uint8_t)(1u << k);
            uint8_t byte_mask = 0;
            uint8_t bytes[8];
            int nb = 0;
            for (size_t b = 0; b < wn; b++)
            {
                if (shad[wo + b] != live[wo + b])
                {
                    byte_mask |= (uint8_t)(1u << b);
                    bytes[nb++] = live[wo + b];
                }
            }
            w_put(w, &byte_mask, 1);
            w_put(w, bytes, (size_t)nb);
        }
        if (!w->failed)
            w->buf[wm_at] = word_mask;
        memcpy(shad + o, live + o, (o + NS_LINE <= len) ? NS_LINE : len - o);
    }
}
static void rec_u32(uint32_t v) { rec_put(&v, 4); }
static void rec_i32(int32_t v) { rec_put(&v, 4); }

/** Appends the lines of one 4 KB page of `live` that differ from `shad` (new bytes), and updates `shad`.
 *  `len` may be short of a page (the end of a region). The entry head (kind and id) is written by `head` only when
 *  something changed. */
static void diff_page(uint8_t *shad, const uint8_t *live, size_t len, void (*head)(const void *), const void *ctx)
{
    if (memcmp(shad, live, len) == 0)
        return;
    uint64_t line_mask = 0;
    const int lines = (int)((len + NS_LINE - 1) / NS_LINE);
    for (int ln = 0; ln < lines; ln++)
    {
        const size_t o = (size_t)ln * NS_LINE;
        const size_t n = (o + NS_LINE <= len) ? NS_LINE : len - o;
        if (memcmp(shad + o, live + o, n) != 0)
            line_mask |= (uint64_t)1 << ln;
    }
    head(ctx);
    rec_put(&line_mask, sizeof(line_mask));
    for (int ln = 0; ln < lines; ln++)
    {
        if (((line_mask >> ln) & 1u) == 0)
            continue;
        const size_t o = (size_t)ln * NS_LINE;
        uint8_t word_mask = 0;
        const size_t wm_at = rec_len;
        rec_u8(0);
        for (int w = 0; w < 8; w++)
        {
            const size_t wo = o + (size_t)w * 8;
            if (wo >= len)
                break;
            const size_t wn = (wo + 8 <= len) ? 8 : len - wo;
            if (memcmp(shad + wo, live + wo, wn) == 0)
                continue;
            word_mask |= (uint8_t)(1u << w);
            uint8_t byte_mask = 0;
            uint8_t bytes[8];
            int nb = 0;
            for (size_t b = 0; b < wn; b++)
            {
                if (shad[wo + b] != live[wo + b])
                {
                    byte_mask |= (uint8_t)(1u << b);
                    bytes[nb++] = live[wo + b];
                }
            }
            rec_u8(byte_mask);
            rec_put(bytes, nb);
        }
        if (!rec_failed)
            rec[wm_at] = word_mask;
        memcpy(shad + o, live + o, (o + NS_LINE <= len) ? NS_LINE : len - o);
    }
}

struct MemHeadCtx {
    uint8_t region;
    uint32_t page;
};
static void mem_head(const void *ctx)
{
    const struct MemHeadCtx *c = (const struct MemHeadCtx *)ctx;
    rec_u8(NsK_MemLines);
    rec_u8(c->region);
    rec_u32(c->page);
}

struct PageHeadCtx {
    int32_t cx, cy;
    uint32_t page;
};
static void page_head(const void *ctx)
{
    const struct PageHeadCtx *c = (const struct PageHeadCtx *)ctx;
    rec_u8(NsK_PageLines);
    rec_i32(c->cx);
    rec_i32(c->cy);
    rec_u32(c->page);
}

/* parts of `game` no client needs, skipped whole when diffing (they are excluded too, or unused) */
struct NsSkip {
    size_t off, len;
};
static struct NsSkip skips[16];
static int skip_count;
#define SKIP_FIELD(field) do { if (skip_count < 16) { skips[skip_count].off = offsetof(struct Game, field); \
    skips[skip_count].len = sizeof(((struct Game *)0)->field); skip_count++; } } while (0)

static void skips_setup(void)
{
    skip_count = 0;
    SKIP_FIELD(conf);
    SKIP_FIELD(computer);
    SKIP_FIELD(computer_task);
    SKIP_FIELD(script);
    SKIP_FIELD(navigation_map);
    SKIP_FIELD(things_data); /* per client */
    if (world_paged)
    {
        /* the chunked world keeps its map in pages; the flat arrays are left behind */
        SKIP_FIELD(map);
        SKIP_FIELD(slabmap);
        SKIP_FIELD(slab_ext_data);
        SKIP_FIELD(slab_ext_data_initial);
    }
}

static TbBool page_skipped(size_t off, size_t n)
{
    for (int i = 0; i < skip_count; i++)
    {
        if ((off >= skips[i].off) && (off + n <= skips[i].off + skips[i].len))
            return true;
    }
    return false;
}

/** Are all elements of an array (things, creature controls) on this page unused, now and as the clients have them? */
static TbBool page_elements_idle(const uint8_t *live, const uint8_t *shad, size_t off, size_t n, size_t arr_off,
    size_t elem_size, uint32_t count, size_t flag_off, uint32_t flag_mask, size_t flag_size)
{
    if ((off < arr_off) || (off + n > arr_off + (size_t)count * elem_size))
        return false; /* not inside the array: diffed */
    const size_t first = (off - arr_off) / elem_size, last = (off + n - 1 - arr_off) / elem_size;
    for (size_t e = first; e <= last; e++)
    {
        const size_t fo = arr_off + e * elem_size + flag_off;
        uint32_t a = 0, b = 0;
        memcpy(&a, live + fo, flag_size);
        memcpy(&b, shad + fo, flag_size);
        if (((a | b) & flag_mask) != 0)
            return false;
    }
    return true;
}

static void diff_region(int r)
{
    const struct NsRegionDef *rd = &regions[r];
    if ((rd->base == NULL) || (shadow[r] == NULL))
        return;
    for (size_t off = 0; off < rd->len; off += NS_PAGE)
    {
        const size_t n = (off + NS_PAGE <= rd->len) ? NS_PAGE : rd->len - off;
        if (r == NsR_Game)
        {
            if (page_skipped(off, n))
                continue;
            /* a page that holds the first or last bytes of the things: those bytes go per client */
            const size_t t1 = offsetof(struct Game, things_data), t2 = t1 + sizeof(game.things_data);
            if ((off < t2) && (off + n > t1))
            {
                const size_t a = max(off, t1), b = min(off + n, t2);
                memcpy(shadow[r] + a, rd->base + a, b - a);
            }
            if (page_elements_idle(rd->base, shadow[r], off, n, offsetof(struct Game, things_data), sizeof(struct Thing),
                    THINGS_COUNT, offsetof(struct Thing, alloc_flags), TAlF_Exists, sizeof(((struct Thing *)0)->alloc_flags)))
                continue;
            if (page_elements_idle(rd->base, shadow[r], off, n, offsetof(struct Game, cctrl_data),
                    sizeof(struct CreatureControl), CREATURES_COUNT, offsetof(struct CreatureControl, creature_control_flags),
                    CCFlg_Exists, sizeof(((struct CreatureControl *)0)->creature_control_flags)))
                continue;
        }
        if (r == NsR_Game)
            excl_copy_page(shadow[r], rd->base, off, n); /* never sent */
        struct MemHeadCtx c = {(uint8_t)r, (uint32_t)(off / NS_PAGE)};
        stats_pages_compared++;
        const size_t before = rec_len;
        diff_page(shadow[r] + off, rd->base + off, n, mem_head, &c);
        if (rec_len != before)
            stats_pages_changed++;
        int part = NsP_Statics;
        if (r == NsR_Game)
        {
            part = NsP_Game;
            if ((off >= offsetof(struct Game, things_data)) && (off < offsetof(struct Game, things_data) + sizeof(game.things_data)))
                part = NsP_Things;
            else if ((off >= offsetof(struct Game, cctrl_data)) && (off < offsetof(struct Game, cctrl_data) + sizeof(game.cctrl_data)))
                part = NsP_Cctrl;
        }
        stats_part[part] += rec_len - before;
    }
}

static struct NsPageShadow **page_slot(int32_t cx, int32_t cy)
{
    const uint32_t h = ((uint32_t)cx * 73856093u) ^ ((uint32_t)cy * 19349663u);
    struct NsPageShadow **pp = &page_shadows[h % NS_PAGE_BUCKETS];
    while ((*pp != NULL) && (((*pp)->cx != cx) || ((*pp)->cy != cy)))
        pp = &(*pp)->next;
    return pp;
}

static uint8_t page_tmp[NS_PG_SIZE];

/** Is the page as the clients have it? The map blocks' chain heads don't count (each client rebuilds its chains), so
 *  the shadow takes the live ones first. */
static TbBool page_unchanged(const struct WorldPage *pg, uint8_t *sh)
{
    struct Map *smap = (struct Map *)sh;
    for (int i = 0; i < WP_CHUNK_STL_CELLS; i++)
        smap[i].mapwho = pg->map[i].mapwho;
    if (memcmp(sh, pg->map, NS_PG_MAP) != 0) return false;
    sh += NS_PG_MAP;
    if (memcmp(sh, pg->stat_light, NS_PG_STAT) != 0) return false;
    sh += NS_PG_STAT;
    if (memcmp(sh, pg->lightness, NS_PG_LIGHT) != 0) return false;
    sh += NS_PG_LIGHT;
    if (memcmp(sh, pg->slab, NS_PG_SLAB) != 0) return false;
    sh += NS_PG_SLAB;
    if (memcmp(sh, pg->ext, NS_PG_EXT) != 0) return false;
    sh += NS_PG_EXT;
    return memcmp(sh, pg->ext_initial, NS_PG_EXT) == 0;
}

/** A page's shadow brought up to date before a client gets it whole (it wasn't compared while no client had it). */
static void page_fresh(struct NsPageShadow *ps)
{
    if (!ps->stale)
        return;
    struct WorldPage *pg = wp_page(ps->cx, ps->cy);
    if (pg != NULL)
        page_gather(pg, ps->data);
    ps->stale = 0;
}

static void diff_pages(void)
{
    if (!world_paged)
        return;
    page_pass++;
    for (struct WorldPage *pg = wp_first(); pg != NULL; pg = pg->next)
    {
        struct NsPageShadow **pp = page_slot(pg->cx, pg->cy);
        if ((*pp != NULL) && !numask_any((*pp)->known))
        {
            /* no client has it: compared when one gets it (page_fresh) */
            (*pp)->stale = 1;
            (*pp)->seen = page_pass;
            continue;
        }
        if ((*pp != NULL) && page_unchanged(pg, (*pp)->data))
        {
            (*pp)->seen = page_pass;
            continue;
        }
        page_gather(pg, page_tmp);
        if (*pp == NULL)
        {
            /* a new page: each client gets it whole when it is in its area */
            struct NsPageShadow *ps = (struct NsPageShadow *)calloc(1, sizeof(struct NsPageShadow) + NS_PG_SIZE);
            if (ps == NULL)
            {
                rec_failed = true;
                return;
            }
            ps->cx = pg->cx;
            ps->cy = pg->cy;
            memcpy(ps->data, page_tmp, NS_PG_SIZE);
            *pp = ps;
        } else
        {
            (*pp)->diff_off = (uint32_t)rec_len;
            for (size_t off = 0; off < NS_PG_SIZE; off += NS_PAGE)
            {
                struct PageHeadCtx c = {pg->cx, pg->cy, (uint32_t)(off / NS_PAGE)};
                const size_t n = (off + NS_PAGE <= NS_PG_SIZE) ? NS_PAGE : NS_PG_SIZE - off;
                diff_page((*pp)->data + off, page_tmp + off, n, page_head, &c);
            }
            (*pp)->diff_len = (uint32_t)(rec_len - (*pp)->diff_off);
            (*pp)->diff_pass = page_pass;
        }
        (*pp)->seen = page_pass;
    }
    /* pages the server let go */
    for (int b = 0; b < NS_PAGE_BUCKETS; b++)
    {
        struct NsPageShadow **pp = &page_shadows[b];
        while (*pp != NULL)
        {
            if ((*pp)->seen != page_pass)
            {
                struct NsPageShadow *gone = *pp;
                if (numask_any(gone->known) && (gone_count < NS_GONE_MAX))
                {
                    gone_pages[gone_count].cx = gone->cx;
                    gone_pages[gone_count].cy = gone->cy;
                    gone_pages[gone_count].known = gone->known;
                    gone_count++;
                }
                *pp = gone->next;
                free(gone);
                continue;
            }
            pp = &(*pp)->next;
        }
    }
}

static void free_page_shadows(void)
{
    for (int b = 0; b < NS_PAGE_BUCKETS; b++)
    {
        while (page_shadows[b] != NULL)
        {
            struct NsPageShadow *n = page_shadows[b]->next;
            free(page_shadows[b]);
            page_shadows[b] = n;
        }
    }
}

static void rec_begin(uint32_t flags)
{
    rec_len = 0;
    rec_failed = false;
    struct NsHeader h = {NS_MAGIC, (uint32_t)game.play_gameturn, flags};
    rec_put(&h, sizeof(h));
}

/** The clients the stream goes to: seated users other than the host, and spectators, that are logged in. */
/* test switch KFX_NET_SIMCLIENTS=N: simulated clients on free spectator slots (records made, compressed, not sent) */
static NetUserMask sim_mask;
static int sim_wanted = -1;
static ThingIndex sim_follow[MAX_NET_CONNECTIONS];

static NetUserId simcheck_client; /* KFX_NET_SIMCHECK: below */

static NetUserMask client_mask(void)
{
    NetUserMask m = numask_none();
    for (NetUserId id = SERVER_ID + 1; id < MAX_NET_CONNECTIONS; id++)
    {
        if (IsUserActive(id))
            numask_add(&m, id);
    }
    if (sim_wanted < 0)
    {
        const char *v = getenv("KFX_NET_SIMCLIENTS");
        sim_wanted = (v != NULL) ? max(0, min(atoi(v), MAX_NET_SPECTATORS)) : 0;
    }
    /* a real client taking a slot ends the simulated one there; free spectator slots fill up to the number asked
       (the self-check stops if it was that one: its copy was made for the simulated client) */
    if ((simcheck_client >= 0) && numask_has(m, simcheck_client) && numask_has(sim_mask, simcheck_client))
    {
        SYNCLOG("Thin-client server: a real client took the self-checked slot %d; the self-check stops", (int)simcheck_client);
        simcheck_client = -2;
    }
    sim_mask = numask_andnot(sim_mask, m);
    for (NetUserId id = MAX_NET_USERS; (id < MAX_NET_CONNECTIONS) && (numask_count(sim_mask) < sim_wanted); id++)
    {
        if (!numask_has(m, id) && !numask_has(sim_mask, id))
        {
            numask_add(&sim_mask, id);
            sim_follow[id] = 0;
        }
    }
    return numask_or(m, sim_mask);
}

/* KFX_NET_SIMCHECK=1: the first simulated client's things, as a client would have them */
static struct Thing *simcheck_things;
static NetUserId simcheck_client = -1;
static uint32_t simcheck_bad, simcheck_turns;
static void simcheck_apply(const uint8_t *from, const uint8_t *to);
static void simcheck_compare(NetUserId c);

static TbBool sim_client(NetUserId c)
{
    return (c >= 0) && (c < MAX_NET_CONNECTIONS) && numask_has(sim_mask, c);
}

/** A simulated client's view: the creature it follows, the k-th one of the creature list when it has none. */
static TbBool sim_view(NetUserId c, MapSubtlCoord *vx, MapSubtlCoord *vy)
{
    struct Thing *thing = thing_get(sim_follow[c]);
    if (!thing_exists(thing) || (thing->class_id != TCls_Creature))
    {
        const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
        int k = (int)c - MAX_NET_USERS;
        long guard = 0;
        thing = INVALID_THING;
        for (long i = slist->index; (i > 0) && (guard++ <= (long)slist->count);)
        {
            struct Thing *t = thing_get(i);
            if (thing_is_invalid(t))
                break;
            i = t->next_of_class;
            thing = t;
            if (k-- <= 0)
                break;
        }
        if (!thing_exists(thing))
            return false;
        sim_follow[c] = thing->index;
    }
    *vx = thing->mappos.x.stl.num;
    *vy = thing->mappos.y.stl.num;
    return true;
}

/** A client's area. */
struct NsInterest {
    TbBool all;                 /* spectators, and a player whose map is open */
    PlayerNumber plyr;
    TbBool view, heart;
    int32_t vcx, vcy, hcx, hcy; /* chunks */
};

static void interest_of(NetUserId c, struct NsInterest *in)
{
    memset(in, 0, sizeof(*in));
    in->plyr = PLAYER_NONE;
    if (sim_client(c))
    {
        MapSubtlCoord vx, vy;
        if (sim_view(c, &vx, &vy))
        {
            in->view = true;
            in->vcx = vx / WP_CHUNK_STL;
            in->vcy = vy / WP_CHUNK_STL;
        }
        return;
    }
    if ((c < 0) || (c >= MAX_NET_USERS))
    {
        in->all = true;
        return;
    }
    in->plyr = get_net_user_player_number(c);
    const struct UserState *ustate = get_user_state(c);
    if ((in->plyr < 0) || user_state_invalid(ustate) || (get_user_view_type(c) == PVT_MapScreen))
    {
        in->all = true;
        return;
    }
    MapSubtlCoord vx = ustate->dungeon_camera.x / COORD_PER_STL, vy = ustate->dungeon_camera.y / COORD_PER_STL;
    const struct Thing *ctrl = thing_get(ustate->controlled_thing_idx);
    if ((ustate->controlled_thing_idx > 0) && thing_exists(ctrl))
    {
        vx = ctrl->mappos.x.stl.num;
        vy = ctrl->mappos.y.stl.num;
    }
    in->view = true;
    in->vcx = vx / WP_CHUNK_STL;
    in->vcy = vy / WP_CHUNK_STL;
    const struct Thing *heart = find_players_dungeon_heart(in->plyr);
    if (thing_exists(heart))
    {
        in->heart = true;
        in->hcx = heart->mappos.x.stl.num / WP_CHUNK_STL;
        in->hcy = heart->mappos.y.stl.num / WP_CHUNK_STL;
    }
}

static TbBool chunk_in_interest(const struct NsInterest *in, int32_t cx, int32_t cy)
{
    if (in->all)
        return true;
    if (in->view && (abs(cx - in->vcx) <= NS_VIEW_CHUNKS) && (abs(cy - in->vcy) <= NS_VIEW_CHUNKS))
        return true;
    return in->heart && (abs(cx - in->hcx) <= NS_HEART_CHUNKS) && (abs(cy - in->hcy) <= NS_HEART_CHUNKS);
}

static TbBool thing_in_interest(const struct NsInterest *in, const struct Thing *t)
{
    return chunk_in_interest(in, t->mappos.x.stl.num / WP_CHUNK_STL, t->mappos.y.stl.num / WP_CHUNK_STL);
}

/** Whether a client gets a live thing: its own, one in its area, or one in its hand (a body carried out of it). */
static TbBool thing_relevant(const struct NsInterest *in, const struct Thing *t)
{
    if (in->all || (t->owner == in->plyr) || thing_in_interest(in, t))
        return true;
    return (in->plyr >= 0) && ((t->alloc_flags & TAlF_IsInLimbo) != 0) && thing_is_in_power_hand_list(t, in->plyr);
}

static void crec_put(const void *p, size_t n)
{
    if (crec_len + n > crec_cap)
    {
        size_t cap = (crec_cap == 0) ? (1u << 20) : crec_cap;
        while (cap < crec_len + n)
            cap *= 2;
        uint8_t *nb = (uint8_t *)realloc(crec, cap);
        if (nb == NULL)
        {
            crec_failed = true;
            return;
        }
        crec = nb;
        crec_cap = cap;
    }
    memcpy(crec + crec_len, p, n);
    crec_len += n;
}

/** Compresses a record and sends it reliably to one client. */
/** zlib at Z_BEST_SPEED, as compress2 makes it, but with one stream kept and reset for each record: setting a
 *  stream up costs more than compressing a turn's record. */
static TbBool ns_compress(uint8_t *dst, uLongf *dst_len, const uint8_t *src, size_t len)
{
    static __thread z_stream zs; /* each thread's own */
    static __thread TbBool zs_ready = false;
    if (!zs_ready)
    {
        memset(&zs, 0, sizeof(zs));
        if (deflateInit(&zs, Z_BEST_SPEED) != Z_OK)
            return compress2(dst, dst_len, src, len, Z_BEST_SPEED) == Z_OK;
        zs_ready = true;
    } else if (deflateReset(&zs) != Z_OK)
        return false;
    zs.next_in = (Bytef *)src;
    zs.avail_in = (uInt)len;
    zs.next_out = dst;
    zs.avail_out = (uInt)*dst_len;
    if (deflate(&zs, Z_FINISH) != Z_STREAM_END)
        return false;
    *dst_len = zs.total_out;
    return true;
}

/** A state message (made by send_record, or by a keyframe worker) to one client, reliably. */
static void send_state_message(const char *msg, size_t total, NetUserId c)
{
    const int64_t t0 = get_time_tick_ns();
    if (!sim_client(c))
        send_to_active_peers(1, NetSend_Reliable, msg, total, numask_of(c));
    stats_bytes += (uint32_t)total;
    stats_records++;
    stats_t[NsT_Send] += get_time_tick_ns() - t0;
}

static void send_record(const uint8_t *buf, size_t len, NetUserId c)
{
    uLongf comp_len = compressBound(len);
    char *msg = (char *)malloc(1 + sizeof(struct NsWire) + comp_len);
    if (msg == NULL)
        return;
    int64_t t0 = get_time_tick_ns();
    if (!ns_compress((uint8_t *)(msg + 1 + sizeof(struct NsWire)), &comp_len, buf, len))
    {
        free(msg);
        return;
    }
    const int64_t t1 = get_time_tick_ns();
    stats_t[NsT_Compress] += t1 - t0;
    t0 = t1;
    msg[0] = NETMSG_STATE_TURN;
    struct NsWire w = {(uint32_t)len, (uint32_t)comp_len, 0, 0};
    memcpy(msg + 1, &w, sizeof(w));
    const size_t total = 1 + sizeof(struct NsWire) + comp_len;
    if (!sim_client(c))
        send_to_active_peers(1, NetSend_Reliable, msg, total, numask_of(c));
    stats_bytes += (uint32_t)total;
    stats_records++;
    free(msg);
    stats_t[NsT_Send] += get_time_tick_ns() - t0;
}

/* the turn's shared part, compressed once (send_split_record) */
static uint8_t *shared_z;
static size_t shared_z_len, shared_z_cap, shared_z_raw;

/** Compresses the first `len` bytes of the turn's record, the part every client's record starts with. */
static TbBool compress_shared(const uint8_t *buf, size_t len)
{
    const int64_t t0 = get_time_tick_ns();
    uLongf comp_len = compressBound(len);
    if (shared_z_cap < comp_len)
    {
        uint8_t *grown = (uint8_t *)realloc(shared_z, comp_len);
        if (grown == NULL)
            return false;
        shared_z = grown;
        shared_z_cap = comp_len;
    }
    if (!ns_compress(shared_z, &comp_len, buf, len))
        return false;
    shared_z_len = comp_len;
    shared_z_raw = len;
    stats_t[NsT_Compress] += get_time_tick_ns() - t0;
    return true;
}


/******************************************************************************/
/* keyframes, compressed off the main thread */
/* A keyframe is the whole state, about 55 MB before compression. Compressing it on the main thread took the
   server's turn about 140 ms, a hitch every player saw when someone joined. The record is still made on the main
   thread (the state as it is that turn); a worker thread compresses it. Until it is sent, the client's turn records
   are held, and then go after it, in order (the reliable channel keeps the order). */
#define NS_KF_JOBS 3     /* each holds a keyframe and its compressed copy: about 110 MB */
#define NS_KF_PER_TURN 2 /* keyframes started a turn */
struct NsKfJob {
    SDL_Thread *thread;
    SDL_AtomicInt done;
    NetUserId client;
    uint32_t gen;
    GameTurn turn;
    uint8_t *raw;
    size_t raw_len;
    char *msg;
    size_t msg_len;
    int64_t build_ns, compress_ns;
};
static struct NsKfJob kf_jobs[NS_KF_JOBS];
struct NsHeld {
    struct NsHeld *next;
    size_t len;
    uint8_t data[];
};
static struct {
    uint32_t gen;              /* bumped by each keyframe made for the client: an older one still compressing is dropped */
    TbBool waiting;            /* its newest keyframe isn't sent yet: its turn records are held */
    struct NsHeld *held, *held_tail;
} kf_cl[MAX_NET_CONNECTIONS];
/* the first turn a client may get its next keyframe: one every NS_KF_GAP turns at most, whatever it asks */
#define NS_KF_GAP 40
static GameTurn kf_next_turn[MAX_NET_CONNECTIONS];

/** zlib at Z_BEST_SPEED in a message; thread-safe (compress2 keeps no state between calls). */
static char *make_state_message(const uint8_t *raw, size_t len, size_t *msg_len)
{
    uLongf comp_len = compressBound(len);
    char *msg = (char *)malloc(1 + sizeof(struct NsWire) + comp_len);
    if (msg == NULL)
        return NULL;
    if (compress2((Bytef *)(msg + 1 + sizeof(struct NsWire)), &comp_len, raw, len, Z_BEST_SPEED) != Z_OK)
    {
        free(msg);
        return NULL;
    }
    msg[0] = NETMSG_STATE_TURN;
    struct NsWire w = {(uint32_t)len, (uint32_t)comp_len, 0, 0};
    memcpy(msg + 1, &w, sizeof(w));
    *msg_len = 1 + sizeof(struct NsWire) + comp_len;
    return msg;
}

static int kf_worker(void *data)
{
    struct NsKfJob *job = (struct NsKfJob *)data;
    const uint64_t t0 = SDL_GetTicksNS();
    job->msg = make_state_message(job->raw, job->raw_len, &job->msg_len);
    job->compress_ns = (int64_t)(SDL_GetTicksNS() - t0);
    free(job->raw);
    job->raw = NULL;
    SDL_SetAtomicInt(&job->done, 1);
    return 0;
}

static void held_drop(NetUserId c)
{
    while (kf_cl[c].held != NULL)
    {
        struct NsHeld *h = kf_cl[c].held;
        kf_cl[c].held = h->next;
        free(h);
    }
    kf_cl[c].held_tail = NULL;
}

static void held_add(NetUserId c, const uint8_t *buf, size_t len)
{
    struct NsHeld *h = (struct NsHeld *)malloc(sizeof(struct NsHeld) + len);
    if (h == NULL)
    {
        /* can't keep it: the client starts again from a keyframe */
        held_drop(c);
        numask_add(&keyframe_wanted, c);
        return;
    }
    h->next = NULL;
    h->len = len;
    memcpy(h->data, buf, len);
    if (kf_cl[c].held_tail != NULL)
        kf_cl[c].held_tail->next = h;
    else
        kf_cl[c].held = h;
    kf_cl[c].held_tail = h;
}

/** The keyframe record just made in `rec` for client c goes to a worker (the record buffer goes with it; the next
 *  record gets a new one). Without a worker it is compressed and sent here, as before. */
static void kf_start(NetUserId c, int64_t build_ns)
{
    kf_cl[c].gen++;
    held_drop(c);
    kf_cl[c].waiting = false;
    struct NsKfJob *job = NULL;
    for (int i = 0; (i < NS_KF_JOBS) && (job == NULL); i++)
        if (kf_jobs[i].thread == NULL)
            job = &kf_jobs[i];
    if (job != NULL)
    {
        job->client = c;
        job->gen = kf_cl[c].gen;
        job->turn = game.play_gameturn;
        job->raw = rec;
        job->raw_len = rec_len;
        job->msg = NULL;
        job->msg_len = 0;
        job->build_ns = build_ns;
        job->compress_ns = 0;
        SDL_SetAtomicInt(&job->done, 0);
        job->thread = SDL_CreateThread(kf_worker, "keyframe_compress", job);
        if (job->thread != NULL)
        {
            rec = NULL;
            rec_len = 0;
            rec_cap = 0;
            kf_cl[c].waiting = true;
            return;
        }
        job->raw = NULL;
    }
    send_record(rec, rec_len, c);
    SYNCLOG("Thin-client server: keyframe of turn %lu for user %d sent from the main thread (made in %.1f ms)",
        (unsigned long)game.play_gameturn, (int)c, (double)build_ns / 1e6);
}

/** Workers free for a keyframe now. */
static int kf_free_jobs(void)
{
    int n = 0;
    for (int i = 0; i < NS_KF_JOBS; i++)
        if (kf_jobs[i].thread == NULL)
            n++;
    return n;
}

/** Keyframes the workers have finished: each goes out, then its client's held records, in order. A keyframe whose
 *  client left, or that a newer one replaced, is dropped. */
static void kf_poll(NetUserMask clients)
{
    for (int i = 0; i < NS_KF_JOBS; i++)
    {
        struct NsKfJob *job = &kf_jobs[i];
        if ((job->thread == NULL) || !SDL_GetAtomicInt(&job->done))
            continue;
        SDL_WaitThread(job->thread, NULL);
        job->thread = NULL;
        const NetUserId c = job->client;
        if (numask_has(clients, c) && (job->gen == kf_cl[c].gen) && kf_cl[c].waiting)
        {
            kf_cl[c].waiting = false;
            if (job->msg != NULL)
            {
                send_state_message(job->msg, job->msg_len, c);
                int held = 0;
                for (struct NsHeld *h = kf_cl[c].held; h != NULL; h = h->next, held++)
                    send_record(h->data, h->len, c);
                SYNCLOG("Thin-client server: keyframe of turn %lu for user %d sent at turn %lu, then %d held turns "
                    "(made in %.1f ms, compressed in %.1f ms on a worker, %u bytes)", (unsigned long)job->turn, (int)c,
                    (unsigned long)game.play_gameturn, held, (double)job->build_ns / 1e6, (double)job->compress_ns / 1e6,
                    (unsigned)job->msg_len);
            } else
            {
                WARNLOG("Thin-client server: the keyframe for user %d couldn't be compressed; another one is made", (int)c);
                numask_add(&keyframe_wanted, c);
            }
            held_drop(c);
        }
        free(job->msg);
        job->msg = NULL;
    }
}

/** Level end: the workers finish, and everything waiting goes. */
static void kf_stop_all(void)
{
    for (int i = 0; i < NS_KF_JOBS; i++)
    {
        struct NsKfJob *job = &kf_jobs[i];
        if (job->thread != NULL)
            SDL_WaitThread(job->thread, NULL);
        job->thread = NULL;
        free(job->msg);
        job->msg = NULL;
        free(job->raw);
        job->raw = NULL;
    }
    for (NetUserId c = 0; c < MAX_NET_CONNECTIONS; c++)
    {
        held_drop(c);
        kf_cl[c].waiting = false;
        kf_cl[c].gen++;
    }
}

static void thing_head_w(struct NsWriter *w, const void *ctx)
{
    const uint8_t k = NsK_ThingLines;
    w_put(w, &k, 1);
    w_put(w, ctx, sizeof(uint16_t));
}

/** The bytes each client keeps for itself (its lists, its sound emitter): never compared, never sent. */
static void keep_client_bytes(struct Thing *dst, const struct Thing *src)
{
    dst->next_on_mapblk = src->next_on_mapblk;
    dst->prev_on_mapblk = src->prev_on_mapblk;
    dst->next_of_class = src->next_of_class;
    dst->prev_of_class = src->prev_of_class;
    dst->snd_emitter_id = src->snd_emitter_id;
    dst->last_turn_drawn = src->last_turn_drawn;
}

/** The changes of the things in a client's area (and of its own), against its copy; appended to `rec`. */
/** A client's copy of a thing that is gone, or not to be sent to it: dropped when it is gone from the client's area
 *  too (to the client the thing goes back to zero); else the client sees it go. */
static void client_thing_leaves(struct NsWriter *w, NetUserId c, const struct NsInterest *in, ThingIndex i, struct Thing *mine)
{
    static const struct Thing zero_thing;
    const uint16_t idx = (uint16_t)i;
    const TbBool me = (mine->alloc_flags & TAlF_Exists) != 0;
    if (!me || !thing_in_interest(in, mine))
    {
        struct Thing target = zero_thing;
        keep_client_bytes(&target, mine);
        diff_page_w(w, (uint8_t *)mine, (const uint8_t *)&target, sizeof(struct Thing), thing_head_w, &idx);
        cl_drop_copy(c, i);
        return;
    }
    const struct Thing *live = &game.things_data[i];
    keep_client_bytes(mine, live);
    diff_page_w(w, (uint8_t *)mine, (const uint8_t *)live, sizeof(struct Thing), thing_head_w, &idx);
}

static void diff_client_things(struct NsWriter *w, NetUserId c, const struct NsInterest *in)
{
    /* the live things (the compact list of the turn) */
    for (int n = 0; n < live_count; n++)
    {
        const struct NsLive *li = &live_list[n];
        const ThingIndex i = li->idx;
        struct Thing *mine = cl_copy(c, i);
        TbBool rel = in->all || (li->owner == in->plyr) || chunk_in_interest(in, li->cx, li->cy);
        if (!rel && li->limbo && (in->plyr >= 0))
            rel = thing_is_in_power_hand_list(&game.things_data[i], in->plyr);
        if (!rel)
        {
            if (mine != NULL)
                client_thing_leaves(w, c, in, i, mine);
            continue;
        }
        if (mine == NULL)
        {
            mine = cl_new_copy(c, i);
            if (mine == NULL)
            {
                w->failed = true;
                return;
            }
        } else if (thing_same[i])
        {
            continue; /* the same as at the last turn, when this copy was brought up to date */
        }
        const struct Thing *live = &game.things_data[i];
        const uint16_t idx = (uint16_t)i;
        keep_client_bytes(mine, live);
        diff_page_w(w, (uint8_t *)mine, (const uint8_t *)live, sizeof(struct Thing), thing_head_w, &idx);
    }
    /* the client's copies of things that are gone */
    const ThingIndex end = (ThingIndex)min((long)THINGS_COUNT, (long)ns_cl[c].hi + 1);
    for (ThingIndex i = 1; i < end; i++)
    {
        if ((ns_cl[c].slot[i] == 0) || (live_bits[i >> 3] & (1u << (i & 7))))
            continue;
        client_thing_leaves(w, c, in, i, cl_copy(c, i));
    }
}

static TbBool client_has_thing(NetUserId c, ThingIndex i)
{
    if ((ns_cl[c].slot == NULL) || (i <= 0) || (i >= THINGS_COUNT))
        return false;
    const struct Thing *mine = cl_copy(c, i);
    return (mine != NULL) && ((mine->alloc_flags & TAlF_Exists) != 0);
}


static TbBool server_alloc(void)
{
    for (int r = 0; r < NsR_Count; r++)
    {
        if ((regions[r].base == NULL) || (shadow[r] != NULL))
            continue;
        shadow[r] = (uint8_t *)malloc(regions[r].len);
        if (shadow[r] == NULL)
            return false;
        memcpy(shadow[r], regions[r].base, regions[r].len);
    }
    return true;
}

/** A keyframe for one client: the whole state, the pages of its area; its copy of the things is the whole. */
static void build_keyframe_for(NetUserId c)
{
    struct NsInterest in;
    interest_of(c, &in);
    if (!cl_init(c))
    {
        rec_failed = true;
        return;
    }
    rec_begin(NsF_Keyframe);
    size_t game_at = 0;
    for (int r = 0; r < NsR_Count; r++)
    {
        if (regions[r].base == NULL)
            continue;
        rec_u8(NsK_MemFull);
        rec_u8((uint8_t)r);
        rec_u32((uint32_t)regions[r].len);
        if (r == NsR_Game)
            game_at = rec_len;
        rec_put(regions[r].base, regions[r].len);
    }
    /* only the things relevant to it: the other slots go empty in the record; the ones sent get a copy kept here */
    if (!rec_failed && (regions[NsR_Game].base == (uint8_t *)&game))
    {
        for (ThingIndex i = 1; (i < THINGS_COUNT) && !rec_failed; i++)
        {
            const struct Thing *live = &game.things_data[i];
            if (((live->alloc_flags & TAlF_Exists) != 0) && thing_relevant(&in, live))
            {
                struct Thing *mine = cl_new_copy(c, i);
                if (mine == NULL)
                    rec_failed = true;
                else
                    *mine = *live;
                continue;
            }
            const size_t off = offsetof(struct Game, things_data) + (size_t)i * sizeof(struct Thing);
            memset(rec + game_at + off, 0, sizeof(struct Thing));
        }
        if (!rec_failed && sim_client(c) && (getenv("KFX_NET_SIMCHECK") != NULL) &&
            ((simcheck_client < 0) || (simcheck_client == c)))
        {
            if (simcheck_things == NULL)
                simcheck_things = (struct Thing *)malloc(sizeof(game.things_data));
            if (simcheck_things != NULL)
            {
                memcpy(simcheck_things, rec + game_at + offsetof(struct Game, things_data), sizeof(game.things_data));
                simcheck_client = c;
                SYNCLOG("Thin-client server: checking user %d's things against a client's", (int)c);
            }
        }
    }
    for (int b = 0; b < NS_PAGE_BUCKETS; b++)
    {
        for (struct NsPageShadow *ps = page_shadows[b]; ps != NULL; ps = ps->next)
        {
            numask_del(&ps->known, c);
            if (!chunk_in_interest(&in, ps->cx, ps->cy))
                continue;
            page_fresh(ps);
            rec_u8(NsK_PageAdd);
            rec_i32(ps->cx);
            rec_i32(ps->cy);
            rec_put(ps->data, NS_PG_SIZE);
            numask_add(&ps->known, c);
        }
    }
    rec_u8(NsK_PageSetEnd);
}

/******************************************************************************/
/* the clients' own parts of a turn on worker threads (KFX_NET_STREAM_THREADS=n; 0: all on the main thread). The main
   thread decides each client's area and its page lines (the page shadows are shared), the workers compare its things,
   add its events and compress, the main thread sends in client order. A client's record is the same whichever thread
   made it. */
struct NsClientJob {
    NetUserId c;
    struct NsInterest in;
    uint8_t *own;                   /* the client's part: page lines, thing lines, events, the end */
    size_t own_len, own_cap;
    uint8_t *z;                     /* own, compressed */
    size_t z_len, z_cap;
    TbBool failed;                  /* out of memory: the client gets a keyframe instead */
    TbBool held;                    /* its keyframe is being compressed: the record waits, raw */
    uint32_t things_bytes;
} __attribute__((aligned(64)));     /* a cache line or two each: the workers write them at the same time */
static struct NsClientJob cjobs[MAX_NET_CONNECTIONS];
static int cjob_count;
static SDL_AtomicInt cjob_next;
static SDL_Semaphore *cjob_go, *cjob_done;
static int cworker_count = -1;
/* each thread's writer for the thing lines it makes */
static __thread struct NsWriter tl_writer;

static void job_put(struct NsClientJob *j, const void *p, size_t n)
{
    if (j->failed || (n == 0))
        return;
    if (j->own_len + n > j->own_cap)
    {
        size_t cap = (j->own_cap > 0) ? j->own_cap : 65536;
        while (cap < j->own_len + n)
            cap *= 2;
        uint8_t *nb = (uint8_t *)realloc(j->own, cap);
        if (nb == NULL)
        {
            j->failed = true;
            return;
        }
        j->own = nb;
        j->own_cap = cap;
    }
    memcpy(j->own + j->own_len, p, n);
    j->own_len += n;
}

static void job_events(struct NsClientJob *j); /* below, after the event types */


/** A client's own part: its things, its events, the end; compressed unless the record waits for its keyframe. */
static void job_run(struct NsClientJob *j)
{
    /* the thing lines go to this thread's writer (the main thread's rec holds the turn's shared part) */
    struct NsWriter *w = &tl_writer;
    w->len = 0;
    w->failed = false;
    diff_client_things(w, j->c, &j->in);
    if ((j->c == simcheck_client) && (simcheck_things != NULL) && !w->failed)
    {
        simcheck_apply(w->buf, w->buf + w->len);
        simcheck_compare(j->c);
    }
    j->things_bytes = (uint32_t)w->len;
    if (w->failed)
    {
        j->failed = true;
        return;
    }
    job_put(j, w->buf, w->len);
    job_events(j);
    const uint8_t end = NsK_End;
    job_put(j, &end, 1);
    if (j->failed || j->held)
        return;
    uLongf comp_len = compressBound(j->own_len);
    if (j->z_cap < comp_len)
    {
        uint8_t *nz = (uint8_t *)realloc(j->z, comp_len);
        if (nz == NULL)
        {
            j->failed = true;
            return;
        }
        j->z = nz;
        j->z_cap = comp_len;
    }
    if (!ns_compress(j->z, &comp_len, j->own, j->own_len))
    {
        j->failed = true;
        return;
    }
    j->z_len = comp_len;
}

static int cworker_main(void *data)
{
    (void)data;
    for (;;)
    {
        SDL_WaitSemaphore(cjob_go);
        for (int k = SDL_AddAtomicInt(&cjob_next, 1); k < cjob_count; k = SDL_AddAtomicInt(&cjob_next, 1))
            job_run(&cjobs[k]);
        SDL_SignalSemaphore(cjob_done);
    }
    return 0;
}

static void cworkers_start(void)
{
    if (cworker_count >= 0)
        return;
    cworker_count = 0;
    const char *v = getenv("KFX_NET_STREAM_THREADS");
    int n = (v != NULL) ? atoi(v) : min(8, max(0, SDL_GetNumLogicalCPUCores() - 2));
    if (n > 16)
        n = 16;
    if (n <= 0)
        return;
    cjob_go = SDL_CreateSemaphore(0);
    cjob_done = SDL_CreateSemaphore(0);
    if ((cjob_go == NULL) || (cjob_done == NULL))
        return;
    for (int i = 0; i < n; i++)
    {
        SDL_Thread *t = SDL_CreateThread(cworker_main, "stream_client", NULL);
        if (t == NULL)
            break;
        SDL_DetachThread(t);
        cworker_count++;
    }
    SYNCLOG("Thin-client server: %d threads make the clients' records", cworker_count);
}

/** A client's record whose own part a job compressed: the shared part as compressed once, then that. */
static void send_job_record(const struct NsClientJob *j, size_t shared_end)
{
    if ((shared_z_raw == 0) || (shared_z_raw != shared_end))
    {
        /* the shared part wasn't compressed apart: the whole record, compressed here */
        crec_len = 0;
        crec_failed = false;
        crec_put(rec, shared_end);
        crec_put(j->own, j->own_len);
        if (!crec_failed)
            send_record(crec, crec_len, j->c);
        return;
    }
    const int64_t t0 = get_time_tick_ns();
    char *msg = (char *)malloc(1 + sizeof(struct NsWire) + shared_z_len + j->z_len);
    if (msg == NULL)
        return;
    msg[0] = NETMSG_STATE_TURN;
    struct NsWire w = {(uint32_t)(shared_z_raw + j->own_len), (uint32_t)(shared_z_len + j->z_len), (uint32_t)shared_z_raw,
        (uint32_t)shared_z_len};
    memcpy(msg + 1, &w, sizeof(w));
    memcpy(msg + 1 + sizeof(struct NsWire), shared_z, shared_z_len);
    memcpy(msg + 1 + sizeof(struct NsWire) + shared_z_len, j->z, j->z_len);
    const size_t total = 1 + sizeof(struct NsWire) + shared_z_len + j->z_len;
    if (!sim_client(j->c))
        send_to_active_peers(1, NetSend_Reliable, msg, total, numask_of(j->c));
    stats_bytes += (uint32_t)total;
    stats_records++;
    free(msg);
    stats_t[NsT_Send] += get_time_tick_ns() - t0;
}

/** Each streaming client's record of the turn: the shared part, the pages of its area, the things near it, the events
 *  it can hear. */
static void stream_clients(NetUserMask clients, size_t shared_end)
{
    cworkers_start();
    cjob_count = 0;
    for (NetUserId c = SERVER_ID + 1; c < MAX_NET_CONNECTIONS; c++)
    {
        if (!numask_has(clients, c) || numask_has(keyframe_wanted, c) || (ns_cl[c].slot == NULL))
            continue;
        struct NsClientJob *j = &cjobs[cjob_count++];
        j->c = c;
        interest_of(c, &j->in);
        j->own_len = 0;
        j->z_len = 0;
        j->failed = false;
        j->held = kf_cl[c].waiting;
        j->things_bytes = 0;
        for (int b = 0; b < NS_PAGE_BUCKETS; b++)
        {
            for (struct NsPageShadow *ps = page_shadows[b]; ps != NULL; ps = ps->next)
            {
                const TbBool want = chunk_in_interest(&j->in, ps->cx, ps->cy), has = numask_has(ps->known, c);
                if (want && has)
                {
                    if ((ps->diff_pass == page_pass) && (ps->diff_len > 0))
                        job_put(j, rec + ps->diff_off, ps->diff_len);
                } else if (want)
                {
                    page_fresh(ps);
                    const uint8_t k = NsK_PageAdd;
                    job_put(j, &k, 1);
                    job_put(j, &ps->cx, 4);
                    job_put(j, &ps->cy, 4);
                    job_put(j, ps->data, NS_PG_SIZE);
                    numask_add(&ps->known, c);
                } else if (has)
                {
                    const uint8_t k = NsK_PageFree;
                    job_put(j, &k, 1);
                    job_put(j, &ps->cx, 4);
                    job_put(j, &ps->cy, 4);
                    numask_del(&ps->known, c);
                }
            }
        }
        for (int g = 0; g < gone_count; g++)
        {
            if (!numask_has(gone_pages[g].known, c))
                continue;
            const uint8_t k = NsK_PageFree;
            job_put(j, &k, 1);
            job_put(j, &gone_pages[g].cx, 4);
            job_put(j, &gone_pages[g].cy, 4);
        }
    }
    /* the clients' things, events and compression */
    const int64_t t0 = get_time_tick_ns();
    SDL_SetAtomicInt(&cjob_next, 0);
    if ((cworker_count > 0) && (cjob_count > 1))
    {
        for (int i = 0; i < cworker_count; i++)
            SDL_SignalSemaphore(cjob_go);
        for (int i = 0; i < cworker_count; i++)
            SDL_WaitSemaphore(cjob_done);
    } else
    {
        for (int k = 0; k < cjob_count; k++)
            job_run(&cjobs[k]);
    }
    stats_t[NsT_Things] += get_time_tick_ns() - t0;
    /* sent in client order */
    for (int k = 0; k < cjob_count; k++)
    {
        struct NsClientJob *j = &cjobs[k];
        stats_part[NsP_Things] += j->things_bytes;
        if (j->failed)
        {
            WARNLOG("Thin-client server: no memory for user %d's record at turn %lu; it gets a keyframe", (int)j->c,
                (unsigned long)game.play_gameturn);
            numask_add(&keyframe_wanted, j->c);
            continue;
        }
        if (j->held)
        {
            crec_len = 0;
            crec_failed = false;
            crec_put(rec, shared_end);
            crec_put(j->own, j->own_len);
            if (!crec_failed)
                held_add(j->c, crec, crec_len); /* after its keyframe, which a worker is compressing */
            continue;
        }
        send_job_record(j, shared_end);
    }
}

void netstate_server_after_turn(void)
{
    if (!netstate_is_server())
        return;
    {
        /* the world list: the players seated and the seats there are; the hub, if this server runs one */
        int seated = 0;
        for (NetUserId u = 0; u < MAX_NET_USERS; u++)
            if (user_present(u))
                seated++;
        const PlayerNumber *seats = NULL;
        wnet_server_tick(seated, world_seat_players(&seats));
        wnet_hub_poll();
    }
    const int64_t t0 = get_time_tick_ns();
    const NetUserMask clients = client_mask();
    if (!server_started)
    {
        skips_setup();
        if (!server_alloc())
        {
            ERRORLOG("Thin-client server: out of memory for the shadow state");
            return;
        }
        server_started = true;
        SYNCLOG("Thin-client server started at turn %lu: %u bytes of game state, %u of statics",
            (unsigned long)game.play_gameturn, (unsigned)regions[NsR_Game].len, (unsigned)regions[NsR_Statics].len);
    }
    kf_poll(clients);
    /* a client that is new (it joined late, took a seat back, or reconnected) starts from the whole state */
    known_clients = numask_and(known_clients, clients);
    const NetUserMask new_clients = numask_andnot(clients, known_clients);
    for (NetUserId c = 0; c < MAX_NET_CONNECTIONS; c++)
        if (numask_has(new_clients, c))
            kf_next_turn[c] = 0; /* a new client in this slot: no waiting */
    keyframe_wanted = numask_or(keyframe_wanted, new_clients);
    known_clients = numask_or(known_clients, clients);
    /* clients that left: their copies go */
    for (NetUserId c = 0; c < MAX_NET_CONNECTIONS; c++)
    {
        if (!numask_has(clients, c) && (kf_cl[c].waiting || (kf_cl[c].held != NULL)))
        {
            held_drop(c);
            kf_cl[c].waiting = false;
            kf_cl[c].gen++;
        }
        if ((ns_cl[c].slot != NULL) && !numask_has(clients, c))
            cl_free(c);
    }
    /* the shared part of the turn: everything but the things, and the chunk pages' changes (kept per page) */
    rec_begin(0);
    int64_t tp = get_time_tick_ns();
    for (int r = 0; r < NsR_Count; r++)
        diff_region(r);
    const size_t shared_end = rec_len;
    int64_t tn = get_time_tick_ns();
    stats_t[NsT_Shared] += tn - tp;
    shared_z_raw = 0;
    if (!rec_failed && (shared_end > 0) && !compress_shared(rec, shared_end))
        shared_z_raw = 0; /* each record is compressed whole then */
    tp = tn;
    gone_count = 0;
    diff_pages();
    const size_t pages_end = rec_len;
    if (rec_failed)
    {
        /* the shadows took changes that can't be sent: every client starts again from a keyframe */
        WARNLOG("Thin-client server: the turn's changes couldn't be recorded (out of memory); keyframes for everyone");
        keyframe_wanted = numask_or(keyframe_wanted, clients);
    }
    stats_part[NsP_Pages] += pages_end - shared_end;
    tn = get_time_tick_ns();
    stats_t[NsT_Pages] += tn - tp;
    /* each streaming client: the shared part, the pages of its area, the things near it, the events it can hear */
    tp = get_time_tick_ns();
    hash_things();
    stats_t[NsT_Things] += get_time_tick_ns() - tp;
    if (!rec_failed)
        stream_clients(clients, shared_end);
    events_count = 0;
    events_len = 0;
    /* keyframes: the whole state as it is now, for the clients that asked */
    /* a keyframe is about 55 MB before compression (and as much again while a worker compresses it): at most
       NS_KF_PER_TURN start a turn, the others wait their turn; one that couldn't be made is tried again */
    const NetUserMask kf = numask_and(keyframe_wanted, clients);
    keyframe_wanted = numask_and(keyframe_wanted, clients);
    tp = get_time_tick_ns();
    int kf_started = 0;
    const int kf_room = min(NS_KF_PER_TURN, kf_free_jobs()); /* all workers busy: they wait (no main-thread hitch) */
    for (NetUserId c = SERVER_ID + 1; (c < MAX_NET_CONNECTIONS) && numask_any(kf) && (kf_started < kf_room); c++)
    {
        if (!numask_has(kf, c) || (game.play_gameturn < kf_next_turn[c]))
            continue; /* not asked, or it had one a moment ago: it waits (still wanted) */
        const int64_t kt0 = get_time_tick_ns();
        build_keyframe_for(c);
        rec_u8(NsK_End);
        kf_started++;
        if (rec_failed)
        {
            WARNLOG("Thin-client server: no memory for a keyframe for user %d at turn %lu; it is tried again", (int)c,
                (unsigned long)game.play_gameturn);
            break;
        }
        SYNCLOG("Thin-client server: keyframe at turn %lu for user %d, %u bytes before compression",
            (unsigned long)game.play_gameturn, (int)c, (unsigned)rec_len);
        kf_start(c, get_time_tick_ns() - kt0);
        numask_del(&keyframe_wanted, c);
        kf_next_turn[c] = game.play_gameturn + NS_KF_GAP;
    }
    tn = get_time_tick_ns();
    stats_t[NsT_Keyframes] += tn - tp;
    stats_ns += tn - t0;
    if ((game.play_gameturn % 200) == 0)
    {
        last_stream_ms = (double)stats_ns / 200e6;
        last_sim_ms = (double)stats_sim_ns / 200e6;
        SYNCLOG("Thin-client server: %u records, %u bytes in the last 200 turns (%u bytes a turn per client), %.2f ms a turn"
            " (%d clients, %d of them simulated)", (unsigned)stats_records, (unsigned)stats_bytes,
            (unsigned)(stats_bytes / max(1u, stats_records)), (double)stats_ns / 200e6, numask_count(clients),
            numask_count(numask_and(sim_mask, clients)));
        SYNCLOG("Thin-client server: raw bytes a turn by part: things %u, creature controls %u, other game %u, statics %u, "
            "pages %u, events %u", (unsigned)(stats_part[NsP_Things] / 200), (unsigned)(stats_part[NsP_Cctrl] / 200),
            (unsigned)(stats_part[NsP_Game] / 200), (unsigned)(stats_part[NsP_Statics] / 200),
            (unsigned)(stats_part[NsP_Pages] / 200), (unsigned)(stats_part[NsP_Events] / 200));
        SYNCLOG("Thin-client server: the simulation took %.2f ms a turn", (double)stats_sim_ns / 200e6);
        stats_sim_ns = 0;
        SYNCLOG("Thin-client server: ms a turn by part: shared diff %.2f (4 KB pages: %u compared, %u changed), chunk pages %.2f, "
            "client things %.2f, compress %.2f, send %.2f, keyframes %.2f",
            (double)stats_t[NsT_Shared] / 200e6, (unsigned)(stats_pages_compared / 200), (unsigned)(stats_pages_changed / 200),
            (double)stats_t[NsT_Pages] / 200e6, (double)stats_t[NsT_Things] / 200e6, (double)stats_t[NsT_Compress] / 200e6,
            (double)stats_t[NsT_Send] / 200e6,
            (double)stats_t[NsT_Keyframes] / 200e6);
        memset(stats_t, 0, sizeof(stats_t));
        stats_pages_compared = 0;
        stats_pages_changed = 0;
        memset(stats_part, 0, sizeof(stats_part));
        stats_bytes = 0;
        stats_records = 0;
        stats_ns = 0;
    }
}

void netstate_server_user_reset(NetUserId user)
{
    if ((user <= SERVER_ID) || (user >= MAX_NET_USERS))
        return;
    next_input_seq[user] = 0;
    memset(&last_input[user], 0, sizeof(last_input[user]));
    idle_turns[user] = 0;
    clear_user_packet_history(user);
    numask_del(&known_clients, user); /* a keyframe goes to the new occupant */
    kf_next_turn[user] = 0;
}

void netstate_server_request_keyframe(NetUserId user)
{
    if ((user > SERVER_ID) && (user < MAX_NET_CONNECTIONS))
        numask_add(&keyframe_wanted, user);
}

/******************************************************************************/
/* inputs (server) */
static TbBool packet_has_event(const struct Packet *p)
{
    return (p->action != PckA_None) ||
        ((p->control_flags & (PCtr_LBtnClick | PCtr_RBtnClick | PCtr_LBtnRelease | PCtr_RBtnRelease)) != 0);
}

void netstate_server_take_input(NetUserId user, struct Packet *out)
{
    const GameTurn turn = game.play_gameturn;
    if ((user < 0) || (user >= MAX_NET_USERS))
    {
        memset(out, 0, sizeof(*out));
        return;
    }
    if (!network_user_active(user))
    {
        /* a user who left: the packets the host makes for it (leaving the game) */
        const struct Packet *p = get_history_packet(user, turn);
        if (p != NULL)
            *out = *p;
        else
            memset(out, 0, sizeof(*out));
        out->turn = turn;
        return;
    }
    for (;;)
    {
        /* the oldest input not taken yet */
        const struct Packet *best = NULL;
        TbBool later = false;
        for (GameTurn t = next_input_seq[user]; t < next_input_seq[user] + PACKET_HISTORY_SIZE; t++)
        {
            const struct Packet *p = get_history_packet(user, t);
            if (p == NULL)
                continue;
            if (best == NULL)
                best = p;
            else
            {
                later = true;
                break;
            }
        }
        if (best == NULL)
        {
            /* nothing new arrived after a gap: an input number may have been skipped (a keyframe on the client
               restarts nothing, numbers only grow), so look further ahead once */
            const struct Packet *latest = get_latest_history_packet(user);
            if ((latest != NULL) && ((GameTurnDelta)(latest->turn - next_input_seq[user]) >= PACKET_HISTORY_SIZE))
            {
                next_input_seq[user] = latest->turn;
                continue;
            }
            break;
        }
        next_input_seq[user] = best->turn + 1;
        if (later && !packet_has_event(best))
            continue; /* superseded by a later input: a backlog drains */
        *out = *best;
        last_input[user] = *best;
        idle_turns[user] = 0;
        out->turn = turn;
        return;
    }
    /* nothing new: the user keeps still (movement goes on for half a second, to ride out jitter) */
    *out = last_input[user];
    out->action = PckA_None;
    out->actn_par1 = 0;
    out->actn_par2 = 0;
    out->control_flags &= ~(uint32_t)(PCtr_LBtnClick | PCtr_RBtnClick | PCtr_LBtnRelease | PCtr_RBtnRelease);
    if (++idle_turns[user] > 10)
    {
        out->control_flags &= ~(uint32_t)(PCtr_MoveUp | PCtr_MoveDown | PCtr_MoveLeft | PCtr_MoveRight |
            PCtr_ViewZoomIn | PCtr_ViewZoomOut | PCtr_ViewTiltUp | PCtr_ViewTiltDown | PCtr_ViewTiltReset |
            PCtr_Ascend | PCtr_Descend | PCtr_ViewZoomPos);
    }
    out->turn = turn;
    if (is_packet_empty(out))
        out->turn = turn; /* keep it non-empty for the turn's bookkeeping */
}

/******************************************************************************/
/* events (server) */
static void event_put(const void *p, size_t n)
{
    if ((events_count >= NS_MAX_EVENTS) || (events_len + n > sizeof(events_buf)))
        return;
    memcpy(events_buf + events_len, p, n);
    events_len += n;
}

static int events_suppressed;

enum NsEventKind {
    NsE_ThingSample = 1,
    NsE_PlayerMessage,
    NsE_RoomMessage,
    NsE_PlayerSample,
};

#pragma pack(1)
struct NsEvThingSample {
    uint8_t kind;
    uint16_t thing_idx;
    int16_t smptbl_idx;
    uint16_t pitch;
    int8_t fil1D;
    uint8_t ctype;
    uint8_t flags;
    int32_t priority;
    uint32_t loudness;
};
struct NsEvPlayerMessage {   /* also NsE_RoomMessage (msg_idx: room kind, delay: message kind) and NsE_PlayerSample */
    uint8_t kind;
    int8_t plyr_idx;
    int32_t msg_idx;
    int32_t delay;
};
#pragma pack()

/** The turn's events for one client, into its record. */
/** The turn's events this client can hear (the events a thing it has makes, and the players' ones). */
static void job_events(struct NsClientJob *j)
{
    if (events_count == 0)
        return;
    const size_t at = j->own_len;
    uint16_t n = 0;
    const uint8_t kind = NsK_Events;
    job_put(j, &kind, 1);
    job_put(j, &n, 2);
    for (size_t pos = 0; pos < events_len;)
    {
        const uint8_t k = events_buf[pos];
        const size_t len = (k == NsE_ThingSample) ? sizeof(struct NsEvThingSample) : sizeof(struct NsEvPlayerMessage);
        if (k == NsE_ThingSample)
        {
            uint16_t idx;
            memcpy(&idx, events_buf + pos + 1, 2);
            if (!client_has_thing(j->c, idx))
            {
                pos += len;
                continue;
            }
        }
        job_put(j, events_buf + pos, len);
        n++;
        pos += len;
    }
    if (j->failed)
        return;
    if (n == 0)
        j->own_len = at;
    else
        memcpy(j->own + at + 1, &n, 2);
}

void netstate_events_suppress(TbBool on)
{
    events_suppressed += on ? 1 : -1;
    if (events_suppressed < 0)
        events_suppressed = 0;
}

void netstate_event_thing_sample(ThingIndex thing_idx, short smptbl_idx, unsigned short pitch, char fil1D, unsigned char ctype,
    unsigned char flags, long priority, unsigned long loudness)
{
    if ((events_suppressed > 0) || !netstate_is_server() || (thing_idx <= 0) || (thing_idx >= THINGS_COUNT))
        return;
    struct NsEvThingSample e = {NsE_ThingSample, (uint16_t)thing_idx, smptbl_idx, pitch, (int8_t)fil1D, ctype, flags,
        (int32_t)priority, (uint32_t)loudness};
    event_put(&e, sizeof(e));
    events_count++;
}

/** A message for one player: recorded only when it is a client's (the host plays its own). */
static void event_for_player(uint8_t kind, PlayerNumber plyr_idx, long a, long b)
{
    if ((events_suppressed > 0) || !netstate_is_server() || (plyr_idx == my_player_number) || (plyr_idx < 0) ||
        (plyr_idx >= PLAYERS_COUNT))
        return;
    struct NsEvPlayerMessage e = {kind, (int8_t)plyr_idx, (int32_t)a, (int32_t)b};
    event_put(&e, sizeof(e));
    events_count++;
}

void netstate_event_player_message(PlayerNumber plyr_idx, long msg_idx, long delay)
{
    event_for_player(NsE_PlayerMessage, plyr_idx, msg_idx, delay);
}

void netstate_event_room_message(PlayerNumber plyr_idx, long rkind, long msg_kind)
{
    event_for_player(NsE_RoomMessage, plyr_idx, rkind, msg_kind);
}

void netstate_event_player_sample(PlayerNumber plyr_idx, long smptbl_idx)
{
    event_for_player(NsE_PlayerSample, plyr_idx, smptbl_idx, 0);
}

/******************************************************************************/
/* client */
TbError netstate_client_receive(const char *buffer, size_t size)
{
    if (size < sizeof(struct NsWire))
        return Lb_OK;
    struct NsWire w;
    memcpy(&w, buffer, sizeof(w));
    if ((w.comp_len != size - sizeof(w)) || (w.raw_len < sizeof(struct NsHeader)) || (w.raw_len > (256u << 20)) ||
        (w.shared_raw > w.raw_len) || (w.shared_comp > w.comp_len) || ((w.shared_raw == 0) != (w.shared_comp == 0)))
    {
        WARNLOG("Thin client: bad state record (%u bytes)", (unsigned)size);
        return Lb_OK;
    }
    uint8_t *raw = (uint8_t *)malloc(w.raw_len);
    if (raw == NULL)
        return Lb_OK;
    const Bytef *z = (const Bytef *)buffer + sizeof(w);
    uLongf shared_len = w.shared_raw, own_len = w.raw_len - w.shared_raw;
    TbBool ok = true;
    if (w.shared_raw > 0)
        ok = (uncompress(raw, &shared_len, z, w.shared_comp) == Z_OK) && (shared_len == w.shared_raw);
    if (ok)
        ok = (uncompress(raw + w.shared_raw, &own_len, z + w.shared_comp, w.comp_len - w.shared_comp) == Z_OK) &&
            (own_len == w.raw_len - w.shared_raw);
    if (!ok)
    {
        free(raw);
        WARNLOG("Thin client: a state record didn't decompress");
        return Lb_OK;
    }
    struct NsHeader h;
    memcpy(&h, raw, sizeof(h));
    if (h.magic != NS_MAGIC)
    {
        free(raw);
        return Lb_OK;
    }
    if ((h.flags & NsF_Keyframe) && network_user_is_spectator(netstate.my_id))
        network_spectator_resync_pending = true; /* ends a joining spectator's wait */
    if (h.flags & NsF_Keyframe)
    {
        /* everything before the keyframe is superseded */
        while (queue_count > 0)
        {
            free(queue[queue_head].data);
            queue_head = (queue_head + 1) % NS_QUEUE_MAX;
            queue_count--;
        }
        client_keyframe_asked = false;
    }
    if (queue_count >= NS_QUEUE_MAX)
    {
        free(raw);
        if (!client_keyframe_asked)
        {
            WARNLOG("Thin client: %d turns behind the server; asking for a keyframe", queue_count);
            network_spectator_request_resync();
            client_keyframe_asked = true;
        }
        return Lb_OK;
    }
    const int at = (queue_head + queue_count) % NS_QUEUE_MAX;
    queue[at].data = raw;
    queue[at].len = w.raw_len;
    queue_count++;
    return Lb_OK;
}

void netstate_client_await_keyframe(void)
{
    client_has_keyframe = false;
}

int netstate_client_queued(void)
{
    return queue_count;
}

/* reading a record */
struct NsReader {
    const uint8_t *p, *end;
    TbBool bad;
};

static TbBool rd_get(struct NsReader *r, void *dst, size_t n)
{
    if (r->bad || ((size_t)(r->end - r->p) < n))
    {
        r->bad = true;
        return false;
    }
    if (dst != NULL)
        memcpy(dst, r->p, n);
    r->p += n;
    return true;
}

/** Applies the lines of one page entry to `dst` (`len` bytes long from the page's start). */
static void apply_lines(struct NsReader *r, uint8_t *dst, size_t len)
{
    uint64_t line_mask;
    if (!rd_get(r, &line_mask, sizeof(line_mask)))
        return;
    for (int ln = 0; ln < (int)NS_LINES_PER_PAGE; ln++)
    {
        if (((line_mask >> ln) & 1u) == 0)
            continue;
        uint8_t word_mask;
        if (!rd_get(r, &word_mask, 1))
            return;
        for (int w = 0; w < 8; w++)
        {
            if (((word_mask >> w) & 1u) == 0)
                continue;
            uint8_t byte_mask;
            if (!rd_get(r, &byte_mask, 1))
                return;
            for (int b = 0; b < 8; b++)
            {
                if (((byte_mask >> b) & 1u) == 0)
                    continue;
                uint8_t v;
                if (!rd_get(r, &v, 1))
                    return;
                const size_t off = (size_t)ln * NS_LINE + (size_t)w * 8 + (size_t)b;
                if (off < len)
                    dst[off] = v;
            }
        }
    }
}


static void client_keyframe_mem(int region, const uint8_t *src, size_t len)
{
    if ((region < 0) || (region >= NsR_Count) || (regions[region].base == NULL) || (len != regions[region].len))
        return;
    if (region == NsR_Game)
    {
        /* this machine's own bytes stay */
        if (excl_save == NULL)
            excl_save = (uint8_t *)malloc(regions[region].len);
        if (excl_save == NULL)
            return;
        excl_copy(excl_save, regions[region].base);
        memcpy(regions[region].base, src, len);
        excl_copy(regions[region].base, excl_save);
    } else
        memcpy(regions[region].base, src, len);
}

static uint32_t keyframe_pass;

/* the chunk page a record is changing, gathered once */
static struct WorldPage *cache_pg;
static uint8_t cache_buf[NS_PG_SIZE];

static void cache_flush(void)
{
    if (cache_pg != NULL)
        page_scatter(cache_pg, cache_buf);
    cache_pg = NULL;
}

/** After the state changed under the client: its own indexes into the state are rebuilt. */
static void client_rebuild_local(void)
{
    /* chains of things on map blocks: from scratch, synced things from the server and this machine's effects */
    if (world_paged)
    {
        for (struct WorldPage *pg = wp_first(); pg != NULL; pg = pg->next)
        {
            for (int i = 0; i < WP_CHUNK_STL_CELLS; i++)
                pg->map[i].mapwho = 0;
        }
    } else
    {
        for (size_t i = 0; i < sizeof(game.map) / sizeof(game.map[0]); i++)
            game.map[i].mapwho = 0;
    }
    /* the thing lists: from the things this client has (the server sends only those near it) */
    static uint8_t in_list[THINGS_COUNT];
    for (ThingIndex i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        in_list[i] = thing_exists(thing) && ((thing->alloc_flags & TAlF_IsInStrucList) != 0);
        thing->alloc_flags &= ~TAlF_IsInStrucList;
        thing->next_of_class = 0;
        thing->prev_of_class = 0;
    }
    for (size_t l = 0; l < NS_THING_CLASS_LISTS; l++)
    {
        game.thing_lists[l].index = 0;
        game.thing_lists[l].count = 0;
    }
    for (ThingIndex i = THINGS_COUNT - 1; i > 0; i--)
    {
        if (in_list[i])
            add_thing_to_its_class_list(thing_get(i));
    }
    /* which things are on the map: the server's flag for synced things, this machine's for its own */
    static uint8_t on_map[THINGS_COUNT];
    for (ThingIndex i = 1; i < THINGS_COUNT; i++)
    {
        struct Thing *thing = thing_get(i);
        on_map[i] = thing_exists(thing) && ((thing->alloc_flags & TAlF_IsInMapWho) != 0);
        thing->next_on_mapblk = 0;
        thing->prev_on_mapblk = 0;
        thing->alloc_flags &= ~TAlF_IsInMapWho;
    }
    for (ThingIndex i = 1; i < THINGS_COUNT; i++)
    {
        if (on_map[i])
            place_thing_in_mapwho(thing_get(i));
    }
}

static void client_play_events(struct NsReader *r);

#define NS_TOUCHED_MAX 512
static int32_t touched_cx[NS_TOUCHED_MAX], touched_cy[NS_TOUCHED_MAX];
static int touched_count;
static TbBool touched_overflow;

static void touch_page(int32_t cx, int32_t cy)
{
    for (int i = touched_count - 1; i >= 0; i--)
    {
        if ((touched_cx[i] == cx) && (touched_cy[i] == cy))
            return;
    }
    if (touched_count >= NS_TOUCHED_MAX)
    {
        touched_overflow = true;
        return;
    }
    touched_cx[touched_count] = cx;
    touched_cy[touched_count] = cy;
    touched_count++;
}

/** Applies one record. */
static void client_apply(const uint8_t *data, size_t len)
{
    struct NsReader r = {data, data + len, false};
    struct NsHeader h;
    rd_get(&r, &h, sizeof(h));
    const TbBool keyframe = (h.flags & NsF_Keyframe) != 0;
    if (keyframe)
        keyframe_pass++;
    touched_count = 0;
    touched_overflow = false;
    static uint8_t tmp[NS_PG_SIZE];
    for (;;)
    {
        uint8_t kind;
        if (!rd_get(&r, &kind, 1) || (kind == NsK_End))
            break;
        switch (kind)
        {
        case NsK_MemLines:
        {
            uint8_t region;
            uint32_t page;
            rd_get(&r, &region, 1);
            rd_get(&r, &page, 4);
            if (r.bad || (region >= NsR_Count) || (regions[region].base == NULL) ||
                ((size_t)page * NS_PAGE >= regions[region].len))
            {
                r.bad = true;
                break;
            }
            const size_t off = (size_t)page * NS_PAGE;
            /* this machine's own bytes (the exclusions) stay, whatever the lines say */
            const TbBool keep_own = (region == NsR_Game) && (excl_save != NULL);
            const size_t n = min((size_t)NS_PAGE, regions[region].len - off);
            if (keep_own)
                excl_copy_page(excl_save, regions[region].base, off, n);
            apply_lines(&r, regions[region].base + off, regions[region].len - off);
            if (keep_own)
                excl_copy_page(regions[region].base, excl_save, off, n);
            break;
        }
        case NsK_MemFull:
        {
            uint8_t region;
            uint32_t n;
            rd_get(&r, &region, 1);
            rd_get(&r, &n, 4);
            if (r.bad || ((size_t)(r.end - r.p) < n))
            {
                r.bad = true;
                break;
            }
            client_keyframe_mem(region, r.p, n);
            r.p += n;
            break;
        }
        case NsK_PageAdd:
        {
            int32_t cx, cy;
            rd_get(&r, &cx, 4);
            rd_get(&r, &cy, 4);
            if (r.bad || ((size_t)(r.end - r.p) < NS_PG_SIZE))
            {
                r.bad = true;
                break;
            }
            cache_flush();
            struct WorldPage *pg = world_paged ? wp_create(cx, cy) : NULL;
            if (pg != NULL)
            {
                page_scatter(pg, r.p);
                pg->wanted_turn = keyframe_pass; /* marks pages named by this keyframe */
                touch_page(cx, cy);
            }
            r.p += NS_PG_SIZE;
            break;
        }
        case NsK_PageLines:
        {
            int32_t cx, cy;
            uint32_t page;
            rd_get(&r, &cx, 4);
            rd_get(&r, &cy, 4);
            rd_get(&r, &page, 4);
            if (r.bad || ((size_t)page * NS_PAGE >= NS_PG_SIZE))
            {
                r.bad = true;
                break;
            }
            struct WorldPage *pg = world_paged ? wp_page(cx, cy) : NULL;
            if (pg == NULL)
            {
                /* a page this client doesn't have: read past the lines */
                apply_lines(&r, tmp, 0);
                break;
            }
            if (cache_pg != pg)
            {
                cache_flush();
                page_gather(pg, cache_buf);
                cache_pg = pg;
            }
            const size_t off = (size_t)page * NS_PAGE;
            apply_lines(&r, cache_buf + off, NS_PG_SIZE - off);
            touch_page(cx, cy);
            break;
        }
        case NsK_PageFree:
        {
            int32_t cx, cy;
            rd_get(&r, &cx, 4);
            rd_get(&r, &cy, 4);
            cache_flush();
            if (!r.bad && world_paged)
                wp_free(cx, cy);
            break;
        }
        case NsK_PageSetEnd:
        {
            cache_flush();
            if (!world_paged)
                break;
            /* the keyframe named every page the server has: the others go */
            for (struct WorldPage *pg = wp_first(); pg != NULL;)
            {
                struct WorldPage *next = pg->next;
                if (pg->wanted_turn != keyframe_pass)
                    wp_free(pg->cx, pg->cy);
                pg = next;
            }
            break;
        }
        case NsK_Events:
            client_play_events(&r);
            break;
        case NsK_ThingLines:
        {
            uint16_t idx;
            rd_get(&r, &idx, 2);
            if (r.bad || (idx == 0) || (idx >= THINGS_COUNT))
            {
                r.bad = true;
                break;
            }
            apply_lines(&r, (uint8_t *)&game.things_data[idx], sizeof(struct Thing));
            break;
        }
        default:
            r.bad = true;
            break;
        }
        if (r.bad)
            break;
    }
    cache_flush();
    if (r.bad)
    {
        WARNLOG("Thin client: a broken record at turn %lu; asking for a keyframe", (unsigned long)h.turn);
        if (!client_keyframe_asked)
        {
            network_spectator_request_resync();
            client_keyframe_asked = true;
        }
    }
    if (keyframe)
    {
        client_has_keyframe = true;
        SYNCLOG("Thin client: keyframe applied at turn %lu", (unsigned long)h.turn);
    }
    client_rebuild_local();
    {
        static struct DungeonCamera last_cam;
        static TbBool last_cam_valid = false;
        const struct DungeonCamera *cam = &get_user_state(get_my_user())->dungeon_camera;
        const long jump = labs((long)cam->x - last_cam.x) + labs((long)cam->y - last_cam.y);
        if (!keyframe && last_cam_valid && (jump > 6 * STL_PER_SLB * COORD_PER_STL))
            sync_local_camera_pose(get_my_user(), cam);
        last_cam = *cam;
        last_cam_valid = true;
    }
    if (keyframe || touched_overflow)
    {
        /* minimap colours and codes of everything; the view goes where the server has this player's camera */
        rederive_local_state();
        if (keyframe)
        {
            network_spectator_finish_seat(); /* a spectator just given a seat: the player's cameras and panel */
            sync_local_camera_pose(get_my_user(), &get_user_state(get_my_user())->dungeon_camera);
        }
    } else
    {
        for (int i = 0; i < touched_count; i++)
            panel_map_update((long)touched_cx[i] * WP_CHUNK_STL, (long)touched_cy[i] * WP_CHUNK_STL, WP_CHUNK_STL, WP_CHUNK_STL);
    }
}

TbBool netstate_client_apply_next(void)
{
    if (queue_count == 0)
        return false;
    struct NsQueued *q = &queue[queue_head];
    struct NsHeader h;
    memcpy(&h, q->data, sizeof(h));
    if (client_has_keyframe || (h.flags & NsF_Keyframe))
        client_apply(q->data, q->len);
    set_flag_value(game.operation_flags, GOF_SingleLevel, client_single_level);
    client_last_turn_ms = LbTimerClock();
    free(q->data);
    q->data = NULL;
    queue_head = (queue_head + 1) % NS_QUEUE_MAX;
    queue_count--;
    return client_has_keyframe;
}

/** The input this thin client sent last (NULL before the first). Its own presentation (the camera, the dig marks)
 *  reads this where lockstep play reads the packet history: a thin client keeps that history by input number. */
const struct Packet *netstate_client_last_input(void)
{
    return client_last_input_valid ? &client_last_input : NULL;
}

/** No turn from the server for a while: say so (the connection times out after 60 s, or comes back). */
void netstate_client_note_stall(void)
{
    static TbClockMSec shown = 0;
    if (!client_has_keyframe || (client_last_turn_ms == 0))
        return;
    const TbClockMSec now = LbTimerClock();
    if ((now - client_last_turn_ms < 3000) || (now - shown < 2000))
        return;
    shown = now;
    show_onscreen_msg(2 * turns_per_second, "Waiting for the server...");
}

void netstate_client_send_input(void)
{
    if (!netstate_is_client() || network_user_is_spectator(netstate.my_id))
        return;
    /* nothing before the first keyframe: this machine's camera isn't where the server has the player yet, and the
       server would move the player's camera to it */
    if (!client_has_keyframe)
        return;
    struct Packet *pckt = get_local_packet();
    pckt->turn = ++input_seq;
    store_packet_history(netstate.my_id, pckt);
    client_last_input = *pckt;
    client_last_input_valid = true;
    if (exchange_frame_message(pckt, game.packets, sizeof(struct Packet), NETMSG_GAMEPLAY_UNSEQUENCED) != Lb_OK)
        WARNLOG("Thin client: input not sent");
}

/******************************************************************************/
/* events (client) */
static void client_play_events(struct NsReader *r)
{
    uint16_t n;
    if (!rd_get(r, &n, 2))
        return;
    for (uint16_t i = 0; (i < n) && !r->bad; i++)
    {
        uint8_t kind;
        if (!rd_get(r, &kind, 1))
            return;
        r->p--; /* each event starts with its kind */
        switch (kind)
        {
        case NsE_ThingSample:
        {
            struct NsEvThingSample e;
            if (!rd_get(r, &e, sizeof(e)))
                return;
            struct Thing *thing = thing_get(e.thing_idx);
            if (thing_exists(thing))
                thing_play_sample(thing, e.smptbl_idx, e.pitch, e.fil1D, e.ctype, e.flags, e.priority, e.loudness);
            break;
        }
        case NsE_PlayerMessage:
        case NsE_RoomMessage:
        case NsE_PlayerSample:
        {
            struct NsEvPlayerMessage e;
            if (!rd_get(r, &e, sizeof(e)))
                return;
            if (e.plyr_idx != my_player_number)
                break;
            if (kind == NsE_PlayerMessage)
                output_message(e.msg_idx, e.delay);
            else if (kind == NsE_RoomMessage)
                output_room_message(e.plyr_idx, (RoomKind)e.msg_idx, (OutputMessageKind)e.delay);
            else
                play_non_3d_sample((SoundSmplTblID)e.msg_idx);
            break;
        }
        default:
            r->bad = true;
            return;
        }
    }
}

/******************************************************************************/
/* KFX_NET_SIMCHECK */
static void simcheck_apply(const uint8_t *from, const uint8_t *to)
{
    struct NsReader r = {from, to, false};
    while ((r.p < r.end) && !r.bad)
    {
        uint8_t kind;
        uint16_t idx;
        if (!rd_get(&r, &kind, 1) || (kind != NsK_ThingLines) || !rd_get(&r, &idx, 2) || (idx == 0) || (idx >= THINGS_COUNT))
        {
            r.bad = true;
            break;
        }
        apply_lines(&r, (uint8_t *)&simcheck_things[idx], sizeof(struct Thing));
    }
    if (r.bad)
        simcheck_bad += 1000000;
}

static void simcheck_compare(NetUserId c)
{
    static const struct Thing zero_thing;
    for (ThingIndex i = 1; i < THINGS_COUNT; i++)
    {
        const struct Thing *mine = cl_copy(c, i);
        struct Thing want = (mine != NULL) ? *mine : zero_thing;
        keep_client_bytes(&want, &simcheck_things[i]);
        if (memcmp(&want, &simcheck_things[i], sizeof(struct Thing)) != 0)
            simcheck_bad++;
    }
    simcheck_turns++;
    if ((simcheck_turns % 200) == 0)
    {
        SYNCLOG("Thin-client server: user %d's things against a client's: %u mismatches in %u turns (%d copies kept, %u dropped by all clients)",
            (int)c, (unsigned)simcheck_bad, (unsigned)simcheck_turns, ns_cl[c].used - ns_cl[c].nfree, (unsigned)stats_dropped_copies);
    }
}

/******************************************************************************/
void netstate_level_start(void)
{
    client_last_input_valid = false;
    client_last_turn_ms = 0;
    client_single_level = flag_is_set(game.operation_flags, GOF_SingleLevel);
    regions[NsR_Game].base = (uint8_t *)&game;
    regions[NsR_Game].len = sizeof(game);
    unsigned char *st = NULL;
    size_t st_len = 0;
    replay_rewind_tracked_statics(&st, &st_len);
    regions[NsR_Statics].base = st;
    regions[NsR_Statics].len = st_len;
    excl_setup();
    netstate_take_all_users();
    /* a thin client that stops for a while (a dragged window, a hang) holds nobody up, so it is kept for a minute */
    if (network_is_active() && (getenv("KFX_NET_LOCKSTEP") == NULL))
        LbNetwork_SetPeerTimeout(60000);
    server_started = false;
    keyframe_wanted = numask_none();
    known_clients = numask_none();
    sim_mask = numask_none();
    memset(next_input_seq, 0, sizeof(next_input_seq));
    memset(last_input, 0, sizeof(last_input));
    memset(idle_turns, 0, sizeof(idle_turns));
    events_count = 0;
    events_len = 0;
    client_has_keyframe = false;
    client_keyframe_asked = false;
}

void netstate_level_end(void)
{
    for (int r = 0; r < NsR_Count; r++)
    {
        free(shadow[r]);
        shadow[r] = NULL;
    }
    free_page_shadows();
    for (NetUserId c = 0; c < MAX_NET_CONNECTIONS; c++)
        cl_free(c);
    while (queue_count > 0)
    {
        free(queue[queue_head].data);
        queue_head = (queue_head + 1) % NS_QUEUE_MAX;
        queue_count--;
    }
    queue_head = 0;
    kf_stop_all();
    wnet_server_end();
    wnet_hub_stop();
    free(excl_save);
    excl_save = NULL;
    server_started = false;
    client_has_keyframe = false;
    LbNetwork_SetPeerTimeout(PEER_TIMEOUT_MS);
}

#ifdef __cplusplus
}
#endif
