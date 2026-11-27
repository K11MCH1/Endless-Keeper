// Endless Keeper launcher: the Soul Container's beat heard in time with the glows that beat (fx_heartbeat).
#pragma once
#include <string>

/** Reads the game's own two heart samples (sound.dat 150 and 151, which the engine plays by turns for the Dungeon
    Heart) and opens the sound device, on a thread of its own. Call once the window is shown: the start of the
    sound device (on this thread, as SDL asks) costs nothing before the window. */
void heart_sound_start(const std::wstring &game_dir);
/** The player's settings: `on` (the launcher's switch and the game's sound both on) and the volume (0 .. 1: the game's
    sound-effect volume times the launcher's heartbeat volume). A beat that is sounding takes it at once. */
void heart_sound_settings(bool on, float volume);
/** Once a frame, after fx_frame(dt): a beat is queued so that its thump comes with the glow's. `audible` false (the
    window hidden, or not in front with "mute when not focused") keeps it silent. */
void heart_sound_frame(bool audible, float dt);
void heart_sound_shutdown();
/** Test key "beatlog=1" (EKL_TEST): every queued beat is a line in the launcher's log, played silently. */
void heart_sound_test_log(bool on);
/** The game's sound-effect volume from its save\settings.toml (0 .. 1; 1 when there is none). */
float heart_sound_game_volume(const std::wstring &game_dir);
