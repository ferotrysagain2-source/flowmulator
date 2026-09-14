# Flowmulator

Flowmulator is a native SDL emulation frontend for Linux, built
around a simple station-style menu.

## Features

- GBA emulation through the bundled mGBA Libretro core
- NDS emulation through the bundled DeSmuME Libretro core
- Wii and GameCube launching through Dolphin
- 3DS launching through the bundled Azahar runtime
- Keyboard, controller, and Wii Remote input
- Mouse navigation throughout the frontend
- Per-system control profiles
- Optional CRT filter and overlays
- Multiple themes and built-in Easter eggs
- Native Vulkan rendering for the Dolphin/Libretro path

## Requirements

### Linux

- SDL2
- OpenGL
- Vulkan runtime
- FUSE2 or FUSE3 for the bundled Azahar AppImage
- Dolphin Emulator (`dolphin-emu`)
- BlueZ and `bluetoothctl` for Wii Remote support

On Arch Linux or Omarchy:

```sh
sudo pacman -S sdl2 libgl vulkan-icd-loader dolphin-emu fuse2 bluez
```

You need legally obtained ROM files. Supported formats include:

| System | Extensions |
| --- | --- |
| GBA | `.gba` |
| NDS | `.nds` |
| 3DS | `.3ds`, `.cci` |
| Wii | `.iso`, `.wbfs`, `.rvz` |
| GameCube | `.iso`, `.rvz` |

## Installation

The release package contains:

- `Flowmulator`
- `Helper.sh`
- `ROMS/`
- `SAVES/`

On Linux, run the helper from the extracted release directory:

```sh
./Helper.sh
```

The helper installs dependencies, desktop integration, the application icon,
and Hyprland window rules. It also supports updating, repairing, and
uninstalling the Flowmulator integration. User ROMs and saves are preserved
during updates and uninstall.

Launch the application with:

```sh
./Flowmulator
```

The bundled cores and application assets are embedded in the executable.
Runtime files are extracted to:

```text
~/.cache/flowmulator/runtime
```

## ROMs and saves

Place ROMs in the matching directory:

```text
ROMS/GBA/
ROMS/NDS/
ROMS/3DS/
ROMS/WII/
ROMS/GAMECUBE/
```

Saves are stored in `SAVES/`. NDS keeps its native `.dsv` save and also writes
a `.sav` compatibility copy when a session closes.

## Building

Build the optimized local binary:

```sh
make
```

Create a release package:

```sh
make release RELEASE_VERSION=0.8.1 RELEASE_ZIP=Flowmulator.v0.8.1.zip
```

The package is written to the requested ZIP path, and the extracted release
directory is written beneath `Releases/`.

## Project layout

```text
main.cpp              Frontend, emulator integration, and rendering
Helper.sh             Linux installation and maintenance helper
Makefile              Linux build and packaging targets
CMakeLists.txt        Cross-platform build configuration
.assets/              Source assets embedded into the executable
.runtime/             Runtime cores and bundled emulator files
ROMS/                 User-provided ROMs
SAVES/                User-generated saves and emulator data
```
