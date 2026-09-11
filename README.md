# Flowmulator v1

Flowmulator is a native C++/SDL Linux frontend for the mGBA libretro GBA core.
It loads the core directly with `dlopen`; it does not launch RetroArch or
display a RetroArch UI.

## Requirements

- Linux with SDL2, OpenGL, and `pkg-config`
- The mGBA libretro core at `/usr/lib/libretro/mgba_libretro.so`
- A legally obtained `.gba` ROM

On Arch/Omarchy, install the runtime dependencies with:

```sh
sudo pacman -S sdl2 libgl pkgconf mgba
```

The release includes the real `Flowmulator` executable and a separate
`install-dependencies.sh` script. On a fresh Linux installation, run the
script once before launching the emulator. It installs the dependencies and
does not need to be run again unless the system is reinstalled.

The normal development tree keeps runtime assets in the hidden `.assets/`
directory. The `Release v0.3` package embeds those assets directly in the
Flowmulator binary, so testers do not need an assets folder.
Testers should copy their own `.gba` files into `ROMS/`; saves are created
automatically in `SAVES/`. ROMs and saves are intentionally not included.

- `.gba` ROM loading
- native 240x160 SDL output with centered 3:2 scaling and black bars
- mGBA's complete ARM7TDMI, BIOS, PPU, DMA, timer, audio, input, and save
  emulation through `/usr/lib/libretro/mgba_libretro.so`
- a small native SDL video/input/audio frontend with a pink blossom menu
- optional GPU CRT shader with curvature, beam-weighted scanlines, bloom,
  phosphor triad detail, and vignette when CRT Filter is enabled
- direct `.gba` ROM loading and 240x160 output scaled to a desktop window

Launch **Flowmulator** from the Omarchy launcher and choose a ROM:

```sh
./install-dependencies.sh
./Flowmulator
```

The launcher opens the cherry-blossom frontend first. Choose **Load ROM**,
toggle **CRT Filter**, or open **Controls** before launching. In **Controls**,
select an action and press Enter, then press the new key to rebind it. Use Escape to return. Hold **Tab** for
temporary Turbo Speed, or press
**Shift+Tab** to toggle Turbo Speed on or off. The Turbo key can be rebound
from Controls. Press **F1** to toggle CRT, **F2** to show the live FPS counter,
and **F3** to toggle renderer VSync. Click the title to cycle through the
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

The archive is written to `release/Flowmulator-v1.tar.gz`.

## iOS port

An iOS target is provided in `ios/`. Unlike the Linux build, it statically
links an iOS mGBA libretro core, uses the iOS document picker for `.gba`
imports, stores saves in the app's Documents directory, and maps touch input
to the GBA controls. See `ios/README.md` for the macOS/Xcode build steps.
