// Endless Keeper launcher: the programs' new names (migrate.cpp): ek-launcher.exe (the launcher) and EndlessKeeper.exe
// (the game) since 2026-11-28.
#pragma once
#include <string>

/** A folder an old launcher updated (the bridge keeperfx.exe is there): the shortcuts that start EndlessKeeper.exe
    start ek-launcher.exe, and the bridge and the old program files go. Nothing in any other folder. */
void migrate_program_names(const std::wstring &game_dir);
/** The bridge's files in an update (keeperfx.exe, x64\keeperfx.exe): only old launchers need them, they are not installed. */
bool migrate_bridge_file(const std::wstring &rel_path);
