/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file world_setpiece_script.c
 *     Endless world: a set piece's own map script (see world_setpiece_script.h).
 * @par Purpose:
 *     The user, 2026-11-25: set pieces with "Scripts and everything". A KeeperFX level script assumes it is the whole
 *     level: its players, flags and timers are the level's, and it can win or lose the game. A piece is one place in
 *     a shared, endless world, so each placed piece runs its script by itself, while its ground is loaded and on the
 *     machine that runs the world (never on a joined client):
 *     - its own flags (FLAG0-7) and timers (TIMER0-7) per map player, and GAME_TURN counting from the piece's first
 *       pass; its parties (CREATE_PARTY, ADD_TO_PARTY);
 *     - locations: action points (1, 2, ...) and hero gates (-1, -2, ...) where the piece lies, or a keeper's heart
 *       (PLAYER1 ...); slab coordinates count from the map's top left;
 *     - players: PLAYER0 is whoever comes (any human keeper with a creature in the piece; what is given to player 0
 *       goes to the nearest of them), PLAYER1-7 the piece's keepers, PLAYER_GOOD the heroes;
 *     - kept: IF (FLAGn, TIMERn, GAME_TURN), IF_ACTION_POINT, IF_SLAB_OWNER, IF_SLAB_TYPE, ENDIF, SET_FLAG, ADD_TO_FLAG,
 *       SET_TIMER, NEXT_COMMAND_REUSABLE, CREATE_PARTY, ADD_TO_PARTY, ADD_PARTY_TO_LEVEL, ADD_CREATURE_TO_LEVEL,
 *       ADD_OBJECT_TO_LEVEL (endless rules on boxes and books), QUICK_OBJECTIVE, QUICK_INFORMATION (to the players
 *       in the piece), REVEAL_MAP_LOCATION, CHANGE_SLAB_TYPE, CHANGE_SLAB_OWNER, SET_DOOR, ADD_GOLD_TO_PLAYER (its
 *       keepers);
 *     - left out: everything else, among it WIN_GAME, LOSE_GAME, START_MONEY, MAX_CREATURES, the *_AVAILABLE
 *       commands, rules and creature configuration, alliances, the level's strings (DISPLAY_OBJECTIVE by number),
 *       and Lua scripts. An IF on something it can't read never comes true, so its block never runs.
 *     Heroes and keepers' creatures it adds are graded by the circle they stand in, like the rest of the world.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "world_setpiece_script.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "bflib_basics.h"
#include "chat_box.h"
#include "config.h"
#include "config_creature.h"
#include "config_objects.h"
#include "config_terrain.h"
#include "creature_control.h"
#include "dungeon_data.h"
#include "game_legacy.h"
#include "gui_msgs.h"
#include "lvl_script_commands.h"
#include "map_data.h"
#include "net_state.h"
#include "player_data.h"
#include "player_instances.h"
#include "room_util.h"
#include "server_admin.h"
#include "slab_data.h"
#include "thing_creature.h"
#include "thing_data.h"
#include "thing_doors.h"
#include "thing_list.h"
#include "thing_objects.h"
#include "world_circles.h"
#include "world_setpiece.h"
#include "world_stream.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define SPS_MAX_STMTS 512
#define SPS_MAX_PARTIES 16
#define SPS_PARTY_MEMBERS 16
#define SPS_ALL 255
#define SPS_SLAB (COORD_PER_STL * STL_PER_SLB)

enum SpsOp {
    SO_If = 1, SO_IfFalse, SO_IfAp, SO_IfSlabOwner, SO_IfSlabType, SO_EndIf,
    SO_SetFlag, SO_AddFlag, SO_SetTimer, SO_AddParty, SO_AddCreature, SO_AddObject, SO_Message, SO_Reveal,
    SO_SlabType, SO_SlabOwner, SO_SetDoor, SO_AddGold,
};

enum SpsVar { SV_None = 0, SV_GameTurn, SV_Flag, SV_Timer };
enum SpsCmp { SC_Eq = 0, SC_Ne, SC_Lt, SC_Gt, SC_Le, SC_Ge };

struct SpsStmt {
    uint8_t op;
    uint8_t reusable;
    int16_t end;                /* an IF's ENDIF */
    int32_t a[6];
    char *text;
};

struct SpsMember {
    uint16_t model;
    uint8_t level;
    uint8_t objective;
    uint32_t gold;
};

struct SpsParty {
    char name[32];
    int n;
    struct SpsMember m[SPS_PARTY_MEMBERS];
};

struct SpScript {
    int n;
    struct SpsStmt st[SPS_MAX_STMTS];
    int nparties;
    struct SpsParty parties[SPS_MAX_PARTIES];
};

/* world_setpiece.c's endless rules for a map's objects and slabs */
ThingModel world_setpiece_object_endless(ThingModel model);
SlabKind world_setpiece_slab_endless(SlabKind kind);

/******************************************************************************/
/* reading */
static int sps_split(char *line, char **args, int max)
{
    char *open = strchr(line, '(');
    if (open == NULL)
        return 0;
    char *close = strrchr(open, ')');
    if (close != NULL)
        *close = 0;
    *open = 0;
    int n = 0;
    char *p = open + 1;
    while (*p && (n < max))
    {
        while ((*p == ' ') || (*p == '\t'))
            p++;
        char *start = p;
        if (*p == '"')
        {
            start = ++p;
            while (*p && (*p != '"'))
                p++;
            if (*p == '"')
                *p++ = 0;
            while (*p && (*p != ','))
                p++;
        } else
        {
            while (*p && (*p != ','))
                p++;
        }
        char *e = p;
        if (*p == ',')
            *p++ = 0;
        else
            *p = 0;
        while ((e > start) && ((e[-1] == ' ') || (e[-1] == '\t')))
            *--e = 0;
        args[n++] = start;
    }
    return n;
}

static int sps_player(const char *s)
{
    if (strcasecmp(s, "PLAYER_GOOD") == 0)
        return PLAYER_GOOD;
    if (strcasecmp(s, "PLAYER_NEUTRAL") == 0)
        return PLAYER_NEUTRAL;
    if (strcasecmp(s, "ALL_PLAYERS") == 0)
        return SPS_ALL;
    if ((strncasecmp(s, "PLAYER", 6) == 0) && isdigit((unsigned char)s[6]) && (s[7] == 0) && (s[6] - '0' < SPS_PLAYERS))
        return s[6] - '0';
    return -1;
}

/** A location: an action point (n > 0), a hero gate (-n) or a player's heart (1000 + player); 0 when not readable. */
static int sps_location(const char *s)
{
    const int p = sps_player(s);
    if ((p >= 0) && (p < SPS_PLAYERS))
        return 1000 + p;
    char *end;
    const long v = strtol(s, &end, 10);
    return ((*end == 0) && (v != 0) && (v > -256) && (v < 256)) ? (int)v : 0;
}

static TbBool sps_variable(const char *s, int *var, int *idx)
{
    if (strcasecmp(s, "GAME_TURN") == 0)
    {
        *var = SV_GameTurn;
        *idx = 0;
        return true;
    }
    if ((strncasecmp(s, "FLAG", 4) == 0) && isdigit((unsigned char)s[4]) && (s[5] == 0) && (s[4] - '0' < SPS_FLAGS))
    {
        *var = SV_Flag;
        *idx = s[4] - '0';
        return true;
    }
    if ((strncasecmp(s, "TIMER", 5) == 0) && isdigit((unsigned char)s[5]) && (s[6] == 0) && (s[5] - '0' < SPS_TIMERS))
    {
        *var = SV_Timer;
        *idx = s[5] - '0';
        return true;
    }
    return false;
}

static int sps_compare(const char *s)
{
    static const char *const ops[] = {"==", "!=", "<", ">", "<=", ">="};
    for (int i = 0; i < 6; i++)
        if (strcmp(s, ops[i]) == 0)
            return i;
    if (strcmp(s, "=") == 0)
        return SC_Eq;
    return -1;
}

static long sps_named(const struct NamedCommand *desc, const char *s)
{
    char *end;
    const long v = strtol(s, &end, 10);
    if ((*end == 0) && (end != s))
        return v;
    return get_id(desc, s);
}

static struct SpsParty *sps_party(struct SpScript *sc, const char *name, TbBool add)
{
    for (int i = 0; i < sc->nparties; i++)
        if (strcasecmp(sc->parties[i].name, name) == 0)
            return &sc->parties[i];
    if (!add || (sc->nparties >= SPS_MAX_PARTIES))
        return NULL;
    struct SpsParty *p = &sc->parties[sc->nparties++];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    return p;
}

struct SpScript *sp_script_parse(const char *text, const char *where, int *kept, int *dropped)
{
    *kept = 0;
    *dropped = 0;
    struct SpScript *sc = (struct SpScript *)calloc(1, sizeof(struct SpScript));
    if (sc == NULL)
        return NULL;
    int stack[64], depth = 0;
    TbBool reusable_next = false;
    const char *s = text;
    char dropped_names[200] = "";
    while ((s != NULL) && *s)
    {
        const char *e = strchr(s, '\n');
        size_t n = (e != NULL) ? (size_t)(e - s) : strlen(s);
        char line[512];
        if (n >= sizeof(line))
            n = sizeof(line) - 1;
        memcpy(line, s, n);
        line[n] = 0;
        s = (e != NULL) ? e + 1 : NULL;
        char *p = line;
        while ((*p == ' ') || (*p == '\t'))
            p++;
        for (char *q = p; *q; q++)
            if (*q == '\r')
                *q = 0;
        if ((*p == 0) || (strncasecmp(p, "REM", 3) == 0) || (p[0] == '/' && p[1] == '/'))
            continue;
        char word[64];
        int wl = 0;
        while (p[wl] && (p[wl] != '(') && (p[wl] != ' ') && (p[wl] != '\t') && (wl < 63))
        {
            word[wl] = (char)toupper((unsigned char)p[wl]);
            wl++;
        }
        word[wl] = 0;
        char *args[8] = {NULL};
        const int na = sps_split(p, args, 8);
        if (strcmp(word, "ENDIF") == 0)
        {
            if (depth <= 0)
                continue;
            if (sc->n >= SPS_MAX_STMTS)
                break;
            const int i = sc->n++;
            sc->st[i].op = SO_EndIf;
            sc->st[stack[--depth]].end = (int16_t)i;
            continue;
        }
        if (strcmp(word, "NEXT_COMMAND_REUSABLE") == 0)
        {
            reusable_next = true;
            continue;
        }
        if (strcmp(word, "CREATE_PARTY") == 0)
        {
            if (na >= 1)
                sps_party(sc, args[0], true);
            (*kept)++;
            continue;
        }
        if (strcmp(word, "ADD_TO_PARTY") == 0)
        {
            /* ADD_TO_PARTY(party, creature, level, gold, objective, countdown) */
            struct SpsParty *pt = (na >= 3) ? sps_party(sc, args[0], true) : NULL;
            const long model = (pt != NULL) ? get_id(creature_desc, args[1]) : -1;
            if ((pt != NULL) && (model > 0) && (pt->n < SPS_PARTY_MEMBERS))
            {
                struct SpsMember *m = &pt->m[pt->n++];
                m->model = (uint16_t)model;
                m->level = (uint8_t)max(1, min(10, atoi(args[2])));
                m->gold = (na >= 4) ? (uint32_t)max(0, atoi(args[3])) : 0;
                const long obj = (na >= 5) ? get_id(hero_objective_desc, args[4]) : -1;
                m->objective = (uint8_t)((obj > 0) ? obj : 0);
                (*kept)++;
            } else
                (*dropped)++;
            continue;
        }
        if (sc->n >= SPS_MAX_STMTS)
        {
            (*dropped)++;
            continue;
        }
        struct SpsStmt *st = &sc->st[sc->n];
        memset(st, 0, sizeof(*st));
        TbBool ok = true;
        const TbBool is_if = (strncmp(word, "IF", 2) == 0) && ((word[2] == 0) || (word[2] == '_'));
        char cond_var[64] = "", cond_op[4] = "", cond_val[64] = "";
        if ((strcmp(word, "IF") == 0) && (na == 2))
        {
            /* the usual way: IF(PLAYER0, GAME_TURN >= 40), the comparison in the second argument */
            const char *c = args[1];
            const char *opat = strpbrk(c, "=!<>");
            if (opat != NULL)
            {
                const int ol = (opat[1] == '=') ? 2 : 1; /* ==, !=, <=, >= or <, >, = */
                snprintf(cond_var, sizeof(cond_var), "%.*s", (int)(opat - c), c);
                snprintf(cond_op, sizeof(cond_op), "%.*s", ol, opat);
                snprintf(cond_val, sizeof(cond_val), "%s", opat + ol);
                for (char *q = cond_var + strlen(cond_var); (q > cond_var) && ((q[-1] == ' ') || (q[-1] == '\t'));)
                    *--q = 0;
                char *v = cond_val;
                while ((*v == ' ') || (*v == '\t'))
                    v++;
                memmove(cond_val, v, strlen(v) + 1);
                args[1] = cond_var;
                args[2] = cond_op;
                args[3] = cond_val;
            }
        }
        if (strcmp(word, "IF") == 0)
        {
            /* IF(player, variable, op, value) */
            const int nargs = (cond_op[0] != 0) ? 4 : na;
            int var = 0, idx = 0, cmp = (nargs >= 3) ? sps_compare(args[2]) : -1;
            const int pl = (na >= 1) ? sps_player(args[0]) : -1;
            if ((nargs >= 4) && (pl >= 0) && (pl != SPS_ALL) && (cmp >= 0) && sps_variable(args[1], &var, &idx))
            {
                st->op = SO_If;
                st->a[0] = pl;
                st->a[1] = var;
                st->a[2] = idx;
                st->a[3] = cmp;
                st->a[4] = atoi(args[3]);
            } else
                st->op = SO_IfFalse;
        } else if (strcmp(word, "IF_ACTION_POINT") == 0)
        {
            const int pl = (na >= 2) ? sps_player(args[1]) : -1;
            st->op = ((na >= 2) && (atoi(args[0]) > 0) && (pl >= 0)) ? SO_IfAp : SO_IfFalse;
            st->a[0] = atoi(args[0]);
            st->a[1] = pl;
        } else if (strcmp(word, "IF_SLAB_OWNER") == 0)
        {
            const int pl = (na >= 3) ? sps_player(args[2]) : -1;
            st->op = (pl >= 0) ? SO_IfSlabOwner : SO_IfFalse;
            st->a[0] = (na >= 1) ? atoi(args[0]) : -1;
            st->a[1] = (na >= 2) ? atoi(args[1]) : -1;
            st->a[2] = pl;
        } else if (strcmp(word, "IF_SLAB_TYPE") == 0)
        {
            const long k = (na >= 3) ? sps_named(slab_desc, args[2]) : -1;
            st->op = (k >= 0) ? SO_IfSlabType : SO_IfFalse;
            st->a[0] = (na >= 1) ? atoi(args[0]) : -1;
            st->a[1] = (na >= 2) ? atoi(args[1]) : -1;
            st->a[2] = (int32_t)k;
        } else if (is_if)
        {
            st->op = SO_IfFalse; /* an IF it can't read: its block never runs */
            (*dropped)++;
        } else if ((strcmp(word, "SET_FLAG") == 0) || (strcmp(word, "ADD_TO_FLAG") == 0))
        {
            int var = 0, idx = 0;
            const int pl = (na >= 3) ? sps_player(args[0]) : -1;
            ok = (pl >= 0) && (pl < SPS_PLAYERS) && sps_variable(args[1], &var, &idx) && (var == SV_Flag);
            st->op = (word[0] == 'S') ? SO_SetFlag : SO_AddFlag;
            st->a[0] = pl;
            st->a[1] = idx;
            st->a[2] = ok ? atoi(args[2]) : 0;
        } else if (strcmp(word, "SET_TIMER") == 0)
        {
            int var = 0, idx = 0;
            const int pl = (na >= 2) ? sps_player(args[0]) : -1;
            ok = (pl >= 0) && (pl < SPS_PLAYERS) && sps_variable(args[1], &var, &idx) && (var == SV_Timer);
            st->op = SO_SetTimer;
            st->a[0] = pl;
            st->a[1] = idx;
        } else if (strcmp(word, "ADD_PARTY_TO_LEVEL") == 0)
        {
            /* ADD_PARTY_TO_LEVEL(owner, party, location, count) */
            const int pl = (na >= 3) ? sps_player(args[0]) : -1;
            struct SpsParty *pt = (na >= 3) ? sps_party(sc, args[1], false) : NULL;
            const int loc = (na >= 3) ? sps_location(args[2]) : 0;
            ok = (pl >= 0) && (pl < SPS_PLAYERS) && (pt != NULL) && (loc != 0);
            st->op = SO_AddParty;
            st->a[0] = pl;
            st->a[1] = ok ? (int)(pt - sc->parties) : 0;
            st->a[2] = loc;
            st->a[3] = (na >= 4) ? max(1, min(8, atoi(args[3]))) : 1;
        } else if (strcmp(word, "ADD_CREATURE_TO_LEVEL") == 0)
        {
            /* ADD_CREATURE_TO_LEVEL(owner, creature, location, count, level, gold) */
            const int pl = (na >= 3) ? sps_player(args[0]) : -1;
            const long model = (na >= 3) ? get_id(creature_desc, args[1]) : -1;
            const int loc = (na >= 3) ? sps_location(args[2]) : 0;
            ok = (pl >= 0) && (pl < SPS_PLAYERS) && (model > 0) && (loc != 0);
            st->op = SO_AddCreature;
            st->a[0] = pl;
            st->a[1] = (int32_t)model;
            st->a[2] = loc;
            st->a[3] = (na >= 4) ? max(1, min(16, atoi(args[3]))) : 1;
            st->a[4] = (na >= 5) ? max(1, min(10, atoi(args[4]))) : 1;
            st->a[5] = (na >= 6) ? max(0, atoi(args[5])) : 0;
        } else if (strcmp(word, "ADD_OBJECT_TO_LEVEL") == 0)
        {
            /* ADD_OBJECT_TO_LEVEL(object, location, property[, player]) */
            const long model = (na >= 2) ? get_id(object_desc, args[0]) : -1;
            const int loc = (na >= 2) ? sps_location(args[1]) : 0;
            const ThingModel m = (model > 0) ? world_setpiece_object_endless((ThingModel)model) : 0;
            ok = (m > 0) && (loc != 0);
            st->op = SO_AddObject;
            st->a[0] = m;
            st->a[1] = loc;
            st->a[2] = (na >= 3) ? atoi(args[2]) : 0;
            st->a[3] = (na >= 4) ? sps_player(args[3]) : PLAYER_NEUTRAL;
        } else if ((strcmp(word, "QUICK_OBJECTIVE") == 0) || (strcmp(word, "QUICK_INFORMATION") == 0))
        {
            ok = (na >= 2) && (args[1][0] != 0);
            st->op = SO_Message;
            st->text = ok ? strdup(args[1]) : NULL;
            ok = ok && (st->text != NULL);
        } else if (strcmp(word, "REVEAL_MAP_LOCATION") == 0)
        {
            const int pl = (na >= 3) ? sps_player(args[0]) : -1;
            const int loc = (na >= 3) ? sps_location(args[1]) : 0;
            ok = (pl >= 0) && (loc != 0);
            st->op = SO_Reveal;
            st->a[0] = pl;
            st->a[1] = loc;
            st->a[2] = (na >= 3) ? max(1, min(60, atoi(args[2]))) : 9;
        } else if (strcmp(word, "CHANGE_SLAB_TYPE") == 0)
        {
            const long k = (na >= 3) ? sps_named(slab_desc, args[2]) : -1;
            ok = (k >= 0);
            st->op = SO_SlabType;
            st->a[0] = (na >= 1) ? atoi(args[0]) : -1;
            st->a[1] = (na >= 2) ? atoi(args[1]) : -1;
            st->a[2] = ok ? (int32_t)world_setpiece_slab_endless((SlabKind)k) : 0;
        } else if (strcmp(word, "CHANGE_SLAB_OWNER") == 0)
        {
            const int pl = (na >= 3) ? sps_player(args[2]) : -1;
            ok = (pl >= 0) && (pl < SPS_PLAYERS);
            st->op = SO_SlabOwner;
            st->a[0] = (na >= 1) ? atoi(args[0]) : -1;
            st->a[1] = (na >= 2) ? atoi(args[1]) : -1;
            st->a[2] = pl;
        } else if (strcmp(word, "SET_DOOR") == 0)
        {
            ok = (na >= 3) && ((strcasecmp(args[0], "LOCKED") == 0) || (strcasecmp(args[0], "UNLOCKED") == 0));
            st->op = SO_SetDoor;
            st->a[0] = ok && (strcasecmp(args[0], "LOCKED") == 0);
            st->a[1] = (na >= 2) ? atoi(args[1]) : -1;
            st->a[2] = (na >= 3) ? atoi(args[2]) : -1;
        } else if (strcmp(word, "ADD_GOLD_TO_PLAYER") == 0)
        {
            const int pl = (na >= 2) ? sps_player(args[0]) : -1;
            ok = (pl >= 1) && (pl < SPS_PLAYERS) && (pl != PLAYER_GOOD) && (pl != PLAYER_NEUTRAL);
            st->op = SO_AddGold;
            st->a[0] = pl;
            st->a[1] = (na >= 2) ? atoi(args[1]) : 0;
        } else
            ok = false;
        if (!ok)
        {
            (*dropped)++;
            free(st->text);
            if ((strlen(dropped_names) + strlen(word) + 2 < sizeof(dropped_names)) && (strstr(dropped_names, word) == NULL))
            {
                strcat(dropped_names, " ");
                strcat(dropped_names, word);
            }
            reusable_next = false;
            continue;
        }
        st->reusable = reusable_next ? 1 : 0;
        reusable_next = false;
        if (st->op <= SO_IfSlabType)
        {
            st->end = -1;
            if (depth < 64)
                stack[depth++] = sc->n;
        } else if (st->op != SO_IfFalse)
            (*kept)++;
        sc->n++;
    }
    /* an IF without its ENDIF runs to the end */
    while (depth > 0)
        sc->st[stack[--depth]].end = (int16_t)(sc->n - 1);
    if (*dropped > 0)
        SYNCLOG("World: set piece %s: its script leaves out %d commands:%s", where, *dropped, dropped_names);
    if (*kept <= 0)
    {
        sp_script_free(sc);
        return NULL;
    }
    return sc;
}

void sp_script_free(struct SpScript *s)
{
    if (s == NULL)
        return;
    for (int i = 0; i < s->n; i++)
        free(s->st[i].text);
    free(s);
}

/******************************************************************************/
/* running */
struct SpsPass {
    const struct SpScriptPlace *pl;
    struct SpScriptRun *run;
    PlayerNumber visitors[16];
    int nvisitors;
    TbBool visitors_known;
};

/** The engine position of a map position (map coordinates from its top left); false when its ground isn't loaded. */
static TbBool sps_engine_pos(const struct SpScriptPlace *pl, int32_t mx, int32_t my, struct Coord3d *pos)
{
    MapSlabCoord ex, ey;
    if ((mx < 0) || (my < 0) || !world_engine_slab(pl->x0 + mx / SPS_SLAB, pl->y0 + my / SPS_SLAB, &ex, &ey))
        return false;
    pos->x.val = (MapCoord)(ex * SPS_SLAB + mx % SPS_SLAB);
    pos->y.val = (MapCoord)(ey * SPS_SLAB + my % SPS_SLAB);
    pos->z.val = 0;
    return true;
}

static TbBool sps_location_pos(const struct SpScriptPlace *pl, int loc, struct Coord3d *pos, int32_t *range)
{
    if (range != NULL)
        *range = 3 * COORD_PER_STL;
    if (loc >= 1000)
    {
        const int p = loc - 1000;
        if ((p <= 0) || (p >= SPS_PLAYERS) || (pl->keeper_of[p] < 0))
            return false;
        MapSlabCoord ex, ey;
        if (!world_engine_slab(pl->heart_x[p], pl->heart_y[p], &ex, &ey))
            return false;
        pos->x.val = (MapCoord)(ex * SPS_SLAB + SPS_SLAB / 2);
        pos->y.val = (MapCoord)(ey * SPS_SLAB + SPS_SLAB / 2);
        pos->z.val = 0;
        return true;
    }
    const struct SpScriptPoint *list = (loc > 0) ? pl->aps : pl->gates;
    const int n = (loc > 0) ? pl->naps : pl->ngates;
    const int num = (loc > 0) ? loc : -loc;
    for (int i = 0; i < n; i++)
        if (list[i].num == num)
        {
            if ((range != NULL) && (loc > 0))
                *range = max(list[i].range, COORD_PER_STL);
            return sps_engine_pos(pl, list[i].x, list[i].y, pos);
        }
    return false;
}

/** Does this engine player answer to the map's player p (0: any human keeper)? */
static TbBool sps_is_player(const struct SpScriptPlace *pl, int p, PlayerNumber owner)
{
    if (p == SPS_ALL)
        return true;
    if (p == 0)
        return world_player_is_human(owner);
    if (p == PLAYER_GOOD)
        return owner == PLAYER_GOOD;
    if (p == PLAYER_NEUTRAL)
        return owner == game.neutral_player_num;
    return (p > 0) && (p < SPS_PLAYERS) && (pl->keeper_of[p] >= 0) && (owner == pl->keeper_of[p]);
}

/** The human keepers with a creature on the piece's map (the players a message is for). */
static void sps_visitors(struct SpsPass *ps)
{
    if (ps->visitors_known)
        return;
    ps->visitors_known = true;
    ps->nvisitors = 0;
    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    long guard = 0;
    for (long i = slist->index; (i > 0) && (guard++ <= (long)slist->count);)
    {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing))
            break;
        i = thing->next_of_class;
        if (!world_player_is_human(thing->owner))
            continue;
        int64_t wx, wy;
        world_slab_to_world(subtile_slab(thing->mappos.x.stl.num), subtile_slab(thing->mappos.y.stl.num), &wx, &wy);
        if ((wx < ps->pl->x0) || (wy < ps->pl->y0) || (wx >= ps->pl->x0 + ps->pl->w) || (wy >= ps->pl->y0 + ps->pl->h))
            continue;
        TbBool seen = false;
        for (int k = 0; k < ps->nvisitors; k++)
            seen = seen || (ps->visitors[k] == thing->owner);
        if (!seen && (ps->nvisitors < 16))
            ps->visitors[ps->nvisitors++] = thing->owner;
    }
}

/** The engine player for one the map names as an owner; player 0 is the first visitor (nobody when none). */
static PlayerNumber sps_owner(struct SpsPass *ps, int p)
{
    if (p == PLAYER_GOOD)
        return PLAYER_GOOD;
    if (p == 0)
    {
        sps_visitors(ps);
        return (ps->nvisitors > 0) ? ps->visitors[0] : game.neutral_player_num;
    }
    if ((p > 0) && (p < SPS_PLAYERS) && (p != PLAYER_NEUTRAL) && (ps->pl->keeper_of[p] >= 0))
        return ps->pl->keeper_of[p];
    return game.neutral_player_num;
}

static void sps_tell(PlayerNumber p, const char *text)
{
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

static TbBool sps_slab(const struct SpScriptPlace *pl, int x, int y, MapSlabCoord *ex, MapSlabCoord *ey)
{
    return (x >= 0) && (y >= 0) && (x < pl->w) && (y < pl->h) && world_engine_slab(pl->x0 + x, pl->y0 + y, ex, ey);
}

static struct Thing *sps_creature(struct SpsPass *ps, const struct Coord3d *at, ThingModel model, PlayerNumber owner,
    int level, uint32_t gold, int objective)
{
    struct Coord3d pos = *at;
    struct Thing *thing = create_creature(&pos, model, owner);
    if (thing_is_invalid(thing))
        return NULL;
    /* heroes and keepers' creatures as strong as the circle they stand in (never weaker than the script says) */
    if ((owner == PLAYER_GOOD) || ((owner != game.neutral_player_num) && !world_player_is_human(owner)))
    {
        int64_t wx, wy;
        int circle = 1;
        world_slab_to_world(subtile_slab(pos.x.stl.num), subtile_slab(pos.y.stl.num), &wx, &wy);
        level = max(level, min(10, world_circles_level_at_world(wx, wy, level, &circle)));
    }
    init_creature_level(thing, (CrtrExpLevel)(max(1, min(10, level)) - 1));
    thing->creature.gold_carried = gold;
    struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
    if ((owner == PLAYER_GOOD) && (objective > 0) && !creature_control_invalid(cctrl))
    {
        sps_visitors(ps);
        cctrl->party.original_objective = (unsigned char)objective;
        cctrl->party.objective = (unsigned char)objective;
        cctrl->party.target_plyr_idx = (ps->nvisitors > 0) ? ps->visitors[0] : PLAYER0;
    }
    return thing;
}

static int32_t sps_value(struct SpsPass *ps, int pl, int var, int idx)
{
    const uint32_t now = (uint32_t)game.play_gameturn;
    switch (var)
    {
    case SV_GameTurn: return (int32_t)(now - ps->run->since);
    case SV_Flag: return ps->run->flags[pl][idx];
    case SV_Timer: return ps->run->timers[pl][idx] ? (int32_t)(now - ps->run->timers[pl][idx]) : (int32_t)(now - ps->run->since);
    default: return 0;
    }
}

static TbBool sps_compare_values(int cmp, int32_t a, int32_t b)
{
    switch (cmp)
    {
    case SC_Eq: return a == b;
    case SC_Ne: return a != b;
    case SC_Lt: return a < b;
    case SC_Gt: return a > b;
    case SC_Le: return a <= b;
    default: return a >= b;
    }
}

static TbBool sps_condition(struct SpsPass *ps, const struct SpsStmt *st)
{
    const struct SpScriptPlace *pl = ps->pl;
    switch (st->op)
    {
    case SO_If:
        return sps_compare_values(st->a[3], sps_value(ps, st->a[0] % SPS_PLAYERS, st->a[1], st->a[2]), st->a[4]);
    case SO_IfAp:
    {
        struct Coord3d pos;
        int32_t range;
        if (!sps_location_pos(pl, st->a[0], &pos, &range))
            return false;
        const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
        long guard = 0;
        for (long i = slist->index; (i > 0) && (guard++ <= (long)slist->count);)
        {
            struct Thing *thing = thing_get(i);
            if (thing_is_invalid(thing))
                break;
            i = thing->next_of_class;
            const long dx = (long)thing->mappos.x.val - pos.x.val, dy = (long)thing->mappos.y.val - pos.y.val;
            if ((labs(dx) <= range) && (labs(dy) <= range) && sps_is_player(pl, st->a[1], thing->owner))
                return true;
        }
        return false;
    }
    case SO_IfSlabOwner:
    case SO_IfSlabType:
    {
        MapSlabCoord ex, ey;
        if (!sps_slab(pl, st->a[0], st->a[1], &ex, &ey))
            return false;
        const struct SlabMap *slb = get_slabmap_block(ex, ey);
        if (st->op == SO_IfSlabType)
            return slb->kind == (SlabKind)st->a[2];
        return sps_is_player(pl, st->a[2], slabmap_owner(slb));
    }
    default:
        return false;
    }
}

/** Runs a command; false when it couldn't yet (its place not loaded): it is tried again on the next pass. */
static TbBool sps_command(struct SpsPass *ps, const struct SpScript *sc, const struct SpsStmt *st)
{
    const struct SpScriptPlace *pl = ps->pl;
    struct SpScriptRun *run = ps->run;
    struct Coord3d pos;
    switch (st->op)
    {
    case SO_SetFlag:
        run->flags[st->a[0]][st->a[1]] = st->a[2];
        return true;
    case SO_AddFlag:
        run->flags[st->a[0]][st->a[1]] += st->a[2];
        return true;
    case SO_SetTimer:
        run->timers[st->a[0]][st->a[1]] = max(1u, (uint32_t)game.play_gameturn);
        return true;
    case SO_AddCreature:
    {
        if (!sps_location_pos(pl, st->a[2], &pos, NULL))
            return false;
        const PlayerNumber owner = sps_owner(ps, st->a[0]);
        int made = 0;
        for (int k = 0; k < st->a[3]; k++)
            made += (sps_creature(ps, &pos, (ThingModel)st->a[1], owner, st->a[4], (uint32_t)st->a[5], 0) != NULL) ? 1 : 0;
        SYNCLOG("World: set piece %s: its script adds %d %s for player %d", pl->name, made, creature_code_name(st->a[1]), (int)owner);
        return true;
    }
    case SO_AddParty:
    {
        if (!sps_location_pos(pl, st->a[2], &pos, NULL))
            return false;
        const struct SpsParty *pt = &sc->parties[st->a[1]];
        const PlayerNumber owner = sps_owner(ps, st->a[0]);
        int made = 0;
        for (int c = 0; c < st->a[3]; c++)
            for (int k = 0; k < pt->n; k++)
                made += (sps_creature(ps, &pos, pt->m[k].model, owner, pt->m[k].level, pt->m[k].gold, pt->m[k].objective) != NULL) ? 1 : 0;
        SYNCLOG("World: set piece %s: its script adds the party %s (%d creatures) for player %d", pl->name, pt->name, made, (int)owner);
        return true;
    }
    case SO_AddObject:
    {
        if (!sps_location_pos(pl, st->a[1], &pos, NULL))
            return false;
        const PlayerNumber owner = ((st->a[3] >= 0) && (st->a[3] != PLAYER_NEUTRAL)) ? sps_owner(ps, st->a[3]) : game.neutral_player_num;
        struct Thing *thing = create_object(&pos, (ThingModel)st->a[0], owner, -1);
        if (!thing_is_invalid(thing) && object_is_gold_pile(thing) && (st->a[2] > 0))
        {
            thing->valuable.gold_stored = 0;
            add_gold_to_pile(thing, st->a[2]);
        }
        return true;
    }
    case SO_Message:
        sps_visitors(ps);
        for (int k = 0; k < ps->nvisitors; k++)
            sps_tell(ps->visitors[k], st->text);
        SYNCLOG("World: set piece %s: its script says \"%s\" to %d players", pl->name, st->text, ps->nvisitors);
        return true;
    case SO_Reveal:
    {
        int32_t range;
        if (!sps_location_pos(pl, st->a[1], &pos, &range))
            return false;
        const MapSubtlCoord r = (MapSubtlCoord)st->a[2], x = pos.x.stl.num, y = pos.y.stl.num;
        if (st->a[0] == 0)
        {
            sps_visitors(ps);
            for (int k = 0; k < ps->nvisitors; k++)
                reveal_map_area(ps->visitors[k], x - r, x + r, y - r, y + r);
        } else
        {
            const PlayerNumber p = sps_owner(ps, st->a[0]);
            if (p != game.neutral_player_num)
                reveal_map_area(p, x - r, x + r, y - r, y + r);
        }
        return true;
    }
    case SO_SlabType:
    case SO_SlabOwner:
    case SO_SetDoor:
    {
        MapSlabCoord ex, ey;
        if (!sps_slab(pl, st->a[0], st->a[1], &ex, &ey))
            return (st->a[0] < 0) || (st->a[1] < 0) || (st->a[0] >= pl->w) || (st->a[1] >= pl->h); /* off the map: done */
        if (st->op == SO_SlabType)
            replace_slab_from_script(ex, ey, (unsigned char)st->a[2]);
        else if (st->op == SO_SlabOwner)
            change_slab_owner_from_script(ex, ey, sps_owner(ps, st->a[2]));
        else
        {
            struct Thing *door = get_door_for_position(slab_subtile_center(ex), slab_subtile_center(ey));
            if (!thing_is_invalid(door))
            {
                if (st->a[0])
                    lock_door(door);
                else
                    unlock_door(door);
            }
        }
        return true;
    }
    case SO_AddGold:
        if (pl->keeper_of[st->a[0]] < 0)
            return false;
        player_add_offmap_gold(pl->keeper_of[st->a[0]], st->a[1]);
        return true;
    default:
        return true;
    }
}

void sp_script_run(const struct SpScript *sc, const struct SpScriptPlace *pl, struct SpScriptRun *run)
{
    if ((sc == NULL) || netstate_is_client())
        return;
    if (run->since == 0)
    {
        run->since = max(1u, (uint32_t)game.play_gameturn);
        SYNCLOG("World: set piece %s: its script starts (%d statements, %d parties)", pl->name, sc->n, sc->nparties);
    }
    run->nbits = (uint16_t)sc->n;
    struct SpsPass ps;
    memset(&ps, 0, sizeof(ps));
    ps.pl = pl;
    ps.run = run;
    for (int i = 0; i < sc->n;)
    {
        const struct SpsStmt *st = &sc->st[i];
        if (st->op <= SO_IfSlabType)
        {
            i = ((st->op != SO_IfFalse) && sps_condition(&ps, st)) ? i + 1 : st->end + 1;
            continue;
        }
        if (st->op == SO_EndIf)
        {
            i++;
            continue;
        }
        const TbBool done = (run->done[i >> 3] & (1u << (i & 7))) != 0;
        if ((!done || st->reusable) && sps_command(&ps, sc, st))
            run->done[i >> 3] |= (uint8_t)(1u << (i & 7));
        i++;
    }
}

#ifdef __cplusplus
}
#endif
