// Endless Keeper launcher: the background - the game itself, running hidden and handing its frames over.
#pragma once
#include <string>
struct SDL_Renderer;
struct ImDrawList;

/** Starts the game hidden as the launcher's background, first thing (it needs no window: its load is most of the wait
    for the launcher's window). False when it can't (the launcher draws a plain one). */
bool engine_view_start(const std::wstring &game_exe, const std::wstring &game_dir);
/** The launcher's renderer is there: the background's pictures can be shown. */
void engine_view_attach(SDL_Renderer *renderer);
void engine_view_shutdown();
/** Takes the game's newest frame; starts the hidden game again when it has ended. */
void engine_view_update(float dt);
/** Where in the window the picture's middle (the fighting) should be: the part the launcher's boxes leave open. */
void engine_view_focus(float x, float y);
/** Draws the game's picture over the whole window. False while there is none yet. */
bool engine_view_draw(ImDrawList *dl, float width, float height);
/** The hidden game stops (the real one is about to run) / runs again. */
void engine_view_pause();
void engine_view_resume();
/** The hidden game has loaded and its first scene is on (the launcher's window waits for this, for a while). */
bool engine_view_ready();
/** A step of the launcher's start: milliseconds since its process began; start_timing_line() lists them all. */
void start_mark(const char *what);
std::string start_timing_line();
/** A line in endlesskeeper-launcher.log, beside the game. */
void engine_view_note(const char *text);
/** The game's folder for that log, also when no background runs (--no-battle: its lines were lost, 2026-11-27). */
void engine_view_set_dir(const std::wstring &game_dir);
/** What is going on in the picture, in a line. */
const char *engine_view_caption();
