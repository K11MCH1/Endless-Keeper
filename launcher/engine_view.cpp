// Endless Keeper launcher: the background - the game itself, running hidden and handing its frames over
// (src/world_attract.c: "-attract <shared memory>").
#include "engine_view.h"
#include "net.h"
#include "../src/world_attract.h"
#include <SDL3/SDL.h>
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

const int VIEW_W = 1280, VIEW_H = 720;

SDL_Renderer *g_ren = nullptr;
SDL_Texture *g_tex = nullptr;
int g_tex_w = 0, g_tex_h = 0;
HANDLE g_map = nullptr, g_process = nullptr;
AttractShared *g_shared = nullptr;
const unsigned char *g_pictures = nullptr;
uint32_t g_seen = 0;
std::wstring g_exe, g_dir;
bool g_wanted = false, g_have_picture = false;
Uint64 g_started_at = 0, g_retry_at = 0;
int g_quick_ends = 0;
float g_shown = 0; /* 0 .. 1: the picture comes in softly once the first frame is here */
/* the last picture of the launcher's last run: shown at once, until the hidden game has loaded and sends its own */
SDL_Texture *g_still = nullptr;
int g_still_w = 0, g_still_h = 0;
float g_live = 0; /* 0 .. 1: how far the live picture has taken over from the still one */
Uint64 g_still_saved_at = 0;
bool g_first_noted = false;
float g_focus_x = -1, g_focus_y = -1, g_at_x = -1, g_at_y = -1; /* asked for; where it is (it follows softly) */

void stop_process()
{
    if (g_process != nullptr)
    {
        TerminateProcess(g_process, 0);
        WaitForSingleObject(g_process, 2000);
        CloseHandle(g_process);
        g_process = nullptr;
    }
}

/** A line in endlesskeeper-launcher.log (beside the game): what the background's game did. */
void note(const char *format, ...)
{
    FILE *f = _wfopen((g_dir + L"/endlesskeeper-launcher.log").c_str(), L"a");
    if (f == nullptr)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fputc(10, f);
    fclose(f);
}

std::wstring still_file() { return g_dir + L"/endlesskeeper-launcher.bg"; }

void load_still()
{
    FILE *f = _wfopen(still_file().c_str(), L"rb");
    if (f == nullptr)
        return;
    uint32_t head[2] = {0, 0};
    if ((fread(head, 4, 2, f) == 2) && (head[0] >= 16) && (head[1] >= 16) && (head[0] <= 4096) && (head[1] <= 4096))
    {
        std::vector<unsigned char> px((size_t)head[0] * head[1] * 4);
        if (fread(px.data(), 1, px.size(), f) == px.size())
        {
            g_still = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, (int)head[0], (int)head[1]);
            if (g_still != nullptr)
            {
                SDL_UpdateTexture(g_still, nullptr, px.data(), (int)head[0] * 4);
                SDL_SetTextureScaleMode(g_still, SDL_SCALEMODE_LINEAR);
                SDL_SetTextureBlendMode(g_still, SDL_BLENDMODE_BLEND);
                g_still_w = (int)head[0];
                g_still_h = (int)head[1];
            }
        }
    }
    fclose(f);
}

/** Keeps the picture on screen (at half its size) for the launcher's next start. */
void save_still()
{
    if ((g_shared == nullptr) || !g_have_picture || (g_shared->fade > 40) || (g_seen == 0))
        return;
    const uint32_t slot = (g_seen - 1) & 1;
    const int w = (int)g_shared->width[slot], h = (int)g_shared->height[slot];
    if ((w < 32) || (h < 32) || ((size_t)w * h * 4 > g_shared->capacity))
        return;
    const unsigned char *src = g_pictures + (size_t)slot * g_shared->capacity;
    const uint32_t head[2] = {(uint32_t)(w / 2), (uint32_t)(h / 2)};
    std::vector<unsigned char> px((size_t)head[0] * head[1] * 4);
    for (uint32_t y = 0; y < head[1]; y++)
        for (uint32_t x = 0; x < head[0]; x++)
        {
            const unsigned char *a = src + ((size_t)(y * 2) * w + x * 2) * 4;
            unsigned char *o = &px[((size_t)y * head[0] + x) * 4];
            o[0] = a[0];
            o[1] = a[1];
            o[2] = a[2];
            o[3] = 255;
        }
    FILE *f = _wfopen(still_file().c_str(), L"wb");
    if (f == nullptr)
        return;
    fwrite(head, 4, 2, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
    g_still_saved_at = SDL_GetTicks();
}

bool start_process()
{
    if ((g_shared == nullptr) || g_exe.empty())
        return false;
    g_shared->frame = 0;
    g_shared->fade = 255;
    g_shared->caption[0] = '\0';
    g_seen = 0;
    g_have_picture = false;
    g_first_noted = false;
    char name[64];
    snprintf(name, sizeof(name), "EndlessKeeperAttract_%lu", (unsigned long)GetCurrentProcessId());
    wchar_t args[1024];
    swprintf(args, 1024, L"\"%ls\" -attract %hs -level 1 -campaign levels/endless -seed %u -nointro -nosound -altinput -log attract.log"
        L" -cfg INGAME_RES=%dx%dw32 -cfg FRAMES_PER_SECOND=30 -cfg AUTOMATIC_REPLAYS=FALSE -cfg FREEZE_GAME_ON_FOCUS_LOST=FALSE"
        L" -cfg CURSOR_EDGE_CAMERA_PANNING=FALSE -cfg API_ENABLED=FALSE",
        g_exe.c_str(), name, (unsigned)(SDL_GetPerformanceCounter() % 99999989u) + 1, VIEW_W, VIEW_H);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(g_exe.c_str(), args, nullptr, nullptr, FALSE, BELOW_NORMAL_PRIORITY_CLASS, nullptr, g_dir.c_str(), &si, &pi))
    {
        note("Background: the game could not be started (error %lu): %s", (unsigned long)GetLastError(), narrow(args).c_str());
        return false;
    }
    note("Background: started %s", narrow(args).c_str());
    CloseHandle(pi.hThread);
    g_process = pi.hProcess;
    g_started_at = SDL_GetTicks();
    return true;
}

} // namespace

bool engine_view_init(SDL_Renderer *renderer, const std::wstring &game_exe, const std::wstring &game_dir)
{
    g_ren = renderer;
    g_exe = game_exe;
    g_dir = game_dir;
    if (GetFileAttributesW(game_exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    const DWORD capacity = (DWORD)VIEW_W * VIEW_H * 4, size = (DWORD)sizeof(AttractShared) + capacity * 2;
    char name[64];
    snprintf(name, sizeof(name), "EndlessKeeperAttract_%lu", (unsigned long)GetCurrentProcessId());
    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, name);
    if (g_map == nullptr)
        return false;
    g_shared = (AttractShared *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (g_shared == nullptr)
        return false;
    memset(g_shared, 0, sizeof(*g_shared));
    g_shared->magic = ATTRACT_MAGIC;
    g_shared->capacity = capacity;
    g_shared->launcher_pid = GetCurrentProcessId();
    g_pictures = (const unsigned char *)(g_shared + 1);
    load_still();
    g_wanted = true;
    return start_process();
}

void engine_view_pause()
{
    save_still();
    g_wanted = false;
    stop_process();
    g_have_picture = false;
    g_shown = 0;
}

void engine_view_resume()
{
    if ((g_shared == nullptr) || g_wanted)
        return;
    g_wanted = true;
    g_quick_ends = 0;
    start_process();
}

void engine_view_shutdown()
{
    save_still();
    g_wanted = false;
    stop_process();
    if (g_tex != nullptr)
        SDL_DestroyTexture(g_tex);
    g_tex = nullptr;
    if (g_still != nullptr)
        SDL_DestroyTexture(g_still);
    g_still = nullptr;
    if (g_shared != nullptr)
        UnmapViewOfFile(g_shared);
    g_shared = nullptr;
    if (g_map != nullptr)
        CloseHandle(g_map);
    g_map = nullptr;
}

void engine_view_update(float dt)
{
    if (g_shared == nullptr)
        return;
    if (g_wanted && (g_process != nullptr) && (WaitForSingleObject(g_process, 0) == WAIT_OBJECT_0))
    {
        /* the game ended (its keeper fell, or it could not run): another, unless it keeps ending at once */
        DWORD code = 0;
        GetExitCodeProcess(g_process, &code);
        note("Background: the game ended after %d s (exit code %lu)", (int)((SDL_GetTicks() - g_started_at) / 1000), (unsigned long)code);
        CloseHandle(g_process);
        g_process = nullptr;
        g_quick_ends = (SDL_GetTicks() - g_started_at < 20000) ? g_quick_ends + 1 : 0;
        g_retry_at = SDL_GetTicks() + 2000;
        g_have_picture = false;
    }
    if (g_wanted && (g_process == nullptr) && (g_quick_ends < 3) && (SDL_GetTicks() >= g_retry_at))
        start_process();
    const uint32_t frame = g_shared->frame;
    if ((frame != g_seen) && (frame > 0))
    {
        g_seen = frame;
        const uint32_t slot = (frame - 1) & 1;
        const int w = (int)g_shared->width[slot], h = (int)g_shared->height[slot];
        if ((w > 0) && (h > 0) && ((size_t)w * h * 4 <= g_shared->capacity))
        {
            if ((g_tex == nullptr) || (w != g_tex_w) || (h != g_tex_h))
            {
                if (g_tex != nullptr)
                    SDL_DestroyTexture(g_tex);
                g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
                if (g_tex != nullptr)
                {
                    SDL_SetTextureScaleMode(g_tex, SDL_SCALEMODE_LINEAR);
                    SDL_SetTextureBlendMode(g_tex, SDL_BLENDMODE_NONE);
                }
                g_tex_w = w;
                g_tex_h = h;
            }
            if (g_tex != nullptr)
            {
                SDL_UpdateTexture(g_tex, nullptr, g_pictures + (size_t)slot * g_shared->capacity, w * 4);
                g_have_picture = true;
                if (!g_first_noted)
                {
                    g_first_noted = true;
                    note("Background: the first picture came %d ms after the game was started", (int)(SDL_GetTicks() - g_started_at));
                }
            }
        }
    }
    const float target = (g_have_picture && (g_shared->fade < 255)) ? 1.0f : 0.0f;
    g_shown += (target - g_shown) * std::min(1.0f, dt * 2.0f);
    /* the live picture takes over from the still one in a second, once the hidden game shows its first scene */
    g_live = std::max(0.0f, std::min(1.0f, g_live + (((target > 0) && (g_shared->fade < 60)) ? dt : -dt * 3.0f)));
    if (g_have_picture && (SDL_GetTicks() - g_still_saved_at > 30000))
        save_still();
}

bool engine_view_draw(ImDrawList *dl, float width, float height)
{
    const bool live = (g_tex != nullptr) && g_have_picture && (g_shown >= 0.004f);
    const bool still = (g_still != nullptr) && (g_live < 0.996f);
    if (!live && !still)
        return false;
    const int pic_w = live ? g_tex_w : g_still_w * 2, pic_h = live ? g_tex_h : g_still_h * 2;
    /* the picture covers the window, its middle where the launcher's boxes leave room: larger as needed, cut at the
       edges, never stretched */
    const float want_x = (g_focus_x >= 0) ? g_focus_x : width * 0.5f, want_y = (g_focus_y >= 0) ? g_focus_y : height * 0.5f;
    if (g_at_x < 0)
    {
        g_at_x = want_x;
        g_at_y = want_y;
    }
    g_at_x += (want_x - g_at_x) * 0.08f;
    g_at_y += (want_y - g_at_y) * 0.08f;
    const float half_w = std::max(g_at_x, width - g_at_x), half_h = std::max(g_at_y, height - g_at_y);
    const float scale = std::max(2.0f * half_w / (float)pic_w, 2.0f * half_h / (float)pic_h);
    const float w = pic_w * scale, h = pic_h * scale, x = g_at_x - w * 0.5f, y = g_at_y - h * 0.5f;
    const float fade = 1.0f - std::min(255u, (unsigned)g_shared->fade) / 255.0f;
    const int a = (int)(255.0f * g_shown * fade);
    if (live)
        dl->AddImage((ImTextureID)(intptr_t)g_tex, ImVec2(x, y), ImVec2(x + w, y + h), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(a, a, a, 255));
    if (still)
        dl->AddImage((ImTextureID)(intptr_t)g_still, ImVec2(x, y), ImVec2(x + w, y + h), ImVec2(0, 0), ImVec2(1, 1),
            IM_COL32(255, 255, 255, (int)(255.0f * (1.0f - g_live))));
    return true;
}

void engine_view_focus(float x, float y)
{
    g_focus_x = x;
    g_focus_y = y;
}

bool engine_view_ready()
{
    return (g_shared != nullptr) && g_have_picture && (g_shared->fade < 80);
}

const char *engine_view_caption()
{
    return ((g_shared != nullptr) && g_have_picture) ? g_shared->caption : "";
}
