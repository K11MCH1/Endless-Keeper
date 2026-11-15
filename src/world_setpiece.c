/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_setpiece.c
 *     Endless world: set pieces (see world_setpiece.h).
 * @par Purpose:
 *     Roadmap page "Set Pieces": "Places drawn by hand, set down in a generated world. Small pieces are made in the
 *     map editor, each for a biome, a circle and a size, and the world places them in its caves and strongholds."
 *     "Any one piece is rare in a world, and it is turned or mirrored when it is placed."
 *
 *     Decided (2026-11-13, the page's open point "how many pieces to begin with, and how often a world uses one"):
 *     eight pieces to begin with, drawn fresh after the page's examples (not copied from the original campaign's
 *     maps): a chessboard of traps with special boxes, a boulder run, a one-slab corridor of Words of Power, a cell
 *     block with a Horned Reaper in the last cell, a causeway over lava, a vault ringed by water, a forgotten shrine
 *     with a Dark Mistress waiting, and a guarded treasure room. The world is cut into 112x112-slab regions; 30% of
 *     them hold one piece (about one per 200x200 slabs), at the first of four seeded places in the region that is
 *     clear (as the heroes outposts keep off the generator's structures, start copies and outposts, and in version 7
 *     off Dead Man's Land and the oceans), at least 80 slabs from the world's start. The piece is picked by weight
 *     among those whose biomes and circles allow the place, turned (0-270 degrees) and mirrored by seed. A region is
 *     decided the first time a chunk it may cover is generated and only if none of its ground was made before;
 *     what was decided (the piece by its name's hash, how it is turned, which place) is saved: XTR2 record "SPC1".
 *
 *     Pieces as text (the built-in ones below, and any file levels/endless_cfgs/setpieces/<name>.txt, read in name
 *     order; a file whose NAME is taken already is left out). Lines:
 *       NAME <word>                  needed; its hash with the text's is what a save keeps
 *       BIOMES all | <biome> ...     worldgen.py's names (earthen, rocky, volcanic, ...); default all
 *       CIRCLES <from> <to>          1-9; default 1 9
 *       WEIGHT <n>                   default 1
 *       MAP ... END                  the rows, all as long, at most 31x31
 *       TRAP <x> <y> <name>          trapdoor.cfg's names; nobody's (they go off for any creature)
 *       OBJECT <x> <y> <name>        objects.cfg's names
 *       CREATURE <x> <y> <name> <level> [HEROES]   nobody's unless HEROES
 *       DOOR <x> <y> <name>          the door on a D cell (default BRACED); nobody's
 *       ; comment
 *     Map characters: ' ' the land's own (rock becomes earth), R rock, E earth, G gold, . claimed floor, , dirt path,
 *     # wall, L lava, W water, H hard floor, D a door on claimed floor; rooms, nobody's (claimable): p prison,
 *     x torture, l library, k training, y graveyard, z temple, $ treasure, h hatchery, b barracks, w workshop,
 *     s scavenger. x, y count from the map's top left corner, before it is turned.
 *     Round the piece the land keeps its own ground with rock turned to earth (the 31x31 place and 10 slabs of rock to
 *     earth from each of its corners), so it can always be dug to.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_setpiece.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>

#include "world_outpost.h"
#include "world_stream.h"
#include "world_gen.h"
#include "world_circles.h"
#include "bflib_basics.h"
#include "bflib_fileio.h"
#include "config.h"
#include "config_creature.h"
#include "config_objects.h"
#include "config_terrain.h"
#include "config_trapdoor.h"
#include "game_legacy.h"
#include "map_data.h"
#include "player_data.h"
#include "player_instances.h"
#include "room_data.h"
#include "slab_data.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_objects.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define SP_MAX 31               /* a piece's largest side; its place is SP_MAX x SP_MAX */
#define SP_REGION 112
#define SP_CHANCE 30
#define SP_SITES 4
#define SP_START_DIST 80
#define SP_MARGIN 4
#define SP_FT_MARGIN 26
#define SP_EXIT 10
#define SP_MAX_THINGS 48
#define SP_MAX_PIECES 64

struct SpThing {
    uint8_t x, y, oclass, level, heroes;
    uint16_t model;
};

struct SpPiece {
    char name[32];
    uint32_t hash;
    int w, h;
    char cell[SP_MAX * SP_MAX];
    uint16_t door[SP_MAX * SP_MAX];
    uint32_t biomes;            /* bit per biome; 0: all */
    int cmin, cmax, weight;
    int nthings;
    struct SpThing things[SP_MAX_THINGS];
};

static struct SpPiece *sp_pieces;
static int sp_count;
static TbBool sp_loaded;

struct SpState {
    int64_t rx, ry;
    uint32_t piece;             /* its name's hash; 0: none */
    uint8_t site, turn;         /* turn: bits 0-1 the quarter turns, bit 2 mirrored */
};
static struct SpState *sp_states;
static int sp_state_count, sp_state_cap;

/******************************************************************************/
/* the pieces to begin with (drawn for Endless Keeper, 2026-11-13) */
static const char *const sp_builtin[] = {
"NAME chessboard\nWEIGHT 3\nCIRCLES 1 9\nMAP\n"
"######D######\n"
"#...........#\n"
"#.H.H.H.H.H.#\n"
"#H.H.H.H.H.H#\n"
"#.H.H.H.H.H.#\n"
"#H.H.H.H.H.H#\n"
"#.H.H.H.H.H.#\n"
"#H.H.H.H.H.H#\n"
"#.H.H.H.H.H.#\n"
"#H.H.H.H.H.H#\n"
"#.H.H.H.H.H.#\n"
"#...........#\n"
"#############\nEND\n"
"TRAP 2 2 LIGHTNING\nTRAP 6 4 POISON_GAS\nTRAP 10 2 LIGHTNING\nTRAP 4 6 WORD_OF_POWER\nTRAP 8 8 POISON_GAS\n"
"TRAP 3 9 LIGHTNING\nTRAP 9 9 WORD_OF_POWER\nTRAP 6 10 LIGHTNING\n"
"OBJECT 3 10 SPECBOX_INCLEV\nOBJECT 9 10 SPECBOX_MKSAFE\nOBJECT 6 6 GOLD_CHEST\n",

"NAME boulder_run\nWEIGHT 3\nCIRCLES 1 9\nMAP\n"
"###D###\n"
"##...##\n" "##...##\n" "##...##\n" "##...##\n" "##...##\n" "##...##\n" "##...##\n" "##...##\n"
"##...##\n" "##...##\n" "##...##\n" "##...##\n" "##...##\n" "##...##\n" "##...##\n" "##...##\n"
"##...##\n" "##...##\n" "##...##\n" "##...##\n"
"#.....#\n"
"#.....#\n"
"#######\nEND\n"
"TRAP 2 20 BOULDER\nTRAP 3 20 BOULDER\nTRAP 4 20 BOULDER\nTRAP 3 10 ALARM\n"
"OBJECT 1 22 GOLD_CHEST\nOBJECT 3 22 GOLD_CHEST\nOBJECT 5 22 GOLD_CHEST\nOBJECT 3 21 SPECBOX_REVMAP\n",

"NAME word_corridor\nWEIGHT 2\nCIRCLES 2 9\nMAP\n"
"##D##\n"
"##.##\n" "##.##\n" "##.##\n" "##.##\n" "##.##\n" "##.##\n" "##.##\n" "##.##\n" "##.##\n" "##.##\n"
"##.##\n" "##.##\n" "##.##\n" "##.##\n" "##.##\n" "##.##\n" "##.##\n" "##.##\n" "##.##\n" "##.##\n"
"#...#\n"
"#...#\n"
"#####\nEND\n"
"TRAP 2 3 WORD_OF_POWER\nTRAP 2 7 WORD_OF_POWER\nTRAP 2 11 WORD_OF_POWER\nTRAP 2 15 WORD_OF_POWER\n"
"TRAP 2 19 WORD_OF_POWER\n"
"OBJECT 2 22 SPECBOX_INCLEV\nOBJECT 1 21 GOLD_CHEST\nOBJECT 3 21 GOLD_CHEST\n",

"NAME cell_block\nWEIGHT 2\nCIRCLES 2 9\nMAP\n"
"#################\n"
"D...............#\n"
"##D###D###D###D##\n"
"#ppp#ppp#ppp#ppp#\n"
"#ppp#ppp#ppp#ppp#\n"
"#ppp#ppp#ppp#ppp#\n"
"#################\nEND\n"
"TRAP 5 1 LIGHTNING\nTRAP 11 1 LIGHTNING\n"
"CREATURE 2 4 SKELETON 3\nCREATURE 6 4 GHOST 3\nCREATURE 10 4 SKELETON 4\nCREATURE 14 4 HORNY 4\n"
"OBJECT 1 5 GRAVE_STONE\nOBJECT 9 5 GRAVE_STONE\n",

"NAME lava_causeway\nWEIGHT 2\nBIOMES volcanic lava_rivers lava_ocean rocky earthen\nCIRCLES 1 9\nMAP\n"
"###D###\n"
"LLL.LLL\n" "LLL.LLL\n" "LLL.LLL\n" "LLL.LLL\n" "LLL.LLL\n" "LLL.LLL\n" "LLL.LLL\n" "LLL.LLL\n" "LLL.LLL\n"
"LLL.LLL\n" "LLL.LLL\n" "LLL.LLL\n" "LLL.LLL\n" "LLL.LLL\n"
"#.....#\n"
"#.....#\n"
"#######\nEND\n"
"TRAP 3 4 LIGHTNING\nTRAP 3 9 LIGHTNING\nTRAP 3 13 POISON_GAS\n"
"OBJECT 1 16 GOLD_CHEST\nOBJECT 5 16 GOLD_CHEST\nOBJECT 3 16 SPECBOX_HEALALL\n",

"NAME water_vault\nWEIGHT 2\nBIOMES flooded flooded_rivers flooded_ocean frozen earthen sandstone\nCIRCLES 1 9\nMAP\n"
"######D######\n"
"#...........#\n"
"#.WWWWWWWWW.#\n"
"#.WWWWWWWWW.#\n"
"#.WW.....WW.#\n"
"#.WW.....WW.#\n"
"#.WW.....WW.#\n"
"#.WW.....WW.#\n"
"#.WW.....WW.#\n"
"#.WWWWWWWWW.#\n"
"#.WWWWWWWWW.#\n"
"#...........#\n"
"#############\nEND\n"
"TRAP 4 6 POISON_GAS\nTRAP 8 6 POISON_GAS\n"
"OBJECT 6 6 SPECBOX_STEALHR\nOBJECT 5 5 GOLD_CHEST\nOBJECT 7 7 GOLD_CHEST\n",

"NAME forgotten_shrine\nWEIGHT 2\nCIRCLES 2 9\nMAP\n"
"#####D#####\n"
"#.........#\n"
"#.........#\n"
"#...,,,...#\n"
"#..,zzz,..#\n"
"#..,zzz,..#\n"
"#..,zzz,..#\n"
"#...,,,...#\n"
"#.........#\n"
"#.........#\n"
"###########\nEND\n"
"CREATURE 5 5 DARK_MISTRESS 5\n"
"OBJECT 2 2 CANDLESTCK\nOBJECT 8 2 CANDLESTCK\nOBJECT 2 8 CANDLESTCK\nOBJECT 8 8 CANDLESTCK\n"
"OBJECT 5 2 STATUE3\nOBJECT 5 8 STATUE4\n",

"NAME guarded_hoard\nWEIGHT 3\nCIRCLES 1 9\nMAP\n"
"####D####\n"
"#.......#\n"
"#.$$$$$.#\n"
"#.$$$$$.#\n"
"#.$$$$$.#\n"
"#.$$$$$.#\n"
"#.$$$$$.#\n"
"#.......#\n"
"#########\nEND\n"
"TRAP 1 1 POISON_GAS\nTRAP 7 1 POISON_GAS\nTRAP 1 7 LIGHTNING\nTRAP 7 7 LIGHTNING\nTRAP 4 1 ALARM\n"
"OBJECT 3 3 GOLD_HOARD_3\nOBJECT 5 5 GOLD_HOARD_4\nOBJECT 4 4 GOLD_HOARD_5\nOBJECT 3 5 GOLD_HOARD_2\nOBJECT 5 3 GOLD_HOARD_2\n",
};

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

static uint32_t sp_hash(const char *s)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return (h == 0) ? 1 : h;
}

/******************************************************************************/
/* reading pieces */
static TbBool sp_map_char(char c)
{
    return (strchr(" REG.,#LWHDpxlkyz$hbws", c) != NULL) && (c != 0);
}

/** Parses one piece's text; false (and a warning naming where) when it is not right. */
static TbBool sp_parse(const char *text, const char *where, struct SpPiece *p)
{
    memset(p, 0, sizeof(*p));
    p->cmin = 1;
    p->cmax = 9;
    p->weight = 1;
    memset(p->cell, ' ', sizeof(p->cell));
    TbBool in_map = false, map_done = false;
    int line_no = 0;
    const char *s = text;
    while (*s)
    {
        const char *e = strchr(s, '\n');
        size_t n = (e != NULL) ? (size_t)(e - s) : strlen(s);
        char line[256];
        if (n >= sizeof(line))
            n = sizeof(line) - 1;
        memcpy(line, s, n);
        line[n] = 0;
        while ((n > 0) && ((line[n - 1] == '\r') || (!in_map && (line[n - 1] == ' '))))
            line[--n] = 0;
        s = (e != NULL) ? e + 1 : s + strlen(s);
        line_no++;
        if (in_map)
        {
            if (strcmp(line, "END") == 0)
            {
                in_map = false;
                map_done = true;
                continue;
            }
            const int len = (int)strlen(line);
            if ((p->h >= SP_MAX) || (len > SP_MAX) || ((p->w > 0) && (len != p->w)))
            {
                WARNLOG("World: set piece %s line %d: map rows must be as long as the first, at most %d, and at most %d rows",
                    where, line_no, SP_MAX, SP_MAX);
                return false;
            }
            for (int x = 0; x < len; x++)
                if (!sp_map_char(line[x]))
                {
                    WARNLOG("World: set piece %s line %d: unknown map character '%c'", where, line_no, line[x]);
                    return false;
                }
            p->w = len;
            memcpy(p->cell + p->h * SP_MAX, line, (size_t)len);
            p->h++;
            continue;
        }
        if ((line[0] == 0) || (line[0] == ';'))
            continue;
        char word[32] = "", a[64] = "", b[64] = "", c[64] = "", d[64] = "", f[64] = "";
        const int got = sscanf(line, "%31s %63s %63s %63s %63s %63s", word, a, b, c, d, f);
        if (strcmp(word, "NAME") == 0)
            snprintf(p->name, sizeof(p->name), "%s", a);
        else if (strcmp(word, "WEIGHT") == 0)
            p->weight = max(0, atoi(a));
        else if (strcmp(word, "CIRCLES") == 0)
        {
            p->cmin = max(1, atoi(a));
            p->cmax = (got >= 3) ? min(9, atoi(b)) : p->cmin;
        }
        else if (strcmp(word, "BIOMES") == 0)
        {
            const char *q = line + strlen("BIOMES");
            char nm[32];
            int used;
            while (sscanf(q, "%31s%n", nm, &used) == 1)
            {
                q += used;
                if (strcmp(nm, "all") == 0)
                {
                    p->biomes = 0;
                    break;
                }
                TbBool known = false;
                for (int bi = 0; bi < WgB_COUNT; bi++)
                    if (strcmp(nm, wg_biome_name(bi)) == 0)
                    {
                        p->biomes |= 1u << bi;
                        known = true;
                    }
                if (!known)
                    WARNLOG("World: set piece %s line %d: unknown biome %s", where, line_no, nm);
            }
        }
        else if (strcmp(word, "MAP") == 0)
            in_map = true;
        else if ((strcmp(word, "TRAP") == 0) || (strcmp(word, "OBJECT") == 0) || (strcmp(word, "CREATURE") == 0) ||
            (strcmp(word, "DOOR") == 0))
        {
            const int x = atoi(a), y = atoi(b);
            if (!map_done || (got < 4) || (x < 0) || (y < 0) || (x >= p->w) || (y >= p->h))
            {
                WARNLOG("World: set piece %s line %d: %s needs x y name inside the map (after MAP ... END)", where, line_no, word);
                return false;
            }
            if (strcmp(word, "DOOR") == 0)
            {
                const long m = get_id(door_desc, c);
                if ((m <= 0) || (p->cell[y * SP_MAX + x] != 'D'))
                {
                    WARNLOG("World: set piece %s line %d: DOOR needs a door name on a D cell", where, line_no);
                    return false;
                }
                p->door[y * SP_MAX + x] = (uint16_t)m;
                continue;
            }
            if (p->nthings >= SP_MAX_THINGS)
            {
                WARNLOG("World: set piece %s line %d: more than %d things", where, line_no, SP_MAX_THINGS);
                return false;
            }
            struct SpThing *t = &p->things[p->nthings];
            memset(t, 0, sizeof(*t));
            t->x = (uint8_t)x;
            t->y = (uint8_t)y;
            long m;
            if (strcmp(word, "TRAP") == 0)
            {
                t->oclass = TCls_Trap;
                m = get_id(trap_desc, c);
            } else if (strcmp(word, "OBJECT") == 0)
            {
                t->oclass = TCls_Object;
                m = get_id(object_desc, c);
                /* boxes endless worlds don't have (no Resurrect, Transfer, Hidden World or Get Gold since 2026-11-08),
                   boxes that harm their finder, and hearts: a file piece may not lay them */
                static const char *const refused[] = {"SPECBOX_RESURCT", "SPECBOX_TRANSFR", "SPECBOX_HIDNWRL",
                    "SPECBOX_GETGOLD", "SPECBOX_MKANGRY", "SPECBOX_MKUNSAFE", "SPECBOX_CUSTOM", "SOUL_CONTAINER",
                    "HEARTFLAME_RED", "HEARTFLAME_BLUE", "HEARTFLAME_GREEN", "HEARTFLAME_YELLOW", "HEARTFLAME_WHITE",
                    "HEARTFLAME_PURPLE", "HEARTFLAME_BLACK", "HEARTFLAME_ORANGE"};
                for (int r = 0; (r < (int)(sizeof(refused) / sizeof(refused[0]))) && (m > 0); r++)
                    if (get_id(object_desc, refused[r]) == m) /* (by model: names are read without regard to case) */
                    {
                        WARNLOG("World: set piece %s line %d: %s can't be in a set piece", where, line_no, c);
                        return false;
                    }
            } else
            {
                t->oclass = TCls_Creature;
                m = get_id(creature_desc, c);
                t->level = (uint8_t)max(1, min(10, atoi(d)));
                t->heroes = (got >= 6) && (strcmp(f, "HEROES") == 0);
            }
            if (m <= 0)
            {
                WARNLOG("World: set piece %s line %d: unknown %s %s", where, line_no, word, c);
                return false;
            }
            t->model = (uint16_t)m;
            p->nthings++;
        }
        else
        {
            WARNLOG("World: set piece %s line %d: unknown line \"%s\"", where, line_no, line);
            return false;
        }
    }
    if ((p->name[0] == 0) || !map_done || (p->w <= 0) || (p->h <= 0) || (p->cmin > p->cmax))
    {
        WARNLOG("World: set piece %s: needs a NAME, a MAP ... END and CIRCLES from <= to", where);
        return false;
    }
    /* the piece as a save knows it: its name and its whole text, so a file edited under the same name is another piece
       (a region decided with the old text builds nothing more of it, as for a removed file; no half old, half new) */
    p->hash = sp_hash(p->name) ^ (sp_hash(text) * 0x9E3779B1u);
    if (p->hash == 0)
        p->hash = 1;
    return true;
}

static int sp_find(uint32_t hash)
{
    for (int i = 0; i < sp_count; i++)
        if (sp_pieces[i].hash == hash)
            return i;
    return -1;
}

static void sp_add(const char *text, const char *where)
{
    if (sp_count >= SP_MAX_PIECES)
        return;
    if (sp_pieces == NULL)
    {
        sp_pieces = (struct SpPiece *)calloc(SP_MAX_PIECES, sizeof(struct SpPiece));
        if (sp_pieces == NULL)
            return;
    }
    struct SpPiece *p = &sp_pieces[sp_count];
    if (!sp_parse(text, where, p))
        return;
    TbBool taken = false;
    for (int i = 0; i < sp_count; i++)
        taken = taken || (strcmp(sp_pieces[i].name, p->name) == 0);
    if (taken)
    {
        WARNLOG("World: set piece %s: the name %s is taken already: left out", where, p->name);
        return;
    }
    sp_count++;
}

static int sp_name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void sp_load_pieces(void)
{
    if (sp_loaded)
        return;
    sp_loaded = true;
    sp_count = 0;
    for (size_t i = 0; i < sizeof(sp_builtin) / sizeof(sp_builtin[0]); i++)
        sp_add(sp_builtin[i], "(built in)");
    const int builtin = sp_count;
    /* the files, in name order (the same on every machine whatever order the folder lists them in) */
    char *names[SP_MAX_PIECES];
    int nnames = 0;
    struct TbFileEntry fe;
    struct TbFileFind *ff = LbFileFindFirst(prepare_file_path(FGrp_VarLevels, "endless_cfgs/setpieces/*.txt"), &fe);
    if (ff != NULL)
    {
        do {
            if (nnames < SP_MAX_PIECES)
            {
                names[nnames] = strdup(fe.Filename);
                if (names[nnames] != NULL)
                    nnames++;
            }
        } while (LbFileFindNext(ff, &fe) >= 0);
        LbFileFindEnd(ff);
    }
    qsort(names, (size_t)nnames, sizeof(names[0]), sp_name_cmp);
    for (int i = 0; i < nnames; i++)
    {
        char rel[256];
        snprintf(rel, sizeof(rel), "endless_cfgs/setpieces/%s", names[i]);
        const char *path = prepare_file_path(FGrp_VarLevels, rel);
        FILE *fh = fopen(path, "rb");
        if (fh != NULL)
        {
            static char buf[16384];
            const size_t len = fread(buf, 1, sizeof(buf) - 1, fh);
            fclose(fh);
            buf[len] = 0;
            sp_add(buf, names[i]);
        }
        free(names[i]);
    }
    SYNCLOG("World: %d set pieces (%d built in, %d from files)", sp_count, builtin, sp_count - builtin);
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
    free(sp_states);
    sp_states = NULL;
    sp_state_count = sp_state_cap = 0;
    sp_loaded = false; /* read again (a file may have come) */
}

/******************************************************************************/
/* where */
static TbBool sp_region_rng(int64_t rx, int64_t ry, struct SpRng *rng)
{
    struct WgWorld *gen = world_generator();
    if (gen == NULL)
        return false;
    rng->s = wg_world_seed(gen) ^ sp_mix((uint64_t)rx * 0x2127599BF4325C37ULL) ^ sp_mix((uint64_t)ry * 0x880355F21E6D1965ULL + 0x53455450ULL);
    return sp_rand(rng, 100) < SP_CHANCE;
}

/** A region's place number `site` (the corner of its SP_MAX x SP_MAX place). */
static TbBool sp_site(int64_t rx, int64_t ry, int site, int64_t *x0, int64_t *y0)
{
    struct SpRng rng;
    if (!sp_region_rng(rx, ry, &rng))
        return false;
    const int span = SP_REGION - SP_MAX - 2 * (SP_EXIT + 2);
    for (int k = 0; k <= site; k++)
    {
        *x0 = rx * SP_REGION + SP_EXIT + 2 + sp_rand(&rng, span);
        *y0 = ry * SP_REGION + SP_EXIT + 2 + sp_rand(&rng, span);
    }
    const int64_t dx = *x0 + SP_MAX / 2 - 8, dy = *y0 + SP_MAX / 2 - 8;
    return dx * dx + dy * dy >= (int64_t)SP_START_DIST * SP_START_DIST;
}

static void sp_extent(int64_t x0, int64_t y0, int64_t *x1, int64_t *y1, int64_t *x2, int64_t *y2)
{
    *x1 = x0 - SP_EXIT;
    *y1 = y0;
    *x2 = x0 + SP_MAX - 1 + SP_EXIT;
    *y2 = y0 + SP_MAX - 1;
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

static TbBool sp_place_clear(int64_t x0, int64_t y0)
{
    struct WgWorld *gen = world_generator();
    struct WgStructRef refs[2];
    const int64_t x1 = x0, y1 = y0, x2 = x0 + SP_MAX - 1, y2 = y0 + SP_MAX - 1;
    const unsigned mask = (1u << WgT_Keeper) | (1u << WgT_Portal) | (1u << WgT_Reward);
    if (wg_structures_in(gen, x1 - SP_MARGIN, y1 - SP_MARGIN, x2 + SP_MARGIN, y2 + SP_MARGIN, mask, refs, 2) > 0)
        return false;
    if (wg_structures_in(gen, x1 - SP_FT_MARGIN, y1 - SP_FT_MARGIN, x2 + SP_FT_MARGIN, y2 + SP_FT_MARGIN,
        1u << WgT_Fortress, refs, 2) > 0)
        return false;
    if (world_stamps_overlap(x1 - SP_MARGIN, y1 - SP_MARGIN, x2 + SP_MARGIN, y2 + SP_MARGIN))
        return false;
    if (world_outpost_in_box(x1 - SP_MARGIN, y1 - SP_MARGIN, x2 + SP_MARGIN, y2 + SP_MARGIN))
        return false;
    return world_land_fits(x1, y1, x2, y2, false);
}

/** The piece for a place: by weight among those its biome and circle allow; -1 none. */
static int sp_pick(int64_t rx, int64_t ry, int64_t mx, int64_t my, uint8_t *turn)
{
    struct WgWorld *gen = world_generator();
    const int biome = wg_biome_at(gen, mx, my);
    const int circle = world_circle_at_world(mx, my);
    int total = 0;
    for (int i = 0; i < sp_count; i++)
    {
        const struct SpPiece *p = &sp_pieces[i];
        if ((p->biomes != 0) && ((biome < 0) || (biome >= 32) || !(p->biomes & (1u << biome))))
            continue;
        if ((circle < p->cmin) || (circle > p->cmax))
            continue;
        total += p->weight;
    }
    if (total <= 0)
        return -1;
    struct SpRng rng;
    rng.s = wg_world_seed(gen) ^ sp_mix((uint64_t)rx * 0x9E3779B1ULL + 0x50494543ULL) ^ sp_mix((uint64_t)ry * 0x85EBCA77ULL);
    int r = sp_rand(&rng, total);
    *turn = (uint8_t)sp_rand(&rng, 8);
    for (int i = 0; i < sp_count; i++)
    {
        const struct SpPiece *p = &sp_pieces[i];
        if ((p->biomes != 0) && ((biome < 0) || (biome >= 32) || !(p->biomes & (1u << biome))))
            continue;
        if ((circle < p->cmin) || (circle > p->cmax))
            continue;
        if (r < p->weight)
            return i;
        r -= p->weight;
    }
    return -1;
}

static struct SpState *sp_decide(int64_t rx, int64_t ry, int32_t gen_cx, int32_t gen_cy)
{
    struct SpState *st = sp_state(rx, ry, false);
    if (st != NULL)
        return st;
    int64_t x0, y0;
    TbBool any = false;
    for (int k = 0; k < SP_SITES; k++)
        any |= sp_site(rx, ry, k, &x0, &y0);
    if (!any)
        return NULL;
    sp_load_pieces();
    st = sp_state(rx, ry, true);
    if (st == NULL)
        return NULL;
    int site = -1;
    for (int k = 0; (k < SP_SITES) && (site < 0); k++)
    {
        int64_t x1, y1, x2, y2;
        if (!sp_site(rx, ry, k, &x0, &y0))
            continue;
        sp_extent(x0, y0, &x1, &y1, &x2, &y2);
        if (!sp_box_made(x1, y1, x2, y2, gen_cx, gen_cy) && sp_place_clear(x0, y0))
            site = k;
    }
    st = sp_state(rx, ry, false);
    if (site < 0)
        return st; /* piece 0: none */
    uint8_t turn = 0;
    const int pi = sp_pick(rx, ry, x0 + SP_MAX / 2, y0 + SP_MAX / 2, &turn);
    if (pi < 0)
        return st;
    st->site = (uint8_t)site;
    st->piece = sp_pieces[pi].hash;
    st->turn = turn;
    SYNCLOG("World: set piece %s at world slab (%lld,%lld), turned %d, %smirrored", sp_pieces[pi].name,
        (long long)(x0 + SP_MAX / 2), (long long)(y0 + SP_MAX / 2), 90 * (turn & 3), (turn & 4) ? "" : "not ");
    return st;
}

/******************************************************************************/
/* turned and mirrored */

/** The size of a piece as placed. */
static void sp_turned_size(const struct SpPiece *p, uint8_t turn, int *w, int *h)
{
    const TbBool across = (turn & 1) != 0;
    *w = across ? p->h : p->w;
    *h = across ? p->w : p->h;
}

/** The piece's own cell under a placed cell (u, v). */
static void sp_source(const struct SpPiece *p, uint8_t turn, int u, int v, int *x, int *y)
{
    int w, h;
    sp_turned_size(p, turn, &w, &h);
    if (turn & 4)
        u = w - 1 - u;
    switch (turn & 3)
    {
    case 0: *x = u; *y = v; break;
    case 1: *x = v; *y = p->h - 1 - u; break;
    case 2: *x = p->w - 1 - u; *y = p->h - 1 - v; break;
    default: *x = p->w - 1 - v; *y = u; break;
    }
}

/** The placed cell of a piece's own cell (x, y). */
static void sp_placed(const struct SpPiece *p, uint8_t turn, int x, int y, int *u, int *v)
{
    int w, h;
    sp_turned_size(p, turn, &w, &h);
    switch (turn & 3)
    {
    case 0: *u = x; *v = y; break;
    case 1: *u = p->h - 1 - y; *v = x; break;
    case 2: *u = p->w - 1 - x; *v = p->h - 1 - y; break;
    default: *u = y; *v = p->w - 1 - x; break;
    }
    if (turn & 4)
        *u = w - 1 - *u;
}

/******************************************************************************/
/* the chunks */
static SlabKind sp_kind(char c)
{
    switch (c)
    {
    case 'R': return SlbT_ROCK;
    case 'E': return SlbT_EARTH;
    case 'G': return SlbT_GOLD;
    case '.': case 'D': return SlbT_CLAIMED;
    case ',': return SlbT_PATH;
    case '#': return SlbT_WALLDRAPE;
    case 'L': return SlbT_LAVA;
    case 'W': return SlbT_WATER;
    case 'H': return SlbT_ROCK_FLOOR;
    case 'p': return SlbT_PRISON;
    case 'x': return SlbT_TORTURE;
    case 'l': return SlbT_LIBRARY;
    case 'k': return SlbT_TRAINING;
    case 'y': return SlbT_GRAVEYARD;
    case 'z': return SlbT_TEMPLE;
    case '$': return SlbT_TREASURE;
    case 'h': return SlbT_GARDEN;
    case 'b': return SlbT_BARRACKS;
    case 'w': return SlbT_WORKSHOP;
    case 's': return SlbT_SCAVENGER;
    default: return SlbT_EARTH;
    }
}

static void sp_thing(struct OutpostOverlay *ov, int32_t cx, int32_t cy, int64_t wx, int64_t wy, const struct SpThing *t)
{
    if ((wx < (int64_t)cx * WG_CHUNK) || (wx >= (int64_t)cx * WG_CHUNK + WG_CHUNK) || (wy < (int64_t)cy * WG_CHUNK) ||
        (wy >= (int64_t)cy * WG_CHUNK + WG_CHUNK) || (ov->thing_count >= OUTPOST_OVERLAY_THINGS) || (t->model == 0))
        return;
    struct OutpostOverlayThing *o = &ov->things[ov->thing_count++];
    memset(o, 0, sizeof(*o));
    o->oclass = t->oclass;
    o->model = t->model;
    o->level = t->level;
    o->heroes = t->heroes;
    o->x = (int32_t)(((wx - (int64_t)cx * WG_CHUNK) * STL_PER_SLB + 1) * COORD_PER_STL + COORD_PER_STL / 2);
    o->y = (int32_t)(((wy - (int64_t)cy * WG_CHUNK) * STL_PER_SLB + 1) * COORD_PER_STL + COORD_PER_STL / 2);
    if (t->oclass == TCls_Trap)
        o->extra = (uint16_t)max(get_trap_model_stats(t->model)->shots, 1);
}

static int sp_overlay_state(const struct SpState *st, int32_t cx, int32_t cy, struct OutpostOverlay *ov)
{
    const int pi = sp_find(st->piece);
    if (pi < 0)
        return 0;
    const struct SpPiece *p = &sp_pieces[pi];
    int64_t x0, y0;
    if (!sp_site(st->rx, st->ry, st->site, &x0, &y0))
        return 0;
    int w, h;
    sp_turned_size(p, st->turn, &w, &h);
    const int64_t px = x0 + (SP_MAX - w) / 2, py = y0 + (SP_MAX - h) / 2;
    const int64_t wx1 = (int64_t)cx * WG_CHUNK, wy1 = (int64_t)cy * WG_CHUNK;
    const int64_t wx2 = wx1 + WG_CHUNK - 1, wy2 = wy1 + WG_CHUNK - 1;
    int decided = 0;
    for (int64_t gy = max(y0, wy1); gy <= min(y0 + SP_MAX - 1, wy2); gy++)
        for (int64_t gx = max(x0, wx1); gx <= min(x0 + SP_MAX - 1, wx2); gx++)
        {
            const int i = (int)((gy - wy1) * WG_CHUNK + (gx - wx1));
            const int u = (int)(gx - px), v = (int)(gy - py);
            char c = ' ';
            uint16_t door = 0;
            if ((u >= 0) && (v >= 0) && (u < w) && (v < h))
            {
                int x, y;
                sp_source(p, st->turn, u, v, &x, &y);
                c = p->cell[y * SP_MAX + x];
                door = p->door[y * SP_MAX + x];
            }
            ov->heroes[i] = 0;
            ov->wall[i] = (c == '#');
            ov->door[i] = 0;
            if (c == ' ')
            {
                ov->set[i] = 2; /* the land's own; rock becomes earth */
                ov->kind[i] = SlbT_EARTH;
            } else
            {
                ov->set[i] = 1;
                ov->kind[i] = sp_kind(c);
                if (c == 'D')
                {
                    const long braced = get_id(door_desc, "BRACED");
                    ov->door[i] = (uint8_t)((door > 0) ? door : ((braced > 0) ? braced : 2));
                }
            }
            decided++;
        }
    /* the ways out: from each corner of the place, outwards along its row */
    for (int k = 0; k < 4; k++)
    {
        const int64_t ey = y0 + ((k & 2) ? SP_MAX - 1 : 0);
        const int64_t ex = x0 + ((k & 1) ? SP_MAX - 1 : 0);
        const int step = (k & 1) ? 1 : -1;
        for (int n = 1; n <= SP_EXIT; n++)
        {
            const int64_t gx = ex + (int64_t)step * n;
            if ((gx < wx1) || (gx > wx2) || (ey < wy1) || (ey > wy2))
                continue;
            const int i = (int)((ey - wy1) * WG_CHUNK + (gx - wx1));
            if (ov->set[i] == 1)
                continue;
            ov->set[i] = 2;
            ov->kind[i] = SlbT_EARTH;
            decided++;
        }
    }
    for (int t = 0; t < p->nthings; t++)
    {
        int u, v;
        sp_placed(p, st->turn, p->things[t].x, p->things[t].y, &u, &v);
        sp_thing(ov, cx, cy, px + u, py + v, &p->things[t]);
    }
    return decided;
}

int world_setpiece_overlay(int32_t cx, int32_t cy, struct OutpostOverlay *ov)
{
    memset(ov, 0, sizeof(*ov));
    if (world_generator() == NULL)
        return 0;
    const int64_t wx1 = (int64_t)cx * WG_CHUNK, wy1 = (int64_t)cy * WG_CHUNK;
    const int64_t wx2 = wx1 + WG_CHUNK - 1, wy2 = wy1 + WG_CHUNK - 1;
    int decided = 0;
    for (int64_t ry = sp_floor_div(wy1, SP_REGION); ry <= sp_floor_div(wy2, SP_REGION); ry++)
        for (int64_t rx = sp_floor_div(wx1 - SP_EXIT, SP_REGION); rx <= sp_floor_div(wx2 + SP_EXIT, SP_REGION); rx++)
        {
            TbBool may = false;
            for (int k = 0; (k < SP_SITES) && !may; k++)
            {
                int64_t x0, y0, x1, y1, x2, y2;
                if (!sp_site(rx, ry, k, &x0, &y0))
                    continue;
                sp_extent(x0, y0, &x1, &y1, &x2, &y2);
                may = (x2 >= wx1) && (x1 <= wx2) && (y2 >= wy1) && (y1 <= wy2);
            }
            if (!may)
                continue;
            const struct SpState *st = sp_decide(rx, ry, cx, cy);
            if ((st == NULL) || (st->piece == 0))
                continue;
            int64_t x0, y0, x1, y1, x2, y2;
            if (!sp_site(rx, ry, st->site, &x0, &y0))
                continue;
            sp_extent(x0, y0, &x1, &y1, &x2, &y2);
            if ((x2 >= wx1) && (x1 <= wx2) && (y2 >= wy1) && (y1 <= wy2))
            {
                sp_load_pieces();
                decided += sp_overlay_state(st, cx, cy, ov);
            }
        }
    return decided;
}

TbBool world_setpiece_in_box(int64_t x1, int64_t y1, int64_t x2, int64_t y2)
{
    if (world_generator() == NULL)
        return false;
    for (int64_t ry = sp_floor_div(y1, SP_REGION); ry <= sp_floor_div(y2, SP_REGION); ry++)
        for (int64_t rx = sp_floor_div(x1 - SP_EXIT, SP_REGION); rx <= sp_floor_div(x2 + SP_EXIT, SP_REGION); rx++)
        {
            const struct SpState *st = sp_state(rx, ry, false);
            if ((st != NULL) && (st->piece == 0))
                continue;
            for (int k = 0; k < SP_SITES; k++)
            {
                int64_t x0, y0, bx1, by1, bx2, by2;
                if (((st != NULL) && (k != st->site)) || !sp_site(rx, ry, k, &x0, &y0))
                    continue;
                sp_extent(x0, y0, &bx1, &by1, &bx2, &by2);
                if ((bx1 <= x2) && (x1 <= bx2) && (by1 <= y2) && (y1 <= by2))
                    return true;
            }
        }
    return false;
}

void world_setpiece_survey(int64_t range)
{
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
    char text[600];
    int len = 0;
    text[0] = 0;
    for (int i = 0; (i < sp_count) && (len < (int)sizeof(text) - 40); i++)
        len += snprintf(text + len, sizeof(text) - (size_t)len, " %s %d", sp_pieces[i].name, by_piece[i]);
    SYNCLOG("World: set pieces within %lld slabs: %d of %d regions:%s", (long long)range, with, regions, text);
}

/******************************************************************************/
/* the test hook */

/** Test hook KFX_WORLD_TESTSETPIECE="T[,name]" (only read in endless worlds): the set piece nearest the start (of
 *  that name if one is given, out to 12 regions). T: its chunks load; the slabs and things inside its place are
 *  logged (kinds, owners) next to the piece's own counts. T+700 (its chunks have frozen before then): they load
 *  again and are logged again. */
static int64_t spt_x0, spt_y0;
static int spt_piece = -1;
static uint8_t spt_turn;

static void spt_scan(const char *when)
{
    MapSlabCoord ex0, ey0;
    if (!world_engine_slab(spt_x0, spt_y0, &ex0, &ey0))
    {
        SYNCLOG("World test: set piece %s: not loaded", when);
        return;
    }
    int floor = 0, walls = 0, rooms = 0, doors = 0, lava = 0, water = 0, hard = 0;
    for (int y = 0; y < SP_MAX; y++)
        for (int x = 0; x < SP_MAX; x++)
        {
            const struct SlabMap *slb = get_slabmap_block(ex0 + x, ey0 + y);
            if (slabmap_block_invalid(slb))
                continue;
            switch (slb->kind)
            {
            case SlbT_CLAIMED: floor++; break;
            case SlbT_LAVA: lava++; break;
            case SlbT_WATER: water++; break;
            case SlbT_ROCK_FLOOR: hard++; break;
            default:
                if (slab_kind_is_door(slb->kind))
                    doors++;
                else if (slab_kind_is_room_wall(slb->kind) || ((slb->kind >= SlbT_WALLDRAPE) && (slb->kind <= SlbT_DAMAGEDWALL)))
                    walls++;
                else if (slab_corresponding_room(slb->kind) > 0)
                    rooms++;
                break;
            }
        }
    int traps = 0, objects = 0, creatures = 0;
    for (int cls = 0; cls < 3; cls++)
    {
        const int class_id = (cls == 0) ? TCls_Trap : ((cls == 1) ? TCls_Object : TCls_Creature);
        const struct StructureList *slist = get_list_for_thing_class(class_id);
        long guard = 0;
        for (long i = slist->index; (i > 0) && (guard++ <= (long)slist->count);)
        {
            struct Thing *thing = thing_get(i);
            if (thing_is_invalid(thing))
                break;
            i = thing->next_of_class;
            const int sx = subtile_slab(thing->mappos.x.stl.num) - ex0, sy = subtile_slab(thing->mappos.y.stl.num) - ey0;
            if ((sx < 0) || (sy < 0) || (sx >= SP_MAX) || (sy >= SP_MAX))
                continue;
            if (cls == 0) traps++;
            else if ((cls == 1) && !thing_is_dungeon_heart(thing)) objects++;
            else if (cls == 2) creatures++;
        }
    }
    int pt = 0, po = 0, pc = 0;
    if (spt_piece >= 0)
        for (int t = 0; t < sp_pieces[spt_piece].nthings; t++)
        {
            const int c = sp_pieces[spt_piece].things[t].oclass;
            if (c == TCls_Trap) pt++; else if (c == TCls_Object) po++; else pc++;
        }
    SYNCLOG("World test: set piece %s: claimed %d, walls %d, rooms %d, doors %d, lava %d, water %d, hard %d; traps %d "
        "(the piece has %d), objects %d (%d, and slab objects), creatures %d (%d)", when, floor, walls, rooms, doors, lava,
        water, hard, traps, pt, objects, po, creatures, pc);
}

void world_setpiece_test_turn(void)
{
    static long base = -2;
    static char want[32];
    if (base == -2)
    {
        const char *v = getenv("KFX_WORLD_TESTSETPIECE");
        base = (v != NULL) ? atol(v) : -1;
        const char *comma = (v != NULL) ? strchr(v, ',') : NULL;
        snprintf(want, sizeof(want), "%s", (comma != NULL) ? comma + 1 : "");
    }
    if ((base < 0) || (world_generator() == NULL))
        return;
    const long t = (long)game.play_gameturn - base;
    if ((t != 0) && (t != 700))
        return;
    if (t == 700)
    {
        world_load_chunks_round_world(spt_x0 + SP_MAX / 2, spt_y0 + SP_MAX / 2, 1);
        spt_scan("after its chunks froze and came back");
        return;
    }
    sp_load_pieces();
    int64_t best_d = -1;
    for (int r = 0; r <= 12; r++)
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
                int64_t x0, y0;
                sp_site(rx, ry, st->site, &x0, &y0);
                const int64_t d = (x0 + SP_MAX / 2) * (x0 + SP_MAX / 2) + (y0 + SP_MAX / 2) * (y0 + SP_MAX / 2);
                if ((best_d < 0) || (d < best_d))
                {
                    best_d = d;
                    spt_x0 = x0;
                    spt_y0 = y0;
                    spt_piece = pi;
                    spt_turn = st->turn;
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
    const int n = world_load_chunks_round_world(spt_x0 + SP_MAX / 2, spt_y0 + SP_MAX / 2, 1);
    SYNCLOG("World test: set piece %s at world slab (%lld,%lld), turned %d, %smirrored: %d chunks loaded",
        sp_pieces[spt_piece].name, (long long)(spt_x0 + SP_MAX / 2), (long long)(spt_y0 + SP_MAX / 2), 90 * (spt_turn & 3),
        (spt_turn & 4) ? "" : "not ", n);
    spt_scan("as found");
}

/******************************************************************************/
/* saves: "SPC1" in XTR2: count, then per decided region rx, ry (int64), the piece's name hash (0: none), the place and
   how it is turned */
#define SP_SAVED 22
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
        buf[20] = sp_states[i].site;
        buf[21] = sp_states[i].turn;
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
        st->site = (uint8_t)min((int)b[20], SP_SITES - 1);
        st->turn = (uint8_t)(b[21] & 7);
    }
    return true;
}

#ifdef __cplusplus
}
#endif
