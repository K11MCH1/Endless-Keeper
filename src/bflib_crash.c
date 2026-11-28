/******************************************************************************/
// Bullfrog Engine Emulation Library - for use to remake classic games like
// Syndicate Wars, Magic Carpet or Dungeon Keeper.
/******************************************************************************/
/** @file bflib_crash.c
 *     Program failure handling system.
 * @par Purpose:
 *     Installs handlers to capture crashes; makes backtrace and clean exit.
 * @par Comment:
 *     None.
 * @author   Tomasz Lis
 * @date     09 Nov 2010 - 11 Nov 2010
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "pre_inc.h"
#include "kfx/renderer/RendererManager.h"
#include "bflib_crash.h"
#include "replay.h"
#include "lag_watch.h"
#include <signal.h>
#include <stdarg.h>
#if !defined(_WIN32)
#define BF_POSIX_CRASH 1
#endif
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <excpt.h>
#include <dbghelp.h>
#include <psapi.h>
#include <shellapi.h>
#endif
#if defined(BF_POSIX_CRASH)
#include <execinfo.h>
#include <ucontext.h>
#include <unistd.h>
#include <dlfcn.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include "bflib_basics.h"
#include "bflib_video.h"
#include "post_inc.h"

/******************************************************************************/
static const char* sigstr(int s)
{
  switch(s)
    {
    case SIGINT : return "Interrupt (ANSI)";
    case SIGILL : return "Illegal instruction (ANSI)";
    case SIGABRT : return "Abort (ANSI)";
    case SIGFPE : return "Floating-point exception (ANSI)";
    case SIGSEGV : return "Segmentation violation (ANSI)";
    case SIGTERM : return "Termination (ANSI)";
#if defined(BF_POSIX_CRASH)
    case SIGHUP : return "Hangup (POSIX)";
    case SIGQUIT : return "Quit (POSIX)";
    case SIGTRAP : return "Trace trap (POSIX)";
    case SIGBUS : return "BUS error (4.2 BSD)";
    case SIGKILL : return "Kill, unblockable (POSIX)";
    case SIGUSR1 : return "User-defined signal 1 (POSIX)";
    case SIGUSR2 : return "User-defined signal 2 (POSIX)";
    case SIGPIPE : return "Broken pipe (POSIX)";
    case SIGALRM : return "Alarm clock (POSIX)";
    case SIGCHLD : return "Child status has changed (POSIX)";
    case SIGCONT : return "Continue (POSIX)";
    case SIGSTOP : return "Stop, unblockable (POSIX)";
    case SIGTSTP : return "Keyboard stop (POSIX)";
    case SIGTTIN : return "Background read from tty (POSIX)";
    case SIGTTOU : return "Background write to tty (POSIX)";
    case SIGURG : return "Urgent condition on socket (4.2 BSD)";
    case SIGXCPU : return "CPU limit exceeded (4.2 BSD)";
    case SIGXFSZ : return "File size limit exceeded (4.2 BSD)";
    case SIGVTALRM : return "Virtual alarm clock (4.2 BSD)";
    case SIGPROF : return "Profiling alarm clock (4.2 BSD)";
    case SIGWINCH : return "Window size change (4.3 BSD, Sun)";
    case SIGIO : return "I/O now possible (4.2 BSD)";
#ifdef SIGSYS
    case SIGSYS : return "Bad system call";
#endif
#ifdef SIGSTKFLT
    case SIGSTKFLT : return "Stack fault";
#endif
#ifdef SIGPWR
    case SIGPWR : return "Power failure restart (System V)";
#endif
#else
    case SIGBREAK : return "Ctrl-Break (Win32)";
#endif
    }
  return "unknown signal";
}

static void log_replay_crashlog(void)
{
    const uint8_t state = replay_crashlog_state;
    if (state == 0)
        return;
    const char *playback = (state == 2) ? "true" : "false";
    if (replay_crashlog_fname[0] == '\0')
        LbErrorLog("packetfile:{\"turn\":%u, \"playback\":%s}\n", (unsigned)replay_crashlog_turn, playback);
    else
        LbErrorLog("packetfile:{\"turn\":%u, \"playback\":%s, \"path\":\"%s\"}\n", (unsigned)replay_crashlog_turn, playback, replay_crashlog_fname);
}

/* Out of memory (2026-11-23, after GitHub issue 3): every crash log says what the memory was, and when it had nearly
   run out the log and the crash box say so: a crash for want of memory looks like any other fault (a failed allocation
   is used as if it had worked). Linux's and Android's out-of-memory killer ends the game with a signal no program can
   catch; the lag watch's "memory is running out" line is what such a game leaves behind. */
static char crash_low_memory[192];

static void crash_log_memory(void)
{
    char mem[192];
    lag_watch_memory_text(mem, sizeof(mem));
    if (mem[0] != '\0')
        LbErrorLog("At the crash, %s.\n", mem + 2);
    if (lag_watch_memory_short(crash_low_memory, sizeof(crash_low_memory)))
        LbErrorLog("The game probably crashed because memory ran out: %s.\n", crash_low_memory);
    else
        crash_low_memory[0] = '\0';
}

void exit_handler(void)
{
    LbErrorLog("Application exit called.\n");
}

/******************************************************************************/
/* Endless Keeper crash report (2026-11-21, the user: "We need a crash handler as well that shows a windows pop up
   window instead of just closing the game and showing nothing", after a crash inside Intel's OpenGL driver left only
   the log): a minidump with every thread, a copy of the log (the launcher's background may write over keeperfx.log),
   both in crashes\, and a message box saying what happened and where the files are. No box for a run nobody watches:
   a dedicated server, the launcher's background (LbErrorParachuteQuiet), windowless test runs (SDL's dummy video
   driver) or KFX_CRASH_QUIET. */
static TbBool crash_quiet = false;

void LbErrorParachuteQuiet(TbBool quiet)
{
    crash_quiet = quiet;
}

#if defined(_WIN32)
static char crash_dump_path[MAX_PATH];
static char crash_log_path[MAX_PATH];
static char crash_log_source[MAX_PATH]; /* the log this run writes (-log, or keeperfx.log) */

static TbBool crash_box_wanted(void)
{
    if (crash_quiet || (getenv("KFX_CRASH_QUIET") != NULL))
        return false;
    const char *drv = getenv("SDL_VIDEO_DRIVER");
    if (drv == NULL)
        drv = getenv("SDL_VIDEODRIVER");
    return !((drv != NULL) && (strcmp(drv, "dummy") == 0));
}

/** Where the fault was: "igxe2hpgicd64.dll+0xbba37", or the address alone. */
static void crash_where(const void *addr, char *out, size_t len, TbBool *in_driver)
{
    HMODULE mod = NULL;
    *in_driver = false;
    if ((addr != NULL) && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)addr, &mod) && (mod != NULL))
    {
        char name[MAX_PATH];
        if (GetModuleFileNameA(mod, name, sizeof(name)) == 0)
            snprintf(name, sizeof(name), "?");
        const char *base = strrchr(name, '\\');
        base = (base != NULL) ? base + 1 : name;
        snprintf(out, len, "%s+0x%llx", base, (unsigned long long)((const char *)addr - (const char *)mod));
        /* the graphics drivers' OpenGL parts: Intel (ig*), NVIDIA (nvoglv*), AMD (atio*, amdxc*) */
        if ((_strnicmp(base, "ig", 2) == 0) || (_strnicmp(base, "nvogl", 5) == 0) || (_strnicmp(base, "atio", 4) == 0) ||
            (_strnicmp(base, "amdx", 4) == 0))
            *in_driver = true;
    } else
        snprintf(out, len, "address %p", addr);
}

/** Writes crashes\crash-<date>-<time>.dmp (every thread, the memory they point at) and notes the log copy's name.
 *  fault_thread: the thread the fault happened on (the report itself is written on a helper thread). */
static TbBool crash_write_dump(LPEXCEPTION_POINTERS info, DWORD fault_thread)
{
    crash_dump_path[0] = '\0';
    crash_log_path[0] = '\0';
    snprintf(crash_log_source, sizeof(crash_log_source), "%s", LbErrorLogFileName());
    char dir[MAX_PATH];
    if (GetCurrentDirectoryA(sizeof(dir) - 16, dir) == 0)
        return false;
    strcat(dir, "\\crashes");
    CreateDirectoryA(dir, NULL);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stem[MAX_PATH];
    snprintf(stem, sizeof(stem), "%s\\crash-%04d%02d%02d-%02d%02d%02d", dir, st.wYear, st.wMonth, st.wDay, st.wHour,
        st.wMinute, st.wSecond);
    snprintf(crash_log_path, sizeof(crash_log_path), "%s.log", stem);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s.dmp", stem);
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = fault_thread;
    mei.ExceptionPointers = info;
    mei.ClientPointers = FALSE;
    const MINIDUMP_TYPE kind = (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
        MiniDumpWithUnloadedModules | MiniDumpWithHandleData);
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, kind, (info != NULL) ? &mei : NULL,
        NULL, NULL);
    CloseHandle(file);
    if (!ok)
    {
        DeleteFileA(path);
        LbErrorLog("Crash report: the dump could not be written (error %lu)\n", (unsigned long)GetLastError());
        return false;
    }
    snprintf(crash_dump_path, sizeof(crash_dump_path), "%s", path);
    LbErrorLog("Crash report: %s\n", crash_dump_path);
    return true;
}

static BOOL CALLBACK crash_hide_window(HWND wnd, LPARAM lparam)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(wnd, &pid);
    /* Async (audit 2026-11-21): ShowWindow on a window of another thread waits for that thread, and with a fault on
       the render thread the window's thread (the main one) is itself waiting for the render thread: the box never came */
    if ((pid == (DWORD)lparam) && IsWindowVisible(wnd))
        ShowWindowAsync(wnd, SW_HIDE);
    return TRUE;
}

/** After the log is closed: its copy beside the dump, then the box. */
static void crash_tell_user(const char *what, const void *addr)
{
    if ((crash_log_path[0] != '\0') && ((crash_log_source[0] == '\0') || !CopyFileA(crash_log_source, crash_log_path, FALSE)))
        crash_log_path[0] = '\0';
    if (!crash_box_wanted())
        return;
    char where[MAX_PATH + 32];
    TbBool in_driver = false;
    crash_where(addr, where, sizeof(where), &in_driver);
    char text[2048];
    int n = snprintf(text, sizeof(text), "Endless Keeper ran into an error and has to close.\n\nWhat happened: %s\nWhere: %s\n",
        what, where);
    if (replay_crashlog_state != 0)
        n += snprintf(text + n, sizeof(text) - n, "Game turn: %u\n", (unsigned)replay_crashlog_turn);
    if (crash_low_memory[0] != '\0')
        n += snprintf(text + n, sizeof(text) - n, "\nMemory had nearly run out: %s. Closing other programs before "
            "playing may help.\n", crash_low_memory);
    if (in_driver)
        n += snprintf(text + n, sizeof(text) - n, "\nThis happened inside the graphics card's driver. Installing the newest "
            "driver for your graphics card often fixes this.\n");
    if ((crash_dump_path[0] != '\0') && (crash_log_path[0] != '\0'))
        n += snprintf(text + n, sizeof(text) - n, "\nA crash report was saved:\n%s\n%s\n\nPlease send both files with your "
            "bug report.\n\nOpen the folder now?", crash_dump_path, crash_log_path);
    else if (crash_dump_path[0] != '\0')
        n += snprintf(text + n, sizeof(text) - n, "\nA crash report was saved:\n%s\n\nPlease send it and the game's log (%s) "
            "with your bug report.\n\nOpen the folder now?", crash_dump_path, crash_log_source);
    else
        n += snprintf(text + n, sizeof(text) - n, "\nNo crash report could be saved. The game's log is %s.\n", crash_log_source);
    /* the game's own windows go first: a full-screen window would cover the box */
    EnumWindows(crash_hide_window, (LPARAM)GetCurrentProcessId());
    const UINT kind = MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND | MB_TASKMODAL | ((crash_dump_path[0] != '\0') ? MB_YESNO : MB_OK);
    if ((MessageBoxA(NULL, text, "Endless Keeper has crashed", kind) == IDYES) && (crash_dump_path[0] != '\0'))
    {
        char args[MAX_PATH + 16];
        snprintf(args, sizeof(args), "/select,\"%s\"", crash_dump_path);
        ShellExecuteA(NULL, "open", "explorer.exe", args, NULL, SW_SHOWNORMAL);
    }
}

static volatile LONG crash_once = 0;
static volatile DWORD crash_thread = 0;        /* the thread whose fault is being reported */
static volatile DWORD crash_helper_thread = 0; /* the thread writing the report */

/** A fault while another fault is being reported (audit 2026-11-21). A second thread waits for good, so the report and
 *  the box are not cut off: the process ends when the first report is done. A fault on the reporting threads
 *  themselves gives up (true: end now). */
static TbBool crash_second_fault(void)
{
    const DWORD me = GetCurrentThreadId();
    if ((me == crash_thread) || (me == crash_helper_thread))
        return true;
    for (;;)
        Sleep(INFINITE);
}

struct CrashJob {
    LPEXCEPTION_POINTERS info;
    DWORD thread;
    const char *what;
    const void *addr;
    TbBool overflow; /* a stack overflow: the log lines are written here too (the faulting thread has no stack) */
};

static void crash_describe(DWORD code, char *what, size_t len);

static DWORD WINAPI crash_job_run(LPVOID param)
{
    struct CrashJob *job = (struct CrashJob *)param;
    crash_helper_thread = GetCurrentThreadId();
    if (job->overflow)
    {
        static char what[96];
        LbErrorLog("Failure code %lx received.\n", (unsigned long)EXCEPTION_STACK_OVERFLOW);
        LbErrorLog("Stack overflow: no stack trace here, see the crash report.\n");
        log_replay_crashlog();
        crash_describe(EXCEPTION_STACK_OVERFLOW, what, sizeof(what));
        job->what = what;
    }
    crash_log_memory();
    crash_write_dump(job->info, job->thread);
    LbErrorLogClose();
    crash_tell_user(job->what, job->addr);
    return 0;
}

/** The dump, the log copy and the box, on a fresh thread with its own stack (audit 2026-11-21): after a stack overflow
 *  the faulting thread has no stack left for MiniDumpWriteDump and MessageBox, and the dump of the faulting thread is
 *  better taken from outside it. Done here if no thread can be made. */
static void crash_report(LPEXCEPTION_POINTERS info, const char *what, const void *addr)
{
    struct CrashJob job;
    job.info = info;
    job.thread = GetCurrentThreadId();
    job.what = what;
    job.addr = addr;
    job.overflow = false;
    HANDLE th = CreateThread(NULL, 256 * 1024, crash_job_run, &job, 0, NULL);
    if (th == NULL)
    {
        crash_job_run(&job);
        return;
    }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
}

/** A stack overflow: a few hundred bytes of stack are left (a 32-bit build died in the crash handler's first log
 *  line, 2026-11-21), so everything, the log lines too, is done on the helper thread; nothing here needs stack (the
 *  job is static). The caller ends the program after. */
static void crash_overflow(LPEXCEPTION_POINTERS info)
{
    static struct CrashJob job;
    job.info = info;
    job.thread = GetCurrentThreadId();
    job.what = "a stack overflow";
    job.addr = info->ExceptionRecord->ExceptionAddress;
    job.overflow = true;
    HANDLE th = CreateThread(NULL, 256 * 1024, crash_job_run, &job, 0, NULL);
    if (th != NULL)
        WaitForSingleObject(th, INFINITE);
}

/** "a memory access fault (exception 0xc0000005)" and the like. */
static void crash_describe(DWORD code, char *what, size_t len)
{
    if (code == EXCEPTION_ACCESS_VIOLATION)
        snprintf(what, len, "a memory access fault (exception 0x%08lx)", (unsigned long)code);
    else if (code == EXCEPTION_INT_DIVIDE_BY_ZERO)
        snprintf(what, len, "a division by zero (exception 0x%08lx)", (unsigned long)code);
    else if (code == EXCEPTION_STACK_OVERFLOW)
        snprintf(what, len, "a stack overflow (exception 0x%08lx)", (unsigned long)code);
    else if (code == EXCEPTION_ILLEGAL_INSTRUCTION)
        snprintf(what, len, "an illegal instruction (exception 0x%08lx)", (unsigned long)code);
    else
        snprintf(what, len, "exception 0x%08lx", (unsigned long)code);
}

/* The last fault of each kind that ends a program, as it happened (copied the moment Windows raised it): on 64-bit
   MinGW the C runtime turns a fault into a signal (ctrl_handler) and the exception's details are gone by then. */
static EXCEPTION_RECORD crash_last_rec;
static CONTEXT crash_last_ctx;
static DWORD crash_last_thread;
static volatile LONG crash_have_last = 0;

static LONG CALLBACK crash_first_chance(PEXCEPTION_POINTERS info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if ((code == EXCEPTION_ACCESS_VIOLATION) || (code == EXCEPTION_INT_DIVIDE_BY_ZERO) || (code == EXCEPTION_STACK_OVERFLOW) ||
        (code == EXCEPTION_ILLEGAL_INSTRUCTION) || (code == EXCEPTION_PRIV_INSTRUCTION) || (code == EXCEPTION_IN_PAGE_ERROR))
    {
        crash_last_rec = *info->ExceptionRecord;
        crash_last_rec.ExceptionRecord = NULL;
        crash_last_ctx = *info->ContextRecord;
        crash_last_thread = GetCurrentThreadId();
        InterlockedExchange(&crash_have_last, 1);
    }
    /* a stack overflow is reported here, first, while some stack is left (the later handlers need more) */
    if ((code == EXCEPTION_STACK_OVERFLOW) && (InterlockedExchange(&crash_once, 1) == 0))
    {
        crash_thread = GetCurrentThreadId();
        crash_overflow(info);
        TerminateProcess(GetCurrentProcess(), (UINT)EXCEPTION_STACK_OVERFLOW);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void ctrl_handler(int sig_id)
{
    signal(sig_id, SIG_DFL);
    LbErrorLog("Failure signal: %s.\n",sigstr(sig_id));
    log_replay_crashlog();
#if defined(_WIN32)
    /* a fault (not Ctrl-C or a close): the crash report and the box too */
    const TbBool fault = (sig_id == SIGILL) || (sig_id == SIGABRT) || (sig_id == SIGFPE) || (sig_id == SIGSEGV);
    if (fault && (InterlockedExchange(&crash_once, 1) != 0))
        crash_second_fault(); /* another thread's fault: waits; the reporting threads' own: on to the end */
    else if (fault)
    {
        crash_thread = GetCurrentThreadId();
        /* the fault behind the signal, when this thread had one: its place and the dump's fault context */
        const TbBool known = (sig_id != SIGABRT) && crash_have_last && (crash_last_thread == GetCurrentThreadId());
        EXCEPTION_POINTERS ep;
        ep.ExceptionRecord = &crash_last_rec;
        ep.ContextRecord = &crash_last_ctx;
        char what[96];
        if (known)
        {
            crash_describe(crash_last_rec.ExceptionCode, what, sizeof(what));
            LbErrorLog("The signal came from %s at %p.\n", what, crash_last_rec.ExceptionAddress);
        } else
            snprintf(what, sizeof(what), "%s", sigstr(sig_id));
        crash_report(known ? &ep : NULL, what, known ? crash_last_rec.ExceptionAddress : NULL);
    }
#endif
    RendererResetScreen(true);
    LbErrorLogClose();
    raise(sig_id);
}

#if defined(_WIN32)
static void
_backtrace(int depth , LPCONTEXT context)
{
    int64_t keeperFxBaseAddr = 0x00000000;
    char mapFileLine[512];

    /* Endless Keeper: the map beside the program, by its own name (EndlessKeeper.map for EndlessKeeper.exe, the
       64-bit one's in x64; 2026-11-28: the programs were renamed, and the 64-bit game opened the 32-bit map of the
       folder it plays from); the old fixed names after it */
    FILE *mapFile = NULL;
    {
        char map_path[MAX_PATH + 8];
        const DWORD n = GetModuleFileNameA(NULL, map_path, MAX_PATH);
        if ((n > 4) && (n < MAX_PATH) && (_stricmp(map_path + n - 4, ".exe") == 0))
        {
            memcpy(map_path + n - 4, ".map", 5);
            mapFile = fopen(map_path, "r");
        }
    }
    #if (BFDEBUG_LEVEL > 7)
    if (mapFile == NULL)
        mapFile = fopen("keeperfx_hvlog.map", "r");
    #else
    if (mapFile == NULL)
        mapFile = fopen("keeperfx.map", "r");
    #endif

    if (mapFile)
    {
        // Get base address from map file
        while (fgets(mapFileLine, sizeof(mapFileLine), mapFile) != NULL)
        {
            if (sscanf(mapFileLine, " %*x __image_base__ = %llx", &keeperFxBaseAddr) == 1)
            {
                SYNCDBG(0, "KeeperFX base address in map file: %I64x", keeperFxBaseAddr);
                break;
            }
        }

        memset(mapFileLine, 0, sizeof(mapFileLine));
        fseek(mapFile, 0, SEEK_SET);

        if(keeperFxBaseAddr == 0x00000000)
        {
            fclose(mapFile);
            mapFile = NULL; /* (audit 2026-11-21: it was read and closed again below, inside the crash handler) */
        }
    }
    else
    {
        LbWarnLog("No keeperfx.map file found for stacktrace map lookups\n");
    }

    STACKFRAME frame;
    memset(&frame,0,sizeof(frame));

#ifdef _WIN64
    /* 64-bit Windows (2026-11-09): dbghelp's StackWalk/STACKFRAME are the 64-bit ones there */
    frame.AddrPC.Offset = context->Rip;
#else
    frame.AddrPC.Offset = context->Eip;
#endif

    frame.AddrPC.Mode = AddrModeFlat;
#ifdef _WIN64
    frame.AddrStack.Offset = context->Rsp;
#else
    frame.AddrStack.Offset = context->Esp;
#endif
    frame.AddrStack.Mode = AddrModeFlat;
#ifdef _WIN64
    frame.AddrFrame.Offset = context->Rbp;
#else
    frame.AddrFrame.Offset = context->Ebp;
#endif
    frame.AddrFrame.Mode = AddrModeFlat;

    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    // Loop through all traces in the stack
#ifdef _WIN64
    const DWORD walk_machine = IMAGE_FILE_MACHINE_AMD64;
#else
    const DWORD walk_machine = IMAGE_FILE_MACHINE_I386;
#endif
    while (StackWalk(walk_machine, process, thread, &frame, context, 0, SymFunctionTableAccess, SymGetModuleBase, 0))
    {
        --depth;
        if (depth < 0)
        {
            break;
        }

        // Get the base address in the module
        // This is where the address space of the functions start
        DWORD_PTR module_base = SymGetModuleBase(process, frame.AddrPC.Offset);

        // Get the name of the module
        // The module will be the keeperfx bin or a library
        const char * module_name = "[unknown module]";
        char module_name_raw[MAX_PATH];
        if (module_base && GetModuleFileNameA((HINSTANCE)module_base, module_name_raw, MAX_PATH))
        {
            module_name = strrchr(module_name_raw,'\\');
            if (module_name != NULL)
            {
                module_name++;
            }
            else
            {
                module_name = module_name_raw;
            }
        }

        // Check if the name of this module starts with 'keeperfx'
        // This can be done better but at this moment it should only match our own keeperfx.exe and keeperfx_hvlog.exe
        if ((strncmp(module_name, "keeperfx", strlen("keeperfx")) == 0) ||
            (_strnicmp(module_name, "EndlessKeeper", strlen("EndlessKeeper")) == 0)) /* (Endless Keeper's name since 2026-11-28) */
        {

            // Look up using the keeperfx.map file
            if(mapFile)
            {

                int64_t checkAddr = frame.AddrPC.Offset - module_base + keeperFxBaseAddr;

                bool addrFound = false;
                int64_t prevAddr = 0x00000000;
                char prevName[512];
                prevName[0] = 0;

                // Loop through all lines in the mapFile
                // This should be pretty fast on modern systems
                while (fgets(mapFileLine, sizeof(mapFileLine), mapFile) != NULL)
                {

                    int64_t addr;
                    char name[512];
                    name[0] = 0;
                    if (
                        sscanf(mapFileLine, "%llx %[^\t\n]", &addr, name) == 2 ||
                        sscanf(mapFileLine, " .text %llx %[^\t\n]", &addr, name) == 2
                    ) {
                        // The offsets in our trace do not point to the start of the function.
                        // However, only the address of the start of our functions is stored in the map file.
                        // So we'll trace back to the last address.
                        if (checkAddr > prevAddr && checkAddr < addr)
                        {
                            int64_t displacement = checkAddr - prevAddr;

                            // Handle library traces
                            // Example: '0x123 lib/thing.o'
                            // We don't want that size at the beginning, but we do want the library path.
                            char *splitPos = strchr(prevName, ' ');
                            if (strncmp(prevName, "0x", 2) == 0 && splitPos != NULL){

                                    // Remove everything before the space
                                    memmove(prevName, splitPos + 1, strlen(splitPos));

                                    // Find the last slash in the string to isolate the filename
                                    char *lastSlash = strrchr(prevName, '/');
                                    if (lastSlash != NULL) {
                                        // Move the filename to the start
                                        memmove(prevName, lastSlash + 1, strlen(lastSlash + 1) + 1);
                                    }

                                    // Prepend the arrow symbol
                                    memmove(prevName + 3, prevName, strlen(prevName) + 1); // make space for the arrow symbol
                                    memcpy(prevName, "-> ", 3); // prepend the arrow symbol
                            }

                            // Log it
                            LbJustLog(
                                "[#%-2d] %-12s : %-36s [0x%I64x+0x%I64x]\t map lookup for: %04x:%08x, base: %08x\n",
                                depth, module_name, prevName, prevAddr, displacement, (uint16_t)context->SegCs, (uint32_t)frame.AddrPC.Offset, (uint32_t)module_base);

                            addrFound = true;
                            break;
                        }
                    }

                    prevAddr = addr;
                    strcpy(prevName, name);
                }

                // Reset buffers
                fseek(mapFile, 0, SEEK_SET);
                memset(mapFileLine, 0, sizeof(mapFileLine));

                if(addrFound)
                {
                    continue;
                }
            }
        }

        // Symbol information for looking up symbols
        char symbol_info[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
        PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)symbol_info;
        pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSymbol->MaxNameLen = MAX_SYM_NAME;

        // The distance between the original function and the call in the trace
        uint64_t sfaDisplacement;

        // First check if we can find the symbol by its address
        // This works if there are any debug symbols available and also works for most OS libraries
        if (SymFromAddr(process, frame.AddrPC.Offset, &sfaDisplacement, pSymbol))
        {
            LbJustLog("[#%-2d] %-12s : %-36s [%04x:%08x+0x%I64x, base %08x]\t symbol lookup\n",
                      depth, module_name, pSymbol->Name, (uint16_t)context->SegCs, (uint32_t)frame.AddrPC.Offset, sfaDisplacement, (uint32_t)module_base);
        }
        else
        {
            // Fallback
            LbJustLog("[#%-2d] %-12s : at %04x:%08x, base %08x\n", depth, module_name, (uint16_t)context->SegCs, (uint32_t)frame.AddrPC.Offset, (uint32_t)module_base);
        }
    }

    if(mapFile){
        fclose(mapFile);
    }
}

static LONG CALLBACK ctrl_handler_w32(LPEXCEPTION_POINTERS info)
{
    if (InterlockedExchange(&crash_once, 1) != 0)
    {
        crash_second_fault(); /* another thread's fault waits for good; the reporting threads' own ends the program */
        return EXCEPTION_EXECUTE_HANDLER;
    }
    crash_thread = GetCurrentThreadId();
    const TbBool overflow = (info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW);
    if (overflow)
    {
        crash_overflow(info);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    switch (info->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        switch (info->ExceptionRecord->ExceptionInformation[0])
        {
        case 0:
            LbErrorLog("Attempt to read from inaccessible memory address.\n");
            break;
        case 1:
            LbErrorLog("Attempt to write to inaccessible memory address.\n");
            break;
        case 8:
            LbErrorLog("User-mode data execution prevention (DEP) violation.\n");
            break;
        default:
            LbErrorLog("Memory access violation, code %d.\n",(int)info->ExceptionRecord->ExceptionInformation[0]);
            break;
        }
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        LbErrorLog("Attempt of integer division by zero.\n");
        break;
    default:
        LbErrorLog("Failure code %lx received.\n",info->ExceptionRecord->ExceptionCode);
        break;
    }
    log_replay_crashlog();
    if (overflow) {
        /* the stack walk needs more stack than an overflowed thread has left; the dump holds the stack */
        LbErrorLog("Stack overflow: no stack trace here, see the crash report.\n");
    }
    else if (!SymInitialize(GetCurrentProcess(), 0, TRUE)) {
        LbErrorLog("Failed to init symbol context\n");
    }
    else {
            _backtrace(16 , info->ContextRecord);
            SymCleanup(GetCurrentProcess());
    }
    {
        char what[96];
        crash_describe(info->ExceptionRecord->ExceptionCode, what, sizeof(what));
        crash_report(info, what, info->ExceptionRecord->ExceptionAddress);
    }
    RendererResetScreen(true);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#if defined(BF_POSIX_CRASH)
static const char *posix_sigcode_str(int sig_id, int si_code)
{
    switch (sig_id)
    {
    case SIGSEGV:
        switch (si_code)
        {
        case SEGV_MAPERR: return "address not mapped to object";
        case SEGV_ACCERR: return "invalid permissions for mapped object";
        default: return "unknown segv code";
        }
    case SIGFPE:
        switch (si_code)
        {
        case FPE_INTDIV: return "integer divide by zero";
        case FPE_INTOVF: return "integer overflow";
        case FPE_FLTDIV: return "floating-point divide by zero";
        case FPE_FLTOVF: return "floating-point overflow";
        case FPE_FLTUND: return "floating-point underflow";
        case FPE_FLTRES: return "floating-point inexact result";
        case FPE_FLTINV: return "floating-point invalid operation";
        case FPE_FLTSUB: return "subscript out of range";
        default: return "unknown fpe code";
        }
    case SIGILL:
        switch (si_code)
        {
        case ILL_ILLOPC: return "illegal opcode";
        case ILL_ILLOPN: return "illegal operand";
        case ILL_ILLADR: return "illegal addressing mode";
        case ILL_ILLTRP: return "illegal trap";
        case ILL_PRVOPC: return "privileged opcode";
        case ILL_PRVREG: return "privileged register";
        case ILL_COPROC: return "coprocessor error";
        case ILL_BADSTK: return "internal stack error";
        default: return "unknown ill code";
        }
#ifdef SIGBUS
    case SIGBUS:
        switch (si_code)
        {
        case BUS_ADRALN: return "invalid address alignment";
        case BUS_ADRERR: return "nonexistent physical address";
        case BUS_OBJERR: return "object-specific hardware error";
        default: return "unknown bus code";
        }
#endif
    default:
        return "unknown signal code";
    }
}

static void log_posix_context(void *context)
{
#if defined(__linux__) && defined(__x86_64__)
    ucontext_t *uctx = (ucontext_t *)context;
#if defined(REG_RIP) && defined(REG_RSP)
    LbErrorLog("Context RIP=%p RSP=%p.\n",
        (void *)uctx->uc_mcontext.gregs[REG_RIP],
        (void *)uctx->uc_mcontext.gregs[REG_RSP]);
#else
    (void)uctx;
#endif
#elif defined(__linux__) && defined(__i386__)
    ucontext_t *uctx = (ucontext_t *)context;
    LbErrorLog("Context EIP=%p ESP=%p.\n",
        (void *)uctx->uc_mcontext.gregs[REG_EIP],
        (void *)uctx->uc_mcontext.gregs[REG_ESP]);
#elif defined(__linux__) && defined(__aarch64__)
    ucontext_t *uctx = (ucontext_t *)context;
    LbErrorLog("Context PC=%p SP=%p.\n",
        (void *)uctx->uc_mcontext.pc,
        (void *)uctx->uc_mcontext.sp);
#else
    (void)context;
#endif
}

static void write_stderr_line(const char *line, size_t line_len)
{
    ssize_t written = write(STDERR_FILENO, line, line_len);
    (void)written;
}

static void _backtrace_posix(int depth)
{
    void *frames[64];
    int max_frames = (depth > (int)(sizeof(frames) / sizeof(frames[0])))
        ? (int)(sizeof(frames) / sizeof(frames[0]))
        : depth;
    int count = backtrace(frames, max_frames);

    if (count > 0)
    {
        backtrace_symbols_fd(frames, count, STDERR_FILENO);

        char **symbols = backtrace_symbols(frames, count);
        if (symbols != NULL)
        {
            int printed_idx = 0;
            for (int idx = 0; idx < count; idx++)
            {
                Dl_info info;
                if (dladdr(frames[idx], &info) != 0)
                {
                    if (info.dli_sname != NULL)
                    {
                        if ((strcmp(info.dli_sname, "_backtrace_posix") == 0)
                         || (strcmp(info.dli_sname, "ctrl_handler_posix") == 0)
                         || (strcmp(info.dli_sname, "write_stderr_line") == 0)
                         || (strcmp(info.dli_sname, "log_posix_context") == 0))
                        {
                            continue;
                        }
                    }

                    const char *module_name = (info.dli_fname != NULL) ? info.dli_fname : "[unknown module]";
                    const char *symbol_name = (info.dli_sname != NULL) ? info.dli_sname : "[unknown symbol]";
                    if ((info.dli_sname == NULL) && (symbols[idx] != NULL))
                    {
                        symbol_name = symbols[idx];
                    }
                    uintptr_t symbol_addr = (uintptr_t)info.dli_saddr;
                    uintptr_t frame_addr = (uintptr_t)frames[idx];
                    uintptr_t displacement = 0;
                    if (symbol_addr != 0 && frame_addr >= symbol_addr)
                    {
                        displacement = frame_addr - symbol_addr;
                    }

                    LbJustLog("[#%-2d] %-20s : %-36s [%p+0x%" PRIxPTR "]\n",
                        printed_idx, module_name, symbol_name, frames[idx], displacement);
                    printed_idx++;
                }
                else
                {
                    LbJustLog("[#%-2d] %s\n", printed_idx, symbols[idx]);
                    printed_idx++;
                }
            }
            free(symbols);
        }
    }
}

static void ctrl_handler_posix(int sig_id, siginfo_t *info, void *context)
{
    const void *fault_addr = (info != NULL) ? info->si_addr : NULL;

    {
        static const char crash_msg[] = "KeeperFX fatal signal received\n";
        write_stderr_line(crash_msg, sizeof(crash_msg) - 1);
    }

    if (sig_id == SIGSEGV)
    {
        static const char segv_msg[] = "Signal: SIGSEGV\n";
        write_stderr_line(segv_msg, sizeof(segv_msg) - 1);
    }
    else if (sig_id == SIGABRT)
    {
        static const char abrt_msg[] = "Signal: SIGABRT\n";
        write_stderr_line(abrt_msg, sizeof(abrt_msg) - 1);
    }
    else if (sig_id == SIGFPE)
    {
        static const char fpe_msg[] = "Signal: SIGFPE\n";
        write_stderr_line(fpe_msg, sizeof(fpe_msg) - 1);
    }

    LbErrorLog("Failure signal: %s (%d).\n", sigstr(sig_id), sig_id);
    if (info != NULL)
    {
        LbErrorLog("Signal code: %d (%s).\n", info->si_code, posix_sigcode_str(sig_id, info->si_code));
    }
    LbErrorLog("Fault address: %p.\n", fault_addr);
    log_replay_crashlog();
    if ((sig_id != SIGINT) && (sig_id != SIGTERM) && (sig_id != SIGHUP) && (sig_id != SIGQUIT))
        crash_log_memory();
    log_posix_context(context);
    _backtrace_posix(16);

    RendererResetScreen(true);
    LbErrorLogClose();

    signal(sig_id, SIG_DFL);
    raise(sig_id);
}

static void install_posix_handler(int sig_id)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = ctrl_handler_posix;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK; /* on its own stack: a stack overflow is reported too */
    {
        int rc = sigaction(sig_id, &sa, NULL);
        if (rc != 0)
        {
            fprintf(stderr,
                "LbErrorParachuteInstall: sigaction failed for signal %d (%s); "
                "crash handler not installed for this signal.\n",
                sig_id, sigstr(sig_id));
#ifndef NDEBUG
            assert(rc == 0);
#endif
        }
    }
}
#endif

void LbErrorParachuteInstall(void)
{
#if defined(BF_POSIX_CRASH)
    /* the handler's own stack (2026-11-23): after a stack overflow the faulting thread has none left, and the handler
       died with the game, reporting nothing (the main thread's; other threads' faults run on their own stacks) */
    {
        static char crash_stack[128 * 1024];
        stack_t ss;
        memset(&ss, 0, sizeof(ss));
        ss.ss_sp = crash_stack;
        ss.ss_size = sizeof(crash_stack);
        if (sigaltstack(&ss, NULL) != 0)
            fprintf(stderr, "LbErrorParachuteInstall: sigaltstack failed; a stack overflow will not be reported.\n");
    }
    install_posix_handler(SIGINT);
    install_posix_handler(SIGILL);
    install_posix_handler(SIGABRT);
    install_posix_handler(SIGFPE);
    install_posix_handler(SIGSEGV);
    install_posix_handler(SIGTERM);
    install_posix_handler(SIGHUP);
    install_posix_handler(SIGQUIT);
#ifdef SIGSYS
    install_posix_handler(SIGSYS);
#endif
#ifdef SIGBUS
    install_posix_handler(SIGBUS);
#endif
#ifdef SIGTRAP
    install_posix_handler(SIGTRAP);
#endif
#else
    signal(SIGINT,ctrl_handler);
    signal(SIGILL,ctrl_handler);
    signal(SIGABRT,ctrl_handler);
    signal(SIGFPE,ctrl_handler);
    signal(SIGSEGV,ctrl_handler);
    signal(SIGTERM,ctrl_handler);
#if defined(BF_POSIX_CRASH)
    signal(SIGHUP,ctrl_handler);
    signal(SIGQUIT,ctrl_handler);
#ifdef SIGSYS
    signal(SIGSYS,ctrl_handler);
#endif
#else
    signal(SIGBREAK,ctrl_handler);
#endif
#endif
    atexit(exit_handler);
#if defined(_WIN32)
    SetUnhandledExceptionFilter(ctrl_handler_w32);
    AddVectoredExceptionHandler(0, crash_first_chance);
    {
        /* stack kept back for a stack overflow's own handling on this (the game's) thread: without it a 32-bit build
           was ended by Windows before any handler ran, with no report (2026-11-21) */
        ULONG guarantee = 64 * 1024;
        SetThreadStackGuarantee(&guarantee);
    }
#endif
}

void LbErrorParachuteUpdate(void)
{
#if defined(_WIN32)
    SetUnhandledExceptionFilter(ctrl_handler_w32);
#endif
}
/******************************************************************************/
