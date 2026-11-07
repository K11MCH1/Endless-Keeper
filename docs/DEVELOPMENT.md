# 🛠️ Development Guide

This guide takes you from a clean Windows PC to your own build of Endless Keeper running in the game.

Endless Keeper is KeeperFX with new systems added, so building it is building KeeperFX from source. The game is a
**32-bit Windows program**, cross-compiled on Linux with **MinGW-w64**. On Windows, the easiest way to get Linux is
**WSL** (Windows Subsystem for Linux), which this guide uses. Native Linux works the same way: skip step 1.

| | What you need |
|:-:|---|
| 🪟 | Windows 10 (version 2004 or later) or Windows 11 |
| 💽 | About 3 GB of free disk space |
| 🌐 | An internet connection (the build downloads its libraries the first time) |
| 🎮 | KeeperFX 1.4.0 Alpha (build 5593) installed with your Dungeon Keeper files, to run what you build |

---

## 1. Install WSL with Ubuntu

Open **PowerShell as Administrator** and run:

```powershell
wsl --install -d Ubuntu-24.04
```

Restart the PC if asked, then open **Ubuntu** from the Start menu and choose a user name and password.

> 💡 Everything from here on is typed in the Ubuntu window.

## 2. Install the build tools

```bash
sudo apt update
sudo apt install -y git cmake ninja-build g++-mingw-w64-i686 gcc-mingw-w64-i686
```

This installs Git, CMake, Ninja and the MinGW-w64 compilers for 32-bit Windows (`i686-w64-mingw32-gcc` and `-g++`).

## 3. Get the source

```bash
mkdir -p ~/src && cd ~/src
git clone https://github.com/K11MCH1/Endless-Keeper.git
cd Endless-Keeper
```

> 💡 Keep the source inside the Linux file system (`~/src`), not under `/mnt/c`: building is many times faster there.

## 4. Configure

```bash
cmake -S . -B out -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=build/cmake/toolchains/mingw32.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_NUMBER=$(cat installer/keeperfx-build.txt) -DPACKAGE_SUFFIX=Alpha
```

The first run downloads the libraries KeeperFX uses (SDL3, LuaJIT, ENet, zlib and others) into `out/deps`. You only
configure once; after that, step 5 is all you need.

| Option | Meaning |
|---|---|
| `CMAKE_TOOLCHAIN_FILE` | Use the MinGW-w64 cross compiler for 32-bit Windows. |
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` (fast, with debug symbols) or `Debug`. |
| `BUILD_NUMBER`, `PACKAGE_SUFFIX` | The version the game reports, e.g. 1.4.0.5593 Alpha: the KeeperFX build Endless Keeper is built on (`installer/keeperfx-build.txt`). |

## 5. Build

```bash
cmake --build out --target keeperfx
```

The first build takes a few minutes. When it is done you have **`out/keeperfx.exe`**. Later builds only recompile
what changed.

## 6. Install your build into the game

Back up your original `keeperfx.exe` once, then copy the files over. With KeeperFX installed in `C:\Games\KeeperFX`
(change the path to yours), from the `Endless-Keeper` folder:

```bash
GAME=/mnt/c/Games/KeeperFX
cp -n "$GAME/keeperfx.exe" "$GAME/keeperfx.exe.original"   # once: a backup of the original
cp out/keeperfx.exe "$GAME/"
cp -r levels/endless.cfg levels/endless levels/endless_cfgs "$GAME/levels/"
cp -r config/fxdata/lua/. "$GAME/fxdata/lua/"
```

| From the repository | To the game folder | What it is |
|---|---|---|
| `out/keeperfx.exe` | `keeperfx.exe` | The game itself. |
| `levels/endless.cfg`, `levels/endless/`, `levels/endless_cfgs/` | `levels/` | The Endless Mode map pack: its level stub and its config changes. |
| `config/fxdata/lua/` | `fxdata/lua/` | The game's Lua scripts (they must match the exe). |

To go back to plain KeeperFX, copy `keeperfx.exe.original` back over `keeperfx.exe`.

## 7. Run and test

Start `keeperfx.exe` and choose **Endless Mode** in the main menu. Some command lines are handy while testing
(run them from the game folder in a Windows command prompt):

| Command | What it does |
|---|---|
| `keeperfx.exe -campaign levels/endless -level 1` | Starts an endless world straight away, skipping the menus. |
| `keeperfx.exe -server 5556 -campaign levels/endless -level 1 -waitusers 2 -nick P1` | Hosts a network world and waits for a second player. |
| `keeperfx.exe -connect 127.0.0.1:5556 -nick P2` | Joins a network world by address. |
| `keeperfx.exe -nointro -nosound -dedicated 5556 -loadseed 12345` | Runs a dedicated server (no player of its own) on port 5556: the newest autosave of world `12345`, or a new world with that seed. |

Add `-nointro -nosound` to start faster. The game writes everything it does to **`keeperfx.log`** in the game
folder. That is the first place to look when something goes wrong, and the file to attach to bug reports.

## 8. Build the setup (optional)

The setup is what players download. Like KeeperFX's own setup, it downloads the KeeperFX base game during
installation, then lays the Endless Keeper package over it and copies the original game files from the player's
Dungeon Keeper.

**The package** (in Ubuntu, from the `Endless-Keeper` folder): the game, its DLLs and its data, made the way KeeperFX
makes its alpha patches, plus the Unicode fonts and the Endless Mode stub level.

```bash
sudo apt install -y build-essential python3 libpng16-16t64
installer/make-package.sh pkg/endless-keeper.7z
```

**The setup** (on Windows): install [Inno Setup](https://jrsoftware.org/isinfo.php) 6.5 or newer
(`winget install JRSoftware.InnoSetup`), unpack `pkg/endless-keeper.7z` into an empty folder (with 7-Zip, for
example), then compile, giving that folder:

```powershell
& "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" /DAppVersion=0.1.0 /DPackageDir=C:\path\to\unpacked installer\endless-keeper.iss
```

This makes `EndlessKeeperSetup-0.1.0.exe`. The KeeperFX release it downloads (its address, checksum and size) is set
at the top of `installer/endless-keeper.iss`. To test without clicking through the wizard:
`EndlessKeeperSetup-0.1.0.exe /VERYSILENT /DIR="C:\Games\EndlessKeeper-test" /DKDIR="<your Dungeon Keeper folder>"`.

---

## 🗂️ Where things are

| Path | What is there |
|---|---|
| `src/world_stream.c` | The endless world: chunks loading and freezing, saving the world, the endless map. |
| `src/world_pages.c` | Chunk pages: the world's map cells beyond the classic 85×85 map. |
| `src/world_gen.c` | The seeded world generator: biomes, liquids, gold, portals, caves, fortresses, keepers. |
| `src/world_progress.c` | Research-book progression and player levels. |
| `src/world_daynight.c` | Day and night, and the hero raids by day. |
| `src/world_wild.c` | Wild creatures by night. |
| `src/world_outpost.c` | Heroes outposts. |
| `src/world_menu.c`, `src/world_net.c` | The Endless Mode menu, the World List and its chat. |
| `src/net_state.c` | Server-run multiplayer: the state stream from the server to the players. |
| `src/server_admin.c` | The dedicated server's admin connection (local only). |
| `levels/endless*` | The Endless Mode map pack. |
| `installer/` | The setup: `endless-keeper.iss` (Inno Setup), `make-package.sh`, the KeeperFX build number. |
| everything else | KeeperFX. Its own documentation is in [`docs/`](.). |

All the endless systems switch on only when a level asks for them (`ENDLESS_WORLD` in the level script), so the
original campaign and other levels play exactly as in KeeperFX.

## ✅ Before you send a change

- Build with no warnings: the build treats many warnings as errors.
- Play a few minutes of an endless world, save, and load the save again.
- Play an original campaign level for a while: it must play exactly as before.
- If you changed multiplayer, host a world and join it from a second copy of the game.

## ❓ Troubleshooting

| Problem | Fix |
|---|---|
| `i686-w64-mingw32-gcc: not found` | Step 2 did not finish: run the `apt install` line again. |
| CMake can't download the libraries | Check your internet connection, delete the `out` folder and configure again. |
| The game says the version doesn't match, or crashes at start | Your KeeperFX game data is a different version: Endless Keeper is built on 1.4.0.5593. |
| The game starts but there is no Endless Mode | `keeperfx.exe` was not replaced (the KeeperFX launcher may have updated it): copy it again. |
| The build is very slow | Move the source from `/mnt/c/...` into `~/src`. |

Questions? Ask **KIMCHI** on the [Keeper Klan Discord](https://discord.com/invite/hE4p7vy2Hb), or open an
[issue](https://github.com/K11MCH1/Endless-Keeper/issues).
