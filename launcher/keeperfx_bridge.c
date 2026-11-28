/* Endless Keeper: the bridge keeperfx.exe (2026-11-28).
 *
 * The game program is EndlessKeeper.exe and the launcher ek-launcher.exe since 2026-11-28 (they were keeperfx.exe and
 * EndlessKeeper.exe). A launcher of 18.1 or older accepts an update only when it holds a keeperfx.exe, and starts the
 * game (and its background battle) as keeperfx.exe or x64\keeperfx.exe; so the update carries this program under both
 * names. It starts EndlessKeeper.exe of its own folder with the same arguments, waits for it and returns its exit code;
 * the game is in a job with it, so when an old launcher ends the bridge (its background battle) the game ends too.
 * The new launcher removes the bridge on its first start (migrate.cpp); the setup never installs it.
 *
 * Built by build.sh (i686, no console window): out/launcher/keeperfx-bridge.exe, packed as keeperfx.exe and
 * x64\keeperfx.exe by installer/make-package.sh. */
#include <windows.h>
#include <wchar.h>

static const wchar_t *after_program(const wchar_t *c)
{
    if (*c == L'"')
    {
        c++;
        while ((*c != 0) && (*c != L'"'))
            c++;
        if (*c != 0)
            c++;
    } else
        while ((*c != 0) && (*c != L' ') && (*c != L'\t'))
            c++;
    while ((*c == L' ') || (*c == L'\t'))
        c++;
    return c;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR line, int show)
{
    (void)inst;
    (void)prev;
    (void)line;
    (void)show;
    wchar_t dir[MAX_PATH];
    const DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if ((n == 0) || (n >= MAX_PATH))
        return 1;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash == NULL)
        return 1;
    *slash = 0;
    wchar_t exe[MAX_PATH + 32];
    _snwprintf(exe, MAX_PATH + 32, L"%ls\\EndlessKeeper.exe", dir);
    exe[MAX_PATH + 31] = 0;
    const wchar_t *args = after_program(GetCommandLineW());
    const size_t len = wcslen(exe) + wcslen(args) + 8;
    wchar_t *cmd = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, len * sizeof(wchar_t));
    if (cmd == NULL)
        return 1;
    _snwprintf(cmd, len, L"\"%ls\" %ls", exe, args);
    cmd[len - 1] = 0;
    /* the game ends with the bridge */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job != NULL)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
        ZeroMemory(&lim, sizeof(lim));
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
    }
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
    {
        MessageBoxW(NULL, L"EndlessKeeper.exe is missing beside keeperfx.exe. Update Endless Keeper again, or run its setup.",
            L"Endless Keeper", MB_ICONERROR | MB_OK);
        return 1;
    }
    if (job != NULL)
        AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}
