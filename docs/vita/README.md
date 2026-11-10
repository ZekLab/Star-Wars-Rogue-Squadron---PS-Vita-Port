# Star Wars: Rogue Squadron for PS Vita

## ☕ Enjoying the port? Buy me a coffee

If this port gives you some good hours on your Vita, you can support the work at
[buymeacoffee.com/tavasiberiu](https://buymeacoffee.com/tavasiberiu).

An unofficial native port of *Star Wars: Rogue Squadron* (N64, USA v1.0) to the PS Vita, built on
[Rogue Squadron 64 Recompiled](https://github.com/MikeSemicolonD/RogueSquadron64Recomp).

> **This project does not include any game code or data.** You need your own dump of the N64 cartridge. The game's code
> is recompiled from it on your computer, and the Vita reads the ROM itself for the game data.

## Status

- Missions run at a stable 30 FPS (average 29.5, 10th percentile 29.2, measured on a PCH-1000 with the ARM clock at
  500 MHz).
- Played on the Vita through mission 4 with sound, voices, saves and the controller setup working.
- Known issues: the later missions have not been played through on the Vita yet, the credits text is clipped, and the
  display-list desyncs of the desktop version (a few broken polygons per run) are still there.

## How it works

The game's MIPS code is translated to C by [N64Recomp](https://github.com/N64Recomp/N64Recomp) and compiled for the
Vita's ARM CPU. [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) stands in for the N64's operating
system. On the Vita:

| N64 part | The port provides |
| --- | --- |
| RSP graphics microcode (Factor 5's custom display lists) | RT64's Factor 5 parser running on a shim, drawn with vitaGL (`src/vita_gfx/`), with a separate resolve thread for texture loads, culling and clipping |
| RSP audio (MusyX) | A native MusyX synthesizer (HLE, from mupen64plus) |
| Controller | The Vita's buttons and sticks, with an on-screen editor in the game's Controller settings |
| Cartridge saves | Files in `ux0:data/RogueSquadronVita/saves/` |

## What you need

| Item | Why |
| --- | --- |
| A PS Vita or PS TV with HENkaku/Ensō and VitaShell | To run the game. |
| `libshacccg.suprx` in `ur0:data/` | vitaGL compiles its shaders with it (extract it once with ShaRKBR33D). |
| Your dump of *Star Wars: Rogue Squadron* (N64, USA v1.0) in `.z64` format | The game code and data come from it. See [Supported version](#supported-version). |
| A Mac or Linux computer with Python 3 | To recompile the game and build the port. |

### Supported version

| File | Size | SHA-1 | xxHash3-64 |
| --- | --- | --- | --- |
| `rogue_squadron.z64` | 16,777,216 bytes | `ed42eed1ee2db646ff7ef94ba8c5421d164a4f0d` | `0x6B66A44153594DEA` |

Other versions (Europe, Japan, v1.1) do not work. The dump must be in `.z64` byte order.

## Building

1. Clone this repository with its submodules (`git clone --recursive`), and the decomp
   [rogue_squadron64](https://github.com/MikeSemicolonD/rogue_squadron64) next to it. Put your ROM in both as
   `rogue_squadron.z64`.
2. Create a Python environment next to them with `splat64` 0.41.1, `spimdisasm`, `pyyaml` and `tomli`
   (`python3 -m venv ../.venv && ../.venv/bin/pip install splat64==0.41.1 spimdisasm pyyaml tomli`).
3. Build the recompiler and generate the C code:

   ```bash
   cmake -S lib/N64ModernRuntime/N64Recomp -B build-n64recomp -G Ninja && ninja -C build-n64recomp N64Recomp
   tools/vita/make_compat_elf.sh
   build-n64recomp/N64Recomp rogue_squadron.toml
   ```

   This writes `RecompiledFuncs/`, made from your ROM. It is ignored by git and stays on your computer.
4. With [VitaSDK](https://vitasdk.org) installed (`VITASDK=~/vitasdk`):

   ```bash
   cd vita && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build RogueSquadronVita.vpk
   ```

   The result is `vita/build/RogueSquadronVita.vpk`. Without the LiveArea images (`tools/vita/livearea_from_art.py`
   makes them from the box art, locally) the VPK uses the system defaults.

## Installation

1. Install `RogueSquadronVita.vpk` with VitaShell.
2. Copy the ROM to `ux0:data/RogueSquadronVita/rogue_squadron.z64` (create the folder if it does not exist).
3. Start the game from the LiveArea. The first launches compile shaders and can stutter briefly; the compiled shaders
   are kept in `shader_cache/`.

If the LiveArea background does not update after installing over an older version, delete the app and install the VPK
again. Saves and settings in `ux0:data/RogueSquadronVita/` are kept.

### Updating

To try a new build without reinstalling, copy its `eboot.bin` over `ux0:app/RSQD64001/eboot.bin`.

## Controls

The default Vita layout:

| Vita | N64 |
| --- | --- |
| Left stick | Control stick (fly) |
| Right stick | C buttons |
| Cross | A |
| Square | B |
| Circle | C-Down |
| Triangle | C-Up |
| Select | Z |
| L, R | L, R |
| Start | Start |
| D-pad | D-pad |

To change it, open the game's **Controller settings** menu:

- UP/DOWN pick an action.
- CROSS listens for a new button; two buttons pressed together make a combination.
- TRIANGLE restores the Vita layout above.
- LEFT/RIGHT switch the game's own presets.
- CIRCLE goes back.

The bindings are saved to `roguesq_input.json`.

## Files in `ux0:data/RogueSquadronVita/`

| File | Purpose |
| --- | --- |
| `rogue_squadron.z64` | The ROM. |
| `saves/` | Save data. |
| `roguesq_input.json` | Button bindings. |
| `roguesq_env.txt` | Optional settings, one `NAME=VALUE` per line (`#` starts a comment). |
| `log.txt`, `log_prev.txt` | Log of the current and the previous session. |
| `shader_cache/` | Compiled shaders. |

## Options

Settings for `roguesq_env.txt`:

| Setting | Default | What it does |
| --- | --- | --- |
| `ROGUESQ_VITA_ASPECT` | stretched | `4:3` keeps the original shape with black bars. |
| `ROGUESQ_VITA_THREAD_PRIORITIES` | 1 | `0` leaves the port's thread priorities at the system defaults. |
| `ROGUESQ_VG_BENCH` | off | `bench/mission` runs the parser benchmark on a capture before the game starts. |

## Reporting issues

Open an issue with:

- What you were doing (mission, craft), and whether it happens again.
- `ux0:data/RogueSquadronVita/log.txt` (and `log_prev.txt` if the game was started again since).
- After a crash, the newest `psp2core-*.psp2dmp` file in `ux0:data/`.

## Legal notice

This project is not affiliated with or endorsed by Lucasfilm, LucasArts, Factor 5 or Nintendo. *Star Wars* and
*Rogue Squadron* are trademarks of their respective owners. This project does not distribute game code or data: you
need your own dump of the game.

## Credits and licenses

This project is licensed under the GPL-3.0, like Rogue Squadron 64 Recompiled.

- [Rogue Squadron 64 Recompiled](https://github.com/MikeSemicolonD/RogueSquadron64Recomp) by MikeSemicolonD and
  [Jhalvordson](https://github.com/Jhalvordson/RogueSquadron64Recomp): the desktop recompilation this port is built on.
- [rogue_squadron64](https://github.com/MikeSemicolonD/rogue_squadron64): the decompilation that names the game's
  functions.
- [N64Recomp](https://github.com/N64Recomp/N64Recomp), [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime)
  and [RT64](https://github.com/rt64/rt64) by Wiseguy, DarioSamo and contributors.
- [mupen64plus-rsp-hle](https://github.com/mupen64plus/mupen64plus-rsp-hle): the MusyX synthesizer.
- [VitaSDK](https://vitasdk.org) and [vitaGL](https://github.com/Rinnegatamante/vitaGL): toolchain and renderer.
