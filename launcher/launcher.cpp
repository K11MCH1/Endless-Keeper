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
#include "fx.h"
#include "heart_sound.h"
#include "migrate.h"
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
static bool g_manual_check = false; /* "Check for Update" was pressed: its answer is shown when the check is done */
static bool g_show_settings = false, g_show_options = false;
static std::string g_message;       /* a box with one line and OK */
static HANDLE g_game_process = nullptr;
/* test hook EKL_TEST="shot=<file.bmp>;after=<seconds>;settings=<tab>;size=<w>x<h>": the window as drawn, saved after that
   long (then the launcher closes), with Settings open on that tab */
static std::string t_shot, t_tab;
static bool t_bare = false; /* "bare=1": the battle alone */
static bool t_hidden = false; /* "hidden=1": the window is never shown (a test that only wants the picture) */
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

/* The launcher's looks (test branch 2026-11-23). LAUNCHER_LOOK in endlesskeeper-launcher.cfg; test key "look=<n>".
   Classic is the launcher as it was. The two modern ones keep its layout and differ in what carries the picture:
   - Molten: the battle is the picture and the boxes are dark glass over it (its blur shows through); light comes
     from below (lava, embers) and from the Play button, which beats like the Dungeon Heart; a book face speaks
     (titles, buttons) and a plain face is read (notes, settings).
   - Obsidian: lines and letters carry it: sharp black slabs with a gold hairline, spaced capitals, gold dust. */
enum { LOOK_CLASSIC = 0, LOOK_MOLTEN = 1, LOOK_OBSIDIAN = 2 };
static int g_look = LOOK_MOLTEN;
static ImFont *g_font_head = nullptr, *g_font_body = nullptr; /* the book face; the plain face for reading */
static SDL_Texture *g_header_light = nullptr; /* the header again, adding light (the sheen over the title) */
static float g_intro = 0;           /* seconds since the window came (or came back): the entrance */
static std::string t_hover;         /* test key "hover=<button id>": that button is drawn as under the pointer */
static bool t_fake_install = false; /* test key "fakeinstall=1": the update box with a progress bar, nothing installed */

static int t_look = -1;             /* test key "look=<0|1|2>": that look, whatever the settings say */
static bool t_still = false;        /* test key "fx=0": no motion */
static bool g_restyle = false;      /* the look changed: the style is set again */
static bool g_mute_unfocused = false; /* keeperfx.cfg's "mute audio when not focused": the heartbeat too */
static float g_game_sfx_volume = 1.0f; /* the game's sound-effect volume (save\settings.toml), 0 .. 1 */

/** The heartbeat's switch and volume from these launcher settings (the saved ones, or the Settings window's own). */
static void heart_sound_from(const CfgFile &lcfg)
{
    const float percent = (float)std::max(0, std::min(100, atoi(lcfg.get("LAUNCHER_HEART_VOLUME", "15").c_str())));
    heart_sound_settings(!lcfg.flag("GAME_PARAM_NO_SOUND", false) && lcfg.flag("LAUNCHER_HEART_SOUND", true),
        g_game_sfx_volume * percent / 100.0f);
}
static int look_from(const std::string &name);
static float box_alpha(const char *name);

/** The look and its motion, from the launcher's settings (and Windows' own "show animations" switch). */
static void apply_look()
{
    g_look = (t_look >= 0) ? t_look : look_from(g_lcfg.get("LAUNCHER_LOOK", "MOLTEN"));
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    fx_motion = (g_look != LOOK_CLASSIC) && g_lcfg.flag("LAUNCHER_EFFECTS", true) && (animations != FALSE) && !t_still;
    g_restyle = true;
    /* the Soul Container's beat with the glows: the game's sound on, the launcher's switch on, at the game's own
       sound-effect volume times the launcher's heartbeat volume (the game's is read again when the game has closed:
       it may have been changed there) */
    g_game_sfx_volume = heart_sound_game_volume(g_game_dir);
    heart_sound_from(g_lcfg);
    g_mute_unfocused = g_cfg.flag("MUTE_AUDIO_ON_FOCUS_LOST", false);
}

static ImFont *head_font()
{
    return (g_font_head != nullptr) ? g_font_head : ImGui::GetFont();
}

/** The font of what is read: the plain face in the Molten look, else the book face. */
static ImFont *body_font()
{
    return ((g_look == LOOK_MOLTEN) && (g_font_body != nullptr)) ? g_font_body : head_font();
}

static int look_from(const std::string &name)
{
    return (_stricmp(name.c_str(), "CLASSIC") == 0) ? LOOK_CLASSIC : (_stricmp(name.c_str(), "OBSIDIAN") == 0) ? LOOK_OBSIDIAN : LOOK_MOLTEN;
}

/** How far in a piece of the window is, 0 to 1: it starts `delay` seconds after the window came. */
static float intro(float delay, float seconds = 0.5f)
{
    if (!fx_motion || (g_look == LOOK_CLASSIC))
        return 1.0f;
    return fx_ease_out((g_intro - delay) / seconds);
}

static ImU32 alpha_of(ImU32 col, float alpha)
{
    const ImU32 a = (ImU32)(((col >> IM_COL32_A_SHIFT) & 0xFF) * std::max(0.0f, std::min(1.0f, alpha)));
    return (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

static ImU32 mix(ImU32 a, ImU32 b, float t)
{
    t = std::max(0.0f, std::min(1.0f, t));
    ImU32 out = 0;
    for (int shift = 0; shift < 32; shift += 8)
        out |= ((ImU32)(((a >> shift) & 0xFF) + (((b >> shift) & 0xFF) - (float)((a >> shift) & 0xFF)) * t + 0.5f) & 0xFF) << shift;
    return out;
}

/** In capitals (the Obsidian look's spaced titles), any alphabet. */
static std::string upper(const char *text)
{
    std::wstring w = widen(text);
    if (!w.empty())
        CharUpperBuffW(&w[0], (DWORD)w.size());
    return narrow(w);
}

static void open_url(const char *url)
{
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

/******************************************************************************/
/* the look */

static void text_shadow(ImDrawList *dl, float size, ImVec2 pos, ImU32 col, const char *text, ImFont *font = nullptr)
{
    if (font == nullptr)
        font = head_font();
    if (g_look == LOOK_CLASSIC)
        dl->AddText(font, size, ImVec2(pos.x + std::max(1.0f, size * 0.06f), pos.y + std::max(1.0f, size * 0.08f)), IM_COL32(0, 0, 0, 220), text);
    else
        /* a lighter shadow straight under the letters (it fades with them) */
        dl->AddText(font, size, ImVec2(pos.x, pos.y + std::max(1.0f, size * 0.07f)), IM_COL32(0, 0, 0, (int)(170.0f * ((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f)), text);
    dl->AddText(font, size, pos, col, text);
}

/** A slab of dark stone with a bronze rim and studs in its corners; in the modern looks dark glass over the blurred
    battle (Molten) or a sharp black slab with a gold hairline and bright corners (Obsidian). */
static void panel(ImDrawList *dl, ImVec2 a, ImVec2 b)
{
    if (g_look == LOOK_MOLTEN)
    {
        const float r = 16.0f * S;
        fx_shadow(dl, a, b, r, 40.0f * S, IM_COL32(0, 0, 0, 150));
        if (fx_backdrop_draw(dl, a, b, r, IM_COL32(150, 140, 136, 255)))
            fx_rect_gradient(dl, a, b, IM_COL32(32, 18, 13, 128), IM_COL32(9, 6, 6, 184), r);
        else
            fx_rect_gradient(dl, a, b, IM_COL32(28, 18, 14, 232), IM_COL32(11, 8, 7, 240), r);
        fx_rect_gradient(dl, ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + 72.0f * S), IM_COL32(255, 190, 130, 24), IM_COL32(255, 190, 130, 0), r, false,
            ImDrawFlags_RoundCornersTop);
        dl->AddRect(a, b, IM_COL32(255, 214, 170, 40), r, 0, 1.0f);
        return;
    }
    if (g_look == LOOK_OBSIDIAN)
    {
        const float r = 2.0f * S, k = 18.0f * S, t = std::max(1.5f, 2.0f * S);
        fx_shadow(dl, a, b, r, 30.0f * S, IM_COL32(0, 0, 0, 170));
        fx_rect_gradient(dl, a, b, IM_COL32(18, 16, 24, 240), IM_COL32(8, 7, 11, 246), r);
        dl->AddRect(a, b, IM_COL32(170, 132, 62, 150), r, 0, 1.0f);
        for (int i = 0; i < 4; i++)
        {
            const float x = (i & 1) ? b.x : a.x, y = (i & 2) ? b.y : a.y, sx = (i & 1) ? -1.0f : 1.0f, sy = (i & 2) ? -1.0f : 1.0f;
            dl->AddLine(ImVec2(x, y), ImVec2(x + sx * k, y), C_GOLD_HI, t);
            dl->AddLine(ImVec2(x, y), ImVec2(x, y + sy * k), C_GOLD_HI, t);
        }
        return;
    }
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

/** The label in a modern button: in the middle, made smaller when it is too long for it (a translation), with
    `spacing` between its letters. */
static void button_label(ImDrawList *dl, ImVec2 a, ImVec2 size, float font_size, ImU32 col, const char *label, float spacing)
{
    ImFont *font = head_font();
    const float room = size.x - 22.0f * S;
    float width = fx_text_spaced(nullptr, font, font_size, a, col, label, spacing * font_size);
    if ((width > room) && (width > 0))
    {
        font_size *= room / width;
        width = fx_text_spaced(nullptr, font, font_size, a, col, label, spacing * font_size);
    }
    const ImVec2 at(floorf(a.x + (size.x - width) * 0.5f), floorf(a.y + (size.y - font_size) * 0.5f));
    fx_text_spaced(dl, font, font_size, ImVec2(at.x, at.y + std::max(1.0f, font_size * 0.07f)), IM_COL32(0, 0, 0, (int)(170.0f * ((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f)),
        label, spacing * font_size);
    fx_text_spaced(dl, font, font_size, at, col, label, spacing * font_size);
}

/** The Molten look's buttons. Play is the one bright thing in the window: molten, with slow currents of light in it,
    a halo that beats like the Dungeon Heart, and sparks off its top edge; the others are dark glass that warms
    under the pointer. */
static void button_molten(ImDrawList *dl, ImGuiID id, const char *label, ImVec2 a, ImVec2 b, bool primary, float font_size, bool enabled, bool hot, bool down, bool sparks)
{
    const float h = fx_follow(id, hot ? 1.0f : 0.0f, 0.18f), p = fx_follow(id + 1, down ? 1.0f : 0.0f, 0.07f);
    const float beat = fx_heartbeat(), t = fx_motion ? fx_time() : 0.0f;
    a.y += p * 2.0f * S;
    b.y += p * 2.0f * S;
    const ImVec2 size(b.x - a.x, b.y - a.y);
    const float r = std::min(size.y * 0.5f, (primary ? 16.0f : 11.0f) * S);
    fx_shadow(dl, a, b, r, 16.0f * S, IM_COL32(0, 0, 0, 130));
    if (primary && enabled)
    {
        fx_halo(dl, a, b, r, (26.0f + 12.0f * h + 8.0f * beat) * S, IM_COL32(255, 80, 24, (int)(130.0f * std::min(1.0f, 0.28f + 0.42f * beat + 0.5f * h))));
        const ImU32 dark = IM_COL32(60, 6, 5, 255);
        fx_rect_gradient(dl, a, b, mix(mix(IM_COL32(176, 34, 16, 255), IM_COL32(228, 66, 26, 255), h), dark, p * 0.5f),
            mix(mix(IM_COL32(84, 9, 7, 255), IM_COL32(126, 18, 10, 255), h), dark, p * 0.5f), r);
        dl->PushClipRect(ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 1), true);
        for (int k = 0; k < 3; k++)
        {
            const float phase = fmodf(t * 0.06f * (1.0f + 0.35f * k) + k * 0.37f, 1.0f);
            fx_glow(dl, ImVec2(a.x + (phase * 1.5f - 0.25f) * size.x, a.y + size.y * (0.64f + 0.18f * sinf(t * 0.8f + k * 2.1f))), size.x * 0.30f, size.y * 0.95f,
                IM_COL32(255, 150, 44, (int)(52.0f + 46.0f * h + 30.0f * beat)));
        }
        dl->PopClipRect();
        fx_rect_gradient(dl, ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + size.y * 0.42f), IM_COL32(255, 226, 190, 26), IM_COL32(255, 226, 190, 0), r, false,
            ImDrawFlags_RoundCornersTop);
        dl->AddRect(a, b, mix(IM_COL32(255, 168, 96, 120), IM_COL32(255, 230, 170, 255), h), r, 0, std::max(1.0f, 1.25f * S));
        if (sparks)
            fx_sparks(ImVec2(a.x + r, a.y - 2.0f * S), ImVec2(b.x - r, a.y + 3.0f * S), 2.5f + 13.0f * h, S);
        button_label(dl, a, size, font_size, mix(IM_COL32(255, 232, 190, 255), IM_COL32(255, 250, 236, 255), h), label, 0.0f);
        return;
    }
    if (!fx_backdrop_draw(dl, a, b, r, IM_COL32(140, 130, 126, 255)))
        dl->AddRectFilled(a, b, IM_COL32(24, 16, 13, 236), r);
    if (!enabled)
    {
        fx_rect_gradient(dl, a, b, IM_COL32(34, 30, 28, 170), IM_COL32(14, 12, 11, 200), r);
        dl->AddRect(a, b, IM_COL32(255, 214, 170, 22), r, 0, 1.0f);
        button_label(dl, a, size, font_size, alpha_of(C_TEXT_DIM, 0.62f), label, 0.0f);
        return;
    }
    fx_rect_gradient(dl, a, b, mix(IM_COL32(44, 26, 18, 150), IM_COL32(150, 46, 20, 205), h), mix(IM_COL32(12, 8, 7, 190), IM_COL32(58, 14, 9, 220), h), r);
    fx_rect_gradient(dl, ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, a.y + size.y * 0.5f), IM_COL32(255, 210, 160, 22), IM_COL32(255, 210, 160, 0), r, false,
        ImDrawFlags_RoundCornersTop);
    dl->AddRect(a, b, mix(IM_COL32(255, 214, 170, 52), C_GOLD_HI, h), r, 0, 1.0f + h * 0.5f * S);
    button_label(dl, a, size, font_size, mix(C_GOLD, IM_COL32(255, 244, 214, 255), h), label, 0.0f);
}

/** The Obsidian look's buttons: black slabs with a gold line. Play has spaced capitals, a glint that runs round its
    rim, and a red core that opens from its middle under the pointer; the others grow a gold underline. */
static void button_obsidian(ImDrawList *dl, ImGuiID id, const char *label, ImVec2 a, ImVec2 b, bool primary, float font_size, bool enabled, bool hot, bool down)
{
    const float h = fx_follow(id, hot ? 1.0f : 0.0f, 0.22f), p = fx_follow(id + 1, down ? 1.0f : 0.0f, 0.07f);
    const float beat = fx_heartbeat(), t = fx_motion ? fx_time() : 0.0f;
    a.y += p * 1.5f * S;
    b.y += p * 1.5f * S;
    const ImVec2 size(b.x - a.x, b.y - a.y);
    const float r = 2.0f * S, line = std::max(1.5f, 2.0f * S);
    fx_shadow(dl, a, b, r, 16.0f * S, IM_COL32(0, 0, 0, 140));
    if (primary && enabled)
        fx_halo(dl, a, b, r, 20.0f * S, IM_COL32(210, 44, 18, (int)(34.0f * beat + 70.0f * h)));
    fx_rect_gradient(dl, a, b, IM_COL32(19, 17, 25, 244), IM_COL32(8, 7, 11, 248), r);
    if (!enabled)
    {
        dl->AddRect(a, b, IM_COL32(120, 104, 80, 90), r, 0, 1.0f);
        button_label(dl, a, size, font_size, alpha_of(C_TEXT_DIM, 0.62f), label, 0.0f);
        return;
    }
    if (primary)
    {
        const float cx = (a.x + b.x) * 0.5f, half = (size.x * 0.5f - 1.0f) * h;
        if (half > 1.0f)
            fx_rect_gradient(dl, ImVec2(cx - half, a.y + 1), ImVec2(cx + half, b.y - 1), IM_COL32(158, 28, 15, (int)(240.0f * h)), IM_COL32(80, 10, 8, (int)(240.0f * h)), 0.0f);
        dl->AddRect(ImVec2(a.x + 5.0f * S, a.y + 5.0f * S), ImVec2(b.x - 5.0f * S, b.y - 5.0f * S), IM_COL32(222, 178, 88, (int)(46.0f + 60.0f * h)), 0.0f, 0, 1.0f);
        dl->AddRect(a, b, mix(C_GOLD, C_GOLD_HI, std::max(h, beat * 0.4f)), r, 0, line);
        if (fx_motion)
        {
            /* the glint: one bright spot going round the rim, clockwise from the top left */
            const float per = 2.0f * (size.x + size.y);
            float d = fmodf(t * 0.13f, 1.0f) * per;
            ImVec2 at;
            if (d < size.x) at = ImVec2(a.x + d, a.y);
            else if ((d -= size.x) < size.y) at = ImVec2(b.x, a.y + d);
            else if ((d -= size.y) < size.x) at = ImVec2(b.x - d, b.y);
            else at = ImVec2(a.x, b.y - (d - size.x));
            dl->PushClipRect(ImVec2(a.x - 3.0f * S, a.y - 3.0f * S), ImVec2(b.x + 3.0f * S, b.y + 3.0f * S), true);
            fx_glow(dl, at, 30.0f * S, 30.0f * S, IM_COL32(255, 220, 150, 70));
            fx_glow(dl, at, 7.0f * S, 7.0f * S, IM_COL32(255, 240, 200, 230));
            dl->PopClipRect();
        }
        button_label(dl, a, size, font_size * 0.86f, mix(C_GOLD_HI, IM_COL32(255, 250, 236, 255), h), upper(label).c_str(), 0.2f);
        return;
    }
    dl->AddRect(a, b, mix(IM_COL32(150, 118, 60, 170), C_GOLD_HI, h), r, 0, 1.0f);
    const float under = (size.x - 28.0f * S) * 0.5f * h;
    if (under > 0.5f)
        dl->AddRectFilled(ImVec2((a.x + b.x) * 0.5f - under, b.y - 8.0f * S), ImVec2((a.x + b.x) * 0.5f + under, b.y - 8.0f * S + line), C_GOLD_HI);
    button_label(dl, ImVec2(a.x, a.y - 1.0f * S), size, font_size, mix(C_GOLD, IM_COL32(255, 244, 214, 255), h), label, 0.0f);
}

/** A button of the game's kind: carved stone, or (primary) the blood red of the Play button. */
static bool button(const char *id, const char *label, ImVec2 pos, ImVec2 size, bool primary, float font_size, bool enabled = true)
{
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::InvisibleButton(id, size);
    ImGui::EndDisabled();
    const bool hot = enabled && (ImGui::IsItemHovered() || (t_hover == id)), down = enabled && ImGui::IsItemActive();
    if (hot)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 a = pos, b(pos.x + size.x, pos.y + size.y);
    if (g_look == LOOK_MOLTEN)
    {
        button_molten(dl, ImGui::GetItemID(), label, a, b, primary, font_size, enabled, hot, down, strcmp(id, "play") == 0);
        return clicked;
    }
    if (g_look == LOOK_OBSIDIAN)
    {
        button_obsidian(dl, ImGui::GetItemID(), label, a, b, primary, font_size, enabled, hot, down);
        return clicked;
    }
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
    const ImVec2 ts = head_font()->CalcTextSizeA(font_size, 4000.0f, 0.0f, label);
    text_shadow(dl, font_size, ImVec2(a.x + (size.x - ts.x) * 0.5f, a.y + (size.y - ts.y) * 0.5f + (down ? 1.5f * S : 0)),
        !enabled ? C_TEXT_DIM : (hot ? IM_COL32(255, 244, 214, 255) : (primary ? IM_COL32(255, 226, 170, 255) : C_GOLD)), label);
    return clicked;
}

/** Text that opens an address: underlined under the pointer. */
static bool link(const char *id, const char *text, float size, ImU32 col)
{
    ImFont *font = head_font();
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
    if (g_look == LOOK_MOLTEN)
    {
        /* softer and rounder: lines nearly gone, the boxes told apart by their fill */
        st.WindowRounding = 14.0f * S;
        st.ChildRounding = 10.0f * S;
        st.FrameRounding = 8.0f * S;
        st.PopupRounding = 10.0f * S;
        st.GrabRounding = 8.0f * S;
        st.TabRounding = 8.0f * S;
        st.ScrollbarRounding = 8.0f * S;
        st.ScrollbarSize = 10.0f * S;
        st.WindowBorderSize = 1.0f;
        st.FrameBorderSize = 0.0f;
        st.FramePadding = ImVec2(10 * S, 6 * S);
        st.WindowTitleAlign = ImVec2(0.5f, 0.5f);
        c[ImGuiCol_Text] = rgb(232, 222, 204);
        c[ImGuiCol_TextDisabled] = rgb(140, 128, 112);
        c[ImGuiCol_WindowBg] = rgb(19, 13, 11, 248);
        c[ImGuiCol_PopupBg] = rgb(24, 16, 13, 250);
        c[ImGuiCol_Border] = rgb(255, 214, 170, 44);
        c[ImGuiCol_FrameBg] = rgb(255, 236, 214, 16);
        c[ImGuiCol_FrameBgHovered] = rgb(255, 236, 214, 30);
        c[ImGuiCol_FrameBgActive] = rgb(255, 170, 110, 46);
        c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = rgb(30, 19, 15);
        c[ImGuiCol_CheckMark] = rgb(255, 190, 104);
        c[ImGuiCol_SliderGrab] = rgb(214, 96, 40);
        c[ImGuiCol_SliderGrabActive] = rgb(255, 150, 70);
        c[ImGuiCol_Button] = rgb(255, 226, 196, 22);
        c[ImGuiCol_ButtonHovered] = rgb(170, 52, 22, 220);
        c[ImGuiCol_ButtonActive] = rgb(120, 26, 14);
        c[ImGuiCol_Header] = rgb(150, 44, 20, 190);
        c[ImGuiCol_HeaderHovered] = rgb(176, 56, 24, 220);
        c[ImGuiCol_HeaderActive] = rgb(196, 64, 26);
        c[ImGuiCol_Separator] = rgb(255, 214, 170, 30);
        c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = rgb(255, 214, 170, 50);
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(255, 214, 170, 110);
        c[ImGuiCol_ScrollbarGrabActive] = rgb(255, 190, 104);
        c[ImGuiCol_Tab] = rgb(0, 0, 0, 0);
        c[ImGuiCol_TabHovered] = rgb(170, 52, 22, 200);
        c[ImGuiCol_TabSelected] = rgb(142, 40, 18);
        c[ImGuiCol_TabSelectedOverline] = rgb(255, 190, 104);
        c[ImGuiCol_ModalWindowDimBg] = rgb(4, 2, 2, 176);
    } else if (g_look == LOOK_OBSIDIAN)
    {
        st.WindowRounding = st.ChildRounding = st.FrameRounding = st.PopupRounding = st.GrabRounding = st.TabRounding = 2.0f * S;
        st.ScrollbarRounding = 0.0f;
        st.ScrollbarSize = 9.0f * S;
        st.WindowBorderSize = 1.0f;
        st.WindowTitleAlign = ImVec2(0.5f, 0.5f);
        c[ImGuiCol_WindowBg] = rgb(13, 12, 17, 250);
        c[ImGuiCol_PopupBg] = rgb(16, 15, 21, 252);
        c[ImGuiCol_Border] = rgb(170, 132, 62, 170);
        c[ImGuiCol_FrameBg] = rgb(28, 26, 36);
        c[ImGuiCol_FrameBgHovered] = rgb(44, 40, 54);
        c[ImGuiCol_FrameBgActive] = rgb(60, 52, 60);
        c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = rgb(20, 18, 26);
        c[ImGuiCol_Button] = rgb(30, 28, 38);
        c[ImGuiCol_ButtonHovered] = rgb(70, 58, 44);
        c[ImGuiCol_ButtonActive] = rgb(130, 26, 14);
        c[ImGuiCol_Separator] = rgb(170, 132, 62, 90);
        c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = rgb(150, 118, 60, 150);
        c[ImGuiCol_Tab] = rgb(0, 0, 0, 0);
        c[ImGuiCol_TabSelected] = rgb(96, 22, 14);
        c[ImGuiCol_ModalWindowDimBg] = rgb(2, 2, 5, 186);
    }
    ImGui::GetIO().FontDefault = body_font();
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
    /* the game is EndlessKeeper.exe (keeperfx.exe until 2026-11-28; that name is now only the bridge old launchers
       need, which migrate.cpp removes) */
    const std::wstring x64 = g_game_dir + L"\\x64\\EndlessKeeper.exe";
    if (!opt_32bit && windows_is_64bit() && (GetFileAttributesW(x64.c_str()) != INVALID_FILE_ATTRIBUTES))
        return x64;
    return g_game_dir + L"\\EndlessKeeper.exe";
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
        g_message = "The game isn't in this folder: EndlessKeeper.exe is missing beside the launcher.";
        return;
    }
    engine_view_pause(); /* the background's game makes way for the real one */
    std::wstring cmd = L"\"" + exe + L"\"" + widen(game_arguments());
    SetEnvironmentVariableW(L"EK_FROM_LAUNCHER", L"1"); /* (the game never hands back to a launcher: ek_handover.c, audit 2026-11-28) */
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
static bool t_check = false;    /* test hook "check=1": Check for Update presses itself once the first list is in */

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
        const Uint64 asked_at = SDL_GetTicks();
        const bool ok = fetch_releases(REPO, list, why);
        {
            char line[96];
            snprintf(line, sizeof(line), "Update check: %s in %d ms", ok ? "answered" : "failed", (int)(SDL_GetTicks() - asked_at));
            engine_view_note(line);
        }
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

/** "Check for Update": the list is fetched again; when it comes, the update prompt if there is a newer release, else a
 *  word that this is the newest (message_boxes). */
static void check_now()
{
    g_manual_check = true;
    g_prompted = false;
    check_updates();
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
        /* the creator's set pieces come with each update while the option is on (a failure leaves the update done) */
        if (ok && g_cfg.flag("OFFICIAL_SET_PIECES", false))
        {
            int count = 0;
            std::string sp_why;
            const SetPiecesResult r = install_setpieces(rel, g_game_dir, [](const char *stage, float fraction) {
                std::lock_guard<std::mutex> lock(g_mx);
                g_up_stage = stage;
                g_up_fraction = fraction;
            }, g_cancel, count, sp_why);
            engine_view_note(("Update: set pieces " + std::string((r == SP_INSTALLED) ? "installed" : (r == SP_LATEST_ALREADY) ?
                "the latest already" : (r == SP_NONE_IN_RELEASE) ? "not in this release" : ("failed: " + sp_why).c_str())).c_str());
        }
        std::lock_guard<std::mutex> lock(g_mx);
        g_up_why = why;
        g_up = ok ? UP_RESTART : UP_INSTALL_FAILED;
    }).detach();
}

/* Settings, Game: "Download" beside the creator's set pieces option */
static std::atomic<bool> g_sp_busy{false};
static std::string g_sp_status; /* under g_mx */

static void start_setpieces_download()
{
    if (g_sp_busy.exchange(true))
        return;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        g_sp_status = "Looking for the latest set pieces...";
    }
    std::thread([] {
        std::vector<Release> list;
        std::string why;
        if (!fetch_releases(REPO, list, why))
        {
            std::lock_guard<std::mutex> lock(g_mx);
            g_sp_status = "The release list could not be fetched: " + why;
            g_sp_busy = false;
            return;
        }
        const Release *rel = nullptr;
        for (const Release &r : list) /* (newest first) */
            if (setpieces_asset(r) != nullptr)
            {
                rel = &r;
                break;
            }
        if (rel == nullptr)
        {
            std::lock_guard<std::mutex> lock(g_mx);
            g_sp_status = "No release has set pieces yet.";
            g_sp_busy = false;
            return;
        }
        static std::atomic<bool> never{false};
        int count = 0;
        const SetPiecesResult r = install_setpieces(*rel, g_game_dir, [](const char *stage, float fraction) {
            std::lock_guard<std::mutex> lock(g_mx);
            char line[96];
            snprintf(line, sizeof(line), "%s %d%%", stage, (int)(fraction * 100.0f));
            g_sp_status = line;
        }, never, count, why);
        std::lock_guard<std::mutex> lock(g_mx);
        if (r == SP_LATEST_ALREADY)
            g_sp_status = "You have the latest set-pieces already.";
        else if (r == SP_INSTALLED)
            g_sp_status = "The set pieces of " + rel->title + " are installed (" + std::to_string(count) + ").";
        else
            g_sp_status = "The set pieces could not be downloaded: " + why;
        g_sp_busy = false;
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
            ImGui::PushStyleColor(ImGuiCol_Text, (g_look == LOOK_CLASSIC) ? ImGui::GetColorU32(ImGuiCol_Text) : alpha_of(C_GOLD, 0.8f));
            ImGui::Bullet();
            ImGui::PopStyleColor();
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
            ImGui::Dummy(ImVec2(0, ((g_look == LOOK_CLASSIC) ? 2 : 6) * S));
            ImGui::PushStyleColor(ImGuiCol_Text, C_GOLD);
            ImGui::PushFont(head_font(), 0.0f); /* (a heading speaks in the book face, whatever the notes are read in) */
            ImGui::TextUnformatted(plain(line.substr(4)).c_str());
            ImGui::PopFont();
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
static bool g_resolutions_listed = false;

/** Asking Windows for every display's modes takes ~0.3 s: done while the window waits for the background's first
    scene, or (no background) when the settings first show them - never before the window (2026-11-27). */
static void list_resolutions()
{
    if (g_resolutions_listed)
        return;
    g_resolutions_listed = true;
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
        list_resolutions();
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
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, box_alpha(titled("Settings").c_str()));
    const bool open = ImGui::BeginPopupModal(titled("Settings").c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open)
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
            {
                /* the creator's set pieces (2026-11-25): off by default; when on they come with each update, and
                   Download fetches them now (for a player on the latest release who had them off) */
                cfg_check(s_cfg, "Download and use the creator's set pieces", "OFFICIAL_SET_PIECES", false);
                ImGui::SameLine();
                ImGui::BeginDisabled(g_sp_busy.load());
                if (ImGui::Button(titled("Download").c_str()))
                {
                    s_cfg.set("OFFICIAL_SET_PIECES", "TRUE");
                    start_setpieces_download();
                }
                ImGui::EndDisabled();
                std::string status;
                {
                    std::lock_guard<std::mutex> lock(g_mx);
                    status = g_sp_status;
                }
                if (!status.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, C_TEXT);
                    ImGui::TextWrapped("%s", T(status.c_str()));
                    ImGui::PopStyleColor();
                }
            }
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
            cfg_combo(s_cfg, "Rendering Backend", "RENDERER", "VULKAN", {{"VULKAN", "Vulkan"}, {"OPENGL", "OpenGL"}, {"SOFTWARE", "Software"}});
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
            cfg_combo(s_cfg, "Window style", "WINDOW_STYLE", "MODERN", {{"MODERN", "Modern"}, {"CLASSIC", "Classic"}});
            cfg_int(s_cfg, "UI scale", "UI_SCALE", 100, 50, 300, true, "%d%%");
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
            /* the looks (test branch 2026-11-23), and their motion: off = the still look */
            cfg_combo(s_lcfg, "Launcher look", "LAUNCHER_LOOK", "MOLTEN", {{"MOLTEN", "Molten"}, {"OBSIDIAN", "Obsidian"}, {"CLASSIC", "Classic"}});
            cfg_check(s_lcfg, "Animated effects", "LAUNCHER_EFFECTS", true);
            cfg_check(s_lcfg, "Heartbeat sound", "LAUNCHER_HEART_SOUND", true);
            ImGui::BeginDisabled(!s_lcfg.flag("LAUNCHER_HEART_SOUND", true));
            cfg_int(s_lcfg, "Heartbeat volume", "LAUNCHER_HEART_VOLUME", 15, 0, 100, true, "%d%%");
            ImGui::EndDisabled();
            heart_sound_from(s_lcfg); /* heard as it is set; Cancel puts the saved one back */
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
                const ImVec2 ts = head_font()->CalcTextSizeA(size, 4000.0f, 0.0f, text);
                ImGui::SetCursorPosX((aw - ts.x) * 0.5f);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                text_shadow(ImGui::GetWindowDrawList(), size, p, col, text);
                ImGui::Dummy(ts);
            };
            auto centred_link = [&](const char *id, const char *text, const char *url) {
                const float size = 17 * S;
                const ImVec2 ts = head_font()->CalcTextSizeA(size, 4000.0f, 0.0f, text);
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
                const ImVec2 ts = head_font()->CalcTextSizeA(size, 4000.0f, 0.0f, text);
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
        apply_look();
        ImGui::CloseCurrentPopup();
    }
    if (button("cancel", T("Cancel"), ImVec2(right - bs.x, base.y + 4 * S), bs, false, 18 * S))
    {
        heart_sound_from(g_lcfg); /* (the heartbeat as saved, not as the window had it) */
        ImGui::CloseCurrentPopup();
    }
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

/** A small tag beside a release's date ("new", "installed"): the new one is lit and beats. */
static void tag(const char *text, bool lit)
{
    ImFont *font = body_font();
    const float fs = 13.0f * S;
    const ImVec2 ts = font->CalcTextSizeA(fs, 4000.0f, 0.0f, text), a = ImGui::GetCursorScreenPos(), pad(8.0f * S, 2.5f * S);
    const ImVec2 b(a.x + ts.x + pad.x * 2, a.y + ts.y + pad.y * 2);
    const float r = (g_look == LOOK_OBSIDIAN) ? 1.0f * S : (b.y - a.y) * 0.5f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (lit)
    {
        fx_halo(dl, a, b, r, 10.0f * S, IM_COL32(255, 90, 30, (int)(50.0f + 100.0f * fx_heartbeat())));
        fx_rect_gradient(dl, a, b, IM_COL32(226, 70, 28, 255), IM_COL32(150, 26, 12, 255), r);
        dl->AddText(font, fs, ImVec2(a.x + pad.x, a.y + pad.y), IM_COL32(255, 244, 220, 255), text);
    } else
    {
        dl->AddRect(a, b, IM_COL32(222, 178, 88, 110), r, 0, 1.0f);
        dl->AddText(font, fs, ImVec2(a.x + pad.x, a.y + pad.y), C_GOLD, text);
    }
    ImGui::Dummy(ImVec2(b.x - a.x, b.y - a.y));
}

static void updates_box(ImVec2 pos, ImVec2 size)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const bool modern = (g_look != LOOK_CLASSIC);
    /* the entrance: the box comes in from the right, its text with it */
    const float in = intro(0.2f, 0.6f);
    const ImVec2 slide(modern ? (1.0f - in) * 28.0f * S : 0.0f, 0.0f);
    const int group = fx_group_begin(dl);
    panel(dl, pos, ImVec2(pos.x + size.x, pos.y + size.y));
    float head_h = 48 * S, side = 16 * S;
    if (g_look == LOOK_MOLTEN)
    {
        text_shadow(dl, 26 * S, ImVec2(pos.x + 24 * S, pos.y + 15 * S), C_GOLD_HI, T("Updates"));
        fx_line_fade(dl, ImVec2(pos.x + 16 * S, pos.y + 54 * S), size.x - 32 * S, IM_COL32(255, 214, 170, 74), std::max(1.0f, S));
        head_h = 56 * S;
        side = 24 * S;
    } else if (g_look == LOOK_OBSIDIAN)
    {
        const float line = std::max(1.5f, 2.0f * S);
        fx_text_spaced(dl, head_font(), 17 * S, ImVec2(pos.x + 24 * S, pos.y + 19 * S), C_GOLD_HI, upper(T("Updates")).c_str(), 0.24f * 17 * S);
        dl->AddRectFilled(ImVec2(pos.x + 24 * S, pos.y + 47 * S), ImVec2(pos.x + 70 * S, pos.y + 47 * S + line), C_GOLD_HI);
        dl->AddLine(ImVec2(pos.x + 78 * S, pos.y + 47 * S + line * 0.5f), ImVec2(pos.x + size.x - 24 * S, pos.y + 47 * S + line * 0.5f), IM_COL32(170, 132, 62, 90), 1.0f);
        head_h = 54 * S;
        side = 24 * S;
    } else
    {
        text_shadow(dl, 24 * S, ImVec2(pos.x + 20 * S, pos.y + 14 * S), C_GOLD, T("Updates"));
        dl->AddLine(ImVec2(pos.x + 16 * S, pos.y + 48 * S), ImVec2(pos.x + size.x - 16 * S, pos.y + 48 * S), C_EDGE, 1.0f);
    }
    fx_group_end(dl, group, modern ? in : 1.0f, slide);
    UpdateState up;
    std::vector<Release> releases;
    std::string why;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        up = g_up;
        releases = g_releases;
        why = g_up_why;
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x + side, pos.y + head_h + 8 * S));
    ImGui::BeginChild("updates", ImVec2(size.x - side - 12 * S, size.y - head_h - 20 * S), 0, 0);
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
        if ((i > 0) && modern)
        {
            ImGui::Dummy(ImVec2(0, 12 * S));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (g_look == LOOK_MOLTEN)
                fx_line_fade(ImGui::GetWindowDrawList(), at, ImGui::GetContentRegionAvail().x - 8 * S, IM_COL32(255, 214, 170, 48), std::max(1.0f, S));
            else
                ImGui::GetWindowDrawList()->AddLine(at, ImVec2(at.x + ImGui::GetContentRegionAvail().x - 8 * S, at.y), IM_COL32(170, 132, 62, 90), 1.0f);
            ImGui::Dummy(ImVec2(0, 10 * S));
        } else if (i > 0)
        {
            ImGui::Dummy(ImVec2(0, 6 * S));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 2 * S));
        }
        if (link("title", plain(r.title).c_str(), (i == 0) ? (modern ? 22 : 21) * S : 18 * S, (i == 0) ? C_GOLD_HI : C_GOLD))
            open_url(r.url.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Open this release's download page"));
        ImGui::PushStyleColor(ImGuiCol_Text, C_TEXT_DIM);
        const bool newer = version_newer(r.version, mine), same = !newer && !version_newer(mine, r.version);
        if (modern)
        {
            /* the date, and what this release is to the player as a tag */
            const float row = ImGui::GetCursorPosY() + 2 * S;
            ImGui::SetCursorPosY(row + 1.5f * S);
            ImGui::TextUnformatted(r.date.c_str());
            if (newer || same)
            {
                ImGui::SameLine(0.0f, 10 * S);
                ImGui::SetCursorPosY(row);
                tag(newer ? T("new") : T("installed"), newer);
            }
        } else
            ImGui::Text("%s%s%s", r.date.c_str(), (newer || same) ? "  -  " : "", newer ? T("new") : (same ? T("installed") : ""));
        ImGui::PopStyleColor();
        release_notes(r.body);
        ImGui::PopID();
    }
    if (modern && ((in < 0.999f) || (slide.x != 0)))
    {
        /* the box's text is another draw list: it fades and slides with the box */
        ImDrawList *child = ImGui::GetWindowDrawList();
        fx_group_end(child, 0, in, slide);
        for (ImDrawCmd &cmd : child->CmdBuffer)
        {
            cmd.ClipRect.x += slide.x;
            cmd.ClipRect.z += slide.x;
        }
    }
    ImGui::EndChild();
}

/** A box of the modern looks fades in when it opens: the alpha to give it this frame. */
static float box_alpha(const char *name)
{
    if (g_look == LOOK_CLASSIC)
        return 1.0f;
    return std::max(0.08f, fx_follow(ImGui::GetID(name) + 7, ImGui::IsPopupOpen(name) ? 1.0f : 0.0f, 0.22f));
}

/** The update's progress: in the modern looks a molten bar with light running along it. */
static void progress_bar(float fraction)
{
    if (g_look == LOOK_CLASSIC)
    {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.72f, 0.16f, 0.08f, 1.0f));
        ImGui::ProgressBar(fraction, ImVec2(-1, 22 * S), "");
        ImGui::PopStyleColor();
        return;
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x, height = 14 * S, r = (g_look == LOOK_OBSIDIAN) ? 1.0f * S : height * 0.5f;
    const ImVec2 a(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + 4 * S), b(a.x + width, a.y + height);
    const float x = a.x + std::max(height, width * std::max(0.0f, std::min(1.0f, fraction))), mid = (a.y + b.y) * 0.5f;
    dl->AddRectFilled(a, b, IM_COL32(255, 236, 214, 20), r);
    fx_halo(dl, a, ImVec2(x, b.y), r, 10 * S, IM_COL32(255, 80, 24, 70));
    fx_rect_gradient(dl, a, ImVec2(x, b.y), IM_COL32(150, 26, 12, 255), IM_COL32(255, 122, 40, 255), r, true);
    if (fx_motion)
    {
        dl->PushClipRect(ImVec2(a.x + r * 0.5f, a.y), ImVec2(x - r * 0.5f, b.y), true);
        fx_glow(dl, ImVec2(a.x - 50 * S + fmodf(fx_time() * 0.6f, 1.0f) * (x - a.x + 100 * S), mid), 50 * S, height * 1.7f, IM_COL32(255, 232, 176, 150));
        dl->PopClipRect();
    }
    fx_glow(dl, ImVec2(x - height * 0.5f, mid), 18 * S, 18 * S, IM_COL32(255, 170, 70, 150));
    ImGui::Dummy(ImVec2(width, height + 10 * S));
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
    if (t_fake_install)
    {
        /* the test's update box: a download that goes round and round, nothing fetched or installed */
        up = UP_INSTALLING;
        stage = "Downloading";
        fraction = fmodf(fx_time() * 0.12f, 1.0f);
        g_prompt_update = true;
    }
    if (t_check && ((up == UP_LISTED) || (up == UP_FAILED)) && !g_prompt_update)
    {
        t_check = false;
        check_now();
        return;
    }
    if (g_manual_check && ((up == UP_LISTED) || (up == UP_FAILED)))
    {
        /* "Check for Update" (2026-11-22): the prompt when there is a newer release, else a word that there is none */
        g_manual_check = false;
        if (up == UP_FAILED)
            g_message = std::string(T("The release list could not be loaded.")) + " (" + why + ")";
        else if (!have_newer)
            g_message = std::string(T("You have the newest version:")) + " " + GAME_TITLE;
    }
    if (g_prompt_update && !ImGui::IsPopupOpen(titled("Update").c_str()) && !ImGui::IsPopupOpen(titled("Settings").c_str()))
        ImGui::OpenPopup(titled("Update").c_str());
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(w - 40 * S, 520 * S), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, box_alpha(titled("Update").c_str()));
    const bool update_box = ImGui::BeginPopupModal(titled("Update").c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (update_box)
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
        if ((up == UP_INSTALLING) && (stage.rfind("Close:", 0) == 0))
        {
            /* programs run out of the game folder: their files can't be replaced, so the update waits for them */
            ImGui::PushStyleColor(ImGuiCol_Text, C_GOLD_HI);
            ImGui::TextUnformatted(T("Close these to update:"));
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, C_TEXT);
            for (size_t at = 6; at < stage.size();)
            {
                const size_t end = std::min(stage.find('|', at), stage.size());
                if (end > at)
                {
                    ImGui::Bullet();
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s", T(stage.substr(at, end - at).c_str()));
                }
                at = end + 1;
            }
            ImGui::Dummy(ImVec2(0, 4 * S));
            ImGui::TextWrapped("%s", T("The update goes on by itself as soon as they are closed."));
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 4 * S));
            if (ImGui::Button(titled("Cancel").c_str(), ImVec2(bw, 0)))
                g_cancel = true;
        } else if (up == UP_INSTALLING)
        {
            const std::string text = (stage == "Downloading") ? percent("Downloading: %p%") : (stage == "Unpacking") ? percent("Extracting: %p%")
                : (stage == "Installing") ? percent("Copying: %p%") : std::string(T("Updating..."));
            ImGui::TextUnformatted(text.c_str());
            progress_bar(fraction);
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
            ImGui::Dummy(ImVec2(0, 4 * S));
            ImGui::TextWrapped("%s", T("A new version is available. Do you want to update now?")); /* (the user, 2026-11-22) */
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
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, box_alpha("Endless Keeper"));
    const bool message_box = ImGui::BeginPopupModal("Endless Keeper", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (message_box)
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

/** The modern looks' background, over the battle and behind everything else: the picture sits back in soft darks
    (behind the title, and in the corners the buttons and the last lines stand in), and the look's own light and
    particles go over it. None of it is ever in front of a letter. */
static void backdrop(ImDrawList *bg, float w, float h, bool scene)
{
    const float beat = fx_heartbeat(), t = fx_motion ? fx_time() : 0.0f;
    if (g_look == LOOK_MOLTEN)
    {
        if (scene)
            bg->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(10, 4, 3, 44));
        fx_blot(bg, ImVec2(w * 0.5f, -h * 0.04f), w * 0.60f, h * 0.56f, IM_COL32(0, 0, 0, 215));
        fx_blot(bg, ImVec2(0, h), w * 0.50f, h * 0.66f, IM_COL32(0, 0, 0, 185));
        fx_blot(bg, ImVec2(w, h), w * 0.40f, h * 0.50f, IM_COL32(0, 0, 0, 130));
        /* light from below, as off lava: it breathes with the heart; the embers rise out of it */
        fx_light_gradient(bg, ImVec2(0, h * 0.66f), ImVec2(w, h), IM_COL32(255, 72, 16, 0), IM_COL32(255, 72, 16, (int)(46.0f + 18.0f * beat)));
        fx_particles(bg, FX_EMBERS, w, h, S);
        fx_backdrop_capture(bg);
    } else
    {
        if (scene)
            bg->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(6, 6, 14, 78));
        fx_blot(bg, ImVec2(w * 0.5f, -h * 0.04f), w * 0.60f, h * 0.56f, IM_COL32(0, 0, 0, 225));
        fx_blot(bg, ImVec2(0, h), w * 0.50f, h * 0.66f, IM_COL32(0, 0, 0, 200));
        fx_blot(bg, ImVec2(w, h), w * 0.40f, h * 0.50f, IM_COL32(0, 0, 0, 150));
        /* cold mist along the bottom, and gold dust in the air */
        for (int k = 0; k < 2; k++)
            fx_glow(bg, ImVec2(w * (0.28f + 0.5f * k) + sinf(t * 0.08f + k * 2.0f) * w * 0.14f, h * 1.04f), w * 0.46f, h * 0.34f, IM_COL32(84, 96, 170, 30));
        fx_particles(bg, FX_DUST, w, h, S);
    }
}

static void frame(float w, float h)
{
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    const bool modern = (g_look != LOOK_CLASSIC);
    const ImU32 sky = (g_look == LOOK_OBSIDIAN) ? IM_COL32(18, 16, 28, 255) : IM_COL32(36, 12, 8, 255);
    bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h), sky, sky, IM_COL32(8, 6, 5, 255), IM_COL32(8, 6, 5, 255));
    bool scene = false;
    if (g_battle && t_bare)
        engine_view_draw(bg, w, h);
    else if (g_battle && engine_view_draw(bg, w, h))
    {
        scene = true;
        if (!modern)
        {
            /* the scene sits back: darker at the top (the title) and the edges */
            bg->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(6, 4, 3, 40));
            bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h * 0.38f), IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
            bg->AddRectFilledMultiColor(ImVec2(0, h * 0.8f), ImVec2(w, h), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150));
        }
    }
    if (t_bare)
        return;
    if (modern)
        backdrop(bg, w, h, scene);
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
        if (!modern)
        {
            dl->AddImage((ImTextureID)(intptr_t)g_header, ImVec2(a.x + 3 * S, a.y + 5 * S), ImVec2(a.x + hw + 3 * S, a.y + hh + 5 * S),
                ImVec2(0, 0), ImVec2(1, 1), IM_COL32(0, 0, 0, 150));
            dl->AddImage((ImTextureID)(intptr_t)g_header, a, ImVec2(a.x + hw, a.y + hh));
        } else
        {
            /* the header comes down into place; the head has a light of its own behind it, its eyes glow with the
               heart's beat, and every few seconds light passes over the title (positions in the picture's own
               pixels: 850 wide, the eyes at 140,110 and 199,111) */
            const float in = intro(0.0f, 0.65f), k = hw / 850.0f, beat = fx_heartbeat();
            const int group = fx_group_begin(dl);
            if (g_look == LOOK_MOLTEN)
                fx_glow(dl, ImVec2(a.x + 170 * k, a.y + 150 * k), 270 * k, 215 * k, IM_COL32(255, 60, 20, (int)(50.0f + 34.0f * beat)));
            dl->AddImage((ImTextureID)(intptr_t)g_header, ImVec2(a.x, a.y + 6 * S), ImVec2(a.x + hw, a.y + hh + 6 * S), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(0, 0, 0, 150));
            dl->AddImage((ImTextureID)(intptr_t)g_header, a, ImVec2(a.x + hw, a.y + hh));
            fx_glow(dl, ImVec2(a.x + 140 * k, a.y + 110 * k), 17 * k, 17 * k, IM_COL32(255, 200, 70, (int)(90.0f + 130.0f * beat)));
            fx_glow(dl, ImVec2(a.x + 199 * k, a.y + 111 * k), 19 * k, 19 * k, IM_COL32(255, 200, 70, (int)(90.0f + 130.0f * beat)));
            const float cycle = fmodf(fx_time(), 8.0f);
            if (fx_motion && (g_header_light != nullptr) && (cycle < 1.4f))
            {
                const int strips = 56;
                const float at = 0.18f + (cycle / 1.4f) * 0.98f; /* from the head's edge to past the title's end */
                for (int i = 0; i < strips; i++)
                {
                    const float u0 = (float)i / strips, u1 = (float)(i + 1) / strips, d = ((u0 + u1) * 0.5f - at) / 0.06f, light = expf(-d * d) * 0.6f;
                    if (light > 0.02f)
                        dl->AddImage((ImTextureID)(intptr_t)g_header_light, ImVec2(a.x + u0 * hw, a.y), ImVec2(a.x + u1 * hw, a.y + hh), ImVec2(u0, 0), ImVec2(u1, 1),
                            IM_COL32(255, 236, 190, (int)(255.0f * light)));
                }
            }
            fx_group_end(dl, group, in, ImVec2(0, -(1.0f - in) * 18 * S));
        }
        top = a.y + hh + pad * 0.6f;
    }
    /* the modern looks' entrance: a piece rises into place and fades in, `delay` seconds after the window came
       (only what is drawn moves: a button can be pressed where it will be from the first frame) */
    auto rise = [&](int group, float delay) {
        const float in = intro(delay, 0.5f);
        fx_group_end(dl, group, in, ImVec2(0, (1.0f - in) * 16 * S));
    };
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
        const float third = (col_w - gap * 2) / 3.0f;
        int group = fx_group_begin(dl);
        if (button("play", T("Play"), ImVec2(pad, top), ImVec2(third, set_h * 1.2f), true, 26 * S, !busy))
            play();
        rise(group, 0.12f);
        group = fx_group_begin(dl);
        if (button("settings", T("Settings"), ImVec2(pad + third + gap, top), ImVec2(third, set_h * 1.2f), false, 20 * S, !busy))
            g_show_settings = true;
        rise(group, 0.2f);
        group = fx_group_begin(dl);
        if (button("check", T("Check for Update"), ImVec2(pad + (third + gap) * 2, top), ImVec2(third, set_h * 1.2f), false, 17 * S,
            !busy && (up != UP_CHECKING) && !opt_no_update))
            check_now();
        rise(group, 0.28f);
        box_pos = ImVec2(pad, top + set_h * 1.2f + gap);
        box_size = ImVec2(w - pad * 2, bottom - box_pos.y);
        engine_view_focus(w * 0.5f, h * 0.5f);
    } else
    {
        const float by = std::max(top, bottom - play_h - gap - set_h);
        int group = fx_group_begin(dl);
        if (button("play", T("Play"), ImVec2(pad, by), ImVec2(col_w, play_h), true, 40 * S, !busy))
            play();
        rise(group, 0.12f);
        /* Settings, and beside it Check for Update (2026-11-22) */
        const float half = (col_w - gap) * 0.5f;
        group = fx_group_begin(dl);
        if (button("settings", T("Settings"), ImVec2(pad, by + play_h + gap), ImVec2(half, set_h), false, 22 * S, !busy))
            g_show_settings = true;
        rise(group, 0.2f);
        group = fx_group_begin(dl);
        if (button("check", T("Check for Update"), ImVec2(pad + half + gap, by + play_h + gap), ImVec2(half, set_h), false, 18 * S,
            !busy && (up != UP_CHECKING) && !opt_no_update))
            check_now();
        rise(group, 0.28f);
        /* the box keeps to the right: the battle shows between it and the buttons */
        const float box_w = std::min(w - (pad + col_w + pad) - pad, std::max(430 * S, w * 0.46f));
        box_pos = ImVec2(w - pad - box_w, top);
        box_size = ImVec2(box_w, bottom - top);
        engine_view_focus(box_pos.x * 0.5f, h * 0.5f);

    }
    if ((box_size.x > 120 * S) && (box_size.y > 90 * S))
        updates_box(box_pos, box_size);
    fx_sparks_draw(dl);
    /* the bottom line: what the battle is, and the version */
    if (!modern)
    {
        const float fs = 15 * S;
        text_shadow(dl, fs, ImVec2(pad, h - pad * 0.5f - fs), IM_COL32(200, 186, 160, 200), engine_view_caption());
        const ImVec2 vs = head_font()->CalcTextSizeA(fs, 4000.0f, 0.0f, GAME_TITLE);
        text_shadow(dl, fs, ImVec2(w - pad - vs.x, h - pad * 0.5f - fs), IM_COL32(200, 186, 160, 220), GAME_TITLE);
    } else
    {
        /* quieter, in the face that is read; a small light before the battle's line says the picture is live */
        const float fs = 14 * S, y = h - pad * 0.5f - fs;
        const int group = fx_group_begin(dl);
        const char *caption = engine_view_caption();
        float x = pad;
        if (g_battle && (caption != nullptr) && (caption[0] != 0))
        {
            const ImVec2 c(pad + 5 * S, y + fs * 0.56f);
            fx_glow(dl, c, 10 * S, 10 * S, IM_COL32(255, 96, 32, (int)(110.0f + 120.0f * fx_heartbeat())));
            dl->AddCircleFilled(c, 2.6f * S, IM_COL32(255, 150, 70, 255));
            x += 17 * S;
        }
        text_shadow(dl, fs, ImVec2(x, y), IM_COL32(214, 202, 182, 205), caption, body_font());
        const ImVec2 vs = body_font()->CalcTextSizeA(fs, 4000.0f, 0.0f, GAME_TITLE);
        text_shadow(dl, fs, ImVec2(w - pad - vs.x, y), IM_COL32(214, 202, 182, 190), GAME_TITLE, body_font());
        fx_group_end(dl, group, intro(0.45f, 0.6f), ImVec2(0, 0));
    }
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
    /* the same picture, adding light: the sheen that passes over the title in the modern looks */
    g_header_light = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, g_header_w, g_header_h);
    if (g_header_light != nullptr)
    {
        SDL_UpdateTexture(g_header_light, nullptr, px, g_header_w * 4);
        SDL_SetTextureBlendMode(g_header_light, SDL_BLENDMODE_ADD);
        SDL_SetTextureScaleMode(g_header_light, SDL_SCALEMODE_LINEAR);
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
    start_mark("main");
    DeleteFileW((g_self_exe + L".old").c_str()); /* the launcher an update replaced */
    migrate_program_names(g_game_dir); /* a folder an old launcher updated: shortcuts to ek-launcher.exe, the bridge gone */
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
    start_mark("config and language");
    if (opt_play)
    {
        const std::wstring exe = game_exe();
        std::wstring cmd = L"\"" + exe + L"\"" + widen(game_arguments());
        SetEnvironmentVariableW(L"EK_FROM_LAUNCHER", L"1"); /* (see above: no hand-back) */
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
    engine_view_set_dir(g_game_dir);
    /* the background's game first: its load is most of the wait for the window, and needs nothing of the launcher's */
    const bool battle_started = !opt_no_battle && engine_view_start(game_exe(), g_game_dir);
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        engine_view_shutdown();
        MessageBoxA(nullptr, SDL_GetError(), "Endless Keeper", MB_ICONERROR);
        return 1;
    }
    start_mark("SDL");
    const float display_scale = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
    g_window = SDL_CreateWindow("Endless Keeper", (int)(1180 * display_scale), (int)(720 * display_scale),
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    g_renderer = (g_window != nullptr) ? SDL_CreateRenderer(g_window, nullptr) : nullptr;
    if (g_renderer == nullptr)
    {
        engine_view_shutdown();
        MessageBoxA(nullptr, SDL_GetError(), "Endless Keeper", MB_ICONERROR);
        return 1;
    }
    start_mark("window and renderer");
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
        /* a font file's bytes, read once and kept for as long as the launcher runs: the two faces share the fonts
           that fill their gaps, so those big files are not read twice */
        auto bytes = [&](const wchar_t *name, int *size) -> void * {
            FILE *f = _wfopen((std::wstring(win) + name).c_str(), L"rb");
            if (f == nullptr)
                return nullptr;
            fseek(f, 0, SEEK_END);
            const long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            void *data = (n > 0) ? malloc((size_t)n) : nullptr;
            if ((data != nullptr) && (fread(data, 1, (size_t)n, f) != (size_t)n))
            {
                free(data);
                data = nullptr;
            }
            fclose(f);
            *size = (int)n;
            return data;
        };
        /* Windows' fonts for the letters a face lacks (Japanese, Korean, Chinese ...) */
        std::vector<std::pair<void *, int>> gaps;
        for (const wchar_t *name : {L"\\Fonts\\segoeui.ttf", L"\\Fonts\\malgunbd.ttf", L"\\Fonts\\malgun.ttf", L"\\Fonts\\YuGothB.ttc", L"\\Fonts\\meiryob.ttc",
                 L"\\Fonts\\msyhbd.ttc", L"\\Fonts\\msyh.ttc"})
        {
            int size = 0;
            void *data = bytes(name, &size);
            if (data != nullptr)
                gaps.push_back({data, size});
        }
        auto face = [&](std::initializer_list<const wchar_t *> names) -> ImFont * {
            ImFont *font = nullptr;
            for (const wchar_t *name : names)
            {
                int size = 0;
                void *data = bytes(name, &size);
                if (data == nullptr)
                    continue;
                ImFontConfig own;
                own.FontDataOwnedByAtlas = false;
                font = io.Fonts->AddFontFromMemoryTTF(data, size, 17.0f, &own);
                if (font != nullptr)
                    break;
                free(data);
            }
            if (font == nullptr)
                font = io.Fonts->AddFontDefault();
            for (const auto &gap : gaps)
            {
                ImFontConfig merge;
                merge.MergeMode = true;
                merge.FontDataOwnedByAtlas = false;
                io.Fonts->AddFontFromMemoryTTF(gap.first, gap.second, 17.0f, &merge);
            }
            return font;
        };
        /* the book face speaks (titles, buttons; all of the Classic and Obsidian looks); the plain face is read (the
           Molten look's notes and settings) */
        g_font_head = face({L"\\Fonts\\palab.ttf", L"\\Fonts\\georgiab.ttf", L"\\Fonts\\segoeuib.ttf", L"\\Fonts\\arialbd.ttf"});
        g_font_body = face({L"\\Fonts\\segoeui.ttf", L"\\Fonts\\arial.ttf"});
    }
    start_mark("fonts");
    ImGui_ImplSDL3_InitForSDLRenderer(g_window, g_renderer);
    ImGui_ImplSDLRenderer3_Init(g_renderer);
    g_header = load_header();
    fx_init(g_renderer);
    start_mark("header");
    g_battle = battle_started;
    if (g_battle)
        engine_view_attach(g_renderer);
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
                else if (key == "hidden") t_hidden = true;
                else if (key == "play") t_play = (float)atof(val.c_str());
                else if (key == "burst") t_burst = atoi(val.c_str());
                else if (key == "version") t_version = val;
                else if (key == "lang") load_language(val);
                else if (key == "update") t_update = true;
                else if (key == "check") t_check = true;
                else if (key == "size") sscanf(val.c_str(), "%dx%d", &t_w, &t_h);
                else if (key == "look") t_look = atoi(val.c_str());
                else if (key == "fx") t_still = (atoi(val.c_str()) == 0);
                else if (key == "hover") t_hover = val;
                else if (key == "fakeinstall") t_fake_install = true;
                else if (key == "beatlog") heart_sound_test_log(atoi(val.c_str()) != 0);
            }
            at = end + 1;
        }
        if ((t_w > 0) && (t_h > 0))
            SDL_SetWindowSize(g_window, t_w, t_h);
    }
    apply_look();
    if (!opt_no_update)
        check_updates();
    else
    {
        std::lock_guard<std::mutex> lock(g_mx);
        g_up = UP_FAILED;
    }
    start_mark("update check started");
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
            /* the game is on: the launcher waits, hidden and silent, and comes back when it has closed */
            heart_sound_frame(false, 0);
            if (WaitForSingleObject(g_game_process, 200) == WAIT_OBJECT_0)
            {
                CloseHandle(g_game_process);
                g_game_process = nullptr;
                engine_view_note("Play: the game has closed; the launcher is back");
                g_cfg.load(g_cfg.path);
                apply_look(); /* (the sound-effect volume may have been changed in the game) */
                engine_view_resume();
                g_intro = 0; /* the window comes back as it first came */
                if (!t_hidden)
                {
                    SDL_ShowWindow(g_window);
                    SDL_RaiseWindow(g_window);
                }
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
                start_mark(engine_view_ready() ? "background ready" : "no background yet");
                if (!t_hidden)
                {
                    SDL_ShowWindow(g_window);
                    SDL_RaiseWindow(g_window);
                }
                start_mark("window shown");
                engine_view_note(start_timing_line().c_str());
                last = SDL_GetPerformanceCounter();
                g_intro = 0;
            } else
            {
                if (!g_resolutions_listed)
                {
                    list_resolutions(); /* (the wait for the background's first scene is longer: it costs nothing here) */
                    start_mark("resolutions");
                }
                SDL_Delay(15);
                continue;
            }
        }
        if ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) != 0)
        {
            heart_sound_frame(false, 0);
            SDL_Delay(100);
            last = SDL_GetPerformanceCounter();
            continue;
        }
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
        const float scale = std::max(0.7f, std::min(2.6f, std::min((float)ph / 720.0f, (float)pw / 1000.0f)));
        if ((fabsf(scale - last_scale) > 0.01f) || g_restyle)
        {
            g_restyle = false;
            last_scale = S = scale;
            apply_style();
            ImGui::GetStyle().FontSizeBase = ((g_look == LOOK_MOLTEN) ? 16.0f : 17.0f) * S; /* (the plain face is larger for its size) */
        }
        const Uint64 now = SDL_GetPerformanceCounter();
        const float dt = (float)((double)(now - last) / (double)SDL_GetPerformanceFrequency());
        last = now;
        fx_frame(dt);
        heart_sound_frame(!g_mute_unfocused || ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0), dt);
        g_intro += std::min(dt, 0.05f); /* (a slow first frame doesn't eat the entrance) */
        if (g_look == LOOK_MOLTEN)
            fx_target_begin(pw, ph); /* its glass needs the frame as a picture */
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
        fx_target_end();
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
        static int frames_shown = 0;
        if (++frames_shown == 2)
        {
            /* the sound device once the window has its first pictures: its start (~tens of ms) never holds the window */
            const Uint64 at = SDL_GetPerformanceCounter();
            heart_sound_start(g_game_dir);
            char line[96];
            snprintf(line, sizeof(line), "Launcher: the sound device started in %.1f ms",
                (double)(SDL_GetPerformanceCounter() - at) * 1000.0 / (double)SDL_GetPerformanceFrequency());
            engine_view_note(line);
        }
    }
    g_cancel = true;
    heart_sound_shutdown();
    engine_view_shutdown();
    fx_shutdown();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(g_renderer);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
