# Flowmulator v1

Flowmulator is a native C++/SDL emulation station for Linux and Windows. It lists GBA, NDS,
3DS, Wii, and GameCube in the station menu. GBA/NDS load through the mGBA and DeSmuME
libretro cores.
Flowmulator loads the GBA and NDS cores directly as shared libraries; it does not
launch RetroArch or display a RetroArch UI. Wii launches standalone Dolphin
with the selected ROM and returns to Flowii when Dolphin exits.

## Requirements

- Linux with SDL2, OpenGL, and FUSE (for the bundled Azahar AppImage)
- Dolphin Emulator available as `dolphin-emu` on `PATH` (Wii uses standalone Dolphin)
- A legally obtained `.gba`, `.nds`, `.iso`, `.wbfs`, or `.rvz` ROM

On Arch/Omarchy, install the runtime dependencies with:

```sh
sudo pacman -S sdl2 libgl dolphin-emu fuse2
```

The release includes the real `Flowmulator` executable and a separate
`Helper.sh` script. The GBA/NDS cores, Azahar 3DS runtime, and application
assets are embedded in the executable; only Dolphin and the Linux graphics/audio
libraries are installed by the helper. On a fresh Linux installation, run the
script once before launching the emulator. When run from a terminal, it offers
install, update, repair, and uninstall options. Update downloads the latest
release without touching user data; repair installs missing dependencies and
restores the launcher, while uninstall removes the Flowmulator application
integration and dependencies but preserves the `ROMS/` and `SAVES/` folders.
The script hides Dolphin's standalone application-menu entry; Dolphin remains
installed as Flowmulator's Wii runtime dependency.
It also adds only Flowmulator to the desktop application menu and uses the
included retro-station icon.

To update an existing installation, choose option 2 in `Helper.sh`. It
downloads the latest `Flowmulator-Linux.tar.gz` release from GitHub, replaces
the application files, and runs the dependency repair step. It never copies,
deletes, or replaces `ROMS/` or `SAVES/`.

## Windows

The frontend also builds and runs natively on 64-bit Windows. Install SDL2
development files, OpenGL headers/libraries, CMake, and a MinGW-w64 or
LLVM/Clang toolchain. From a developer PowerShell or MinGW shell:

```powershell
cmake -S . -B build -G "MinGW Makefiles" `
  -DCMAKE_CXX_COMPILER=g++ -DCMAKE_MAKE_PROGRAM=mingw32-make
cmake --build build --config Release
```

Copy the SDL2 runtime DLL beside `build/Flowmulator.exe`. Copy the Windows
libretro cores into the hidden `.assets/` source directory as
`mgba_libretro.dll` and `desmume_libretro.dll`. Install Dolphin separately
and put `Dolphin.exe` on `PATH` or beside Flowmulator. The build creates
`ROMS\GBA`, `ROMS\NDS`, `ROMS\3DS`, `ROMS\WII`, `ROMS\GAMECUBE`, and
`SAVES` beside the executable. The Linux Azahar 3DS core is embedded in
Flowmulator and extracted to a hidden cache only when needed.
Controller mappings and Dolphin settings are stored under `%APPDATA%`.

The normal development tree keeps source assets in hidden `.assets/` and
`.runtime/` directories. The shipment package contains no hidden runtime
folders: its embedded runtime is extracted into the visible `Runtime/`
directory on first launch.
Testers should copy their own `.gba` files into `ROMS/GBA/`, `.nds` files into
`ROMS/NDS/`, and Wii `.iso`, `.wbfs`, or `.rvz` files into `ROMS/WII/`; saves
are created automatically in `SAVES/`. ROMs and saves
are intentionally not included. DeSmuME uses `.dsv` as its native save format;
Flowmulator also exports a `.sav`-named compatibility copy when an NDS session
closes, while keeping the `.dsv` file as the canonical save.

- `.gba` ROM loading
- native 240x160 SDL output with centered 3:2 scaling and black bars
- mGBA's complete ARM7TDMI, BIOS, PPU, DMA, timer, audio, input, and save
  emulation through the installed mGBA libretro core
- a small native SDL video/input/audio frontend with a pink blossom menu
- optional GPU CRT shader with curvature, beam-weighted scanlines, bloom,
  phosphor triad detail, and vignette when CRT Filter is enabled
- direct `.gba` ROM loading and 240x160 output scaled to a desktop window

Launch **Flowmulator** from the Omarchy launcher and choose a system and ROM:

```sh
./Helper.sh
./Flowmulator
```

The launcher opens the system station first. Choose **GBA**, **NDS**, **3DS**, **WII**, or **GAMECUBE**
to select a ROM. GBA and NDS open their frontend first; Wii loads through the
Dolphin core. The 3DS and GameCube entries are placeholders until their emulator
backends are integrated. Choose **Load ROM**, toggle **CRT Filter**, or open
the GBA **Overlay**, or open **Controls** before launching. The overlay uses
the transparent screen area of the embedded Game Boy Advance artwork. In
**Controls**,
select an action and press Enter, then press the new key to rebind it. Use Escape to return. Hold **Tab** for
temporary Turbo Speed, or press
**Shift+Tab** to toggle Turbo Speed on or off. The Turbo key can be rebound
from Controls. Press **F1** to toggle CRT, **F2** to show the live FPS counter,
**F3** to toggle renderer VSync, and **F4** to toggle the GBA overlay. Click
the title to cycle through the
FLOWMULATOR, DRILBOOR, and BIG FOREHEAD editions. Escape exits the emulator.

## Building and packaging

Build the optimized local binary:

```sh
make
```

Create the tester archive:

```sh
make release
```

The archive is written to `Flowmulator-Linux.tar.gz`.
