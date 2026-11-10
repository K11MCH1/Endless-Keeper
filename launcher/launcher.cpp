// Endless Keeper launcher: the program players start. A themed window over the game itself playing battles (it runs
// hidden behind the launcher: engine_view.cpp), with Play (the 64-bit game where it can run), Settings (keeperfx.cfg),
// the list of releases from GitHub and one-click updating.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "engine_view.h"
#include "net.h"
#include "../src/ver_defs.h"

#ifndef ENDLESS_RELEASE
#define ENDLESS_RELEASE "dev"
#endif
#ifndef ENDLESS_STAGE
#define ENDLESS_STAGE "Alpha"
#endif
#define STR2(x) #x
#define STR(x) STR2(x)
#define GAME_VERSION STR(VER_MAJOR) "." STR(VER_MINOR) "." STR(VER_RELEASE) "-" ENDLESS_RELEASE
#define GAME_TITLE "Endless Keeper " GAME_VERSION " " ENDLESS_STAGE
#define REPO "K11MCH1/Endless-Keeper"
#define REPO_URL "https://github.com/" REPO

/******************************************************************************/
/* KEY=VALUE files that keep their comments and order (keeperfx.cfg, the launcher's own) */

struct CfgFile {
    std::wstring path;
    std::vector<std::string> lines;
    bool dirty = false;

    static std::string trim(std::string s)
    {
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            return std::string();
        return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
    }
    int find(const char *key) const
    {
        const size_t n = strlen(key);
        for (size_t i = 0; i < lines.size(); i++)
        {
            const std::string l = trim(lines[i]);
            if ((l.size() > n) && (_strnicmp(l.c_str(), key, n) == 0))
            {
                const size_t p = l.find_first_not_of(" \t", n);
                if ((p != std::string::npos) && (l[p] == '='))
                    return (int)i;
            }
        }
        return -1;
    }
    void load(const std::wstring &file)
    {
        path = file;
        lines.clear();
        dirty = false;
        std::ifstream f(file.c_str());
        std::string l;
        while (std::getline(f, l))
        {
            if (!l.empty() && (l.back() == '\r'))
                l.pop_back();
            lines.push_back(l);
        }
    }
    bool save()
    {
        std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        for (const std::string &l : lines)
            f << l << "\r\n";
        dirty = false;
        return true;
    }
    std::string get(const char *key, const char *def = "") const
    {
        const int i = find(key);
        if (i < 0)
            return def;
        return trim(lines[i].substr(lines[i].find('=') + 1));
    }
    void set(const char *key, const std::string &value)
    {
        const int i = find(key);
        const std::string line = std::string(key) + "=" + value;
        if (i < 0)
            lines.push_back(line);
        else if (lines[i] == line)
            return;
        else
            lines[i] = line;
        dirty = true;
    }
    bool flag(const char *key, bool def) const
    {
        const std::string v = get(key, def ? "TRUE" : "FALSE");
        for (const char *yes : {"TRUE", "ON", "YES", "ENABLED", "ALWAYS", "1"})
            if (_stricmp(v.c_str(), yes) == 0)
                return true;
        return false;
    }
};

static std::vector<std::string> split(const std::string &s)
{
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string t;
    while (is >> t)
        out.push_back(t);
    return out;
}

static std::string join(const std::vector<std::string> &v)
{
    std::string s;
    for (const std::string &t : v)
        s += (s.empty() ? "" : " ") + t;
    return s;
}

/******************************************************************************/
/* languages: KeeperFX's launcher's translations (the .po files in launcher/lang, GPL v2, by the KeeperFX launcher's translators) for
   the texts the two launchers share, and launcher/lang/own.txt for Endless Keeper's own. Both are inside the program. */

struct Language { const char *code, *name; int resource; };
static const Language g_languages[] = {{"en", "English", 0}, {"cs", "\xC4\x8C" "e\xC5\xA1tina", 201}, {"de", "Deutsch", 202},
    {"es", "Espa\xC3\xB1ol", 203}, {"fr", "Fran\xC3\xA7" "ais", 204}, {"it", "Italiano", 205},
    {"ja", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 206}, {"ko", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4", 207}, {"nl", "Nederlands", 208},
    {"pl", "Polski", 209}, {"pt", "Portugu\xC3\xAAs", 210}, {"ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9", 211},
    {"uk", "\xD0\xA3\xD0\xBA\xD1\x80\xD0\xB0\xD1\x97\xD0\xBD\xD1\x81\xD1\x8C\xD0\xBA\xD0\xB0", 212},
    {"zh-hans", "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87", 213}};
#define OWN_TEXTS_RESOURCE 300
static std::map<std::string, std::string> g_texts;

static std::string resource_text(int id)
{
    const HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (res == nullptr)
        return std::string();
    const HGLOBAL mem = LoadResource(nullptr, res);
    return std::string((const char *)LockResource(mem), SizeofResource(nullptr, res));
}

/** The text of a .po line's "..." (escapes undone), added to out. */
static void po_quoted(const std::string &line, std::string &out)
{
    const size_t a = line.find('"'), b = line.rfind('"');
    if ((a == std::string::npos) || (b <= a))
        return;
    for (size_t i = a + 1; i < b; i++)
    {
        if ((line[i] == '\\') && (i + 1 < b))
        {
            const char c = line[++i];
            out += (c == 'n') ? '\n' : (c == 't') ? '\t' : c;
        } else
            out += line[i];
    }
}

static void load_language(const std::string &code)
{
    g_texts.clear();
    for (const Language &l : g_languages)
    {
        if ((code != l.code) || (l.resource == 0))
            continue;
        /* KeeperFX's launcher's: msgid "..." / msgstr "...", each maybe over several lines */
        std::istringstream is(resource_text(l.resource));
        std::string line, id, str;
        int part = 0;
        auto keep = [&]() {
            if (!id.empty() && !str.empty())
                g_texts[id] = str;
            id.clear();
            str.clear();
        };
        while (std::getline(is, line))
        {
            if (line.rfind("msgid ", 0) == 0)
            {
                keep();
                part = 1;
                po_quoted(line, id);
            } else if (line.rfind("msgstr ", 0) == 0)
            {
                part = 2;
                po_quoted(line, str);
            } else if (!line.empty() && (line[0] == '"'))
                po_quoted(line, (part == 1) ? id : str);
            else if (line.rfind("msgctxt", 0) == 0)
                part = 0;
        }
        keep();
    }
    /* Endless Keeper's own: "[code]" then "English=Translation" lines */
    std::istringstream is(resource_text(OWN_TEXTS_RESOURCE));
    std::string line;
    bool mine = false;
    while (std::getline(is, line))
    {
        if (!line.empty() && (line.back() == '\r'))
            line.pop_back();
        if (!line.empty() && (line[0] == '['))
            mine = (line == "[" + code + "]");
        else if (mine)
        {
            const size_t eq = line.find('=');
            if ((eq != std::string::npos) && (eq > 0) && (eq + 1 < line.size()))
                g_texts[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
}

/** The text in the launcher's language (the English itself when there is none). */
static const char *T(const char *english)
{
    const auto it = g_texts.find(english);
    return (it != g_texts.end()) ? it->second.c_str() : english;
}

/** A window's or tab's title in the launcher's language, its id the English: it stays the same window. */
static std::string titled(const char *english)
{
    return std::string(T(english)) + "###" + english;
}

/** The language Windows is shown in, when the launcher has it. */
static const char *system_language()
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = L"";
    GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH);
    const std::string n = narrow(name);
    if (n.rfind("zh", 0) == 0)
        return ((n.find("Hant") != std::string::npos) || (n.find("TW") != std::string::npos) || (n.find("HK") != std::string::npos)) ? "en" : "zh-hans";
    for (const Language &l : g_languages)
        if (n.rfind(l.code, 0) == 0)
            return l.code;
    return "en";
}

/******************************************************************************/
/* state */

static SDL_Window *g_window = nullptr;
static SDL_Renderer *g_renderer = nullptr;
static std::wstring g_game_dir, g_self_exe;
static CfgFile g_cfg, g_lcfg;       /* keeperfx.cfg and endlesskeeper-launcher.cfg */
static SDL_Texture *g_header = nullptr;
static int g_header_w = 0, g_header_h = 0;
static float S = 1.0f;              /* the interface's scale */
static bool g_battle = false;
static bool opt_no_update = false, opt_no_battle = false, opt_play = false, opt_32bit = false;

enum UpdateState { UP_CHECKING, UP_FAILED, UP_LISTED, UP_INSTALLING, UP_INSTALL_FAILED, UP_RESTART };
static std::mutex g_mx;             /* guards the fields below, shared with the worker threads */
static UpdateState g_up = UP_CHECKING;
static std::vector<Release> g_releases;
static std::string g_up_why, g_up_stage;
static float g_up_fraction = 0;
static std::atomic<bool> g_cancel(false);
static bool g_prompt_update = false, g_prompted = false;
static bool g_show_settings = false, g_show_options = false;
static std::string g_message;       /* a box with one line and OK */
static HANDLE g_game_process = nullptr;
/* test hook EKL_TEST="shot=<file.bmp>;after=<seconds>;settings=<tab>;size=<w>x<h>": the window as drawn, saved after that
   long (then the launcher closes), with Settings open on that tab */
static std::string t_shot, t_tab;
static bool t_bare = false; /* "bare=1": the battle alone */
static float t_play = 0;    /* "play=<seconds>": Play clicks itself then */
static int t_burst = 0;     /* "burst=<n>": that many more screenshots, a quarter of a second apart (<file>_1.bmp ...) */
static float t_after = 0;
static int t_w = 0, t_h = 0;
static ImGuiTabItemFlags tab_flags(const char *name)
{
    return (!t_tab.empty() && (t_tab == name)) ? ImGuiTabItemFlags_SetSelected : 0;
}

static const ImU32 C_GOLD = IM_COL32(222, 178, 88, 255), C_GOLD_HI = IM_COL32(255, 224, 140, 255), C_GOLD_DIM = IM_COL32(128, 98, 46, 255);
static const ImU32 C_TEXT = IM_COL32(226, 212, 186, 255), C_TEXT_DIM = IM_COL32(160, 146, 122, 255);
static const ImU32 C_PANEL = IM_COL32(18, 12, 9, 200), C_EDGE = IM_COL32(104, 78, 36, 255), C_EDGE_IN = IM_COL32(44, 30, 14, 255);

static void open_url(const char *url)
{
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

/******************************************************************************/
/* the look */

static void text_shadow(ImDrawList *dl, float size, ImVec2 pos, ImU32 col, const char *text)
{
    ImFont *font = ImGui::GetFont();
    dl->AddText(font, size, ImVec2(pos.x + std::max(1.0f, size * 0.06f), pos.y + std::max(1.0f, size * 0.08f)), IM_COL32(0, 0, 0, 220), text);
    dl->AddText(font, size, pos, col, text);
}

/** A slab of dark stone with a bronze rim and studs in its corners. */
static void panel(ImDrawList *dl, ImVec2 a, ImVec2 b)
{
    const float r = 5.0f * S;
    dl->AddRectFilled(ImVec2(a.x + 3 * S, a.y + 4 * S), ImVec2(b.x + 3 * S, b.y + 4 * S), IM_COL32(0, 0, 0, 90), r);
    dl->AddRectFilled(a, b, C_PANEL, r);
    dl->AddRectFilledMultiColor(ImVec2(a.x + 2, a.y + 2), ImVec2(b.x - 2, a.y + 46 * S), IM_COL32(70, 44, 20, 70), IM_COL32(70, 44, 20, 70),
        IM_COL32(70, 44, 20, 0), IM_COL32(70, 44, 20, 0));
    dl->AddRect(a, b, C_EDGE, r, 0, std::max(1.5f, 2.0f * S));
    dl->AddRect(ImVec2(a.x + 4 * S, a.y + 4 * S), ImVec2(b.x - 4 * S, b.y - 4 * S), C_EDGE_IN, r * 0.6f, 0, 1.0f);
    for (int i = 0; i < 4; i++)
    {
        const ImVec2 c((i & 1) ? b.x - 9 * S : a.x + 9 * S, (i & 2) ? b.y - 9 * S : a.y + 9 * S);
        const float d = 3.2f * S;
        dl->AddQuadFilled(ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y), C_GOLD_DIM);
    }
}

/** A button of the game's kind: carved stone, or (primary) the blood red of the Play button. */
static bool button(const char *id, const char *label, ImVec2 pos, ImVec2 size, bool primary, float font_size, bool enabled = true)
{
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, size);
    ImGui::EndDisabled();
    const bool hot = enabled && ImGui::IsItemHovered(), down = enabled && ImGui::IsItemActive();
    if (hot)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 a = pos, b(pos.x + size.x, pos.y + size.y);
    const float r = 6.0f * S, t = (float)ImGui::GetTime();
    if (primary && enabled)
    {
        const float glow = hot ? 1.0f : (0.45f + 0.25f * sinf(t * 2.2f));
        for (int g = 3; g >= 1; g--)
            dl->AddRect(ImVec2(a.x - g * 2.5f * S, a.y - g * 2.5f * S), ImVec2(b.x + g * 2.5f * S, b.y + g * 2.5f * S),
                IM_COL32(255, 90, 30, (int)(26 * glow)), r + g * 2.5f * S, 0, 3.0f * S);
    }
    dl->AddRectFilled(ImVec2(a.x + 2 * S, a.y + 3 * S), ImVec2(b.x + 2 * S, b.y + 3 * S), IM_COL32(0, 0, 0, 120), r);
    const ImU32 top = !enabled ? IM_COL32(52, 46, 40, 235) : (primary ? (down ? IM_COL32(120, 16, 10, 255) : (hot ? IM_COL32(214, 52, 26, 255) : IM_COL32(176, 34, 18, 255)))
        : (down ? IM_COL32(34, 24, 16, 245) : (hot ? IM_COL32(86, 62, 34, 245) : IM_COL32(58, 42, 26, 240))));
    const ImU32 bottom = !enabled ? IM_COL32(30, 26, 22, 235) : (primary ? (down ? IM_COL32(70, 8, 6, 255) : IM_COL32(96, 12, 8, 255))
        : (down ? IM_COL32(20, 14, 10, 245) : IM_COL32(28, 20, 13, 240)));
    dl->AddRectFilled(a, b, bottom, r);
    dl->AddRectFilledMultiColor(ImVec2(a.x + r * 0.5f, a.y + 2), ImVec2(b.x - r * 0.5f, a.y + size.y * 0.62f), top, top, bottom, bottom);
    dl->AddRect(a, b, hot ? C_GOLD_HI : (enabled ? C_GOLD : C_GOLD_DIM), r, 0, std::max(1.5f, 2.0f * S));
    dl->AddRect(ImVec2(a.x + 3.5f * S, a.y + 3.5f * S), ImVec2(b.x - 3.5f * S, b.y - 3.5f * S), IM_COL32(0, 0, 0, 110), r * 0.6f, 0, 1.0f);
    const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(font_size, 4000.0f, 0.0f, label);
    text_shadow(dl, font_size, ImVec2(a.x + (size.x - ts.x) * 0.5f, a.y + (size.y - ts.y) * 0.5f + (down ? 1.5f * S : 0)),
        !enabled ? C_TEXT_DIM : (hot ? IM_COL32(255, 244, 214, 255) : (primary ? IM_COL32(255, 226, 170, 255) : C_GOLD)), label);
    return clicked;
}

/** Text that opens an address: underlined under the pointer. */
static bool link(const char *id, const char *text, float size, ImU32 col)
{
    ImFont *font = ImGui::GetFont();
    const ImVec2 pos = ImGui::GetCursorScreenPos(), ts = font->CalcTextSizeA(size, 4000.0f, 0.0f, text);
    const bool clicked = ImGui::InvisibleButton(id, ts);
    const bool hot = ImGui::IsItemHovered();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    text_shadow(dl, size, pos, hot ? C_GOLD_HI : col, text);
    if (hot)
    {
        dl->AddLine(ImVec2(pos.x, pos.y + ts.y), ImVec2(pos.x + ts.x, pos.y + ts.y), C_GOLD_HI, std::max(1.0f, S));
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    return clicked;
}

static void apply_style()
{
    ImGuiStyle &st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowRounding = 6.0f;
    st.ChildRounding = 5.0f;
    st.FrameRounding = 4.0f;
    st.PopupRounding = 5.0f;
    st.GrabRounding = 4.0f;
    st.TabRounding = 4.0f;
    st.ScrollbarRounding = 6.0f;
    st.WindowBorderSize = 2.0f;
    st.FrameBorderSize = 1.0f;
    st.WindowPadding = ImVec2(16, 14);
    st.FramePadding = ImVec2(9, 5);
    st.ItemSpacing = ImVec2(10, 8);
    st.ScrollbarSize = 13.0f;
    st.ScaleAllSizes(S);
    ImVec4 *c = st.Colors;
    auto rgb = [](int r, int g, int b, int a = 255) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); };
    c[ImGuiCol_Text] = rgb(226, 212, 186);
    c[ImGuiCol_TextDisabled] = rgb(128, 116, 98);
    c[ImGuiCol_WindowBg] = rgb(20, 14, 10, 248);
    c[ImGuiCol_ChildBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgb(24, 17, 12, 250);
    c[ImGuiCol_Border] = rgb(104, 78, 36);
    c[ImGuiCol_FrameBg] = rgb(44, 32, 20);
    c[ImGuiCol_FrameBgHovered] = rgb(70, 50, 28);
    c[ImGuiCol_FrameBgActive] = rgb(92, 64, 32);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = rgb(46, 30, 16);
    c[ImGuiCol_CheckMark] = rgb(255, 214, 120);
    c[ImGuiCol_SliderGrab] = rgb(196, 150, 66);
    c[ImGuiCol_SliderGrabActive] = rgb(255, 214, 120);
    c[ImGuiCol_Button] = rgb(64, 46, 26);
    c[ImGuiCol_ButtonHovered] = rgb(104, 74, 36);
    c[ImGuiCol_ButtonActive] = rgb(140, 30, 18);
    c[ImGuiCol_Header] = rgb(96, 28, 16);
    c[ImGuiCol_HeaderHovered] = rgb(132, 40, 22);
    c[ImGuiCol_HeaderActive] = rgb(160, 46, 24);
    c[ImGuiCol_Separator] = rgb(84, 62, 30);
    c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 70);
    c[ImGuiCol_ScrollbarGrab] = rgb(110, 82, 38);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(170, 130, 60);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(222, 178, 88);
    c[ImGuiCol_Tab] = rgb(40, 28, 17);
    c[ImGuiCol_TabHovered] = rgb(132, 40, 22);
    c[ImGuiCol_TabSelected] = rgb(110, 30, 16);
    c[ImGuiCol_TabSelectedOverline] = rgb(222, 178, 88);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 150);
}

/******************************************************************************/
/* starting the game */

static bool windows_is_64bit()
{
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    return (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) || (si.wProcessorArchitecture == 12 /* ARM64 */);
}

static std::wstring game_exe()
{
    const std::wstring x64 = g_game_dir + L"\\x64\\keeperfx.exe";
    if (!opt_32bit && windows_is_64bit() && (GetFileAttributesW(x64.c_str()) != INVALID_FILE_ATTRIBUTES))
        return x64;
    return g_game_dir + L"\\keeperfx.exe";
}

static std::string game_arguments()
{
    std::string args;
    if (g_lcfg.flag("GAME_PARAM_NO_INTRO", true)) args += " -nointro";
    if (g_lcfg.flag("GAME_PARAM_ALEX", false)) args += " -alex";
    if (g_lcfg.flag("GAME_PARAM_NO_SOUND", false)) args += " -nosound";
    if (g_lcfg.flag("GAME_PARAM_USE_CD_MUSIC", false)) args += " -cd";
    if (g_lcfg.flag("GAME_PARAM_VID_SMOOTH", false)) args += " -vidsmooth";
    if (g_lcfg.flag("GAME_PARAM_ALT_INPUT", false)) args += " -altinput";
    const std::string extra = g_lcfg.get("EXTRA_GAME_LAUNCH_OPTIONS");
    if (!extra.empty())
        args += " " + extra;
    return args;
}

static void play()
{
    if (g_game_process != nullptr)
        return;
    const std::wstring exe = game_exe();
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        g_message = "The game isn't in this folder: keeperfx.exe is missing beside the launcher.";
        return;
    }
    engine_view_pause(); /* the background's game makes way for the real one */
    std::wstring cmd = L"\"" + exe + L"\"" + widen(game_arguments());
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_game_dir.c_str(), &si, &pi))
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "The game could not be started (error %lu).", (unsigned long)GetLastError());
        g_message = buf;
        engine_view_resume();
        return;
    }
    CloseHandle(pi.hThread);
    g_game_process = pi.hProcess;
    engine_view_note(("Play: started " + narrow(cmd)).c_str());
    SDL_HideWindow(g_window); /* back when the game has closed */
}

/******************************************************************************/
/* updates */

static std::string t_version; /* test hook "version=1.4.0-6.1": the version the launcher takes itself for */
static bool t_update = false;   /* test hook "update=1": the update box's Update clicks itself */

static std::vector<int> installed_version()
{
    return version_numbers(t_version.empty() ? GAME_VERSION : t_version);
}

static void check_updates()
{
    {
        std::lock_guard<std::mutex> lock(g_mx);
        g_up = UP_CHECKING;
    }
    std::thread([] {
        std::vector<Release> list;
        std::string why;
        const bool ok = fetch_releases(REPO, list, why);
        std::lock_guard<std::mutex> lock(g_mx);
        if (ok)
        {
            g_releases = list;
            g_up = UP_LISTED;
        } else
        {
            g_up_why = why;
            g_up = UP_FAILED;
        }
    }).detach();
}

static void start_update(const Release &rel)
{
    g_cancel = false;
    engine_view_pause(); /* the background's game would keep the files an update replaces */
    {
        std::lock_guard<std::mutex> lock(g_mx);
        g_up = UP_INSTALLING;
        g_up_stage = "Downloading";
        g_up_fraction = 0;
    }
    std::thread([rel] {
        std::string why;
        const bool ok = install_release(rel, g_game_dir, g_self_exe, [](const char *stage, float fraction) {
            std::lock_guard<std::mutex> lock(g_mx);
            g_up_stage = stage;
            g_up_fraction = fraction;
        }, g_cancel, why);
        std::lock_guard<std::mutex> lock(g_mx);
        g_up_why = why;
        g_up = ok ? UP_RESTART : UP_INSTALL_FAILED;
    }).detach();
}

/** A release's notes without what the launcher's font can't show (emoji) or doesn't need (markdown marks). */
static std::string plain(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = (unsigned char)s[i];
        const int n = (c < 0x80) ? 1 : ((c >> 5) == 6) ? 2 : ((c >> 4) == 14) ? 3 : 4;
        if (i + n > s.size())
            break;
        unsigned cp = (n == 1) ? c : (c & (0xFF >> (n + 1)));
        for (int k = 1; k < n; k++)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        const bool drop = (cp >= 0x1F000) || ((cp >= 0x2600) && (cp <= 0x27BF)) || (cp == 0xFE0F) || (cp == 0x200D) || (cp == '`');
        if (!drop && !((cp == '*') && (i + 1 < s.size()) && (s[i + 1] == '*')) && !((cp == '*') && (i > 0) && (s[i - 1] == '*')))
            out.append(s, i, n);
        i += n;
    }
    return CfgFile::trim(out);
}

static void release_notes(const std::string &body)
{
    std::istringstream is(body);
    std::string line, item;
    bool bullet = false;
    auto flush = [&]() {
        if (item.empty())
            return;
        if (bullet)
        {
            ImGui::Bullet();
            ImGui::SameLine();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, C_TEXT);
        ImGui::TextWrapped("%s", item.c_str());
        ImGui::PopStyleColor();
        item.clear();
    };
    while (std::getline(is, line))
    {
        if (!line.empty() && (line.back() == '\r'))
            line.pop_back();
        const std::string t = plain(line);
        if (t.empty())
        {
            flush();
            continue;
        }
        if (line.rfind("### ", 0) == 0)
        {
            flush();
            ImGui::Dummy(ImVec2(0, 2 * S));
            ImGui::PushStyleColor(ImGuiCol_Text, C_GOLD);
            ImGui::TextUnformatted(plain(line.substr(4)).c_str());
            ImGui::PopStyleColor();
        } else if (line.rfind("## ", 0) == 0)
        {
            flush(); /* "Changelog": the box's own heading says it */
        } else if ((line.rfind("- ", 0) == 0) || (line.rfind("* ", 0) == 0))
        {
            flush();
            bullet = true;
            item = plain(line.substr(2));
        } else if ((line[0] == ' ') && !item.empty())
            item += " " + t; /* a bullet's next line */
        else
        {
            flush();
            bullet = false;
            item = t;
        }
    }
    flush();
}

/******************************************************************************/
/* Settings */

static CfgFile s_cfg, s_lcfg; /* the copies being edited */

/** A setting's name on the left (it wraps when it is long), its control on the right half. */
static void row_label(const char *label)
{
    const float full = ImGui::GetContentRegionAvail().x, col = floorf(full * 0.5f);
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(x0 + col - 12 * S);
    ImGui::TextUnformatted(T(label));
    ImGui::PopTextWrapPos();
    ImGui::SameLine(x0 + col);
    ImGui::SetNextItemWidth(-1);
}

static void cfg_check(CfgFile &f, const char *label, const char *key, bool def, bool inverted = false)
{
    bool v = f.flag(key, def) != inverted;
    if (ImGui::Checkbox((std::string(T(label)) + "###" + key).c_str(), &v))
        f.set(key, (v != inverted) ? "TRUE" : "FALSE");
}

static void cfg_combo(CfgFile &f, const char *label, const char *key, const char *def, const std::vector<std::pair<const char *, const char *>> &options)
{
    const std::string cur = f.get(key, def);
    const char *shown = cur.c_str();
    for (const auto &o : options)
        if (_stricmp(o.first, cur.c_str()) == 0)
            shown = T(o.second);
    row_label(label);
    if (ImGui::BeginCombo((std::string("##") + key).c_str(), shown))
    {
        for (const auto &o : options)
            if (ImGui::Selectable((std::string(T(o.second)) + "###" + o.first).c_str(), _stricmp(o.first, cur.c_str()) == 0))
                f.set(key, o.first);
        ImGui::EndCombo();
    }
}

static void cfg_int(CfgFile &f, const char *label, const char *key, int def, int lo, int hi, bool slider, const char *format = "%d")
{
    int v = atoi(f.get(key, std::to_string(def).c_str()).c_str());
    row_label(label);
    const std::string id = std::string("##") + key;
    const bool changed = slider ? ImGui::SliderInt(id.c_str(), &v, lo, hi, format) : ImGui::InputInt(id.c_str(), &v);
    if (changed)
        f.set(key, std::to_string(std::max(lo, std::min(hi, v))));
}

static void cfg_text(CfgFile &f, const char *label, const char *key)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", f.get(key).c_str());
    row_label(label);
    if (ImGui::InputText((std::string("##") + key).c_str(), buf, sizeof(buf)))
        f.set(key, buf);
}

static std::vector<std::string> g_resolutions;

static void list_resolutions()
{
    std::vector<std::pair<int, int>> modes = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1366, 768}, {1600, 900},
        {1920, 1080}, {2560, 1440}, {3840, 2160}};
    int count = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&count);
    for (int d = 0; (displays != nullptr) && (d < count); d++)
    {
        int n = 0;
        SDL_DisplayMode **list = SDL_GetFullscreenDisplayModes(displays[d], &n);
        for (int i = 0; (list != nullptr) && (i < n); i++)
            modes.push_back({list[i]->w, list[i]->h});
        SDL_free(list);
    }
    SDL_free(displays);
    std::sort(modes.begin(), modes.end());
    modes.erase(std::unique(modes.begin(), modes.end()), modes.end());
    for (const auto &m : modes)
        if ((m.first >= 640) && (m.second >= 400))
            g_resolutions.push_back(std::to_string(m.first) + "x" + std::to_string(m.second));
}

/** One screen mode of INGAME_RES / FRONTEND_RES: "DESKTOP", "DESKTOP_FULL", "WxHx32" (full screen) or "WxHw32" (a window). */
static void resolution_row(const char *label, std::string &token, bool optional)
{
    const std::string desktop_full = std::string(T("Fullscreen")) + " (" + T("Match desktop") + ")";
    const char *const kinds[] = {T("Match desktop"), desktop_full.c_str(), T("Fullscreen"), T("Windowed"), T("None")};
    int kind = 0;
    std::string res = "1920x1080";
    if (token.empty()) kind = 4;
    else if (_stricmp(token.c_str(), "DESKTOP") == 0) kind = 0;
    else if (_stricmp(token.c_str(), "DESKTOP_FULL") == 0) kind = 1;
    else
    {
        const size_t x = token.find_first_of("xX"), m = token.find_first_of("xXwW", (x == std::string::npos) ? 0 : x + 1);
        if ((x != std::string::npos) && (m != std::string::npos))
        {
            res = token.substr(0, m);
            kind = ((token[m] == 'w') || (token[m] == 'W')) ? 3 : 2;
        }
    }
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T(label));
    ImGui::SameLine(150 * S);
    ImGui::SetNextItemWidth(250 * S);
    bool changed = false;
    if (ImGui::BeginCombo("##kind", kinds[kind]))
    {
        for (int k = 0; k < (optional ? 5 : 4); k++)
            if (ImGui::Selectable(kinds[k], k == kind))
            {
                kind = k;
                changed = true;
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::BeginDisabled((kind != 2) && (kind != 3));
    if (ImGui::BeginCombo("##res", ((kind == 2) || (kind == 3)) ? res.c_str() : "-"))
    {
        for (const std::string &r : g_resolutions)
            if (ImGui::Selectable(r.c_str(), r == res))
            {
                res = r;
                changed = true;
            }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    if (changed)
        token = (kind == 0) ? "DESKTOP" : (kind == 1) ? "DESKTOP_FULL" : (kind == 2) ? res + "x32" : (kind == 3) ? res + "w32" : "";
}

static void heading(const char *text)
{
    ImGui::Dummy(ImVec2(0, 4 * S));
    ImGui::PushStyleColor(ImGuiCol_Text, C_GOLD);
    ImGui::TextUnformatted(T(text));
    ImGui::PopStyleColor();
    ImGui::Separator();
}

static void settings_window(float w, float h)
{
    const ImVec2 size(std::min(w - 40 * S, 820 * S), std::min(h - 40 * S, 640 * S));
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size);
    if (!ImGui::BeginPopupModal(titled("Settings").c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        return;
    const float foot = 52 * S;
    if (ImGui::BeginTabBar("tabs"))
    {
        if (ImGui::BeginTabItem(titled("Game").c_str(), nullptr, tab_flags("Game")))
        {
            ImGui::BeginChild("game", ImVec2(0, -foot));
            cfg_combo(s_cfg, "Game language", "LANGUAGE", "ENG", {{"ENG", "English"}, {"ITA", "Italian"}, {"FRE", "French"}, {"SPA", "Spanish"},
                {"DUT", "Dutch"}, {"GER", "German"}, {"POL", "Polish"}, {"SWE", "Swedish"}, {"JPN", "Japanese"}, {"RUS", "Russian"},
                {"KOR", "Korean"}, {"CZE", "Czech"}, {"LAT", "Latin"}, {"UKR", "Ukrainian"}, {"POR", "Portuguese"},
                {"CHI", "Chinese (Simplified)"}, {"CHT", "Chinese (Traditional)"}});
            cfg_combo(s_cfg, "Screenshot type", "SCREENSHOT", "PNG", {{"PNG", "PNG"}, {"BMP", "BMP"}});
            {
                /* STARTUP: which of the screens before the menu are shown */
                std::vector<std::string> st = split(s_cfg.get("STARTUP"));
                auto has = [&](const char *t) { return std::find_if(st.begin(), st.end(), [&](const std::string &s) { return _stricmp(s.c_str(), t) == 0; }) != st.end(); };
                bool intro = has("INTRO"), splash = has("LEGAL") || has("FX");
                bool changed = ImGui::Checkbox(titled("Display Intro").c_str(), &intro);
                changed = ImGui::Checkbox(titled("Display splash screens").c_str(), &splash) || changed;
                if (changed)
                {
                    std::string v = splash ? "LEGAL FX" : "";
                    if (intro)
                        v += v.empty() ? "INTRO" : " INTRO";
                    s_cfg.set("STARTUP", v);
                    s_lcfg.set("GAME_PARAM_NO_INTRO", intro ? "FALSE" : "TRUE");
                }
            }
            cfg_check(s_lcfg, "Enable Cheats", "GAME_PARAM_ALEX", false);
            cfg_check(s_cfg, "Enable Censorship", "CENSORSHIP", false);
            cfg_check(s_cfg, "Automatically enable flee", "FLEE_BUTTON_DEFAULT", false);
            cfg_check(s_cfg, "Automatically enable imprison", "IMPRISON_BUTTON_DEFAULT", false);
            cfg_check(s_cfg, "Enable delta time", "DELTA_TIME", true);
            cfg_check(s_cfg, "Freeze game when not focused", "FREEZE_GAME_ON_FOCUS_LOST", false);
            cfg_check(s_cfg, "Automatically save replays", "AUTOMATIC_REPLAYS", true);
            cfg_check(s_cfg, "Exit on LUA error", "EXIT_ON_LUA_ERROR", false);
            cfg_int(s_cfg, "Max. zoom distance\n(default = 60)", "MAX_ZOOM_DISTANCE", 60, 0, 100, true);
            cfg_int(s_cfg, "Gameturns per second\n(default = 20)", "TURNS_PER_SECOND", 20, 1, 200, false);
            {
                std::vector<std::string> r = split(s_cfg.get("MAX_REPLAYS", "5 5 10"));
                r.resize(3, "5");
                int v[3] = {atoi(r[0].c_str()), atoi(r[1].c_str()), atoi(r[2].c_str())};
                static const char *const names[3] = {"Campaign max. replays", "Freeplay max. replays", "Multiplayer max. replays"};
                bool changed = false;
                for (int i = 0; i < 3; i++)
                {
                    row_label(names[i]);
                    ImGui::PushID(i);
                    changed = ImGui::InputInt("##MAX_REPLAYS", &v[i]) || changed;
                    ImGui::PopID();
                }
                if (changed)
                    s_cfg.set("MAX_REPLAYS", std::to_string(std::max(0, v[0])) + " " + std::to_string(std::max(0, v[1])) + " " + std::to_string(std::max(0, v[2])));
            }
            cfg_int(s_cfg, "Replay max. filesize\n(0 = unlimited)", "REPLAY_MAX_SIZE", 32768, 0, 4194304, false);
            cfg_text(s_lcfg, "Game launch options", "EXTRA_GAME_LAUNCH_OPTIONS");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(titled("Graphics").c_str(), nullptr, tab_flags("Graphics")))
        {
            ImGui::BeginChild("graphics", ImVec2(0, -foot));
            {
                std::vector<std::string> v = split(s_cfg.get("INGAME_RES", "DESKTOP"));
                v.resize(3);
                const std::vector<std::string> before = v;
                resolution_row("View 1", v[0], false);
                resolution_row("View 2", v[1], true);
                resolution_row("View 3", v[2], true);
                if (v != before)
                {
                    v.erase(std::remove(v.begin(), v.end(), std::string()), v.end());
                    s_cfg.set("INGAME_RES", join(v));
                }
            }
            ImGui::Separator();
            {
                std::vector<std::string> v = split(s_cfg.get("FRONTEND_RES", "640x480w32 DESKTOP DESKTOP"));
                v.resize(3, "DESKTOP");
                const std::vector<std::string> before = v;
                resolution_row("Menu", v[2], false);
                resolution_row("Movies", v[1], false);
                resolution_row("Failsafe", v[0], false);
                if (v != before)
                    s_cfg.set("FRONTEND_RES", join(v));
            }
            ImGui::Separator();
            cfg_int(s_cfg, "Display monitor", "DISPLAY_NUMBER", 1, 1, 8, false);
            cfg_combo(s_cfg, "Rendering Backend", "RENDERER", "OPENGL", {{"OPENGL", "OpenGL"}, {"SOFTWARE", "Software"}});
            cfg_check(s_cfg, "Enable VSync", "VSYNC", true);
            {
                std::vector<std::string> f = split(s_cfg.get("FRAMES_PER_SECOND", "AUTO 60"));
                bool is_auto = !f.empty() && (_stricmp(f[0].c_str(), "AUTO") == 0);
                int fps = is_auto ? ((f.size() > 1) ? atoi(f[1].c_str()) : 60) : (f.empty() ? 0 : atoi(f[0].c_str()));
                bool changed = ImGui::Checkbox(titled("Automatically determine max. FPS").c_str(), &is_auto);
                row_label("Max. FPS\n(0 = unlimited)");
                changed = ImGui::InputInt("##FPS", &fps) || changed;
                if (changed)
                    s_cfg.set("FRAMES_PER_SECOND", (is_auto ? "AUTO " : "") + std::to_string(std::max(0, fps)));
            }
            cfg_combo(s_cfg, "Viewport mode", "VIEWPORT_MODE", "ORIGINAL", {{"ORIGINAL", "Original"}, {"FULL", "Full"}, {"FULL_LETTERBOX", "Full (Letterbox)"}});
            cfg_combo(s_cfg, "Resize Movies", "RESIZE_MOVIES", "FIT", {{"OFF", "Off"}, {"FIT", "Fit (default)"}, {"STRETCH", "Stretch"}, {"CROP", "Crop"},
                {"PIXELPERFECT", "Pixel perfect"}, {"4BY3", "4:3"}, {"4BY3PP", "4:3 pixel perfect"}});
            cfg_check(s_lcfg, "Smoothen edges", "GAME_PARAM_VID_SMOOTH", false);
            cfg_check(s_cfg, "Enable map fade animation", "PARCHMENT_MAP_FADE", true);
            ImGui::Separator();
            cfg_int(s_cfg, "Creature flower size", "CREATURE_STATUS_SIZE", 16, 0, 64, true);
            cfg_int(s_cfg, "Hand size", "HAND_SIZE", 100, 25, 300, true);
            cfg_int(s_cfg, "Select box line size", "LINE_BOX_SIZE", 150, 25, 500, true);
            cfg_int(s_cfg, "GUI blink rate\n(default = 1)", "GUI_BLINK_RATE", 1, 1, 60, false);
            cfg_int(s_cfg, "Neutral flash rate\n(default = 1)", "NEUTRAL_FLASH_RATE", 1, 1, 60, false);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(titled("Sound").c_str(), nullptr, tab_flags("Sound")))
        {
            ImGui::BeginChild("sound", ImVec2(0, -foot));
            cfg_check(s_lcfg, "Enable sound", "GAME_PARAM_NO_SOUND", false, true);
            cfg_check(s_lcfg, "Use CD music instead of local files", "GAME_PARAM_USE_CD_MUSIC", false);
            cfg_check(s_cfg, "Mute audio when game is not focused", "MUTE_AUDIO_ON_FOCUS_LOST", false);
            cfg_check(s_cfg, "Pause music when game is paused", "PAUSE_MUSIC_WHEN_GAME_PAUSED", false);
            ImGui::Separator();
            cfg_check(s_cfg, "Enable atmospheric sounds", "ATMOSPHERIC_SOUNDS", true);
            cfg_combo(s_cfg, "Frequency", "ATMOS_FREQUENCY", "MEDIUM", {{"LOW", "Low"}, {"MEDIUM", "Medium"}, {"HIGH", "High"}});
            cfg_combo(s_cfg, "Volume", "ATMOS_VOLUME", "MEDIUM", {{"LOW", "Low"}, {"MEDIUM", "Medium"}, {"HIGH", "High"}});
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(titled("Input").c_str(), nullptr, tab_flags("Input")))
        {
            ImGui::BeginChild("input", ImVec2(0, -foot));
            cfg_int(s_cfg, "Mouse sensitivity", "POINTER_SENSITIVITY", 0, 0, 500, true, "%d%%");
            cfg_combo(s_cfg, "Zoom towards cursor", "ZOOM_TO_MOUSE", "WHEEL", {{"ON", "Always"}, {"OFF", "Never"}, {"WHEEL", "Mousewheel only"}});
            cfg_combo(s_cfg, "Rotate around cursor", "ROTATE_AROUND_MOUSE", "OFF", {{"OFF", "Never"}, {"ON", "Always"},
                {"MOVEMENT_KEYS", "Movement Keys Only (While holding Ctrl)"}, {"ROTATION_KEYS", "Rotation Keys Only"}});
            cfg_combo(s_cfg, "Default tag mode", "DEFAULT_TAG_MODE", "SINGLE", {{"SINGLE", "Single"}, {"DRAG", "Drag"}, {"REMEMBER", "Remember Last"}});
            cfg_check(s_cfg, "Enable tag mode toggle (Right-click)", "TAG_MODE_TOGGLING", false);
            cfg_check(s_cfg, "Enable relative mouse mode", "RELATIVE_MOUSE_MODE", true);
            cfg_check(s_lcfg, "Unlock cursor from game window", "GAME_PARAM_ALT_INPUT", false);
            cfg_check(s_cfg, "Lock cursor to the game window", "CAPTURE_CURSOR", true);
            cfg_check(s_cfg, "Unlock cursor when game is paused", "UNLOCK_CURSOR_WHEN_GAME_PAUSED", true);
            cfg_check(s_cfg, "Lock cursor in possession", "LOCK_CURSOR_IN_POSSESSION", true);
            cfg_check(s_cfg, "Screen edge camera panning", "CURSOR_EDGE_CAMERA_PANNING", true);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(titled("Launcher").c_str(), nullptr, tab_flags("Launcher")))
        {
            ImGui::BeginChild("launcher", ImVec2(0, -foot));
            {
                std::vector<std::pair<const char *, const char *>> langs;
                for (const Language &l : g_languages)
                    langs.push_back({l.code, l.name});
                cfg_combo(s_lcfg, "Launcher language", "LAUNCHER_LANGUAGE", "en", langs);
            }
            ImGui::Dummy(ImVec2(0, 6 * S));
            if (ImGui::Button(titled("Show launcher command line options").c_str()))
                g_show_options = true;
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(titled("API").c_str(), nullptr, tab_flags("API")))
        {
            ImGui::BeginChild("api", ImVec2(0, -foot));
            cfg_check(s_cfg, "Enable API", "API_ENABLED", false);
            cfg_int(s_cfg, "API port", "API_PORT", 5599, 1, 65535, false);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(titled("About").c_str(), nullptr, tab_flags("About")))
        {
            ImGui::BeginChild("about", ImVec2(0, -foot));
            const float aw = ImGui::GetContentRegionAvail().x;
            if (g_header != nullptr)
            {
                const float iw = std::min(aw * 0.8f, 520 * S), ih = iw * g_header_h / g_header_w;
                ImGui::SetCursorPosX((aw - iw) * 0.5f);
                ImGui::Image((ImTextureID)(intptr_t)g_header, ImVec2(iw, ih));
            }
            auto centred = [&](const char *text, float size, ImU32 col) {
                const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, 4000.0f, 0.0f, text);
                ImGui::SetCursorPosX((aw - ts.x) * 0.5f);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                text_shadow(ImGui::GetWindowDrawList(), size, p, col, text);
                ImGui::Dummy(ts);
            };
            auto centred_link = [&](const char *id, const char *text, const char *url) {
                const float size = 17 * S;
                const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, 4000.0f, 0.0f, text);
                ImGui::SetCursorPosX((aw - ts.x) * 0.5f);
                if (link(id, text, size, C_GOLD))
                    open_url(url);
            };
            ImGui::Dummy(ImVec2(0, 8 * S));
            centred(GAME_TITLE, 24 * S, C_GOLD_HI);
            ImGui::Dummy(ImVec2(0, 4 * S));
            centred_link("repo", REPO_URL, REPO_URL);
            const float left = ImGui::GetContentRegionAvail().y - 110 * S;
            if (left > 0)
                ImGui::Dummy(ImVec2(0, left));
            ImGui::Separator();
            /* the credits: on the left */
            auto left_line = [&](const char *text, float size, ImU32 col) {
                const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, 4000.0f, 0.0f, text);
                text_shadow(ImGui::GetWindowDrawList(), size, ImGui::GetCursorScreenPos(), col, text);
                ImGui::Dummy(ts);
            };
            left_line(T("Credits"), 20 * S, C_GOLD);
            left_line(T("Endless Keeper is built on KeeperFX, the work of the KeeperFX team and its community."), 16 * S, C_TEXT);
            if (link("kfx", "https://keeperfx.net", 17 * S, C_GOLD))
                open_url("https://keeperfx.net");
            if (link("discord", "Discord - Keeper Klan", 17 * S, C_GOLD))
                open_url("https://discord.gg/hE4p7vy2Hb");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    const ImVec2 bs(130 * S, 34 * S);
    const ImVec2 base = ImGui::GetCursorScreenPos();
    const float right = base.x + ImGui::GetContentRegionAvail().x;
    if (button("save", T("Save"), ImVec2(right - bs.x * 2 - 12 * S, base.y + 4 * S), bs, true, 18 * S))
    {
        if (!s_cfg.save() || !s_lcfg.save())
            g_message = "The settings could not be saved: is the game's folder read-only?";
        g_cfg = s_cfg;
        g_lcfg = s_lcfg;
        load_language(g_lcfg.get("LAUNCHER_LANGUAGE", "en"));
        ImGui::CloseCurrentPopup();
    }
    if (button("cancel", T("Cancel"), ImVec2(right - bs.x, base.y + 4 * S), bs, false, 18 * S))
        ImGui::CloseCurrentPopup();
    if (g_show_options)
    {
        ImGui::OpenPopup(titled("Launcher command line options").c_str());
        g_show_options = false;
    }
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(titled("Launcher command line options").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        static const char *const rows[][2] = {{"--play", "start the game at once, without the launcher's window"},
            {"--game-dir <folder>", "the game's folder, when the launcher isn't in it"},
            {"--no-update-check", "don't look for new releases"}, {"--no-battle", "a plain background: the game doesn't run behind the launcher"},
            {"--32bit", "start the 32-bit game on 64-bit Windows too"}};
        for (const auto &r : rows)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, C_GOLD);
            ImGui::TextUnformatted(r[0]);
            ImGui::PopStyleColor();
            ImGui::SameLine(230 * S);
            ImGui::TextUnformatted(T(r[1]));
        }
        ImGui::Dummy(ImVec2(0, 6 * S));
        if (ImGui::Button(titled("Close").c_str(), ImVec2(120 * S, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::EndPopup();
}

/******************************************************************************/
/* the window */

static void updates_box(ImVec2 pos, ImVec2 size)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    panel(dl, pos, ImVec2(pos.x + size.x, pos.y + size.y));
    text_shadow(dl, 24 * S, ImVec2(pos.x + 20 * S, pos.y + 14 * S), C_GOLD, T("Updates"));
    dl->AddLine(ImVec2(pos.x + 16 * S, pos.y + 48 * S), ImVec2(pos.x + size.x - 16 * S, pos.y + 48 * S), C_EDGE, 1.0f);
    UpdateState up;
    std::vector<Release> releases;
    std::string why;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        up = g_up;
        releases = g_releases;
        why = g_up_why;
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 16 * S, pos.y + 56 * S));
    ImGui::BeginChild("updates", ImVec2(size.x - 28 * S, size.y - 68 * S), 0, 0);
    if (releases.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, C_TEXT_DIM);
        if (up == UP_CHECKING)
            ImGui::TextUnformatted(T("Loading..."));
        else if (opt_no_update)
            ImGui::TextUnformatted("The update check is off (--no-update-check).");
        else
        {
            ImGui::TextWrapped("%s (%s)", T("The release list could not be loaded."), why.c_str());
            ImGui::PopStyleColor();
            if (ImGui::Button(titled("Retry").c_str()))
                check_updates();
            ImGui::PushStyleColor(ImGuiCol_Text, C_TEXT_DIM);
        }
        ImGui::PopStyleColor();
    }
    const std::vector<int> mine = installed_version();
    for (size_t i = 0; i < releases.size(); i++)
    {
        const Release &r = releases[i];
        ImGui::PushID((int)i);
        if (i > 0)
        {
            ImGui::Dummy(ImVec2(0, 6 * S));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 2 * S));
        }
        if (link("title", plain(r.title).c_str(), (i == 0) ? 21 * S : 18 * S, (i == 0) ? C_GOLD_HI : C_GOLD))
            open_url(r.url.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Open this release's download page"));
        ImGui::PushStyleColor(ImGuiCol_Text, C_TEXT_DIM);
        const bool newer = version_newer(r.version, mine), same = !newer && !version_newer(mine, r.version);
        ImGui::Text("%s%s%s", r.date.c_str(), (newer || same) ? "  -  " : "", newer ? T("new") : (same ? T("installed") : ""));
        ImGui::PopStyleColor();
        release_notes(r.body);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

static void message_boxes(float w, float h)
{
    UpdateState up;
    std::string why, stage;
    float fraction;
    Release newest;
    bool have_newer = false;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        up = g_up;
        why = g_up_why;
        stage = g_up_stage;
        fraction = g_up_fraction;
        if (!g_releases.empty() && version_newer(g_releases[0].version, installed_version()))
        {
            newest = g_releases[0];
            have_newer = true;
        }
    }
    if ((up == UP_LISTED) && have_newer && !g_prompted)
    {
        g_prompted = true;
        g_prompt_update = true;
    }
    if (g_prompt_update && !ImGui::IsPopupOpen(titled("Update").c_str()) && !ImGui::IsPopupOpen(titled("Settings").c_str()))
        ImGui::OpenPopup(titled("Update").c_str());
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(w - 40 * S, 520 * S), 0));
    if (ImGui::BeginPopupModal(titled("Update").c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
    {
        const float bw = 150 * S;
        const float full = ImGui::GetContentRegionAvail().x;
        auto percent = [&](const char *english) {
            /* "Downloading: %p%" with the number in */
            std::string text = T(english);
            const size_t at = text.find("%p%");
            if (at != std::string::npos)
                text.replace(at, 3, std::to_string((int)(fraction * 100.0f)) + "%");
            return text;
        };
        if (up == UP_INSTALLING)
        {
            const std::string text = (stage == "Downloading") ? percent("Downloading: %p%") : (stage == "Unpacking") ? percent("Extracting: %p%")
                : (stage == "Installing") ? percent("Copying: %p%") : std::string(T("Updating..."));
            ImGui::TextUnformatted(text.c_str());
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.72f, 0.16f, 0.08f, 1.0f));
            ImGui::ProgressBar(fraction, ImVec2(-1, 22 * S), "");
            ImGui::PopStyleColor();
            if ((stage == "Downloading") && ImGui::Button(titled("Cancel").c_str(), ImVec2(bw, 0)))
                g_cancel = true;
        } else if (up == UP_INSTALL_FAILED)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, C_GOLD_HI);
            ImGui::TextUnformatted(T("Update failed"));
            ImGui::PopStyleColor();
            ImGui::TextWrapped("%s", why.c_str());
            ImGui::Dummy(ImVec2(0, 4 * S));
            if (ImGui::Button(titled("Open this release's download page").c_str(), ImVec2(bw * 1.9f, 0)))
                open_url(newest.url.c_str());
            ImGui::SameLine(full - bw);
            if (ImGui::Button(titled("Close").c_str(), ImVec2(bw, 0)))
            {
                std::lock_guard<std::mutex> lock(g_mx);
                g_up = UP_LISTED;
                g_prompt_update = false;
                engine_view_resume();
                ImGui::CloseCurrentPopup();
            }
        } else if (up == UP_RESTART)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, C_GOLD_HI);
            ImGui::TextUnformatted(T("Update complete!"));
            ImGui::PopStyleColor();
            ImGui::TextWrapped("%s", plain(newest.title).c_str());
            ImGui::Dummy(ImVec2(0, 4 * S));
            if (ImGui::Button(titled("OK").c_str(), ImVec2(bw, 0)))
            {
                STARTUPINFOW si;
                PROCESS_INFORMATION pi;
                memset(&si, 0, sizeof(si));
                si.cb = sizeof(si);
                std::wstring cmd = L"\"" + g_self_exe + L"\"";
                if (CreateProcessW(g_self_exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_game_dir.c_str(), &si, &pi))
                {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
                SDL_Event quit;
                memset(&quit, 0, sizeof(quit));
                quit.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit);
            }
        } else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, C_GOLD_HI);
            ImGui::TextWrapped("%s", plain(newest.title).c_str());
            ImGui::PopStyleColor();
            ImGui::Text("%s %s", T("Current version:"), GAME_TITLE);
            ImGui::Text("%s %s", T("New version:"), plain(newest.title).c_str());
            ImGui::Dummy(ImVec2(0, 6 * S));
            const ImVec2 base = ImGui::GetCursorScreenPos();
            if (button("yes", T("Update"), base, ImVec2(bw, 36 * S), true, 19 * S) || t_update)
            {
                t_update = false;
                start_update(newest);
            }
            if (button("no", T("Cancel"), ImVec2(base.x + full - bw, base.y), ImVec2(bw, 36 * S), false, 19 * S))
            {
                g_prompt_update = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::Dummy(ImVec2(0, 4 * S));
        }
        ImGui::EndPopup();
    }
    if (!g_message.empty() && !ImGui::IsPopupOpen("Endless Keeper"))
        ImGui::OpenPopup("Endless Keeper");
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(w - 40 * S, 480 * S), 0));
    if (ImGui::BeginPopupModal("Endless Keeper", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::TextWrapped("%s", g_message.c_str());
        ImGui::Dummy(ImVec2(0, 4 * S));
        if (ImGui::Button(titled("OK").c_str(), ImVec2(120 * S, 0)))
        {
            g_message.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void frame(float w, float h)
{
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h), IM_COL32(36, 12, 8, 255), IM_COL32(36, 12, 8, 255), IM_COL32(8, 6, 5, 255), IM_COL32(8, 6, 5, 255));
    if (g_battle && t_bare)
        engine_view_draw(bg, w, h);
    else if (g_battle && engine_view_draw(bg, w, h))
    {
        /* the scene sits back: darker at the top (the title) and the edges */
        bg->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(6, 4, 3, 40));
        bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h * 0.38f), IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
        bg->AddRectFilledMultiColor(ImVec2(0, h * 0.8f), ImVec2(w, h), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150));
    }
    if (t_bare)
        return;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float pad = 22 * S;
    const bool narrow = w < 860 * S;
    /* the header: the head and the title */
    float top = pad;
    if (g_header != nullptr)
    {
        const float hh = std::min(h * 0.27f, (narrow ? w - pad * 2 : w * 0.5f) * g_header_h / g_header_w), hw = hh * g_header_w / g_header_h;
        const ImVec2 a((w - hw) * 0.5f, pad * 0.6f);
        dl->AddImage((ImTextureID)(intptr_t)g_header, ImVec2(a.x + 3 * S, a.y + 5 * S), ImVec2(a.x + hw + 3 * S, a.y + hh + 5 * S),
            ImVec2(0, 0), ImVec2(1, 1), IM_COL32(0, 0, 0, 150));
        dl->AddImage((ImTextureID)(intptr_t)g_header, a, ImVec2(a.x + hw, a.y + hh));
        top = a.y + hh + pad * 0.6f;
    }
    /* the buttons on the left (above the box when the window is narrow), the updates beside them */
    const float bottom = h - pad - 22 * S;
    const float col_w = narrow ? (w - pad * 2) : std::min(330 * S, w * 0.3f);
    const float play_h = 78 * S, set_h = 46 * S, gap = 14 * S;
    UpdateState up;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        up = g_up;
    }
    const bool busy = (up == UP_INSTALLING) || (up == UP_RESTART);
    ImVec2 box_pos, box_size;
    if (narrow)
    {
        const float half = (col_w - gap) * 0.5f;
        if (button("play", T("Play"), ImVec2(pad, top), ImVec2(half, set_h * 1.2f), true, 26 * S, !busy))
            play();
        if (button("settings", T("Settings"), ImVec2(pad + half + gap, top), ImVec2(half, set_h * 1.2f), false, 20 * S, !busy))
            g_show_settings = true;
        box_pos = ImVec2(pad, top + set_h * 1.2f + gap);
        box_size = ImVec2(w - pad * 2, bottom - box_pos.y);
        engine_view_focus(w * 0.5f, h * 0.5f);
    } else
    {
        const float by = std::max(top, bottom - play_h - gap - set_h);
        if (button("play", T("Play"), ImVec2(pad, by), ImVec2(col_w, play_h), true, 40 * S, !busy))
            play();
        if (button("settings", T("Settings"), ImVec2(pad, by + play_h + gap), ImVec2(col_w, set_h), false, 22 * S, !busy))
            g_show_settings = true;
        /* the box keeps to the right: the battle shows between it and the buttons */
        const float box_w = std::min(w - (pad + col_w + pad) - pad, std::max(430 * S, w * 0.46f));
        box_pos = ImVec2(w - pad - box_w, top);
        box_size = ImVec2(box_w, bottom - top);
        engine_view_focus(box_pos.x * 0.5f, h * 0.5f);

    }
    if ((box_size.x > 120 * S) && (box_size.y > 90 * S))
        updates_box(box_pos, box_size);
    /* the bottom line: what the battle is, and the version */
    const float fs = 15 * S;
    text_shadow(dl, fs, ImVec2(pad, h - pad * 0.5f - fs), IM_COL32(200, 186, 160, 200), engine_view_caption());
    const ImVec2 vs = ImGui::GetFont()->CalcTextSizeA(fs, 4000.0f, 0.0f, GAME_TITLE);
    text_shadow(dl, fs, ImVec2(w - pad - vs.x, h - pad * 0.5f - fs), IM_COL32(200, 186, 160, 220), GAME_TITLE);
    if (g_show_settings)
    {
        g_show_settings = false;
        s_cfg = g_cfg;
        s_lcfg = g_lcfg;
        ImGui::OpenPopup(titled("Settings").c_str());
    }
    settings_window(w, h);
    message_boxes(w, h);
    ImGui::End();
}

static SDL_Texture *load_header()
{
    const HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(100), (LPCWSTR)RT_RCDATA);
    if (res == nullptr)
        return nullptr;
    const HGLOBAL mem = LoadResource(nullptr, res);
    const void *data = LockResource(mem);
    int comp = 0;
    unsigned char *px = stbi_load_from_memory((const unsigned char *)data, (int)SizeofResource(nullptr, res), &g_header_w, &g_header_h, &comp, 4);
    if (px == nullptr)
        return nullptr;
    SDL_Texture *tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, g_header_w, g_header_h);
    if (tex != nullptr)
    {
        SDL_UpdateTexture(tex, nullptr, px, g_header_w * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    }
    stbi_image_free(px);
    return tex;
}

int main(int, char **)
{
    /* where the game is: beside the launcher, unless told otherwise */
    {
        wchar_t path[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
        g_self_exe = path;
        g_game_dir = g_self_exe.substr(0, g_self_exe.find_last_of(L"\\/"));
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; (argv != nullptr) && (i < argc); i++)
        {
            const std::wstring a = argv[i];
            if (a == L"--play") opt_play = true;
            else if (a == L"--no-update-check") opt_no_update = true;
            else if (a == L"--no-battle") opt_no_battle = true;
            else if (a == L"--32bit") opt_32bit = true;
            else if ((a == L"--game-dir") && (i + 1 < argc)) g_game_dir = argv[++i];
        }
        LocalFree(argv);
        while (!g_game_dir.empty() && ((g_game_dir.back() == L'\\') || (g_game_dir.back() == L'/')))
            g_game_dir.pop_back();
    }
    DeleteFileW((g_self_exe + L".old").c_str()); /* the launcher an update replaced */
    {
        /* and the files it had in use then, which the update renamed out of the way */
        const std::wstring list = g_game_dir + L"/endlesskeeper-launcher.old";
        std::ifstream f(list.c_str());
        std::string name;
        bool all = true;
        while (std::getline(f, name))
            if (!name.empty() && (name.find("..") == std::string::npos))
            {
                const std::wstring old = g_game_dir + L"/" + widen(name);
                all = (DeleteFileW(old.c_str()) || (GetLastError() == ERROR_FILE_NOT_FOUND)) && all;
            }
        f.close();
        if (all)
            DeleteFileW(list.c_str());
    }
    g_cfg.load(g_game_dir + L"\\keeperfx.cfg");
    g_lcfg.load(g_game_dir + L"\\endlesskeeper-launcher.cfg");
    if (g_lcfg.find("LAUNCHER_LANGUAGE") < 0)
    {
        g_lcfg.set("LAUNCHER_LANGUAGE", system_language()); /* the first start: Windows' language, when there is one */
        g_lcfg.save();
    }
    load_language(g_lcfg.get("LAUNCHER_LANGUAGE", "en"));
    if (opt_play)
    {
        const std::wstring exe = game_exe();
        std::wstring cmd = L"\"" + exe + L"\"" + widen(game_arguments());
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_game_dir.c_str(), &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return 0;
        }
    }
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        MessageBoxA(nullptr, SDL_GetError(), "Endless Keeper", MB_ICONERROR);
        return 1;
    }
    const float display_scale = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
    g_window = SDL_CreateWindow("Endless Keeper", (int)(1180 * display_scale), (int)(720 * display_scale),
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    g_renderer = (g_window != nullptr) ? SDL_CreateRenderer(g_window, nullptr) : nullptr;
    if (g_renderer == nullptr)
    {
        MessageBoxA(nullptr, SDL_GetError(), "Endless Keeper", MB_ICONERROR);
        return 1;
    }
    SDL_SetWindowMinimumSize(g_window, (int)(640 * display_scale), (int)(480 * display_scale));
    SDL_SetRenderVSync(g_renderer, 1);
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    {
        /* a book face for the game's feel: Windows' own fonts (none is shipped) */
        wchar_t win[MAX_PATH];
        GetWindowsDirectoryW(win, MAX_PATH);
        bool loaded = false;
        for (const wchar_t *name : {L"\\Fonts\\palab.ttf", L"\\Fonts\\georgiab.ttf", L"\\Fonts\\segoeuib.ttf", L"\\Fonts\\arialbd.ttf"})
        {
            const std::string file = narrow(std::wstring(win) + name);
            if ((GetFileAttributesW((std::wstring(win) + name).c_str()) != INVALID_FILE_ATTRIBUTES) &&
                (io.Fonts->AddFontFromFileTTF(file.c_str(), 17.0f) != nullptr))
            {
                loaded = true;
                break;
            }
        }
        if (!loaded)
            io.Fonts->AddFontDefault();
        /* and Windows' fonts for the letters the book face lacks (Japanese, Korean, Chinese ...) */
        for (const wchar_t *name : {L"\\Fonts\\segoeui.ttf", L"\\Fonts\\malgunbd.ttf", L"\\Fonts\\malgun.ttf", L"\\Fonts\\YuGothB.ttc", L"\\Fonts\\meiryob.ttc",
                 L"\\Fonts\\msyhbd.ttc", L"\\Fonts\\msyh.ttc"})
        {
            if (GetFileAttributesW((std::wstring(win) + name).c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            ImFontConfig merge;
            merge.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(narrow(std::wstring(win) + name).c_str(), 17.0f, &merge);
        }
    }
    ImGui_ImplSDL3_InitForSDLRenderer(g_window, g_renderer);
    ImGui_ImplSDLRenderer3_Init(g_renderer);
    g_header = load_header();
    g_battle = !opt_no_battle && engine_view_init(g_renderer, game_exe(), g_game_dir);
    list_resolutions();
    {
        const char *test = getenv("EKL_TEST");
        std::string spec = (test != nullptr) ? test : "";
        for (size_t at = 0; at < spec.size();)
        {
            const size_t end = std::min(spec.find(';', at), spec.size()), eq = spec.find('=', at);
            if ((eq != std::string::npos) && (eq < end))
            {
                const std::string key = spec.substr(at, eq - at), val = spec.substr(eq + 1, end - eq - 1);
                if (key == "shot") t_shot = val;
                else if (key == "after") t_after = (float)atof(val.c_str());
                else if (key == "settings") t_tab = val;
                else if (key == "bare") t_bare = true;
                else if (key == "play") t_play = (float)atof(val.c_str());
                else if (key == "burst") t_burst = atoi(val.c_str());
                else if (key == "version") t_version = val;
                else if (key == "lang") load_language(val);
                else if (key == "update") t_update = true;
                else if (key == "size") sscanf(val.c_str(), "%dx%d", &t_w, &t_h);
            }
            at = end + 1;
        }
        if ((t_w > 0) && (t_h > 0))
            SDL_SetWindowSize(g_window, t_w, t_h);
    }
    if (!opt_no_update)
        check_updates();
    else
    {
        std::lock_guard<std::mutex> lock(g_mx);
        g_up = UP_FAILED;
    }
    /* the window comes when the game behind it has its first scene (a short wait, and no longer than 12 s) */
    bool window_shown = false;
    const Uint64 opened_at = SDL_GetTicks();
    float last_scale = 0;
    Uint64 last = SDL_GetPerformanceCounter();
    bool running = true;
    while (running)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if ((e.type == SDL_EVENT_QUIT) || ((e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) && (e.window.windowID == SDL_GetWindowID(g_window))))
                running = false;
        }
        if (g_game_process != nullptr)
        {
            /* the game is on: the launcher waits, hidden, and comes back when it has closed */
            if (WaitForSingleObject(g_game_process, 200) == WAIT_OBJECT_0)
            {
                CloseHandle(g_game_process);
                g_game_process = nullptr;
                engine_view_note("Play: the game has closed; the launcher is back");
                g_cfg.load(g_cfg.path);
                engine_view_resume();
                SDL_ShowWindow(g_window);
                SDL_RaiseWindow(g_window);
                last = SDL_GetPerformanceCounter();
            }
            continue;
        }
        if (!window_shown)
        {
            if (g_battle)
                engine_view_update(0.016f);
            if (!g_battle || engine_view_ready() || (SDL_GetTicks() - opened_at > 12000))
            {
                window_shown = true;
                SDL_ShowWindow(g_window);
                SDL_RaiseWindow(g_window);
                last = SDL_GetPerformanceCounter();
            } else
            {
                SDL_Delay(15);
                continue;
            }
        }
        if ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) != 0)
        {
            SDL_Delay(100);
            last = SDL_GetPerformanceCounter();
            continue;
        }
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
        const float scale = std::max(0.7f, std::min(2.6f, std::min((float)ph / 720.0f, (float)pw / 1000.0f)));
        if (fabsf(scale - last_scale) > 0.01f)
        {
            last_scale = S = scale;
            apply_style();
            ImGui::GetStyle().FontSizeBase = 17.0f * S;
        }
        const Uint64 now = SDL_GetPerformanceCounter();
        const float dt = (float)((double)(now - last) / (double)SDL_GetPerformanceFrequency());
        last = now;
        if (g_battle)
            engine_view_update(dt);
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        frame((float)pw, (float)ph);
        ImGui::Render();
        SDL_SetRenderDrawColor(g_renderer, 8, 6, 5, 255);
        SDL_RenderClear(g_renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
        if (!t_shot.empty())
        {
            static float t_time = 0;
            static bool t_opened = false;
            t_time += dt;
            if ((t_play > 0) && (t_time >= t_play))
            {
                t_play = 0;
                play();
            }
            if (!t_tab.empty() && !t_opened && (t_time > 1.0f))
            {
                t_opened = true;
                g_show_settings = true;
            } else if (t_opened && (t_time > 2.0f))
                t_tab.clear(); /* selected: the tab is the player's again */
            if (t_time >= t_after)
            {
                static int t_done = 0;
                SDL_Surface *shot = SDL_RenderReadPixels(g_renderer, nullptr);
                if (shot != nullptr)
                {
                    std::string file = t_shot;
                    if (t_done > 0)
                        file.insert(file.size() - 4, "_" + std::to_string(t_done));
                    SDL_SaveBMP(shot, file.c_str());
                    SDL_DestroySurface(shot);
                }
                t_after += 0.25f;
                running = (t_done++ < t_burst);
            }
        }
        SDL_RenderPresent(g_renderer);
    }
    g_cancel = true;
    engine_view_shutdown();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
