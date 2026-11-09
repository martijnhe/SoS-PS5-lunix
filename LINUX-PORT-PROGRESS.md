# Linux port: progress report

Status on 2026-11-09 (the sections below the table are from 2026-11-07, except where marked). The goal is a native Linux build of SonsOfSparta-PS5: the same approach as the Windows build (the game's own x86-64 code runs directly, PS5 system libraries are reimplemented), producing Linux ELF files instead of Windows PE files.

## Where it stands

| Stage | State |
|---|---|
| Building on Linux (relinker, all 109 PS5 libraries, launcher) | Works |
| Launcher window (settings, game folder picker, first-start progress) | Works |
| Preparing the game from a dump (SELF unpacking, relinking) | Works |
| Loading the program and all game modules | Works |
| Unity / IL2CPP start-up, Vulkan device and window | Works |
| GC thread interrupts, C++/C# exceptions, plugin start-up | Works |
| First frames on screen ("Sony Interactive Entertainment presents") | Works, about 25 FPS |
| Getting past the intro | Works: fixed 2026-11-09 |
| Title screen, menus, new game | Works with scripted pad input (`APS5_PAD_SCRIPT`) |
| Gameplay | Works: the opening Cyclops fight, about 47-60 FPS, 5 minutes without a crash (scripted input) |
| Audio | The game opens a live 48 kHz stereo stream in PipeWire; not yet heard by a person |
| Real controller, long play | Not tested |

## Installed copy (2026-11-09)

`~/Documents/claude/games/SonsOfSparta-PS5`, started by `play.sh` (fullscreen, saved settings). It uses the game files through links into the dump folder in Downloads. Rebuild steps: `ninja -C build-linux libs`, then copy the changed `.prx` files to its `libs/`; `tools/package_linux.sh` empties only the release folder.

The original project (`OverkillLabs3/SoS-PS5`) has had its own Linux work merged since this fork was made (20 commits, not merged here yet). Ultrawide: requested there in issue #32, not implemented.

## Controllers and AppImages (2026-11-09)

Xbox and Switch button prompts run on Linux (`libSceSystemService/XboxPrompts.cpp`; the patch's offsets are 0x10000 lower in the Linux module, confirmed by its code checksums). L3+R3 menu in `libSceVideoOut` (`QuickSettings.hpp`, `ImGuiOverlay.cpp`). Steam Deck defaults in `launcher_linux.cpp` (`OnSteamDeck`). Not yet tried on real Switch, Xbox or Deck hardware: tested with scripted input only.

## Open items

- **Memory grows** about 0.25 GB per minute during play (4.0 to 5.0 GB in 5 minutes). Not yet known whether it levels off.
- `sceKernelSyncOnAddressWait32/64` (`B2n8aDorSH4`, `PZQhiiLXRFs`) are still return-0 stubs, which turn those waits into busy loops.
- Ultrawide: not supported yet. The picture stays 16:9. The game's scripts have an aspect-ratio setting (`aspectRatio` in its settings, `GraphicsSettingsUtils.GetAllPossibleAspectRatio`), but Unity's PS5 player refuses other shapes in `Screen.SetResolution`: its messages include "height must be at 16:9 ratio to width", and also check a list of valid widths and a minimum and maximum. The display buffers are 3840x2160. True ultrawide needs those native checks patched, a forced resolution, and display buffers of the new shape.
- A session with deaths and respawns in the first fight showed brief black frames; these are the game's own death fade.

## Fixed 2026-11-09: about 1 FPS in gameplay

`describePages` (`libSceAgcDriver/Execution/src/GuestMemory.cpp`) opened and parsed `/proc/self/maps` on every page lookup, and unlike the Windows path never stored the answer in the page spans. Each draw cost about 3 ms. It now stores the pages as Windows does and keeps one parsed snapshot per thread, replaced when guest allocations change or after 50 ms. Draws fell to about 24 us and the fight went from 1 to about 58 FPS. Found with `APS5_PROFILE_DRAW=1`, which prints the driver's time per phase every 10 s.

## Fixed 2026-11-09: hang on "Sony Interactive Entertainment presents"

IL2CPP's `GetTimeZoneData` looped forever because the game libc's `mktime` returned a wrong time. `mktime` passes a 16-byte `KernelTimesec` to `sceKernelConvertLocaltimeToUtc` and reads its `west_sec` and `dst_sec`; the port took it for an 8-byte `KernelTimezone`, so those fields stayed uninitialised (`libkernel/Time/Time.cpp`). The three unknown imports suspected earlier are not involved: their call sites pass no output pointer.

`ninja -C build-linux` does not rebuild the NID-patched libraries; use `ninja -C build-linux libs`, then copy the changed `.prx` into the release's `libs/`.

## What was changed

### Build and libraries
- **`libkernel` compiles on Linux.** Seven files used Windows-only APIs. New portable wake event: `core/libs/prx/libkernel/Pthread/include/WakeEvent.hpp`. Ported: `Umtx.cpp`, `Mutex.cpp`, `Equeue.cpp`, `EventFlag.cpp`, AvPlayer (`SafeTransition.hpp`, `libSceAvPlayer.native/Export.cpp`).
- **Thread interrupts for the game's garbage collector** (`System/src/Exception.cpp`). On Windows the target thread is suspended and its context rewritten; on Linux a real-time signal is used, with the same three cases (running game code, parked in a wait, inside host code). Tested: 50/50 deliveries in running code, 50/50 to a parked thread, and a clean timeout for a thread stuck in host code.
- **Libraries find each other** (`$ORIGIN` run path). The NID patcher erased `RUNPATH`; it now keeps `RPATH`/`RUNPATH` (`core/libs/nid/src/ElfNidPatcher.cpp`).
- **`-Bsymbolic` for all host libraries** (`core/libs/CMakeLists.txt`). Without it, the loader bound the host libc's own `_Stdout` to the game libc's copy, which corrupted the game's `stdout` and crashed the first `printf`. Windows DLLs never interpose like this.
- **Thread stack bounds recorded on Linux** (`Pthread/src/Thread.cpp`). The GC got a NULL stack address for every thread, couldn't scan stacks, and freed live objects. Requested stack sizes larger than the default are now honoured.
- **Triangle "blow into the mic"** works on Linux (`common/BlowInput.hpp`, POSIX shared memory).
- `/dev/urandom` and `/dev/random` map to the host devices (`File/src/Open.cpp`).
- `chdir` accepts directories reached through the `app0/` links into the game folder (`libc/src/General.cpp`).

### Relinker (Linux output)
- **Executable placed at `0x400000` (`ET_EXEC`)**, as upstream AnyPS5 does. The game's heap setup expects the executable inside the 40-bit address window (`LinuxElfPatcher.cpp`, `RebaseToFixedAddress`).
- **Entry stub passes the real `argc`/`argv` block.** It used to pass `{argc, pointer to argv}`, which the game read as garbage (`EntryStubBuilder.cpp`, ported from upstream).
- **Unwind tables kept** (`PT_GNU_EH_FRAME`, `PT_GNU_STACK`). They were filtered out with the PS5-only segments, so exceptions could never be caught (`SegmentFilter.cpp`).
- **Deferred module start** (PSN, SaveData, VoiceInputPlugin, lib_burst_generated). The start function is recorded under a private dynamic tag instead of `DT_INIT`, and run when the game first loads the module (`LinuxGuestModuleWriter.cpp`, `GuestModuleWriter.hpp`).

### Runtime module loading (`libkernel/Module/src`)
- **Game module names match without regard to case.** The game loads `Il2CppUserAssemblies.prx`; the file is `Il2cppUserAssemblies.prx`.
- **Deferred modules are started with the `sceKernelLoadStartModule` arguments**, and the result goes back to the caller. PSN reads its plugin arguments there.
- **`sceKernelGetModuleInfoForUnwind` returns real unwind tables** (`eh_frame_hdr`, `eh_frame` and its size, code segment). The game's own C++ runtime finds catch handlers through it; without it every C# exception ended the game.

### Launcher (`launcher/linux/`)
- GTK 3 settings window like the Windows one: game folder picker with live check, display mode, resolution, frame rate, button icons, show at startup, in-game FPS. Windows-only options are shown greyed out.
- Same `launcher.ini` as Windows, plus `[game] folder=`; resolution in the game's settings save.
- First-start progress window, error dialogs, one instance at a time.
- **Stub library for missing PS5 functions** (`MissingImportStubs.cpp`). Like the Windows relinker's soft stubs, imports that no loaded library provides return 0. It is written to `prep/libsos_missing_imports.so`, preloaded, and listed in `logs/missing-imports.txt` (2,321 functions, mostly PSN).
- `prepared.stamp` includes the relinker's modification time, so a rebuilt relinker always re-prepares.
- Shared default game settings moved to `launcher/GameSettingsSeed.hpp` (also used by the Windows launcher).

### Packaging and docs
- `tools/package_linux.sh` builds `release/SonsOfSparta-PS5-Native-<version>-linux-x86_64` and a `.tar.gz`. Running it deletes that folder first, including prepared files and `launcher.ini`.
- `docs/user/LINUX.md`: user guide. `README.md` links to it.

## Testing

- **Unit tests:** 116 of 128 pass. The 12 failures are in code these changes did not touch and are unchanged from the first Linux build:
  - Six Python relinker tests point to scripts missing from the repo.
  - Five GPU-driver/shader tests, and `guest_memory`, have expectations that no longer match the code.
- **Real game:** dump `PPSA28997` version `01.008.001`, AMD Ryzen 5 9600X, Hyprland (Wayland via XWayland).
- The Windows build in the user's other folder doesn't start IL2CPP (`global-metadata.dat` not found through its `app0/Media` link), so it can't be compared past that point.

## Untested or unchanged
- **Windows build:** not built here. These edits touch code it also compiles: the `WakeEvent` wrapper, `Umtx.cpp`, `Open.cpp`, `DynamicLoader.cpp` (`#ifndef _WIN32` parts only), `launcher.cpp` (seed moved to a header), and the relinker's shared `SegmentFilter`/`EntryStubBuilder` (only the Linux patcher uses them).
- **Linux only lacks:** cheat menu, Skip intro, Skip dialogue, Reveal full map, Xbox button icons in game.
- **Display:** SDL is built for X11; Wayland desktops use XWayland.

## Debugging notes

- **Logs:** `logs/game.err`, `logs/game.log`, `logs/prepare.log`, `logs/missing-imports.txt`.
- **Module loading trace:** put `APS5_TRACE_DL=1` in `debug_env.txt` (no limit on successful lookups when tracing).
- **gdb:** `ptrace_scope=1` blocks attaching to a running game. Start it from gdb (the Python `subprocess.Popen` then `attach` pattern), or run it under gdb with `set env LD_PRELOAD <release>/prep/libsos_missing_imports.so`.
- **Software breakpoints** in game modules fail before they're loaded; use hardware breakpoints after `starti`.
- **Disassembling `sos_runtime` offline:** its section headers don't match the segments, so read through the program headers (`objdump -b binary` with file offset = address − 0x400000 + 0x4000).
