# SonsOfSparta-PS5 Native on Linux

The Linux build runs the game's own x86-64 code directly on your CPU, the same way the Windows build does. The PS5 system libraries are the same reimplementation, built as Linux shared libraries, and graphics go through Vulkan.

## Requirements
- 64-bit Linux with glibc and a recent `libstdc++` (any current distribution)
- A Vulkan 1.3 driver (Mesa RADV/ANV or the NVIDIA driver)
- GTK 3 for the launcher window (installed on almost every desktop)
- An X11 session, or XWayland on a Wayland desktop. The launcher sets `SDL_VIDEODRIVER=x11` unless you set it yourself.
- About 25 GB of free space for the game files you copy in

## How to play
1. Extract `SonsOfSparta-PS5-Native-<version>-linux-x86_64.tar.gz` anywhere.
2. Run `./SonsOfSparta-PS5` (from a terminal or your file manager).
3. In the launcher window, under **Game**, choose the folder of **your own** decrypted PS5 dump: the folder that contains `eboot.bin` (or a decrypted `eboot.elf`), `sce_sys`, `sce_module` and `Media`. The launcher checks the folder right away, and Play is available once it is valid. You can also copy those files next to `SonsOfSparta-PS5`, like on Windows; that folder is then used automatically.
4. Check the other settings and press **Play**.

The first start prepares the game from your files, with a small window that shows the progress. It unpacks the executable and modules into `prep/`, builds `sos_runtime` with `tools/relinker`, and links `Media` and `sce_sys` from your game folder into `app0/`. This takes about two minutes. Later starts are fast. Your own files are never modified, and choosing another game folder prepares the game again.

Logs are in the `logs` folder: `prepare.log` for the first start, `game.log` and `game.err` for the game.

## Settings
The launcher window has the same settings as on Windows: game folder, display mode, resolution, frame rate, controller button icons, show launcher at startup, and show in-game FPS. Press **Play** to save them. They are kept in `launcher.ini`, using the same sections and keys as the Windows launcher, and the resolution is kept in the game's own settings save. A `launcher.ini` from Windows works unchanged; the game folder is stored as `[game] folder=...`.

Turning off **Show launcher at startup** starts the game directly with the saved settings. To see the window again, start with `--settings` or set `show=1` under `[launcher]` in `launcher.ini`. The window always opens when the game folder is missing or not valid.

Command-line options:

| Option | Effect |
|---|---|
| `--settings` | Show the launcher window even if it is turned off |
| `--play` | Start directly with the saved settings |
| `--game-folder <folder>` | Use this game folder (it is remembered) |
| `--borderless` / `--windowed` | Display mode for this start |
| `--fps 60\|120\|unlocked` | Frame rate for this start |
| `--show-fps` | Draw the frame rate in the game (F9 toggles) |
| `--prepare` | Only prepare the game files, then exit |
| `--reprepare` | Rebuild the prepared files even if they are current |

Extra environment variables for troubleshooting can go in `debug_env.txt`, one `NAME=value` per line.

Controls are the same as on Windows (see the main README). F11 toggles fullscreen. Holding Triangle emulates blowing into the controller microphone.

## AppImage and Steam Deck

Every commit builds two AppImages on the Releases page: `Linux` and `SteamDeck`. The game is not included. Put your own decrypted dump in a folder named `game` next to the AppImage (the folder with `eboot.bin`, `sce_sys`, `sce_module` and `Media`), make the AppImage executable and start it; or choose the folder in the window that opens. Program files, saves and settings go in `~/.local/share/SonsOfSparta-PS5`.

On a Steam Deck, in Desktop mode: start the `SteamDeck` AppImage once and let it prepare the game, then add it to Steam with **Add a Non-Steam Game**. It starts fullscreen at 1920x1080, the smallest size the game offers, at 60 FPS.

## Controllers and the in-game menu

Button icons follow the controller in use, with Xbox, PlayStation and Nintendo Switch sets. The Steam Deck uses the Xbox set. Nintendo controllers go by the printed letters, so A confirms.

Hold **L3 + R3** (or press F10) for the in-game menu: FPS limit, button icons, swap confirm and back, swap bumpers and triggers, invert aiming. D-pad or left stick moves, Cross confirms, Circle closes. The choices are saved in `quick_menu.ini`.

## Not on Linux yet
These features use Windows-only code. They are shown greyed out in the launcher:
- The in-game cheat menu (F10), Skip Dialogue and Reveal Full Map
- Skip Intro

## Building from source
You need GCC 13 or newer, CMake 3.20 or newer, Ninja, Git, and the X11, Vulkan and GTK 3 development files (on Arch: `base-devel cmake ninja libx11 libxext vulkan-headers gtk3`).

```sh
git clone --recursive https://github.com/OverkillLabs3/SoS-PS5.git
cd SoS-PS5
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --target libs relinker SonsOfSparta-PS5
tools/package_linux.sh 0.4.1
```

`tools/make_appimage.sh` turns that folder into the AppImages; `.github/workflows/build.yml` does all of it for every commit.

The result is `release/SonsOfSparta-PS5-Native-0.4.1-linux-x86_64` and a `.tar.gz` of the same folder.
