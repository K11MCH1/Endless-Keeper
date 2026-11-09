/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file test_highmem.c
 *     Test switch KFX_TEST_HIGHMEM=1: fills the address space below 2 GB.
 * @par Purpose:
 *     The exe is linked large address aware, so on 64-bit Windows it can use 4 GB and its
 *     memory can lie above the 2 GB line. With this switch every free region below that
 *     line is reserved before the game starts, so the game's own allocations land above it:
 *     code that keeps an address in a signed number then fails in a test, not in a big game.
 *     Also the save checks for format changes: KFX_TEST_VERIFYSAVES=1 decodes every save slot at the first turn
 *     (changes nothing), KFX_TEST_SAVESLOT="n@turn" saves to slot n at that turn (any level) and
 *     KFX_TEST_LOADSLOT="n@turn" loads slot n at that turn.
 *     KFX_TEST_CASTCHECK=turn logs, at that turn, whether Cave-In and Lightning Strike may be cast on the first
 *     slab of each sort (own floor and wall, unclaimed, neutral and another keeper's floor) within 60 slabs of
 *     the player's heart (an endless world's map is far too big to scan); changes nothing.
 *     KFX_TEST_SAMPLE=1: a sampling profiler. A thread stops the main thread about every millisecond and counts the
 *     code address it is at (self) and the return addresses up its frame-pointer chain (inclusive, once a sample);
 *     every 500 game turns the top addresses are logged as offsets in the exe (out/sample2sym.py maps them).
 */
/******************************************************************************/
#include "pre_inc.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "test_highmem.h"
#include "globals.h"
#include "game_legacy.h"
#include "kfx/save/SaveManager.h"
#include "game_legacy.h"
#include "kfx/save/SaveManager.h"
#include "magic_powers.h"
#include "config_magic.h"
#include "config_terrain.h"
#include "map_data.h"
#include "map_columns.h"
#include "slab_data.h"
#include "player_data.h"
#include "dungeon_data.h"
#include "thing_data.h"
#include "post_inc.h"

static unsigned long highmem_reserved_kb = 0;
static unsigned long highmem_regions = 0;

#ifdef _WIN32
static void highmem_fill(void)
{
    const uintptr_t line = 0x80000000u;
    uintptr_t addr = 0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < line)
    {
        if (VirtualQuery((void *)addr, &mbi, sizeof(mbi)) == 0)
            break;
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        uintptr_t end = base + mbi.RegionSize;
        if (end <= base)
            break;
        if (mbi.State == MEM_FREE)
        {
            uintptr_t start = (base + 0xFFFFu) & ~(uintptr_t)0xFFFFu;
            uintptr_t stop = (end < line) ? end : line;
            if ((stop > start) && (VirtualAlloc((void *)start, stop - start, MEM_RESERVE, PAGE_NOACCESS) != NULL))
            {
                highmem_reserved_kb += (unsigned long)((stop - start) >> 10);
                highmem_regions++;
            }
        }
        addr = end;
    }
}

__attribute__((constructor)) static void highmem_init(void)
{
    const char *v = getenv("KFX_TEST_HIGHMEM");
    if ((v != NULL) && (v[0] == '1'))
        highmem_fill();
}
#endif

#ifdef _WIN32
#define SAMPLE_TABLE 65536
static HANDLE sample_target;
static uintptr_t sample_lo, sample_hi;
static uint32_t self_addr[SAMPLE_TABLE], self_count[SAMPLE_TABLE];
static uint32_t incl_addr[SAMPLE_TABLE], incl_count[SAMPLE_TABLE];
static volatile uint32_t samples_total;

static void sample_count(uint32_t *addr, uint32_t *count, uint32_t a)
{
    uint32_t h = (a * 2654435761u) >> 16;
    for (int n = 0; n < 64; n++)
    {
        const uint32_t k = (h + (uint32_t)n) & (SAMPLE_TABLE - 1);
        if (addr[k] == a)
        {
            count[k]++;
            return;
        }
        if (addr[k] == 0)
        {
            addr[k] = a;
            count[k] = 1;
            return;
        }
    }
}

static DWORD WINAPI sample_main(LPVOID arg)
{
    (void)arg;
#ifdef _WIN64
    (void)sample_count;
    return 0; /* the sampler walks 32-bit frames: not on 64-bit builds */
#else
    for (;;)
    {
        Sleep(1);
        if (SuspendThread(sample_target) == (DWORD)-1)
            continue;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(sample_target, &ctx))
        {
            const uint32_t eip = (uint32_t)ctx.Eip;
            if ((eip >= sample_lo) && (eip < sample_hi))
                sample_count(self_addr, self_count, eip - (uint32_t)sample_lo);
            else
                sample_count(self_addr, self_count, 1); /* outside the exe (system or library code) */
            uint32_t seen[48];
            int nseen = 0;
            uint32_t ebp = (uint32_t)ctx.Ebp;
            const uint32_t esp = (uint32_t)ctx.Esp;
            for (int d = 0; d < 48; d++)
            {
                if ((ebp < esp) || (ebp > esp + 0x800000u) || (ebp & 3u))
                    break;
                const uint32_t *frame = (const uint32_t *)(uintptr_t)ebp;
                const uint32_t ret = frame[1], next = frame[0];
                if ((ret >= sample_lo) && (ret < sample_hi))
                {
                    const uint32_t off = ret - (uint32_t)sample_lo;
                    TbBool dup = false;
                    for (int k = 0; (k < nseen) && !dup; k++)
                        dup = (seen[k] == off);
                    if (!dup)
                    {
                        seen[nseen++] = off;
                        sample_count(incl_addr, incl_count, off);
                    }
                }
                if (next <= ebp)
                    break;
                ebp = next;
            }
            samples_total++;
        }
        ResumeThread(sample_target);
    }
    return 0;
#endif
}

static void sample_start(void)
{
    const uint8_t *base = (const uint8_t *)GetModuleHandle(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    sample_lo = (uintptr_t)base;
    sample_hi = sample_lo + nt->OptionalHeader.SizeOfImage;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sample_target,
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    HANDLE th = CreateThread(NULL, 0, sample_main, NULL, 0, NULL);
    if (th != NULL)
    {
        SetThreadPriority(th, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle(th);
    }
    JUSTMSG("Test: sampling profiler started (exe at %p, %lu bytes)", (void *)sample_lo, (unsigned long)(sample_hi - sample_lo));
}

static void sample_report_table(const char *what, const uint32_t *addr, const uint32_t *count, int top)
{
    static uint32_t idx[SAMPLE_TABLE];
    int n = 0;
    for (int k = 0; k < SAMPLE_TABLE; k++)
        if (addr[k] != 0)
            idx[n++] = (uint32_t)k;
    /* the top ones, by selection (once every 1000 turns) */
    for (int i = 0; (i < top) && (i < n); i++)
    {
        int best = i;
        for (int j = i + 1; j < n; j++)
            if (count[idx[j]] > count[idx[best]])
                best = j;
        const uint32_t t = idx[i];
        idx[i] = idx[best];
        idx[best] = t;
        JUSTMSG("Test: sample %s +0x%x %u", what, (unsigned)addr[idx[i]], (unsigned)count[idx[i]]);
    }
}
#endif

void test_highmem_report(void)
{
    static TbBool reported = false;
    if (reported || (highmem_regions == 0))
        return;
    reported = true;
    void *probe = malloc(1 << 20);
    JUSTMSG("Test: high memory: %lu MB below 2 GB reserved in %lu regions; a new 1 MB block is at %p",
        highmem_reserved_kb >> 10, highmem_regions, probe);
    free(probe);
}

/* Where Cave-In and Lightning Strike may be cast: the first slab of each sort on the map. The reveal check is
   skipped (PwCast_Unrevealed) so that only the ground rule decides. */
static void test_cast_check(void)
{
    const PlayerNumber plyr = my_player_number;
    const PowerKind kinds[2] = {PwrK_CAVEIN, PwrK_LIGHTNING};
    for (int k = 0; k < 2; k++)
        JUSTMSG("Test: cast check: %s castability flags 0x%llx", power_code_name(kinds[k]),
            (unsigned long long)get_power_model_stats(kinds[k])->can_cast_flags);
    static const char *sorts[5] = {"own floor", "own wall", "unclaimed ground", "neutral claimed floor", "another keeper's floor"};
    TbBool found[5] = {false, false, false, false, false};
    MapSlabCoord fx[5], fy[5];
    MapSlabCoord cx = game.map_tiles_x / 2, cy = game.map_tiles_y / 2;
    const struct Thing *heart = get_player_soul_container(plyr);
    if (thing_exists(heart))
    {
        cx = subtile_slab(heart->mappos.x.stl.num);
        cy = subtile_slab(heart->mappos.y.stl.num);
    }
    const MapSlabCoord x0 = (cx - 60 < 1) ? 1 : cx - 60, x1 = (cx + 60 > game.map_tiles_x - 2) ? game.map_tiles_x - 2 : cx + 60;
    const MapSlabCoord y0 = (cy - 60 < 1) ? 1 : cy - 60, y1 = (cy + 60 > game.map_tiles_y - 2) ? game.map_tiles_y - 2 : cy + 60;
    JUSTMSG("Test: cast check: player %d, slabs (%d,%d) to (%d,%d)", (int)plyr, (int)x0, (int)y0, (int)x1, (int)y1);
    for (MapSlabCoord y = y0; y <= y1; y++)
        for (MapSlabCoord x = x0; x <= x1; x++)
        {
            const MapSubtlCoord sx = slab_subtile_center(x), sy = slab_subtile_center(y);
            struct SlabMap *slb = get_slabmap_block(x, y);
            const struct Map *mapblk = get_map_block_at(sx, sy);
            const TbBool blocking = ((mapblk->flags & SlbAtFlg_Blocking) != 0) && !subtile_is_liquid(sx, sy);
            const PlayerNumber owner = slabmap_owner(slb);
            int sort;
            if (owner == plyr)
                sort = blocking ? 1 : 0;
            else if (blocking || subtile_is_liquid(sx, sy))
                continue;
            else if ((get_slab_stats(slb)->category == SlbAtCtg_Unclaimed) || subtile_is_unclaimed_path(sx, sy))
                sort = 2;
            else if (owner == game.neutral_player_num)
                sort = 3;
            else
                sort = 4;
            if (!found[sort])
            {
                found[sort] = true;
                fx[sort] = x;
                fy[sort] = y;
            }
        }
    for (int s = 0; s < 5; s++)
    {
        if (!found[s])
        {
            JUSTMSG("Test: cast check: no %s on the map", sorts[s]);
            continue;
        }
        const MapSubtlCoord sx = slab_subtile_center(fx[s]), sy = slab_subtile_center(fy[s]);
        JUSTMSG("Test: cast check: %s at slab (%d,%d) owner %d: Cave-In %s, Lightning Strike %s", sorts[s], (int)fx[s],
            (int)fy[s], (int)slabmap_owner(get_slabmap_block(fx[s], fy[s])),
            can_cast_power_at_xy(plyr, PwrK_CAVEIN, sx, sy, PwCast_Unrevealed) ? "allowed" : "refused",
            can_cast_power_at_xy(plyr, PwrK_LIGHTNING, sx, sy, PwCast_Unrevealed) ? "allowed" : "refused");
    }
}

void test_hooks_turn(void)
{
#ifdef _WIN32
    static int sampling = -1;
    if (sampling < 0)
    {
        const char *v = getenv("KFX_TEST_SAMPLE");
        sampling = ((v != NULL) && (v[0] == '1')) ? 1 : 0;
        if (sampling)
            sample_start();
    }
    if (sampling && (game.play_gameturn > 0) && ((game.play_gameturn % 500) == 0))
    {
        JUSTMSG("Test: sample report at turn %lu: %u samples", (unsigned long)game.play_gameturn, (unsigned)samples_total);
        sample_report_table("self", self_addr, self_count, 60);
        sample_report_table("incl", incl_addr, incl_count, 150);
        /* each report covers the turns since the last one (a level start or a big load stays in the first) */
        memset(self_addr, 0, sizeof(self_addr));
        memset(self_count, 0, sizeof(self_count));
        memset(incl_addr, 0, sizeof(incl_addr));
        memset(incl_count, 0, sizeof(incl_count));
        samples_total = 0;
    }
#endif
    static TbBool verified = false;
    static int load_slot = -2;
    static GameTurn load_turn = 0;
    if (!verified)
    {
        verified = true;
        const char *v = getenv("KFX_TEST_VERIFYSAVES");
        if ((v != NULL) && (v[0] == '1'))
        {
            int found = 0, loadable = 0;
            for (int slot = 0; slot < 100; slot++)
            {
                char detail[160] = "";
                enum SaveCheckResult res = SaveManager_Verify(slot, detail, sizeof(detail));
                if (res == SvChk_Missing)
                    continue;
                found++;
                if (res == SvChk_Loadable)
                    loadable++;
                JUSTMSG("Test: save slot %d: %s%s%s", slot,
                    (res == SvChk_Loadable) ? "loadable" : (res == SvChk_Damaged) ? "DAMAGED" : "INCOMPATIBLE",
                    (detail[0] != 0) ? " - " : "", detail);
            }
            JUSTMSG("Test: %d of %d saves are loadable", loadable, found);
        }
    }
    if (load_slot == -2)
    {
        load_slot = -1;
        const char *v = getenv("KFX_TEST_LOADSLOT");
        int n, t;
        if ((v != NULL) && (sscanf(v, "%d@%d", &n, &t) == 2) && (n >= 0))
        {
            load_slot = n;
            load_turn = (GameTurn)t;
        }
    }
    static int save_slot = -2;
    static GameTurn save_turn = 0;
    if (save_slot == -2)
    {
        save_slot = -1;
        const char *v = getenv("KFX_TEST_SAVESLOT");
        int n, t;
        if ((v != NULL) && (sscanf(v, "%d@%d", &n, &t) == 2) && (n >= 0))
        {
            save_slot = n;
            save_turn = (GameTurn)t;
        }
    }
    static long cast_check_turn = -2;
    if (cast_check_turn == -2)
    {
        const char *v = getenv("KFX_TEST_CASTCHECK");
        cast_check_turn = (v != NULL) ? atol(v) : -1;
    }
    if ((cast_check_turn >= 0) && (game.play_gameturn >= (GameTurn)cast_check_turn))
    {
        cast_check_turn = -1;
        test_cast_check();
    }
    if ((save_slot >= 0) && (game.play_gameturn >= save_turn))
    {
        int slot = save_slot;
        save_slot = -1;
        const int saved = SaveManager_FillSlot(slot, "Test save") && SaveManager_Save(slot);
        JUSTMSG("Test: saved slot %d at turn %lu: %s", slot, (unsigned long)game.play_gameturn, saved ? "ok" : "FAILED");
    }
    if ((load_slot >= 0) && (game.play_gameturn >= load_turn))
    {
        int slot = load_slot;
        load_slot = -1;
        enum SaveCheckResult why = SvChk_Loadable;
        enum SaveLoadOutcome outcome = SaveManager_Load(slot, &why);
        JUSTMSG("Test: loaded slot %d: outcome %d, check %d", slot, (int)outcome, (int)why);
    }
}
/******************************************************************************/
