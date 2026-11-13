# LEGO Star Wars: The Complete Saga — VR mod by RanitaVR

**Developed by [RanitaVR](https://github.com/ranitadesign)**

An unofficial, fan-made PC VR mod for *LEGO Star Wars: The Complete Saga*, played in
first person with motion controllers:

- swing the lightsaber with your hand, block blaster bolts with the blade
- aim blasters with the controller
- pick Force targets by looking at them
- fly pods and ships from a cockpit, with your hands on the controls
- cutscenes and menus on a big screen in front of you; HUD on a panel in the world

It runs through OpenXR on any PC VR headset (SteamVR, Quest via Link / Air Link /
Virtual Desktop / Steam Link, Index, Vive...).

> **Early, experimental build.** Expect bugs and occasional crashes.

## Play

You need your own copy of **LEGO Star Wars: The Complete Saga from Steam**. No game data
is included here.

1. Download the mod: the zip on the [Releases](../../releases) page, or the
   [`release/LEGO Star Wars VR`](release/LEGO%20Star%20Wars%20VR) folder of this repository.
2. Launch the game once normally from Steam, then quit (installs DirectX 9 files).
3. Start SteamVR, then run `LEGO Star Wars VR.exe`.

Controls, settings and troubleshooting:
[release/LEGO Star Wars VR/README.txt](release/LEGO%20Star%20Wars%20VR/README.txt).

## How it works

The mod is built on [saga-pc](https://github.com/opensagadev/saga-pc), the community
decompilation of the game's PC executable. The VR code lives in hooks that compile to
nothing in the matching build, so the reference build stays byte-identical:

| Path | What |
| --- | --- |
| `src/vr/` | Stereo replay, head tracking, first person, hands, lightsaber, blasters, cockpits, HUD panel, input |
| `src/vr/vr.h` | The hook macros placed in game code (`SAGA_VR_*`), empty outside the VR build |
| `xr/saga_xr.c` | `saga_vr.dll`: OpenXR session, Direct3D 11 swapchains, controller actions |
| `xr/launcher.c` | `LEGO Star Wars VR.exe`: finds the game data and the OpenXR runtime, starts the game |
| `release/.../RanitaVR.bmp` | The "Developed by RanitaVR" plate shown in the headset after the opening logos |

## Build

1. Set up the saga-pc toolchain as described in [CONTRIBUTING.md](CONTRIBUTING.md)
   (original MSVC 2005 compiler under Wine).
2. Game executable: `make BUILD_KIND=Debug SAGA_VR=1` → `build/LEGOStarWarsSaga_vr.exe`.
3. `saga_vr.dll`: `xr/build.sh <OpenXR-SDK dir> <out dir>` (needs [zig](https://ziglang.org)).
4. Launcher: see the build line at the top of `xr/launcher.c`.
5. Put them next to `openxr_loader.dll` (from the
   [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK), x86) and `saga_vr.ini`.

`make` (Matching) still builds the original, byte-matching executable.

## Credits and license

- VR mod developed by **RanitaVR** — [github.com/ranitadesign](https://github.com/ranitadesign),
  repository [LEGO-STAR-WARS-TCS-VR](https://github.com/ranitadesign/LEGO-STAR-WARS-TCS-VR).
- [saga-pc](https://github.com/opensagadev/saga-pc) by the opensagadev contributors: the
  decompilation this mod is built on. Its original README is in
  [docs/saga-pc-README.md](docs/saga-pc-README.md).
- Source is distributed under the [GNU GPL version 3](LICENSE), like saga-pc.
- `openxr_loader.dll` is the Khronos OpenXR loader, under the Apache License 2.0.

LEGO, Star Wars and *LEGO Star Wars: The Complete Saga* belong to their respective
owners. This is an independent fan project, not affiliated with or endorsed by them.
It does not include or distribute the original game or its assets.
