# RPCS3 libretro core

This directory contains the incremental libretro frontend for RPCS3. The
current Windows preview links the real emulator, boots PS3 content, renders it
with RPCS3's Vulkan backend, supplies XRGB8888 frames, routes 48 kHz stereo
audio, maps the first RetroPad to a standard PS3 controller and publishes
PPU/SPU compilation progress through RetroArch messages and a video progress
bar.

This is still an experimental core. The initial CPU video readback is costly,
only the first controller is mapped and libretro save states are not exposed.

## Configure and build

The build downloads a pinned copy of `libretro-common` when
`RPCS3_LIBRETRO_API_DIR` is empty. An existing official header can be used
instead:

```powershell
cmake -S . -B build-libretro -G Ninja `
  -DBUILD_RPCS3_GUI=OFF `
  -DBUILD_LIBRETRO=ON `
  -DRPCS3_LIBRETRO_API_DIR=C:/path/to/libretro-common/include

cmake --build build-libretro --target rpcs3_libretro
```

The ABI/callback smoke test can be built and run with:

```powershell
cmake --build build-libretro --target check_rpcs3_libretro
```

Set `RPCS3_RETROARCH_ROOT` during configuration to copy the completed core to
the frontend automatically:

```powershell
-DRPCS3_RETROARCH_ROOT=C:/path/to/RetroArch-Win64
```

The core expects a working RPCS3 data directory at `<system>/rpcs3`, where
`<system>` is the frontend's libretro system directory. In particular, that
directory must contain installed PS3 firmware (`dev_flash`) and the usual
RPCS3 configuration/VFS data. On Windows the core also recognizes this layout
during DLL initialization, before the libretro environment callback is
available.

### Core options

RetroArch exposes an `RPCS3 Resolution Scale` option with 100%, 150%, 200% and
300% values. The option controls RPCS3's internal rendering resolution and is
applied when the next content is loaded. It is independent of RetroArch's
window/output scaling. Higher values require substantially more VRAM and CPU
readback bandwidth with the current experimental video bridge.

## Milestone boundaries

Implemented:

- Complete mandatory libretro symbol surface.
- Core discovery metadata.
- Logging and user-visible messages.
- Non-Qt RPCS3 frontend based on `EmuCallbacks`.
- PS3 ISO/content boot, including the legacy ISO fallback used by older RPCS3
  builds.
- Hidden Win32 Vulkan render surface.
- Vulkan/OpenGL presentation capture and XRGB8888 libretro video delivery.
- RPCS3 audio backend bridged to the libretro 48 kHz stereo batch callback.
- Player-one RetroPad mapping for digital buttons and both analog sticks.
- Strong/weak libretro rumble output.
- PPU/SPU progress stages (`Analyzing`, `Scanning`, `Loading`, `Compiling`,
  `Linking`, `Applying PPU Code` and `Building SPU Cache`) surfaced through
  `RETRO_ENVIRONMENT_SET_MESSAGE` and an XRGB8888 progress overlay.
- Main-thread callback dispatch and safe game/core unload from RetroArch.
- Cleanup of RPCS3's process-wide Windows exception hooks before the core DLL
  is unloaded.

Next milestone:

- Extend controller support beyond player one and add motion input.
- Replace the initial CPU frame readback path with a shared hardware-rendering
  path where the frontend and RPCS3 renderer can safely interoperate.
- Add core options, save-directory mapping and broader title/regression tests.
