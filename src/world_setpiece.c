/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_setpiece.c
 *     Endless world: set pieces, whole maps placed at random in a generated world (see world_setpiece.h).
 * @par Purpose:
 *     The user, 2026-11-25: "a player could create a map of any size in the standard of the original game and place
 *     them into Endless to spawn randomly. Scripts and everything. The biomes options are fine. Needs a chance or
 *     weight input." Special boxes, spellbooks, spells and traps that endless worlds turn off stay off ("Drop them");
 *     enemies are graded like the rest of the world; pieces come only in generator version 9 worlds ("v9"); the maps
 *     blend in, "so no unnatural edges". (This replaces the 2026-11-13 31x31 piece loader, never merged.)
 *
 *     A piece is a folder levels/endless_cfgs/setpieces/<folder>/ holding one map's files (any name: the first .slb
 *     found gives the others' name; .slb and .own needed; .lof/.lif MAPSIZE, .inf, .slx, .tngfx or .tng, .clm + .dat
 *     read when there) and setpiece.cfg:
 *       NAME <word>                  default: the folder's name
 *       BIOMES all | <biome> ...     worldgen.py's names (earthen, rocky, volcanic, ...); default all
 *       WEIGHT <n>                   how often it is picked against the other pieces; default 10
 *       CHANCE <percent>             how often it then goes down when picked (else the region stays empty); 100
 *       CIRCLES <from> <to>          1-9; default 1 9
 *     The creator's own pieces are in setpieces/official/<folder>/, read only with keeperfx.cfg OFFICIAL_SET_PIECES.
 *
 *     What becomes of a map:
 *     - Blending: the map's outer slab, and all of the plain rock and earth joined to it, is the land's own (the
 *       world's ground shows there); what is left is the piece. Round the piece, in a ring 2-5 slabs wide whose width
 *       wanders with the place, the land's rock becomes earth, so it can be dug to and its outline is the map's own.
 *     - Players: each player 1-7 with a dungeon heart is a keeper of the world's (the endless keepers' brain,
 *       world_keeper_ai.c, and the world's rules for them); player 0 is whoever comes: its ground, rooms and
 *       creatures are nobody's (its heart is left as floor); a player without a heart is nobody's; the heroes stay
 *       heroes. The heroes and keepers' creatures are graded by the circle they land in (world_stream.c's
 *       circles_grade_chunk runs after the pieces).
 *     - Endless rules: special boxes endless worlds don't have are left out (only Steal Hero, Multiply, Increase
 *       Level, Make Safe and Heal All stay); a spellbook of a power that stays off is left out, any other becomes an
 *       endless research book; rooms that stay off become claimed floor; hearts and their flames are made by their
 *       slabs; effect generators are left out (a world's chunk keeps none).
 *     - Columns, bridges and lights: a map's own columns (.clm/.dat), the liquid under its bridges (.wlb) and its static
 *       lights (.lgtfx/.lgt; dynamic ones are left out) go back on its slabs whenever they are put on the engine map
 *       and are still as the piece made them (world_setpiece_columns_box). Each light is attached to its slab, so a
 *       chunk that freezes, or a slab placed again (dug, claimed, built on), takes its light with it, as a torch's.
 *     The world is cut into SP_REGION-slab regions; SP_REGION_CHANCE% of them may hold a piece, at the first of
 *     SP_SITES seeded places where the piece picked (by WEIGHT among those its biome and circle allow, then its
 *     CHANCE) fits clear of the generator's keepers (rival pairs too) and fortresses, the start and its copies, and
 *     only on ground never made. Portals and reward caves under a piece give way to it; the heroes outposts keep off
 *     the pieces (decided before them). A region is decided the first time it is asked about; the decision (the piece's name and
 *     the hash of its files, its place) is saved: XTR2 record "SPC2". A piece edited or removed afterwards builds
 *     nothing more of itself.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_setpiece.h"
#include "world_setpiece_script.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>
#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "world_outpost.h"
#include "world_stream.h"
#include "world_gen.h"
#include "world_circles.h"
#include "world_progress.h"
#include "bflib_basics.h"
#include "bflib_fileio.h"
#include "config.h"
#include "config_creature.h"
#include "config_objects.h"
#include "config_terrain.h"
#include "config_trapdoor.h"
#include "config_magic.h"
#include "frontmenu_ingame_map.h"
#include "game_legacy.h"
#include "light_data.h"
#include "map_blocks.h"
#include "map_columns.h"
#include "map_data.h"
#include "net_state.h"
#include "player_data.h"
#include "player_instances.h"
#include "room_data.h"
#include "slab_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_objects.h"
#include "value_util.h"
#include <toml.h>
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define SP_MAX_SIDE 170          /* KeeperFX's largest map */
#define SP_MAX_PIECES 128
#define SP_MAX_THINGS 4096
#define SP_REGION 320
#define SP_REGION_CHANCE 40
#define SP_SITES 4
#define SP_FRINGE 5              /* the widest blending ring */
#define SP_CLEAR 6               /* room kept round the ring (to the generator's small structures) */
#define SP_KEEPER_CLEAR 24       /* to a generator keeper (and the rival pairs' battlefields) */
#define SP_FORT_CLEAR 26
#define SP_START_DIST 160        /* the piece's nearest slab to the world's start */
#define SP_MAX_KEEPERS 7
#define SP_DEFAULT_MONEY 3000
#define SP_MAX_LIGHTS 512

TbBool world_official_setpieces = false;

struct SpThing {
    int32_t x, y;               /* map coordinates from the piece's top left */
    uint8_t oclass, owner, level;   /* owner: the map's player number; level 1..10 */
    uint16_t model;
    uint16_t extra;
    uint32_t gold;
};

struct SpLight {
    int32_t x, y, z;            /* map coordinates from the piece's top left */
    uint16_t radius;
    uint8_t intensity, flags;
};

struct SpKeeper {
    uint8_t player;             /* the map's player number */
    int hx, hy;                 /* heart, piece slabs */
    int bx1, by1, bx2, by2;     /* its owned slabs */
};

struct SpPiece {
    char name[SETPIECE_NAME_LEN];
    char where[160];            /* the folder, for the log */
    uint32_t hash;
    int w, h;
    uint8_t *kind;              /* w*h engine slab kinds (after the endless rules) */
    uint8_t *owner;             /* w*h: SPO_Neutral, SPO_Heroes or SPO_Keeper0 + piece keeper index */
    uint8_t *tex;               /* w*h texture packs */
    uint8_t *door;              /* w*h door models (kind is claimed floor) */
    uint8_t *wlb;               /* w*h what is under a bridge (.wlb), or NULL */
    uint8_t *mask;              /* w*h: 1 the piece's, 0 the land's own */
    uint8_t *dist;              /* (w+2F)*(h+2F): thirds of a slab to the nearest piece slab (255: far) */
    uint16_t *dat;              /* (3w+1)*(3h+1) column index into clm, or NULL */
    struct Column *clm;
    int nclm;
    int nthings;
    struct SpThing *things;
    int nlights;
    struct SpLight *lights;     /* its static lights (on its own slabs) */
    int nkeepers;
    struct SpKeeper keepers[SP_MAX_KEEPERS];
    int cx1, cy1, cx2, cy2;     /* the piece slabs' box */
    struct SpScript *script;    /* its map script (world_setpiece_script.c), or NULL */
    int naps, ngates;
    struct SpScriptPoint aps[64], gates[16];
    uint32_t biomes;            /* bit per biome; 0: all */
    int cmin, cmax, weight, chance;
};

static struct SpPiece *sp_pieces;
static int sp_count;
static TbBool sp_loaded;

struct SpState {
    int64_t rx, ry;
    uint32_t piece;             /* its hash; 0: none */
    int64_t x0, y0;             /* world slab of the map's top left */
    struct SpScriptRun *run;    /* its script's state (once it ran) */
};
static struct SpState *sp_states;
static int sp_state_count, sp_state_cap;

/******************************************************************************/
static uint64_t sp_mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

struct SpRng {
    uint64_t s;
};

static int sp_rand(struct SpRng *r, int n)
{
    r->s = sp_mix(r->s);
    return (n > 0) ? (int)(r->s % (uint64_t)n) : 0;
}

static int64_t sp_floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if (((a % b) != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

static uint32_t sp_fnv(uint32_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static TbBool sp_active(void)
{
    struct WgWorld *gen = world_generator();
    return (gen != NULL) && (wg_world_version(gen) >= WORLD_SETPIECES_FROM_VERSION);
}

/******************************************************************************/
/* files */
static uint8_t *sp_read(const char *path, size_t *len)
{
    *len = 0;
    FILE *fh = fopen(path, "rb");
    if (fh == NULL)
        return NULL;
    fseek(fh, 0, SEEK_END);
    long n = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if ((n < 0) || (n > 64L * 1024 * 1024))
    {
        fclose(fh);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)n + 1);
    if (buf == NULL)
    {
        fclose(fh);
        return NULL;
    }
    *len = fread(buf, 1, (size_t)n, fh);
    fclose(fh);
    buf[*len] = 0;
    return buf;
}

static int sp_name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/** The folders (or with want_dirs false, the files ending in ext) in dir, sorted by name; returns how many. */
static int sp_list(const char *dir, TbBool want_dirs, const char *ext, char **names, int max)
{
    int n = 0;
#ifdef _WIN32
    char pat[400];
    snprintf(pat, sizeof(pat), "%s/*", dir);
    struct _finddata_t fd;
    const intptr_t hf = _findfirst(pat, &fd);
    if (hf == -1)
        return 0;
    do {
        const TbBool is_dir = (fd.attrib & _A_SUBDIR) != 0;
        const char *nm = fd.name;
#else
    DIR *dh = opendir(dir);
    if (dh == NULL)
        return 0;
    struct dirent *de;
    while ((de = readdir(dh)) != NULL)
    {
        const char *nm = de->d_name;
        char full[600];
        snprintf(full, sizeof(full), "%s/%s", dir, nm);
        struct stat stt;
        const TbBool is_dir = (stat(full, &stt) == 0) && S_ISDIR(stt.st_mode);
#endif
        if ((nm[0] == '.') || (is_dir != want_dirs))
            continue;
        if (!want_dirs)
        {
            const size_t ln = strlen(nm), le = strlen(ext);
            if ((ln <= le) || (strcasecmp(nm + ln - le, ext) != 0))
                continue;
        }
        if (n < max)
        {
            names[n] = strdup(nm);
            if (names[n] != NULL)
                n++;
        }
#ifdef _WIN32
    } while (_findnext(hf, &fd) == 0);
    _findclose(hf);
#else
    }
    closedir(dh);
#endif
    qsort(names, (size_t)n, sizeof(names[0]), sp_name_cmp);
    return n;
}

/******************************************************************************/
/* setpiece.cfg */
static void sp_parse_cfg(const char *text, const char *where, struct SpPiece *p)
{
    const char *s = text;
    int line_no = 0;
    while ((s != NULL) && *s)
    {
        const char *e = strchr(s, '\n');
        size_t n = (e != NULL) ? (size_t)(e - s) : strlen(s);
        char line[256];
        if (n >= sizeof(line))
            n = sizeof(line) - 1;
        memcpy(line, s, n);
        line[n] = 0;
        s = (e != NULL) ? e + 1 : NULL;
        line_no++;
        char *c = strchr(line, ';');
        if (c != NULL)
            *c = 0;
        for (char *q = line; *q; q++)
            if ((*q == '=') || (*q == '\r') || (*q == '\t'))
                *q = ' ';
        char word[32] = "", a[64] = "", b[64] = "";
        const int got = sscanf(line, "%31s %63s %63s", word, a, b);
        if (got <= 0)
            continue;
        if (strcasecmp(word, "NAME") == 0)
            snprintf(p->name, sizeof(p->name), "%s", a);
        else if (strcasecmp(word, "WEIGHT") == 0)
            p->weight = max(0, atoi(a));
        else if (strcasecmp(word, "CHANCE") == 0)
            p->chance = max(0, min(100, atoi(a)));
        else if (strcasecmp(word, "CIRCLES") == 0)
        {
            p->cmin = max(1, atoi(a));
            p->cmax = (got >= 3) ? min(9, atoi(b)) : p->cmin;
        }
        else if (strcasecmp(word, "BIOMES") == 0)
        {
            const char *q = line;
            while (*q == ' ')
                q++;
            q += strlen(word);
            char nm[32];
            int used;
            p->biomes = 0;
            while (sscanf(q, "%31s%n", nm, &used) == 1)
            {
                q += used;
                if (strcasecmp(nm, "all") == 0)
                {
                    p->biomes = 0;
                    break;
                }
                TbBool known = false;
                for (int bi = 0; bi < WgB_COUNT; bi++)
                    if (strcasecmp(nm, wg_biome_name(bi)) == 0)
                    {
                        p->biomes |= 1u << bi;
                        known = true;
                    }
                if (!known)
                    WARNLOG("World: set piece %s, setpiece.cfg line %d: unknown biome %s", where, line_no, nm);
            }
        }
        else
            WARNLOG("World: set piece %s, setpiece.cfg line %d: unknown line \"%s\"", where, line_no, word);
    }
}

/** MAPSIZE from a .lof or .lif ("MAPSIZE = 120 80"); false when none. */
static TbBool sp_mapsize(const char *text, int *w, int *h)
{
    const char *s = text;
    while ((s != NULL) && *s)
    {
        while ((*s == ' ') || (*s == '\t'))
            s++;
        if (strncasecmp(s, "MAPSIZE", 7) == 0)
        {
            const char *q = s + 7;
            while ((*q == ' ') || (*q == '\t') || (*q == '='))
                q++;
            int a = 0, b = 0;
            if (sscanf(q, "%d %d", &a, &b) >= 1)
            {
                *w = a;
                *h = (b > 0) ? b : a;
                return true;
            }
        }
        s = strchr(s, '\n');
        if (s != NULL)
            s++;
    }
    return false;
}

/******************************************************************************/
/* the endless rules for a map's things and slabs */
static TbBool sp_box_allowed(ThingModel model)
{
    static const char *const allowed[] = {"SPECBOX_STEALHR", "SPECBOX_MULTPLY", "SPECBOX_INCLEV", "SPECBOX_MKSAFE",
        "SPECBOX_HEALALL"};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        if (get_id(object_desc, allowed[i]) == (long)model)
            return true;
    return false;
}

static TbBool sp_is_heart_object(ThingModel model)
{
    const char *nm = object_code_name(model);
    return (model == ObjMdl_SoulCountainer) || ((nm != NULL) && (strncasecmp(nm, "HEARTFLAME", 10) == 0));
}

/** What an object of the map becomes in an endless world: its model, or 0 to leave it out. */
static ThingModel sp_object_rule(ThingModel model, int *dropped_box, int *dropped_book, int *turned_book)
{
    if ((model <= 0) || (model >= game.conf.object_conf.object_types_count))
        return 0;
    if (sp_is_heart_object(model))
        return 0;
    const struct ObjectConfigStats *objst = get_object_model_stats(model);
    if (objst->genre == OCtg_LairTotem)
        return 0; /* a creature makes its own lair (one from the map has no creature that owns it) */
    if (objst->genre == OCtg_SpecialBox)
    {
        if (sp_box_allowed(model))
            return model;
        (*dropped_box)++;
        return 0;
    }
    if (objst->genre == OCtg_Spellbook)
    {
        const ThingModel book = world_progress_book_model();
        if ((book > 0) && (model == book))
            return model;
        const PowerKind pw = game.conf.object_conf.object_to_power_artifact[model];
        if ((pw > 0) && world_progress_power_banned(pw))
        {
            (*dropped_book)++;
            return 0;
        }
        if (world_progress_active() && (book > 0))
        {
            (*turned_book)++;
            return book;
        }
        return model;
    }
    return model;
}

static SlabKind sp_slab_rule(SlabKind kind)
{
    const RoomKind rk = slab_corresponding_room(kind);
    if ((rk > 0) && world_progress_room_banned(rk))
        return SlbT_CLAIMED;
    if (kind == SlbT_SCAVENGER_WALL)
        return SlbT_WALLDRAPE;
    return kind;
}

/******************************************************************************/
/* reading a map */
static TbBool sp_read_things_tng(struct SpPiece *p, const uint8_t *buf, size_t len, const uint8_t *own_of, int *bad)
{
    if (len < 2)
        return false;
    const unsigned total = (unsigned)(buf[0] | (buf[1] << 8));
    const size_t rec = 21;
    for (unsigned k = 0; (k < total) && (2 + (k + 1) * rec <= len); k++)
    {
        const uint8_t *r = buf + 2 + k * rec;
        if (p->nthings >= SP_MAX_THINGS)
        {
            (*bad)++;
            break;
        }
        struct SpThing *t = &p->things[p->nthings];
        memset(t, 0, sizeof(*t));
        t->x = r[0] | (r[1] << 8);
        t->y = r[2] | (r[3] << 8);
        t->oclass = r[6];
        t->model = r[7];
        t->owner = r[8];
        const uint8_t *params = r + 13;
        if (t->oclass == TCls_Creature)
            t->level = (uint8_t)max(1, min(10, (int)params[1] + 1));
        else if (t->oclass == TCls_Door)
            t->extra = params[1]; /* locked */
        else if (t->oclass == TCls_Object)
            t->extra = params[1]; /* hero gate number */
        (void)own_of;
        p->nthings++;
    }
    return true;
}

static TbBool sp_read_things_tngfx(struct SpPiece *p, uint8_t *buf, const char *where, int *bad)
{
    VALUE root, *rp = &root;
    char err[255] = "";
    if (toml_parse((char *)buf, err, sizeof(err), rp))
    {
        WARNLOG("World: set piece %s: its .tngfx can't be read: %s", where, err);
        return false;
    }
    VALUE *arr = value_dict_get(rp, "thing");
    int total = 0;
    VALUE *common = value_dict_get(rp, "common");
    if (value_type(arr) == VALUE_ARRAY)
        total = value_array_size(arr);
    else
    {
        arr = NULL;
        total = (common != NULL) ? value_int32(value_dict_get(common, "ThingsCount")) : 0;
    }
    for (int k = 0; k < total; k++)
    {
        VALUE *sec;
        if (arr != NULL)
            sec = value_array_get(arr, k);
        else
        {
            char key[32];
            snprintf(key, sizeof(key), "thing%d", k);
            sec = value_dict_get(rp, key);
        }
        if (value_type(sec) != VALUE_DICT)
            continue;
        const int oclass = value_parse_class(value_dict_get(sec, "ThingType"));
        if (oclass < 0)
            continue;
        int model = value_int32(value_dict_get(sec, "Subtype"));
        VALUE *sid = value_dict_get(sec, "SubtypeStringID");
        if ((sid != NULL) && (value_type(sid) == VALUE_STRING))
            model = value_parse_model(oclass, sid);
        if (model <= 0)
        {
            (*bad)++;
            continue;
        }
        if (p->nthings >= SP_MAX_THINGS)
        {
            (*bad)++;
            break;
        }
        struct SpThing *t = &p->things[p->nthings];
        memset(t, 0, sizeof(*t));
        t->oclass = (uint8_t)oclass;
        t->model = (uint16_t)model;
        const int owner = value_int32(value_dict_get(sec, "Ownership"));
        t->owner = (uint8_t)((owner >= 0) ? owner : PLAYER_NEUTRAL);
        t->x = (int32_t)value_read_stl_coord(value_dict_get(sec, "SubtileX"));
        t->y = (int32_t)value_read_stl_coord(value_dict_get(sec, "SubtileY"));
        if (oclass == TCls_Creature)
        {
            const int lv = value_int32(value_dict_get(sec, "CreatureLevel"));
            t->level = (uint8_t)((lv >= 1) && (lv <= 10) ? lv : 1);
            const int gold = value_int32(value_dict_get(sec, "CreatureGold"));
            t->gold = (gold > 0) ? (uint32_t)gold : 0;
        }
        else if (oclass == TCls_Door)
        {
            const int lk = value_int32(value_dict_get(sec, "DoorLocked"));
            t->extra = (lk > 0) ? 1 : 0;
        }
        else if (oclass == TCls_Object)
        {
            const int gate = value_int32(value_dict_get(sec, "HerogateNumber"));
            t->extra = (uint16_t)((gate > 0) ? gate : 0);
            const int gold = value_int32(value_dict_get(sec, "GoldValue"));
            t->gold = (gold > 0) ? (uint32_t)gold : 0;
        }
        p->nthings++;
    }
    value_fini(rp);
    return true;
}

static void sp_free_piece(struct SpPiece *p)
{
    free(p->kind);
    free(p->owner);
    free(p->tex);
    free(p->door);
    free(p->wlb);
    free(p->mask);
    free(p->dist);
    free(p->dat);
    free(p->clm);
    free(p->things);
    free(p->lights);
    sp_script_free(p->script);
    memset(p, 0, sizeof(*p));
}

/** The piece's own slabs (mask) and the distance field the blending ring is cut from. */
static void sp_blend(struct SpPiece *p)
{
    const int w = p->w, h = p->h;
    /* the outer slab, and plain rock and earth joined to it, are the land's own */
    uint8_t *seen = (uint8_t *)calloc((size_t)w * h, 1);
    int *queue = (int *)malloc(sizeof(int) * (size_t)w * h);
    if ((seen == NULL) || (queue == NULL))
    {
        free(seen);
        free(queue);
        memset(p->mask, 1, (size_t)w * h);
        return;
    }
    int qh = 0, qt = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if ((x == 0) || (y == 0) || (x == w - 1) || (y == h - 1))
            {
                seen[y * w + x] = 1;
                queue[qt++] = y * w + x;
            }
    while (qh < qt)
    {
        const int i = queue[qh++];
        const int x = i % w, y = i / w;
        static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4; d++)
        {
            const int nx = x + dx[d], ny = y + dy[d];
            if ((nx < 0) || (ny < 0) || (nx >= w) || (ny >= h))
                continue;
            const int j = ny * w + nx;
            if (seen[j])
                continue;
            const SlabKind k = p->kind[j];
            if (((k == SlbT_ROCK) || (k == SlbT_EARTH) || (k == SlbT_TORCHDIRT)) && (p->owner[j] == SPO_Neutral))
            {
                seen[j] = 1;
                queue[qt++] = j;
            }
        }
    }
    p->cx1 = w;
    p->cy1 = h;
    p->cx2 = -1;
    p->cy2 = -1;
    for (int i = 0; i < w * h; i++)
    {
        p->mask[i] = seen[i] ? 0 : 1;
        if (p->mask[i])
        {
            p->cx1 = min(p->cx1, i % w);
            p->cy1 = min(p->cy1, i / w);
            p->cx2 = max(p->cx2, i % w);
            p->cy2 = max(p->cy2, i / w);
        }
    }
    free(seen);
    free(queue);
    /* distance to the nearest piece slab (3/4 chamfer, in thirds of a slab), out to the ring's widest */
    const int ew = w + 2 * SP_FRINGE, eh = h + 2 * SP_FRINGE;
    uint16_t *d = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)ew * eh);
    if (d == NULL)
        return;
    for (int y = 0; y < eh; y++)
        for (int x = 0; x < ew; x++)
        {
            const int u = x - SP_FRINGE, v = y - SP_FRINGE;
            d[y * ew + x] = ((u >= 0) && (v >= 0) && (u < w) && (v < h) && p->mask[v * w + u]) ? 0 : 60000;
        }
    for (int pass = 0; pass < 2; pass++)
    {
        const int y0 = pass ? eh - 1 : 0, y1 = pass ? -1 : eh, sy = pass ? -1 : 1;
        const int x0 = pass ? ew - 1 : 0, x1 = pass ? -1 : ew, sx = pass ? -1 : 1;
        for (int y = y0; y != y1; y += sy)
            for (int x = x0; x != x1; x += sx)
            {
                uint16_t best = d[y * ew + x];
                const int nb[4][3] = {{-sx, 0, 3}, {0, -sy, 3}, {-sx, -sy, 4}, {sx, -sy, 4}};
                for (int k = 0; k < 4; k++)
                {
                    const int nx = x + nb[k][0], ny = y + nb[k][1];
                    if ((nx < 0) || (ny < 0) || (nx >= ew) || (ny >= eh))
                        continue;
                    const int c = d[ny * ew + nx] + nb[k][2];
                    if (c < best)
                        best = (uint16_t)c;
                }
                d[y * ew + x] = best;
            }
    }
    for (int i = 0; i < ew * eh; i++)
        p->dist[i] = (uint8_t)min(255, (int)d[i]);
    free(d);
}

/** Reads the piece in folder dir (a full path); false (and a warning) when it isn't usable. */
static TbBool sp_load_folder(const char *dir, const char *folder, struct SpPiece *p)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->where, sizeof(p->where), "%s", folder);
    snprintf(p->name, sizeof(p->name), "%s", folder);
    p->cmin = 1;
    p->cmax = 9;
    p->weight = 10;
    p->chance = 100;
    char path[600];
    size_t len;
    uint32_t hash = 2166136261u;
    snprintf(path, sizeof(path), "%s/setpiece.cfg", dir);
    uint8_t *cfg = sp_read(path, &len);
    if (cfg == NULL)
    {
        WARNLOG("World: set piece %s: no setpiece.cfg: left out", folder);
        return false;
    }
    sp_parse_cfg((const char *)cfg, folder, p);
    hash = sp_fnv(hash, cfg, len);
    free(cfg);
    char *maps[4];
    const int nmaps = sp_list(dir, false, ".slb", maps, 4);
    if (nmaps <= 0)
    {
        WARNLOG("World: set piece %s: no map (.slb) in its folder: left out", folder);
        return false;
    }
    char base[300];
    snprintf(base, sizeof(base), "%s/%.*s", dir, (int)(strlen(maps[0]) - 4), maps[0]);
    for (int i = 0; i < nmaps; i++)
        free(maps[i]);
    /* the size */
    int w = 85, h = 85;
    for (int e = 0; e < 2; e++)
    {
        snprintf(path, sizeof(path), "%s.%s", base, e ? "lif" : "lof");
        uint8_t *t = sp_read(path, &len);
        if (t == NULL)
            continue;
        const TbBool got = sp_mapsize((const char *)t, &w, &h);
        free(t);
        if (got)
            break;
    }
    if ((w < 3) || (h < 3) || (w > SP_MAX_SIDE) || (h > SP_MAX_SIDE))
    {
        WARNLOG("World: set piece %s: map size %dx%d is not 3-%d slabs a side: left out", folder, w, h, SP_MAX_SIDE);
        return false;
    }
    p->w = w;
    p->h = h;
    const int sw = 3 * w + 1, sh = 3 * h + 1;
    snprintf(path, sizeof(path), "%s.slb", base);
    uint8_t *slb = sp_read(path, &len);
    if ((slb == NULL) || (len < (size_t)2 * w * h))
    {
        WARNLOG("World: set piece %s: its .slb is not %dx%d slabs: left out", folder, w, h);
        free(slb);
        return false;
    }
    hash = sp_fnv(hash, slb, len);
    snprintf(path, sizeof(path), "%s.own", base);
    size_t olen;
    uint8_t *own = sp_read(path, &olen);
    if ((own == NULL) || (olen < (size_t)sw * sh))
    {
        WARNLOG("World: set piece %s: its .own is missing or short: left out", folder);
        free(slb);
        free(own);
        return false;
    }
    hash = sp_fnv(hash, own, olen);
    p->kind = (uint8_t *)malloc((size_t)w * h);
    p->owner = (uint8_t *)calloc((size_t)w * h, 1);
    p->tex = (uint8_t *)calloc((size_t)w * h, 1);
    p->door = (uint8_t *)calloc((size_t)w * h, 1);
    p->mask = (uint8_t *)calloc((size_t)w * h, 1);
    p->dist = (uint8_t *)malloc((size_t)(w + 2 * SP_FRINGE) * (h + 2 * SP_FRINGE));
    p->things = (struct SpThing *)malloc(sizeof(struct SpThing) * SP_MAX_THINGS);
    uint8_t *player_of = (uint8_t *)malloc((size_t)w * h);
    if (!p->kind || !p->owner || !p->tex || !p->door || !p->mask || !p->dist || !p->things || !player_of)
    {
        free(slb);
        free(own);
        free(player_of);
        sp_free_piece(p);
        return false;
    }
    for (int i = 0; i < w * h; i++)
    {
        unsigned k = (unsigned)(slb[2 * i] | (slb[2 * i + 1] << 8));
        if (k >= (unsigned)game.conf.slab_conf.slab_types_count)
            k = SlbT_ROCK;
        p->kind[i] = (uint8_t)k;
        const int x = i % w, y = i / w;
        player_of[i] = own[(3 * y + 1) * sw + 3 * x + 1];
    }
    free(slb);
    free(own);
    /* textures: the .slx per slab, else the map's own (.inf) */
    int inf_tex = 0;
    snprintf(path, sizeof(path), "%s.inf", base);
    uint8_t *inf = sp_read(path, &len);
    if ((inf != NULL) && (len >= 1))
        inf_tex = inf[0];
    free(inf);
    snprintf(path, sizeof(path), "%s.slx", base);
    uint8_t *slx = sp_read(path, &len);
    for (int i = 0; i < w * h; i++)
        p->tex[i] = (uint8_t)(((slx != NULL) && (len >= (size_t)w * h) && slx[i]) ? slx[i] : inf_tex);
    if (slx != NULL)
        hash = sp_fnv(hash, slx, len);
    free(slx);
    /* things */
    int bad = 0;
    snprintf(path, sizeof(path), "%s.tngfx", base);
    uint8_t *tng = sp_read(path, &len);
    if (tng != NULL)
    {
        hash = sp_fnv(hash, tng, len);
        sp_read_things_tngfx(p, tng, folder, &bad);
    } else
    {
        snprintf(path, sizeof(path), "%s.tng", base);
        tng = sp_read(path, &len);
        if (tng != NULL)
        {
            hash = sp_fnv(hash, tng, len);
            sp_read_things_tng(p, tng, len, NULL, &bad);
        }
    }
    free(tng);
    /* action points (.aptfx, else .apt) and hero gates, for the map's script */
    snprintf(path, sizeof(path), "%s.aptfx", base);
    uint8_t *apt = sp_read(path, &len);
    if (apt != NULL)
    {
        hash = sp_fnv(hash, apt, len);
        VALUE root, *rp = &root;
        char err[255] = "";
        if (toml_parse((char *)apt, err, sizeof(err), rp) == 0)
        {
            VALUE *arr = value_dict_get(rp, "actionpoint");
            VALUE *common = value_dict_get(rp, "common");
            const int total = (value_type(arr) == VALUE_ARRAY) ? value_array_size(arr) :
                ((common != NULL) ? value_int32(value_dict_get(common, "ActionPointsCount")) : 0);
            for (int k = 0; (k < total) && (p->naps < 64); k++)
            {
                VALUE *sec = arr;
                char key[32];
                if (value_type(arr) == VALUE_ARRAY)
                    sec = value_array_get(arr, k);
                else
                {
                    snprintf(key, sizeof(key), "actionpoint%d", k);
                    sec = value_dict_get(rp, key);
                }
                if (value_type(sec) != VALUE_DICT)
                    continue;
                struct SpScriptPoint *ap = &p->aps[p->naps++];
                ap->num = (uint16_t)max(0, value_int32(value_dict_get(sec, "PointNumber")));
                ap->x = (int32_t)value_read_stl_coord(value_dict_get(sec, "SubtileX"));
                ap->y = (int32_t)value_read_stl_coord(value_dict_get(sec, "SubtileY"));
                ap->range = (int32_t)value_read_stl_coord(value_dict_get(sec, "PointRange"));
            }
            value_fini(rp);
        }
        free(apt);
    } else
    {
        snprintf(path, sizeof(path), "%s.apt", base);
        apt = sp_read(path, &len);
        if ((apt != NULL) && (len >= 4))
        {
            hash = sp_fnv(hash, apt, len);
            const uint32_t total = (uint32_t)(apt[0] | (apt[1] << 8) | (apt[2] << 16) | ((uint32_t)apt[3] << 24));
            for (uint32_t k = 0; (k < total) && (4 + (k + 1) * 8 <= len) && (p->naps < 64); k++)
            {
                const uint8_t *r = apt + 4 + k * 8;
                struct SpScriptPoint *ap = &p->aps[p->naps++];
                ap->x = r[0] | (r[1] << 8);
                ap->y = r[2] | (r[3] << 8);
                ap->range = r[4] | (r[5] << 8);
                ap->num = (uint16_t)(r[6] | (r[7] << 8));
            }
        }
        free(apt);
    }
    for (int t = 0; (t < p->nthings) && (p->ngates < 16); t++)
    {
        const struct SpThing *th = &p->things[t];
        if ((th->oclass == TCls_Object) && (th->model > 0) && (th->model < game.conf.object_conf.object_types_count) &&
            (get_object_model_stats(th->model)->genre == OCtg_HeroGate))
        {
            struct SpScriptPoint *g = &p->gates[p->ngates++];
            g->num = th->extra;
            g->x = th->x;
            g->y = th->y;
            g->range = COORD_PER_STL * 3;
        }
    }
    /* the script (.txt); a .lua is left out (KeeperFX's Lua speaks to the whole level) */
    int script_kept = 0, script_dropped = 0;
    snprintf(path, sizeof(path), "%s.txt", base);
    uint8_t *txt = sp_read(path, &len);
    if (txt != NULL)
    {
        hash = sp_fnv(hash, txt, len);
        p->script = sp_script_parse((const char *)txt, folder, &script_kept, &script_dropped);
        free(txt);
    }
    snprintf(path, sizeof(path), "%s.lua", base);
    if (LbFileExists(path))
        WARNLOG("World: set piece %s: its Lua script is left out (set pieces run DK scripts, .txt)", folder);
    /* static lights (.lgtfx, else .lgt); dynamic ones are left out (a light that moves belongs to a thing) */
    int dynamic_lights = 0;
    p->lights = (struct SpLight *)malloc(sizeof(struct SpLight) * SP_MAX_LIGHTS);
    snprintf(path, sizeof(path), "%s.lgtfx", base);
    uint8_t *lgt = sp_read(path, &len);
    if ((lgt != NULL) && (p->lights != NULL))
    {
        hash = sp_fnv(hash, lgt, len);
        VALUE root, *rp = &root;
        char err[255] = "";
        if (toml_parse((char *)lgt, err, sizeof(err), rp) == 0)
        {
            VALUE *arr = value_dict_get(rp, "light");
            VALUE *common = value_dict_get(rp, "common");
            const int total = (value_type(arr) == VALUE_ARRAY) ? value_array_size(arr) :
                ((common != NULL) ? value_int32(value_dict_get(common, "LightsCount")) : 0);
            for (int k = 0; (k < total) && (p->nlights < SP_MAX_LIGHTS); k++)
            {
                VALUE *sec;
                char key[32];
                if (value_type(arr) == VALUE_ARRAY)
                    sec = value_array_get(arr, k);
                else
                {
                    snprintf(key, sizeof(key), "light%d", k);
                    sec = value_dict_get(rp, key);
                }
                if (value_type(sec) != VALUE_DICT)
                    continue;
                if (value_coerce_bool(value_dict_get(sec, "Dynamic")))
                {
                    dynamic_lights++;
                    continue;
                }
                struct SpLight *l = &p->lights[p->nlights++];
                l->x = (int32_t)value_read_stl_coord(value_dict_get(sec, "SubtileX"));
                l->y = (int32_t)value_read_stl_coord(value_dict_get(sec, "SubtileY"));
                l->z = (int32_t)value_read_stl_coord(value_dict_get(sec, "SubtileZ"));
                l->radius = (uint16_t)value_read_stl_coord(value_dict_get(sec, "LightRange"));
                l->intensity = (uint8_t)value_uint32(value_dict_get(sec, "LightIntensity"));
                l->flags = 0;
            }
            value_fini(rp);
        }
    } else if (p->lights != NULL)
    {
        free(lgt);
        snprintf(path, sizeof(path), "%s.lgt", base);
        lgt = sp_read(path, &len);
        if ((lgt != NULL) && (len >= 4))
        {
            hash = sp_fnv(hash, lgt, len);
            const uint32_t total = (uint32_t)(lgt[0] | (lgt[1] << 8) | (lgt[2] << 16) | ((uint32_t)lgt[3] << 24));
            for (uint32_t k = 0; (k < total) && (4 + (k + 1) * 20 <= len) && (p->nlights < SP_MAX_LIGHTS); k++)
            {
                const uint8_t *r = lgt + 4 + k * 20; /* LegacyInitLight */
                if (r[17] != 0)
                {
                    dynamic_lights++;
                    continue;
                }
                struct SpLight *l = &p->lights[p->nlights++];
                l->radius = (uint16_t)(r[0] | (r[1] << 8));
                l->intensity = r[2];
                l->flags = r[3];
                l->x = r[10] | (r[11] << 8);
                l->y = r[12] | (r[13] << 8);
                l->z = r[14] | (r[15] << 8);
            }
        }
    }
    free(lgt);
    /* what is under the bridges (the world's chunks don't keep it: world_setpiece_columns_box puts it back) */
    snprintf(path, sizeof(path), "%s.wlb", base);
    uint8_t *wlb = sp_read(path, &len);
    if ((wlb != NULL) && (len >= (size_t)w * h))
    {
        p->wlb = wlb;
        wlb = NULL;
    }
    free(wlb);
    /* columns */
    snprintf(path, sizeof(path), "%s.clm", base);
    uint8_t *clm = sp_read(path, &len);
    snprintf(path, sizeof(path), "%s.dat", base);
    size_t dlen;
    uint8_t *dat = sp_read(path, &dlen);
    if ((clm != NULL) && (dat != NULL) && (len >= 8) && (dlen >= (size_t)2 * sw * sh))
    {
        int total = (int)(clm[0] | (clm[1] << 8) | (clm[2] << 16) | ((uint32_t)clm[3] << 24));
        const int fit = (int)((len - 8) / sizeof(struct Column));
        if ((total < 0) || (total > fit))
            total = fit;
        p->clm = (struct Column *)malloc(sizeof(struct Column) * (size_t)max(total, 1));
        p->dat = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)sw * sh);
        if ((p->clm != NULL) && (p->dat != NULL))
        {
            memcpy(p->clm, clm + 8, sizeof(struct Column) * (size_t)total);
            p->nclm = total;
            for (int i = 0; i < sw * sh; i++)
                p->dat[i] = (uint16_t)(-(int)(dat[2 * i] | (dat[2 * i + 1] << 8)));
            hash = sp_fnv(hash, clm, len);
            hash = sp_fnv(hash, dat, dlen);
        } else
        {
            free(p->clm);
            free(p->dat);
            p->clm = NULL;
            p->dat = NULL;
        }
    }
    free(clm);
    free(dat);
    /* the keepers: players 1-7 with a heart (not the heroes, nor nobody) */
    for (int pl = 1; pl < PLAYERS_COUNT && p->nkeepers < SP_MAX_KEEPERS; pl++)
    {
        if ((pl == PLAYER_GOOD) || (pl == PLAYER_NEUTRAL))
            continue;
        int n = 0;
        long sx = 0, sy = 0;
        int bx1 = w, by1 = h, bx2 = -1, by2 = -1;
        for (int i = 0; i < w * h; i++)
        {
            if (player_of[i] != pl)
                continue;
            bx1 = min(bx1, i % w);
            by1 = min(by1, i / w);
            bx2 = max(bx2, i % w);
            by2 = max(by2, i / w);
            if (p->kind[i] == SlbT_DUNGHEART)
            {
                sx += i % w;
                sy += i / w;
                n++;
            }
        }
        if (n == 0)
            continue;
        struct SpKeeper *k = &p->keepers[p->nkeepers++];
        k->player = (uint8_t)pl;
        k->hx = (int)(sx / n);
        k->hy = (int)(sy / n);
        k->bx1 = bx1;
        k->by1 = by1;
        k->bx2 = bx2;
        k->by2 = by2;
    }
    /* owners and the endless rules on slabs */
    for (int i = 0; i < w * h; i++)
    {
        const int pl = player_of[i];
        uint8_t o = SPO_Neutral;
        if (pl == PLAYER_GOOD)
            o = SPO_Heroes;
        for (int k = 0; k < p->nkeepers; k++)
            if (p->keepers[k].player == pl)
                o = (uint8_t)(SPO_Keeper0 + k);
        SlabKind kind = sp_slab_rule((SlabKind)p->kind[i]);
        if ((kind == SlbT_DUNGHEART) && (o == SPO_Neutral))
            kind = SlbT_CLAIMED; /* player 0's heart, or a heart nobody keeps: floor (the heroes keep theirs) */
        if (slab_kind_has_no_ownership(kind))
            o = SPO_Neutral;
        p->kind[i] = (uint8_t)kind;
        p->owner[i] = o;
    }
    free(player_of);
    /* things: owners, doors onto their slabs, the endless rules */
    int boxes = 0, books = 0, turned = 0, gens = 0, kept = 0;
    const long braced = get_id(door_desc, "BRACED");
    for (int t = 0; t < p->nthings; t++)
    {
        struct SpThing th = p->things[t];
        const int sx = th.x / (COORD_PER_STL * STL_PER_SLB), sy = th.y / (COORD_PER_STL * STL_PER_SLB);
        if ((sx < 0) || (sy < 0) || (sx >= w) || (sy >= h))
            continue;
        uint8_t o = SPO_Neutral;
        if (th.owner == PLAYER_GOOD)
            o = SPO_Heroes;
        for (int k = 0; k < p->nkeepers; k++)
            if (p->keepers[k].player == th.owner)
                o = (uint8_t)(SPO_Keeper0 + k);
        if (th.oclass == TCls_Door)
        {
            p->door[sy * w + sx] = (uint8_t)th.model;
            continue;
        }
        if (th.oclass == TCls_Object)
        {
            const ThingModel m = sp_object_rule((ThingModel)th.model, &boxes, &books, &turned);
            if (m <= 0)
                continue;
            th.model = (uint16_t)m;
        }
        else if (th.oclass == TCls_Trap)
            th.extra = (uint16_t)max(get_trap_model_stats(th.model)->shots, 1);
        else if (th.oclass != TCls_Creature)
        {
            gens++;
            continue;
        }
        th.owner = o;
        p->things[kept++] = th;
    }
    p->nthings = kept;
    /* a door slab with no door thing on it gets the door its kind says (braced when it says nothing) */
    for (int i = 0; i < w * h; i++)
    {
        if (!slab_kind_is_door((SlabKind)p->kind[i]))
            continue;
        if (p->door[i] == 0)
            p->door[i] = (uint8_t)((braced > 0) ? braced : 2);
        p->kind[i] = SlbT_CLAIMED;
    }
    p->hash = (hash == 0) ? 1 : hash;
    sp_blend(p);
    {
        /* a light on the land's own ground (outside the piece's shape) is left out */
        int kept_lights = 0;
        for (int k = 0; k < p->nlights; k++)
        {
            const int u = p->lights[k].x / (COORD_PER_STL * STL_PER_SLB), v = p->lights[k].y / (COORD_PER_STL * STL_PER_SLB);
            if ((u >= 0) && (v >= 0) && (u < w) && (v < h) && p->mask[v * w + u])
                p->lights[kept_lights++] = p->lights[k];
        }
        p->nlights = kept_lights;
    }
    if (p->cx2 < 0)
    {
        WARNLOG("World: set piece %s: nothing left of the map once its rock and earth joined to the edge are the land's: left out", folder);
        sp_free_piece(p);
        return false;
    }
    SYNCLOG("World: set piece %s (%s): %dx%d slabs, its own %dx%d of them, %d keepers, %d things (left out: %d special "
        "boxes and %d spellbooks endless worlds don't have, %d effect generators; %d spellbooks made research books)%s%s; "
        "%d static lights (%d dynamic left out), %d action points, %d hero gates, script: %d commands kept, %d left out",
        p->name, folder, w, h, p->cx2 - p->cx1 + 1, p->cy2 - p->cy1 + 1, p->nkeepers, p->nthings, boxes, books, gens,
        turned, (p->clm != NULL) ? ", own columns" : "", (bad > 0) ? ", some things unreadable" : "", p->nlights,
        dynamic_lights, p->naps, p->ngates,
        script_kept, script_dropped);
    return true;
}

static void sp_load_scope(const char *dir, const char *scope)
{
    char *names[SP_MAX_PIECES];
    const int n = sp_list(dir, true, NULL, names, SP_MAX_PIECES);
    for (int i = 0; i < n; i++)
    {
        if ((scope[0] == 0) && (strcasecmp(names[i], "official") == 0))
        {
            free(names[i]);
            continue;
        }
        if (sp_count < SP_MAX_PIECES)
        {
            char full[500], where[200];
            snprintf(full, sizeof(full), "%s/%s", dir, names[i]);
            snprintf(where, sizeof(where), "%s%s", scope, names[i]);
            struct SpPiece *p = &sp_pieces[sp_count];
            if (sp_load_folder(full, where, p))
            {
                TbBool taken = false;
                for (int j = 0; j < sp_count; j++)
                    taken = taken || (strcmp(sp_pieces[j].name, p->name) == 0);
                if (taken)
                {
                    WARNLOG("World: set piece %s: the name %s is taken already: left out", where, p->name);
                    sp_free_piece(p);
                } else
                    sp_count++;
            }
        }
        free(names[i]);
    }
}

static void sp_load_pieces(void)
{
    if (sp_loaded)
        return;
    sp_loaded = true;
    for (int i = 0; i < sp_count; i++)
        sp_free_piece(&sp_pieces[i]);
    sp_count = 0;
    if (sp_pieces == NULL)
        sp_pieces = (struct SpPiece *)calloc(SP_MAX_PIECES, sizeof(struct SpPiece));
    if (sp_pieces == NULL)
        return;
    char dir[400];
    snprintf(dir, sizeof(dir), "%s", prepare_file_path(FGrp_VarLevels, "endless_cfgs/setpieces"));
    sp_load_scope(dir, "");
    const int own = sp_count;
    if (world_official_setpieces)
    {
        char odir[460];
        snprintf(odir, sizeof(odir), "%s/official", dir);
        sp_load_scope(odir, "official/");
    }
    SYNCLOG("World: %d set pieces (%d of the player's own, %d of the creator's%s)", sp_count, own, sp_count - own,
        world_official_setpieces ? "" : ": the creator's are off in keeperfx.cfg, OFFICIAL_SET_PIECES");
}

static int sp_find(uint32_t hash)
{
    for (int i = 0; i < sp_count; i++)
        if (sp_pieces[i].hash == hash)
            return i;
    return -1;
}

/******************************************************************************/
/* what was decided */
static struct SpState *sp_state(int64_t rx, int64_t ry, TbBool add)
{
    for (int i = 0; i < sp_state_count; i++)
        if ((sp_states[i].rx == rx) && (sp_states[i].ry == ry))
            return &sp_states[i];
    if (!add)
        return NULL;
    if (sp_state_count >= sp_state_cap)
    {
        const int ncap = (sp_state_cap > 0) ? sp_state_cap * 2 : 32;
        struct SpState *n = (struct SpState *)realloc(sp_states, (size_t)ncap * sizeof(*n));
        if (n == NULL)
            return NULL;
        sp_states = n;
        sp_state_cap = ncap;
    }
    struct SpState *st = &sp_states[sp_state_count++];
    memset(st, 0, sizeof(*st));
    st->rx = rx;
    st->ry = ry;
    return st;
}

void world_setpieces_reset(void)
{
    for (int i = 0; i < sp_state_count; i++)
        free(sp_states[i].run);
    free(sp_states);
    sp_states = NULL;
    sp_state_count = sp_state_cap = 0;
    sp_loaded = false; /* read again (a file may have come) */
}

/******************************************************************************/
/* where */

/** The box of world slabs a piece placed at (x0, y0) changes, its blending ring included. */
static void sp_extent(const struct SpPiece *p, int64_t x0, int64_t y0, int64_t *x1, int64_t *y1, int64_t *x2, int64_t *y2)
{
    *x1 = x0 + p->cx1 - SP_FRINGE;
    *y1 = y0 + p->cy1 - SP_FRINGE;
    *x2 = x0 + p->cx2 + SP_FRINGE;
    *y2 = y0 + p->cy2 + SP_FRINGE;
}

static TbBool sp_box_made(int64_t x1, int64_t y1, int64_t x2, int64_t y2, int32_t skip_cx, int32_t skip_cy)
{
    for (int64_t cy = sp_floor_div(y1, WG_CHUNK); cy <= sp_floor_div(y2, WG_CHUNK); cy++)
        for (int64_t cx = sp_floor_div(x1, WG_CHUNK); cx <= sp_floor_div(x2, WG_CHUNK); cx++)
        {
            if ((cx == skip_cx) && (cy == skip_cy))
                continue;
            if (world_chunk_made((int32_t)cx, (int32_t)cy))
                return true;
        }
    return false;
}

/* why places were refused (the survey logs them) */
static int sp_why[8];
enum { SpW_Start = 0, SpW_Stamp, SpW_Keeper, SpW_Fortress, SpW_Land, SpW_Made };

/** Test switch KFX_WORLD_SETPIECENEAR=slabs (a played session, a test world only): pieces may stand from this far
 *  from the start (their nearest slab; else SP_START_DIST), and every region may hold one (else SP_REGION_CHANCE%). */
static int sp_near_switch(void)
{
    static int near = -2;
    if (near == -2)
    {
        const char *v = getenv("KFX_WORLD_SETPIECENEAR");
        near = (v != NULL) ? max(0, atoi(v)) : -1;
    }
    return near;
}

static TbBool sp_place_clear(int64_t x1, int64_t y1, int64_t x2, int64_t y2)
{
    struct WgWorld *gen = world_generator();
    struct WgStructRef refs[2];
    /* the start: no slab of the piece within SP_START_DIST of it */
    struct WgStructRef start;
    wg_start_ref(gen, &start);
    const int64_t nx = max(x1, min(start.heart_x, x2)), ny = max(y1, min(start.heart_y, y2));
    if ((nx - start.heart_x) * (nx - start.heart_x) + (ny - start.heart_y) * (ny - start.heart_y) <
        (int64_t)((sp_near_switch() >= 0) ? sp_near_switch() : SP_START_DIST) *
        ((sp_near_switch() >= 0) ? sp_near_switch() : SP_START_DIST))
        return (sp_why[SpW_Start]++, false);
    if (world_stamps_overlap(x1 - SP_CLEAR, y1 - SP_CLEAR, x2 + SP_CLEAR, y2 + SP_CLEAR))
        return (sp_why[SpW_Stamp]++, false);
    /* (the heroes outposts keep off the pieces, which are decided first; portals and reward caves under a piece give
       way to it: in version 9 a portal stands about every 111 slabs, so nearly every whole map would cover one) */
    /* keepers (a rival pair is two of them, with its battlefield between) and fortresses keep further off */
    if (wg_structures_in(gen, x1 - SP_KEEPER_CLEAR, y1 - SP_KEEPER_CLEAR, x2 + SP_KEEPER_CLEAR, y2 + SP_KEEPER_CLEAR,
        1u << WgT_Keeper, refs, 2) > 0)
        return (sp_why[SpW_Keeper]++, false);
    if (wg_structures_in(gen, x1 - SP_FORT_CLEAR, y1 - SP_FORT_CLEAR, x2 + SP_FORT_CLEAR, y2 + SP_FORT_CLEAR,
        1u << WgT_Fortress, refs, 2) > 0)
        return (sp_why[SpW_Fortress]++, false);
    if (!world_land_fits(x1, y1, x2, y2, false))
        return (sp_why[SpW_Land]++, false);
    return true;
}

/** The piece for a place: by weight among those its biome and circle allow and that fit in the region; -1 none. */
static int sp_pick(struct SpRng *rng, int64_t mx, int64_t my)
{
    struct WgWorld *gen = world_generator();
    const int biome = wg_biome_at(gen, mx, my);
    const int circle = world_circle_at_world(mx, my);
    int total = 0;
    for (int pass = 0; pass < 2; pass++)
    {
        int r = pass ? sp_rand(rng, total) : 0;
        for (int i = 0; i < sp_count; i++)
        {
            const struct SpPiece *p = &sp_pieces[i];
            if ((p->biomes != 0) && ((biome < 0) || (biome >= 32) || !(p->biomes & (1u << biome))))
                continue;
            if ((circle < p->cmin) || (circle > p->cmax) || (p->weight <= 0))
                continue;
            if (pass == 0)
                total += p->weight;
            else if (r < p->weight)
                return i;
            else
                r -= p->weight;
        }
        if (total <= 0)
            return -1;
    }
    return -1;
}

static struct SpState *sp_decide(int64_t rx, int64_t ry, int32_t gen_cx, int32_t gen_cy)
{
    struct SpState *st = sp_state(rx, ry, false);
    if (st != NULL)
        return st;
    struct WgWorld *gen = world_generator();
    struct SpRng rng;
    rng.s = wg_world_seed(gen) ^ sp_mix((uint64_t)rx * 0x2127599BF4325C37ULL) ^
        sp_mix((uint64_t)ry * 0x880355F21E6D1965ULL + 0x53504332ULL);
    const TbBool may = (sp_rand(&rng, 100) < SP_REGION_CHANCE) || (sp_near_switch() >= 0);
    sp_load_pieces();
    st = sp_state(rx, ry, true);
    if ((st == NULL) || !may || (sp_count <= 0))
        return st;
    for (int k = 0; k < SP_SITES; k++)
    {
        const int64_t mx = rx * SP_REGION + SP_REGION / 8 + sp_rand(&rng, SP_REGION * 3 / 4);
        const int64_t my = ry * SP_REGION + SP_REGION / 8 + sp_rand(&rng, SP_REGION * 3 / 4);
        const int pi = sp_pick(&rng, mx, my);
        const int roll = sp_rand(&rng, 100);
        if (pi < 0)
            continue;
        const struct SpPiece *p = &sp_pieces[pi];
        if (roll >= p->chance)
            continue;
        /* the piece's own slabs centred on the place, the whole of it (ring and room) inside the region */
        int64_t x0 = mx - (p->cx1 + p->cx2) / 2, y0 = my - (p->cy1 + p->cy2) / 2;
        int64_t x1, y1, x2, y2;
        sp_extent(p, x0, y0, &x1, &y1, &x2, &y2);
        const int64_t lo_x = rx * SP_REGION + SP_CLEAR, hi_x = rx * SP_REGION + SP_REGION - 1 - SP_CLEAR;
        const int64_t lo_y = ry * SP_REGION + SP_CLEAR, hi_y = ry * SP_REGION + SP_REGION - 1 - SP_CLEAR;
        if (x2 - x1 > hi_x - lo_x || y2 - y1 > hi_y - lo_y)
            continue;
        if (x1 < lo_x) { x0 += lo_x - x1; }
        if (x2 > hi_x) { x0 -= x2 - hi_x; }
        if (y1 < lo_y) { y0 += lo_y - y1; }
        if (y2 > hi_y) { y0 -= y2 - hi_y; }
        sp_extent(p, x0, y0, &x1, &y1, &x2, &y2);
        if (sp_box_made(x1, y1, x2, y2, gen_cx, gen_cy))
        {
            sp_why[SpW_Made]++;
            continue;
        }
        if (!sp_place_clear(x1, y1, x2, y2))
            continue;
        st = sp_state(rx, ry, false);
        st->piece = p->hash;
        st->x0 = x0;
        st->y0 = y0;
        SYNCLOG("World: set piece %s at world slabs (%lld,%lld)-(%lld,%lld), place %d of region (%lld,%lld)", p->name,
            (long long)(x0 + p->cx1), (long long)(y0 + p->cy1), (long long)(x0 + p->cx2), (long long)(y0 + p->cy2), k,
            (long long)rx, (long long)ry);
        return st;
    }
    return sp_state(rx, ry, false);
}

/******************************************************************************/
/* the chunks */

/** The blending ring's width at a world slab, in thirds of a slab: 2-5 slabs, wandering smoothly with the place. */
static int sp_ring_at(int64_t wx, int64_t wy)
{
    const int64_t cell = 7;
    const int64_t gx = sp_floor_div(wx, cell), gy = sp_floor_div(wy, cell);
    const int fx = (int)(wx - gx * cell), fy = (int)(wy - gy * cell);
    const uint64_t salt = wg_world_seed(world_generator()) ^ 0x52494E47ULL;
    int v[4];
    for (int k = 0; k < 4; k++)
        v[k] = (int)(sp_mix(salt ^ sp_mix((uint64_t)(gx + (k & 1)) * 0x9E3779B1ULL) ^ ((uint64_t)(gy + (k >> 1)) * 0x85EBCA77ULL)) % 1000);
    const int top = v[0] * (int)(cell - fx) + v[1] * fx, bot = v[2] * (int)(cell - fx) + v[3] * fx;
    const int n = (top * (int)(cell - fy) + bot * fy) / (int)(cell * cell); /* 0..999 */
    return 6 + (n * 9) / 1000; /* 6..15 thirds: 2..5 slabs */
}

static int sp_keeper_slot(struct SetPieceOverlay *ov, const struct SpPiece *p, int k, int64_t x0, int64_t y0)
{
    const struct SpKeeper *pk = &p->keepers[k];
    const int64_t hx = x0 + pk->hx, hy = y0 + pk->hy;
    for (int i = 0; i < ov->keeper_count; i++)
        if ((ov->keepers[i].heart_x == hx) && (ov->keepers[i].heart_y == hy))
            return i;
    if (ov->keeper_count >= SETPIECE_CHUNK_KEEPERS)
        return -1;
    struct SetPieceKeeper *sk = &ov->keepers[ov->keeper_count];
    sk->heart_x = hx;
    sk->heart_y = hy;
    sk->bbox_x1 = x0 + pk->bx1;
    sk->bbox_y1 = y0 + pk->by1;
    sk->bbox_x2 = x0 + pk->bx2;
    sk->bbox_y2 = y0 + pk->by2;
    const int half = max(pk->bx2 - pk->bx1, pk->by2 - pk->by1) / 2;
    sk->threat_radius = max(24, half + 12);
    sk->money = SP_DEFAULT_MONEY;
    return ov->keeper_count++;
}

static uint8_t sp_owner_in(struct SetPieceOverlay *ov, const struct SpPiece *p, uint8_t o, int64_t x0, int64_t y0)
{
    if (o < SPO_Keeper0)
        return o;
    const int slot = sp_keeper_slot(ov, p, o - SPO_Keeper0, x0, y0);
    return (slot >= 0) ? (uint8_t)(SPO_Keeper0 + slot) : (uint8_t)SPO_Neutral;
}

static int sp_overlay_state(const struct SpState *st, int32_t cx, int32_t cy, struct SetPieceOverlay *ov)
{
    const int pi = sp_find(st->piece);
    if (pi < 0)
        return 0;
    const struct SpPiece *p = &sp_pieces[pi];
    const int64_t wx1 = (int64_t)cx * WG_CHUNK, wy1 = (int64_t)cy * WG_CHUNK;
    const int ew = p->w + 2 * SP_FRINGE;
    int decided = 0;
    for (int j = 0; j < WG_CHUNK; j++)
        for (int i = 0; i < WG_CHUNK; i++)
        {
            const int64_t gx = wx1 + i, gy = wy1 + j;
            const int64_t u = gx - st->x0, v = gy - st->y0;
            if ((u < -SP_FRINGE) || (v < -SP_FRINGE) || (u >= p->w + SP_FRINGE) || (v >= p->h + SP_FRINGE))
                continue;
            const int c = j * WG_CHUNK + i;
            if ((u >= 0) && (v >= 0) && (u < p->w) && (v < p->h) && p->mask[v * p->w + u])
            {
                const int s = (int)(v * p->w + u);
                ov->set[c] = 1;
                ov->kind[c] = p->kind[s];
                ov->owner[c] = sp_owner_in(ov, p, p->owner[s], st->x0, st->y0);
                ov->texture[c] = p->tex[s];
                ov->door[c] = p->door[s];
                decided++;
                continue;
            }
            const int d = p->dist[(v + SP_FRINGE) * ew + (u + SP_FRINGE)];
            if ((d > 0) && (d <= sp_ring_at(gx, gy)) && (ov->set[c] == 0))
            {
                ov->set[c] = 2;
                ov->kind[c] = SlbT_EARTH;
                decided++;
            }
        }
    for (int t = 0; t < p->nthings; t++)
    {
        const struct SpThing *th = &p->things[t];
        const int64_t wxm = st->x0 * (COORD_PER_STL * STL_PER_SLB) + th->x, wym = st->y0 * (COORD_PER_STL * STL_PER_SLB) + th->y;
        const int64_t rxm = wxm - wx1 * (COORD_PER_STL * STL_PER_SLB), rym = wym - wy1 * (COORD_PER_STL * STL_PER_SLB);
        if ((rxm < 0) || (rym < 0) || (rxm >= WG_CHUNK * COORD_PER_STL * STL_PER_SLB) ||
            (rym >= WG_CHUNK * COORD_PER_STL * STL_PER_SLB) || (ov->thing_count >= SETPIECE_OVERLAY_THINGS))
            continue;
        const int sx = th->x / (COORD_PER_STL * STL_PER_SLB), sy = th->y / (COORD_PER_STL * STL_PER_SLB);
        if (!p->mask[sy * p->w + sx])
            continue; /* on the land's own ground */
        struct SetPieceThing *o = &ov->things[ov->thing_count++];
        memset(o, 0, sizeof(*o));
        o->oclass = th->oclass;
        o->model = th->model;
        o->owner = sp_owner_in(ov, p, th->owner, st->x0, st->y0);
        o->level = th->level;
        o->x = (int32_t)rxm;
        o->y = (int32_t)rym;
        o->extra = th->extra;
        o->gold = th->gold;
    }
    return decided;
}

/** The regions whose pieces may reach into the box. */
static void sp_regions_of(int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t *rx1, int64_t *ry1, int64_t *rx2, int64_t *ry2)
{
    *rx1 = sp_floor_div(x1, SP_REGION);
    *ry1 = sp_floor_div(y1, SP_REGION);
    *rx2 = sp_floor_div(x2, SP_REGION);
    *ry2 = sp_floor_div(y2, SP_REGION);
}

int world_setpiece_overlay(int32_t cx, int32_t cy, struct SetPieceOverlay *ov)
{
    memset(ov, 0, sizeof(*ov));
    if (!sp_active())
        return 0;
    const int64_t wx1 = (int64_t)cx * WG_CHUNK, wy1 = (int64_t)cy * WG_CHUNK;
    int64_t rx1, ry1, rx2, ry2;
    sp_regions_of(wx1, wy1, wx1 + WG_CHUNK - 1, wy1 + WG_CHUNK - 1, &rx1, &ry1, &rx2, &ry2);
    int decided = 0;
    for (int64_t ry = ry1; ry <= ry2; ry++)
        for (int64_t rx = rx1; rx <= rx2; rx++)
        {
            const struct SpState *st = sp_decide(rx, ry, cx, cy);
            if ((st != NULL) && (st->piece != 0))
                decided += sp_overlay_state(st, cx, cy, ov);
        }
    return decided;
}

TbBool world_setpiece_in_box(int64_t x1, int64_t y1, int64_t x2, int64_t y2)
{
    if (!sp_active())
        return false;
    int64_t rx1, ry1, rx2, ry2;
    sp_regions_of(x1, y1, x2, y2, &rx1, &ry1, &rx2, &ry2);
    for (int64_t ry = ry1; ry <= ry2; ry++)
        for (int64_t rx = rx1; rx <= rx2; rx++)
        {
            const struct SpState *st = sp_decide(rx, ry, INT32_MIN, INT32_MIN);
            if ((st == NULL) || (st->piece == 0))
                continue;
            const int pi = sp_find(st->piece);
            if (pi < 0)
                continue;
            int64_t bx1, by1, bx2, by2;
            sp_extent(&sp_pieces[pi], st->x0, st->y0, &bx1, &by1, &bx2, &by2);
            if ((bx1 <= x2) && (x1 <= bx2) && (by1 <= y2) && (y1 <= by2))
                return true;
        }
    return false;
}

/******************************************************************************/
/* the pieces' own columns */
/* map_blocks.c, not in a header */
void place_column_on_map_element(struct Column *ncol, MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** Is this static light (attached to that slab, at that place) on the map already? */
static TbBool sp_light_there(SlabCodedCoords slbnum, const struct Coord3d *pos)
{
    long i = game.thing_lists[TngList_StaticLights].index;
    for (unsigned long k = 0; (i > 0) && (k <= LIGHTS_COUNT); k++)
    {
        const struct Light *lgt = &game.lish.lights[i];
        i = lgt->next_in_list;
        if ((lgt->attached_slb == slbnum) && (lgt->mappos.x.val == pos->x.val) && (lgt->mappos.y.val == pos->y.val))
            return true;
    }
    return false;
}

/** A placed piece's static lights on the engine slabs of the box that are still as the piece made them. Each is
 *  attached to its slab: a slab put away (its chunk freezes) or placed again (dug, claimed, built on) takes its light
 *  with it, as a torch's; the next time the slab comes in as the piece made it, the light comes back. */
static int sp_lights_box(const struct SpState *st, const struct SpPiece *p, MapSlabCoord ex1, MapSlabCoord ey1,
    MapSlabCoord ex2, MapSlabCoord ey2)
{
    int made = 0;
    for (int k = 0; k < p->nlights; k++)
    {
        const struct SpLight *l = &p->lights[k];
        const int u = l->x / (COORD_PER_STL * STL_PER_SLB), v = l->y / (COORD_PER_STL * STL_PER_SLB);
        MapSlabCoord ex, ey;
        if (!world_engine_slab(st->x0 + u, st->y0 + v, &ex, &ey) || (ex < ex1) || (ex > ex2) || (ey < ey1) || (ey > ey2))
            continue;
        const struct SlabMap *slb = get_slabmap_block(ex, ey);
        if (slabmap_block_invalid(slb) || (slb->kind != p->kind[v * p->w + u]))
            continue;
        struct InitLight il;
        memset(&il, 0, sizeof(il));
        il.mappos.x.val = (MapCoord)(ex * COORD_PER_STL * STL_PER_SLB + l->x % (COORD_PER_STL * STL_PER_SLB));
        il.mappos.y.val = (MapCoord)(ey * COORD_PER_STL * STL_PER_SLB + l->y % (COORD_PER_STL * STL_PER_SLB));
        il.mappos.z.val = (MapCoord)l->z;
        il.attached_slb = get_slab_number(ex, ey);
        if (sp_light_there(il.attached_slb, &il.mappos))
            continue;
        if (light_count_lights() >= LIGHTS_COUNT * 3 / 4)
        {
            static GameTurn told = (GameTurn)-1;
            if (told != game.play_gameturn)
                WARNLOG("World: set piece %s: the map's lights are left out here: %d of %d lights are in use", p->name,
                    light_count_lights(), LIGHTS_COUNT);
            told = game.play_gameturn;
            break;
        }
        il.radius = (short)l->radius;
        il.intensity = l->intensity;
        il.flags = l->flags;
        il.is_dynamic = 0;
        if (light_create_light(&il) > 0)
            made++;
    }
    return made;
}

void world_setpiece_columns_box(MapSlabCoord ex1, MapSlabCoord ey1, MapSlabCoord ex2, MapSlabCoord ey2)
{
    if (!sp_active() || (sp_state_count <= 0))
        return;
    int placed = 0, lights = 0;
    for (int n = 0; n < sp_state_count; n++)
    {
        const struct SpState *st = &sp_states[n];
        if (st->piece == 0)
            continue;
        const int pi = sp_find(st->piece);
        if (pi < 0)
            continue;
        const struct SpPiece *p = &sp_pieces[pi];
        lights += sp_lights_box(st, p, ex1, ey1, ex2, ey2);
        if ((p->dat == NULL) && (p->wlb == NULL))
            continue;
        const int sw = 3 * p->w + 1;
        for (MapSlabCoord ey = ey1; ey <= ey2; ey++)
            for (MapSlabCoord ex = ex1; ex <= ex2; ex++)
            {
                int64_t wx, wy;
                world_slab_to_world(ex, ey, &wx, &wy);
                const int64_t u = wx - st->x0, v = wy - st->y0;
                if ((u < 0) || (v < 0) || (u >= p->w) || (v >= p->h) || !p->mask[v * p->w + u])
                    continue;
                struct SlabMap *slb = get_slabmap_block(ex, ey);
                if (slabmap_block_invalid(slb) || (slb->kind != p->kind[v * p->w + u]) || (p->door[v * p->w + u] != 0))
                    continue; /* dug, claimed, built on: the engine's own columns */
                if ((p->wlb != NULL) && (p->wlb[v * p->w + u] != 0) && (slabmap_wlb(slb) == 0))
                    slabmap_set_wlb(slb, p->wlb[v * p->w + u]); /* the liquid under the map's bridge */
                if (p->dat == NULL)
                    continue;
                for (int sy = 0; sy < STL_PER_SLB; sy++)
                    for (int sx = 0; sx < STL_PER_SLB; sx++)
                    {
                        const int ci = p->dat[(3 * v + sy) * sw + 3 * u + sx];
                        if ((ci <= 0) || (ci >= p->nclm))
                            continue;
                        const MapSubtlCoord stl_x = ex * STL_PER_SLB + sx, stl_y = ey * STL_PER_SLB + sy;
                        struct Column col = p->clm[ci];
                        col.use = 0;
                        col.bitfields &= ~CLF_ACTIVE;
                        const struct Column *now = get_column_at(stl_x, stl_y);
                        if (!column_invalid(now) && (memcmp(now->cubes, col.cubes, sizeof(col.cubes)) == 0) &&
                            (now->floor_texture == col.floor_texture) && (now->orient == col.orient))
                            continue;
                        place_column_on_map_element(&col, stl_x, stl_y);
                        placed++;
                    }
            }
    }
    if (placed + lights > 0)
    {
        SYNCDBG(8, "World: set pieces: %d of their own columns and %d lights back on slabs (%d,%d)-(%d,%d)", placed, lights,
            (int)ex1, (int)ey1, (int)ex2, (int)ey2);
    }
}

/******************************************************************************/
void world_setpiece_survey(int64_t range)
{
    if (!sp_active())
    {
        SYNCLOG("World: set pieces: none in a world of generator version %d (from %d on)",
            (world_generator() != NULL) ? wg_world_version(world_generator()) : 0, WORLD_SETPIECES_FROM_VERSION);
        return;
    }
    sp_load_pieces();
    int regions = 0, with = 0;
    int by_piece[SP_MAX_PIECES] = {0};
    for (int64_t ry = sp_floor_div(-range, SP_REGION); ry <= sp_floor_div(range, SP_REGION); ry++)
        for (int64_t rx = sp_floor_div(-range, SP_REGION); rx <= sp_floor_div(range, SP_REGION); rx++)
        {
            regions++;
            const struct SpState *st = sp_decide(rx, ry, INT32_MIN, INT32_MIN);
            if ((st == NULL) || (st->piece == 0))
                continue;
            with++;
            const int pi = sp_find(st->piece);
            if (pi >= 0)
                by_piece[pi]++;
        }
    char text[800];
    int len = 0;
    text[0] = 0;
    for (int i = 0; (i < sp_count) && (len < (int)sizeof(text) - 48); i++)
        len += snprintf(text + len, sizeof(text) - (size_t)len, " %s %d", sp_pieces[i].name, by_piece[i]);
    SYNCLOG("World: set pieces within %lld slabs: %d of %d regions:%s; places refused for the start %d, start copies %d, "
        "keepers %d, fortresses %d, the land %d, ground made %d", (long long)range, with, regions, text,
        sp_why[SpW_Start], sp_why[SpW_Stamp], sp_why[SpW_Keeper], sp_why[SpW_Fortress], sp_why[SpW_Land], sp_why[SpW_Made]);
}

/******************************************************************************/
/* the test hook */

/** Test hook KFX_WORLD_TESTSETPIECE="T[,name]" (only read in endless worlds): the set piece nearest the start (of
 *  that name if one is given, out to 8 regions). T: its chunks load; the slabs and things on its own slabs are logged
 *  next to the piece's own counts, and how many of the land's slabs round it the ring turned to earth. T+700 (its
 *  chunks have frozen before then): they load again and are logged again. */
static int64_t spt_x0, spt_y0;
static int spt_piece = -1;

/** The engine slab of the piece's corner, found from the middle of its own slabs (the corner is the land's own ground
 *  and may not be loaded; the window is one block, so the offset holds). */
static TbBool spt_engine_origin(const struct SpPiece *p, MapSlabCoord *ex0, MapSlabCoord *ey0)
{
    const int u = (p->cx1 + p->cx2) / 2, v = (p->cy1 + p->cy2) / 2;
    MapSlabCoord ex, ey;
    if (!world_engine_slab(spt_x0 + u, spt_y0 + v, &ex, &ey))
        return false;
    *ex0 = (MapSlabCoord)(ex - u);
    *ey0 = (MapSlabCoord)(ey - v);
    return true;
}

static void spt_scan(const char *when)
{
    if ((spt_piece < 0) || (spt_piece >= sp_count))
        return;
    const struct SpPiece *p = &sp_pieces[spt_piece];
    int same = 0, differ = 0, unloaded = 0, owned = 0, cols = 0, colsame = 0;
    for (int v = 0; v < p->h; v++)
        for (int u = 0; u < p->w; u++)
        {
            if (!p->mask[v * p->w + u])
                continue;
            MapSlabCoord ex, ey;
            if (!world_engine_slab(spt_x0 + u, spt_y0 + v, &ex, &ey))
            {
                unloaded++;
                continue;
            }
            const struct SlabMap *slb = get_slabmap_block(ex, ey);
            const SlabKind want = (SlabKind)p->kind[v * p->w + u];
            if ((slb->kind == want) || (p->door[v * p->w + u] && slab_kind_is_door(slb->kind)) ||
                ((want == SlbT_EARTH) && (slb->kind == SlbT_TORCHDIRT)) || slab_kind_is_room_wall(slb->kind))
                same++;
            else
                differ++;
            if (slabmap_owner(slb) != game.neutral_player_num)
                owned++;
            if ((p->dat != NULL) && (slb->kind == want))
                for (int k = 0; k < 9; k++)
                {
                    const int ci = p->dat[(3 * v + k / 3) * (3 * p->w + 1) + 3 * u + k % 3];
                    if ((ci <= 0) || (ci >= p->nclm))
                        continue;
                    cols++;
                    const struct Column *now = get_column_at(ex * STL_PER_SLB + k % 3, ey * STL_PER_SLB + k / 3);
                    if (!column_invalid(now) && (memcmp(now->cubes, p->clm[ci].cubes, sizeof(now->cubes)) == 0))
                        colsame++;
                }
        }
    int ring = 0, ring_rock = 0;
    for (int64_t v = -SP_FRINGE; v < p->h + SP_FRINGE; v++)
        for (int64_t u = -SP_FRINGE; u < p->w + SP_FRINGE; u++)
        {
            if ((u >= 0) && (v >= 0) && (u < p->w) && (v < p->h) && p->mask[v * p->w + u])
                continue;
            const int d = p->dist[(v + SP_FRINGE) * (p->w + 2 * SP_FRINGE) + (u + SP_FRINGE)];
            if ((d == 0) || (d > sp_ring_at(spt_x0 + u, spt_y0 + v)))
                continue;
            MapSlabCoord ex, ey;
            if (!world_engine_slab(spt_x0 + u, spt_y0 + v, &ex, &ey))
                continue;
            ring++;
            const struct SlabMap *slb = get_slabmap_block(ex, ey);
            if (slb->kind == SlbT_ROCK)
                ring_rock++;
        }
    int lights_here = 0;
    {
        MapSlabCoord lx0, ly0;
        if (spt_engine_origin(p, &lx0, &ly0))
        {
            long i = game.thing_lists[TngList_StaticLights].index;
            for (unsigned long k = 0; (i > 0) && (k <= LIGHTS_COUNT); k++)
            {
                const struct Light *lgt = &game.lish.lights[i];
                i = lgt->next_in_list;
                const int u = subtile_slab(lgt->mappos.x.stl.num) - lx0, v = subtile_slab(lgt->mappos.y.stl.num) - ly0;
                if ((u >= 0) && (v >= 0) && (u < p->w) && (v < p->h) && p->mask[v * p->w + u] &&
                    (lgt->attached_slb == get_slab_number(lx0 + u, ly0 + v)))
                    lights_here++;
            }
        }
    }
    int pc = 0, po = 0, pt = 0, things[3] = {0, 0, 0};
    for (int t = 0; t < p->nthings; t++)
    {
        const int c = p->things[t].oclass;
        if (c == TCls_Creature) pc++; else if (c == TCls_Object) po++; else if (c == TCls_Trap) pt++;
    }
    MapSlabCoord ex0, ey0;
    if (spt_engine_origin(p, &ex0, &ey0))
    {
        const ThingClass classes[3] = {TCls_Creature, TCls_Object, TCls_Trap};
        for (int c = 0; c < 3; c++)
        {
            const struct StructureList *slist = get_list_for_thing_class(classes[c]);
            long guard = 0;
            for (long i = slist->index; (i > 0) && (guard++ <= (long)slist->count);)
            {
                struct Thing *thing = thing_get(i);
                if (thing_is_invalid(thing))
                    break;
                i = thing->next_of_class;
                const int u = subtile_slab(thing->mappos.x.stl.num) - ex0, v = subtile_slab(thing->mappos.y.stl.num) - ey0;
                if ((u < 0) || (v < 0) || (u >= p->w) || (v >= p->h) || !p->mask[v * p->w + u])
                    continue;
                if ((classes[c] == TCls_Object) && (thing_is_dungeon_heart(thing) || object_is_infant_food(thing)))
                    continue;
                things[c]++;
            }
        }
    }
    SYNCLOG("World test: set piece %s %s: its own slabs %d as made, %d otherwise, %d not loaded, %d owned; ring %d slabs "
        "of the land, %d still rock; columns %d of %d its own; creatures %d (the piece has %d), objects %d (%d, and room "
        "and slab objects), traps %d (%d); static lights on its slabs %d (the map has %d, and slab objects' own)", p->name,
        when, same, differ, unloaded, owned, ring, ring_rock, colsame, cols, things[0], pc, things[1], po, things[2], pt,
        lights_here, p->nlights);
}

void world_setpiece_test_turn(void)
{
    static long base = -2;
    static char want[SETPIECE_NAME_LEN];
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_TESTSETPIECE");
        base = (v != NULL) ? atol(v) : -1;
        const char *comma = (v != NULL) ? strchr(v, ',') : NULL;
        snprintf(want, sizeof(want), "%s", (comma != NULL) ? comma + 1 : "");
    }
    if ((base < 0) || !sp_active())
        return;
    const long t = (long)game.play_gameturn - base;
    if ((t == 1) || (t == 690))
    {
        /* the lights in use with the piece's chunks loaded, and once they have frozen (its lights must go with them) */
        SYNCLOG("World test: set piece: %d lights in use %s", light_count_lights(), (t == 1) ? "with its chunks loaded" :
            "with its chunks frozen");
        return;
    }
    if ((t != 0) && (t != 700))
        return;
    if (t == 700)
    {
        const struct SpPiece *p = &sp_pieces[spt_piece];
        world_load_chunks_round_world(spt_x0 + (p->cx1 + p->cx2) / 2, spt_y0 + (p->cy1 + p->cy2) / 2,
            1 + max(p->cx2 - p->cx1, p->cy2 - p->cy1) / (2 * WG_CHUNK));
        spt_scan("after its chunks froze and came back");
        return;
    }
    sp_load_pieces();
    int64_t best_d = -1;
    for (int r = 0; r <= 8; r++)
    {
        for (int ry = -r; ry <= r; ry++)
            for (int rx = -r; rx <= r; rx++)
            {
                if ((abs(rx) != r) && (abs(ry) != r))
                    continue;
                const struct SpState *st = sp_decide(rx, ry, INT32_MIN, INT32_MIN);
                if ((st == NULL) || (st->piece == 0))
                    continue;
                const int pi = sp_find(st->piece);
                if ((pi < 0) || ((want[0] != 0) && (strcmp(want, sp_pieces[pi].name) != 0)))
                    continue;
                const int64_t mx = st->x0 + sp_pieces[pi].w / 2, my = st->y0 + sp_pieces[pi].h / 2;
                const int64_t d = mx * mx + my * my;
                if ((best_d < 0) || (d < best_d))
                {
                    best_d = d;
                    spt_x0 = st->x0;
                    spt_y0 = st->y0;
                    spt_piece = pi;
                }
            }
        if (best_d >= 0)
            break;
    }
    if (best_d < 0)
    {
        SYNCLOG("World test: set piece: no %s near the start", (want[0] != 0) ? want : "piece");
        base = -1;
        return;
    }
    const struct SpPiece *p = &sp_pieces[spt_piece];
    const int n = world_load_chunks_round_world(spt_x0 + (p->cx1 + p->cx2) / 2, spt_y0 + (p->cy1 + p->cy2) / 2,
        1 + max(p->cx2 - p->cx1, p->cy2 - p->cy1) / (2 * WG_CHUNK));
    SYNCLOG("World test: set piece %s at world slab (%lld,%lld): %d chunks loaded", p->name, (long long)spt_x0,
        (long long)spt_y0, n);
    spt_scan("as found");
}

/** Test switch KFX_WORLD_SETPIECEREVEAL=T[,name] (a played session): at T the set piece nearest the start (that name if
 *  given) is decided, its ground with its blending ring and the ground between it and the start are loaded and revealed
 *  to player 0, as if seen, and this machine's camera goes to the piece. Nothing else changes. */
static TbBool spr_keep;
static int64_t spr_x1, spr_y1, spr_x2, spr_y2;

/** With KFX_WORLD_SETPIECEREVEAL: the revealed piece's ground and its ring stay loaded for the rest of the session (a
 *  played look at it; the way from the start freezes as usual). Not saved. */
void world_setpiece_want_chunks(void (*want)(int64_t wx, int64_t wy, int r))
{
    if (!spr_keep)
        return;
    for (int64_t y = spr_y1; y <= spr_y2 + 15; y += 16)
        for (int64_t x = spr_x1; x <= spr_x2 + 15; x += 16)
            want(min(x, spr_x2), min(y, spr_y2), 0);
}

void world_setpiece_reveal_turn(void)
{
    static long at = -2;
    static char want[SETPIECE_NAME_LEN];
    if (at == -2)
    {
        const char *v = getenv("KFX_WORLD_SETPIECEREVEAL");
        at = (v != NULL) ? max(2L, atol(v)) : -1;
        const char *comma = (v != NULL) ? strchr(v, ',') : NULL;
        snprintf(want, sizeof(want), "%s", (comma != NULL) ? comma + 1 : "");
    }
    if ((at < 0) || ((long)game.play_gameturn != at) || !sp_active())
        return;
    sp_load_pieces();
    int64_t best_d = -1, bx0 = 0, by0 = 0;
    int best = -1;
    for (int r = 0; (r <= 4) && (best_d < 0); r++)
        for (int ry = -r; ry <= r; ry++)
            for (int rx = -r; rx <= r; rx++)
            {
                if ((abs(rx) != r) && (abs(ry) != r))
                    continue;
                const struct SpState *st = sp_decide(rx, ry, INT32_MIN, INT32_MIN);
                if ((st == NULL) || (st->piece == 0))
                    continue;
                const int pi = sp_find(st->piece);
                if ((pi < 0) || ((want[0] != 0) && (strcmp(want, sp_pieces[pi].name) != 0)))
                    continue;
                const int64_t mx = st->x0 + sp_pieces[pi].w / 2, my = st->y0 + sp_pieces[pi].h / 2;
                const int64_t d = mx * mx + my * my;
                if ((best_d < 0) || (d < best_d))
                {
                    best_d = d;
                    bx0 = st->x0;
                    by0 = st->y0;
                    best = pi;
                }
            }
    const struct Thing *heart = find_players_dungeon_heart(PLAYER0);
    if ((best < 0) || !thing_exists(heart))
    {
        SYNCLOG("World set piece reveal: no %s near the start, or no heart", (want[0] != 0) ? want : "piece");
        return;
    }
    const struct SpPiece *p = &sp_pieces[best];
    int64_t sx, sy;
    world_slab_to_world(subtile_slab(heart->mappos.x.stl.num), subtile_slab(heart->mappos.y.stl.num), &sx, &sy);
    const int64_t x1 = min(bx0 + p->cx1, sx - 10) - SP_FRINGE - 2, y1 = min(by0 + p->cy1, sy - 10) - SP_FRINGE - 2;
    const int64_t x2 = max(bx0 + p->cx2, sx + 10) + SP_FRINGE + 2, y2 = max(by0 + p->cy2, sy + 10) + SP_FRINGE + 2;
    if ((x2 - x1 > 400) || (y2 - y1 > 400))
    {
        SYNCLOG("World set piece reveal: %s is too far to reveal the way (%ld x %ld slabs)", p->name, (long)(x2 - x1),
            (long)(y2 - y1));
        return;
    }
    int loaded = 0;
    for (int64_t y = y1 + 8; y <= y2 + 8; y += 32)
        for (int64_t x = x1 + 8; x <= x2 + 8; x += 32)
            loaded += world_load_chunks_round_world(x, y, 1);
    MapSlabCoord ex1, ey1, ex2, ey2;
    if (!world_engine_slab(x1, y1, &ex1, &ey1) || !world_engine_slab(x2, y2, &ex2, &ey2))
    {
        SYNCLOG("World set piece reveal: the box did not load (%d chunks loaded)", loaded);
        return;
    }
    reveal_map_area(PLAYER0, slab_subtile(ex1, 0), slab_subtile(ex2, 2), slab_subtile(ey1, 0), slab_subtile(ey2, 2));
    panel_map_update(slab_subtile(ex1, 0), slab_subtile(ey1, 0), slab_subtile(ex2 - ex1 + 1, 0),
        slab_subtile(ey2 - ey1 + 1, 0));
    spr_x1 = bx0 + p->cx1 - SP_FRINGE - 2;
    spr_y1 = by0 + p->cy1 - SP_FRINGE - 2;
    spr_x2 = bx0 + p->cx2 + SP_FRINGE + 2;
    spr_y2 = by0 + p->cy2 + SP_FRINGE + 2;
    spr_keep = true;
    MapSlabCoord mx, my;
    if (!netstate_dedicated() && world_engine_slab(bx0 + (p->cx1 + p->cx2) / 2, by0 + (p->cy1 + p->cy2) / 2, &mx, &my))
        world_local_camera_to(subtile_coord_center(slab_subtile_center(mx)), subtile_coord_center(slab_subtile_center(my)));
    SYNCLOG("World set piece reveal: %s at world slabs (%lld,%lld)-(%lld,%lld), %.0f slabs from the heart; world slabs "
        "(%ld,%ld)-(%ld,%ld) revealed to player 0, %d chunks loaded", p->name, (long long)(bx0 + p->cx1),
        (long long)(by0 + p->cy1), (long long)(bx0 + p->cx2), (long long)(by0 + p->cy2),
        hypot((double)(bx0 + (p->cx1 + p->cx2) / 2 - sx), (double)(by0 + (p->cy1 + p->cy2) / 2 - sy)), (long)x1, (long)y1,
        (long)x2, (long)y2, loaded);
}

/******************************************************************************/
/* the endless rules, for the scripts (world_setpiece_script.c) */
ThingModel world_setpiece_object_endless(ThingModel model)
{
    int boxes = 0, books = 0, turned = 0;
    return sp_object_rule(model, &boxes, &books, &turned);
}

SlabKind world_setpiece_slab_endless(SlabKind kind)
{
    return sp_slab_rule(kind);
}

/******************************************************************************/
/* the scripts: each placed piece with one, every SP_SCRIPT_EVERY turns while its ground is loaded */
#define SP_SCRIPT_EVERY 4

void world_setpieces_turn(void)
{
    if (!sp_active() || (sp_state_count <= 0) || ((game.play_gameturn % SP_SCRIPT_EVERY) != 0))
        return;
    for (int n = 0; n < sp_state_count; n++)
    {
        struct SpState *st = &sp_states[n];
        const int pi = (st->piece != 0) ? sp_find(st->piece) : -1;
        if ((pi < 0) || (sp_pieces[pi].script == NULL))
            continue;
        const struct SpPiece *p = &sp_pieces[pi];
        MapSlabCoord ex, ey;
        if (!world_engine_slab(st->x0 + (p->cx1 + p->cx2) / 2, st->y0 + (p->cy1 + p->cy2) / 2, &ex, &ey))
            continue; /* its ground is not loaded: it waits */
        if (st->run == NULL)
        {
            st->run = (struct SpScriptRun *)calloc(1, sizeof(struct SpScriptRun));
            if (st->run == NULL)
                continue;
        }
        struct SpScriptPlace pl;
        memset(&pl, 0, sizeof(pl));
        pl.name = p->name;
        pl.x0 = st->x0;
        pl.y0 = st->y0;
        pl.w = p->w;
        pl.h = p->h;
        pl.aps = p->aps;
        pl.naps = p->naps;
        pl.gates = p->gates;
        pl.ngates = p->ngates;
        for (int k = 0; k < SPS_PLAYERS; k++)
            pl.keeper_of[k] = -1;
        for (int k = 0; k < p->nkeepers; k++)
        {
            const struct SpKeeper *pk = &p->keepers[k];
            if (pk->player >= SPS_PLAYERS)
                continue;
            pl.heart_x[pk->player] = st->x0 + pk->hx;
            pl.heart_y[pk->player] = st->y0 + pk->hy;
            MapSlabCoord hx, hy;
            if (world_engine_slab(pl.heart_x[pk->player], pl.heart_y[pk->player], &hx, &hy))
            {
                const PlayerNumber owner = slabmap_owner(get_slabmap_block(hx, hy));
                if ((owner != game.neutral_player_num) && (owner != PLAYER_GOOD))
                    pl.keeper_of[pk->player] = owner;
            }
        }
        sp_script_run(p->script, &pl, st->run);
    }
}

/* saves: "SPS1" in XTR2: count, then per region whose piece's script has run: rx, ry (int64), its state */
#define SP_RUN_SAVED (16 + (size_t)sizeof(struct SpScriptRun))
size_t world_setpiece_scripts_save_size(void)
{
    size_t n = 0;
    for (int i = 0; i < sp_state_count; i++)
        n += (sp_states[i].run != NULL) ? 1 : 0;
    return (n > 0) ? 4 + n * SP_RUN_SAVED : 0;
}

void world_setpiece_scripts_save(uint8_t *buf)
{
    uint32_t n = 0;
    uint8_t *b = buf + 4;
    for (int i = 0; i < sp_state_count; i++)
    {
        if (sp_states[i].run == NULL)
            continue;
        memcpy(b, &sp_states[i].rx, 8);
        memcpy(b + 8, &sp_states[i].ry, 8);
        memcpy(b + 16, sp_states[i].run, sizeof(struct SpScriptRun));
        b += SP_RUN_SAVED;
        n++;
    }
    memcpy(buf, &n, 4);
}

/** (read after SPC2, which has made the regions) */
TbBool world_setpiece_scripts_load(const uint8_t *buf, size_t len)
{
    uint32_t n;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if (n > (len - 4) / SP_RUN_SAVED)
        return false;
    for (uint32_t i = 0; i < n; i++)
    {
        const uint8_t *b = buf + 4 + (size_t)i * SP_RUN_SAVED;
        int64_t rx, ry;
        memcpy(&rx, b, 8);
        memcpy(&ry, b + 8, 8);
        struct SpState *st = sp_state(rx, ry, false);
        if (st == NULL)
            continue;
        if (st->run == NULL)
            st->run = (struct SpScriptRun *)calloc(1, sizeof(struct SpScriptRun));
        if (st->run != NULL)
            memcpy(st->run, b + 16, sizeof(struct SpScriptRun));
    }
    return true;
}

void world_setpiece_scripts_reset(void)
{
    /* (the runs go with the regions: world_setpieces_reset) */
}

/******************************************************************************/
/* saves: "SPC2" in XTR2: count, then per decided region rx, ry (int64), the piece's hash (0: none), x0, y0 (int64) */
#define SP_SAVED 36
size_t world_setpieces_save_size(void)
{
    return (sp_state_count > 0) ? 4 + (size_t)sp_state_count * SP_SAVED : 0;
}

void world_setpieces_save(uint8_t *buf)
{
    const uint32_t n = (uint32_t)sp_state_count;
    memcpy(buf, &n, 4);
    buf += 4;
    for (int i = 0; i < sp_state_count; i++)
    {
        memcpy(buf, &sp_states[i].rx, 8);
        memcpy(buf + 8, &sp_states[i].ry, 8);
        memcpy(buf + 16, &sp_states[i].piece, 4);
        memcpy(buf + 20, &sp_states[i].x0, 8);
        memcpy(buf + 28, &sp_states[i].y0, 8);
        buf += SP_SAVED;
    }
}

TbBool world_setpieces_load(const uint8_t *buf, size_t len)
{
    world_setpieces_reset();
    uint32_t n;
    if (len < 4)
        return false;
    memcpy(&n, buf, 4);
    if (n > (len - 4) / SP_SAVED)
        return false;
    for (uint32_t i = 0; i < n; i++)
    {
        const uint8_t *b = buf + 4 + (size_t)i * SP_SAVED;
        int64_t rx, ry;
        memcpy(&rx, b, 8);
        memcpy(&ry, b + 8, 8);
        struct SpState *st = sp_state(rx, ry, true);
        if (st == NULL)
            return false;
        memcpy(&st->piece, b + 16, 4);
        memcpy(&st->x0, b + 20, 8);
        memcpy(&st->y0, b + 28, 8);
    }
    return true;
}

#ifdef __cplusplus
}
#endif
