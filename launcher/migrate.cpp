// Endless Keeper launcher: the programs' new names (2026-11-28, the user): the launcher is ek-launcher.exe and the game
// EndlessKeeper.exe (they were EndlessKeeper.exe and keeperfx.exe).
//
// A folder updated by a launcher of 18.1 or older holds the bridge keeperfx.exe (keeperfx_bridge.c) and the player's
// shortcuts still point at EndlessKeeper.exe, which is now the game (it hands the first start over to the launcher:
// src/ek_handover.c). While the bridge is there this moves those shortcuts to ek-launcher.exe and removes the bridge and
// the old program files. A folder made by the setup has no bridge: nothing to do.
#include "migrate.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

static std::wstring lower(std::wstring s)
{
    for (wchar_t &c : s)
        c = (c == L'/') ? L'\\' : (wchar_t)towlower(c);
    return s;
}

static bool exists(const std::wstring &p)
{
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static void note(const std::wstring &game_dir, const char *text)
{
    FILE *f = _wfopen((game_dir + L"\\endlesskeeper-launcher.log").c_str(), L"a");
    if (f == nullptr)
        return;
    fprintf(f, "Program names: %s\n", text);
    fclose(f);
}

static std::wstring known_folder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && (p != nullptr))
        out = p;
    CoTaskMemFree(p);
    return out;
}

/** The .lnk files in a folder (and its subfolders, depth levels down). */
static void shortcuts_in(const std::wstring &dir, int depth, std::vector<std::wstring> &out)
{
    if (dir.empty())
        return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        const std::wstring name = fd.cFileName;
        if ((name == L".") || (name == L".."))
            continue;
        const std::wstring path = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (depth > 0)
                shortcuts_in(path, depth - 1, out);
        } else if ((name.size() > 4) && (lower(name.substr(name.size() - 4)) == L".lnk"))
            out.push_back(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/** Shortcuts that start old_target now start new_target (in dir). Returns how many were changed. */
static int move_shortcuts(const std::wstring &old_target, const std::wstring &new_target, const std::wstring &dir)
{
    std::vector<std::wstring> links;
    /* test switch EKL_MIGRATE_DIR=<folder>: only the shortcuts in that folder (a test never touches the player's own) */
    wchar_t test_dir[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"EKL_MIGRATE_DIR", test_dir, MAX_PATH) > 0)
        shortcuts_in(test_dir, 0, links);
    const std::wstring roaming = (test_dir[0] != 0) ? std::wstring() : known_folder(FOLDERID_RoamingAppData);
    if (test_dir[0] == 0)
        shortcuts_in(known_folder(FOLDERID_Desktop), 0, links);
    if (test_dir[0] == 0)
    {
        shortcuts_in(known_folder(FOLDERID_PublicDesktop), 0, links);
        shortcuts_in(known_folder(FOLDERID_Programs), 2, links);
        shortcuts_in(known_folder(FOLDERID_CommonPrograms), 2, links);
    }
    if (!roaming.empty())
    {
        shortcuts_in(roaming + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar", 0, links);
        shortcuts_in(roaming + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\StartMenu", 0, links);
        shortcuts_in(roaming + L"\\Microsoft\\Internet Explorer\\Quick Launch", 0, links);
    }
    const std::wstring want = lower(old_target);
    int moved = 0;
    for (const std::wstring &link : links)
    {
        IShellLinkW *sl = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void **)&sl)))
            continue;
        IPersistFile *pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void **)&pf)))
        {
            wchar_t target[MAX_PATH * 2] = {0};
            if (SUCCEEDED(pf->Load(link.c_str(), STGM_READWRITE)) &&
                SUCCEEDED(sl->GetPath(target, MAX_PATH * 2, nullptr, 0)) && (lower(target) == want))
            {
                wchar_t icon[MAX_PATH * 2] = {0};
                int icon_index = 0;
                sl->GetIconLocation(icon, MAX_PATH * 2, &icon_index);
                sl->SetPath(new_target.c_str());
                sl->SetWorkingDirectory(dir.c_str());
                if ((icon[0] == 0) || (lower(icon) == want))
                    sl->SetIconLocation(new_target.c_str(), 0); /* (the launcher's icon) */
                if (SUCCEEDED(pf->Save(link.c_str(), TRUE)))
                    moved++;
            }
            pf->Release();
        }
        sl->Release();
    }
    if (moved > 0)
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); /* (icons drawn anew) */
    return moved;
}

/** The bridge is a small program (~17 KB); a keeperfx.exe of megabytes is the old game. */
static bool small_file(const std::wstring &p)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a))
        return false;
    return (a.nFileSizeHigh == 0) && (a.nFileSizeLow < (1u << 20));
}

void migrate_program_names(const std::wstring &game_dir)
{
    const std::wstring bridge = game_dir + L"\\keeperfx.exe", game = game_dir + L"\\EndlessKeeper.exe";
    const std::wstring launcher = game_dir + L"\\ek-launcher.exe";
    if (!exists(bridge) || !exists(game) || !exists(launcher))
        return;
    /* (audit 2026-11-28) only the real bridge, and only with the x64 game in place: after an update that stopped part
       way (EndlessKeeper.exe new, keeperfx.exe still the old game) the old programs were deleted and nothing offered
       to finish the update */
    const std::wstring x64_dir = game_dir + L"\\x64";
    if (!small_file(bridge) || (exists(x64_dir) && !exists(x64_dir + L"\\EndlessKeeper.exe")))
    {
        note(game_dir, "keeperfx.exe is not the bridge, or x64\\EndlessKeeper.exe is missing (an update not finished): nothing moved or removed");
        return;
    }
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int moved = move_shortcuts(game, launcher, game_dir);
    if (SUCCEEDED(co))
        CoUninitialize();
    int removed = 0, kept = 0;
    for (const wchar_t *old : {L"keeperfx.exe", L"keeperfx.map", L"keeperfx_hvlog.exe", L"keeperfx_hvlog.map",
                               L"x64\\keeperfx.exe", L"x64\\keeperfx.map", L"EndlessKeeper.exe.old"})
    {
        const std::wstring p = game_dir + L"\\" + old;
        if (!exists(p))
            continue;
        if (DeleteFileW(p.c_str()))
            removed++;
        else
            kept++; /* (in use: the next start removes it) */
    }
    char line[160];
    snprintf(line, sizeof(line), "%d shortcut%s moved to ek-launcher.exe, %d old program file%s removed%s", moved,
        (moved == 1) ? "" : "s", removed, (removed == 1) ? "" : "s", (kept > 0) ? " (some in use: next time)" : "");
    note(game_dir, line);
}

bool migrate_bridge_file(const std::wstring &rel_path)
{
    const std::wstring r = lower(rel_path);
    return (r == L"keeperfx.exe") || (r == L"x64\\keeperfx.exe");
}
