/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file ek_handover.c
 *     Endless Keeper: the game hands over to the launcher once, after the programs were renamed.
 * @par Purpose:
 *     Since 2026-11-28 (the user) the game program is EndlessKeeper.exe and the launcher ek-launcher.exe; they were
 *     keeperfx.exe and EndlessKeeper.exe. A launcher of 18.1 or older that updates its folder writes the new game
 *     (EndlessKeeper.exe) over itself, so the player's "Endless Keeper" shortcuts would start the game without the
 *     launcher. The update carries a bridge keeperfx.exe (launcher/keeperfx_bridge.c, which old launchers need to
 *     accept it); while that bridge is still in the folder the shortcuts have not been moved yet, so the game started
 *     with no arguments (as a shortcut starts it) starts ek-launcher.exe instead and ends. The launcher moves the
 *     shortcuts and removes the bridge (launcher/migrate.cpp); from then on EndlessKeeper.exe starts the game.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "ek_handover.h"

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#endif
#include "post_inc.h"

int ek_handover_to_launcher(int argc)
{
#ifdef _WIN32
    if (argc > 1)
        return 0;
    wchar_t dir[MAX_PATH];
    const DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if ((n == 0) || (n >= MAX_PATH))
        return 0;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if ((slash == NULL) || (_wcsicmp(slash + 1, L"EndlessKeeper.exe") != 0))
        return 0;
    *slash = 0;
    wchar_t launcher[MAX_PATH + 32], bridge[MAX_PATH + 32], cmd[MAX_PATH + 40];
    _snwprintf(launcher, MAX_PATH + 32, L"%ls\\ek-launcher.exe", dir);
    launcher[MAX_PATH + 31] = 0;
    _snwprintf(bridge, MAX_PATH + 32, L"%ls\\keeperfx.exe", dir);
    bridge[MAX_PATH + 31] = 0;
    if ((GetFileAttributesW(launcher) == INVALID_FILE_ATTRIBUTES) || (GetFileAttributesW(bridge) == INVALID_FILE_ATTRIBUTES))
        return 0;
    _snwprintf(cmd, MAX_PATH + 40, L"\"%ls\"", launcher);
    cmd[MAX_PATH + 39] = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(launcher, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
#else
    (void)argc;
    return 0;
#endif
}
