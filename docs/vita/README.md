# Star Wars: Rogue Squadron – PS Vita

Native PS Vita port of Rogue Squadron 64 Recompiled. The game runs at a stable 30 FPS in missions (measured on a PCH-1000 with the ARM clock at 500 MHz).

## Requirements

- A PS Vita or PS TV with HENkaku/Ensō and VitaShell.
- `libshacccg.suprx` in `ur0:data/` (the shader compiler vitaGL needs; extract it once with ShaRKBR33D).
- Your own dump of *Star Wars: Rogue Squadron* (N64, USA v1.0, `.z64`).

## Install

1. Copy `RogueSquadronVita.vpk` to the Vita and install it with VitaShell.
2. Copy the ROM to `ux0:data/RogueSquadronVita/rogue_squadron.z64` (the folder is created on first launch; you can also create it yourself).
3. Start the game from the home screen. The first launches compile shaders and can stutter briefly; the compiled shaders are kept in `shader_cache/`.

If the LiveArea background does not update after reinstalling over an older version, delete the app and install the VPK again. Saves and settings in `ux0:data/RogueSquadronVita/` are kept.

## Controls

Open the game's **Controller settings** menu. TRIANGLE applies the Vita preset; UP/DOWN pick an action, CROSS listens for a new button (two buttons at once make a combination), LEFT/RIGHT switch the game's presets, CIRCLE goes back. The left stick flies the craft; the right stick can be bound to actions.

## Files in `ux0:data/RogueSquadronVita/`

| File | Purpose |
|---|---|
| `rogue_squadron.z64` | the ROM |
| `saves/` | save data |
| `roguesq_input.json` | button bindings |
| `roguesq_env.txt` | optional settings, one `NAME=VALUE` per line (`#` comments) |
| `log.txt`, `log_prev.txt` | log of the current and the previous session |
| `shader_cache/` | compiled shaders |

Useful `roguesq_env.txt` settings: `ROGUESQ_VITA_ASPECT=4:3` (pillarbox instead of full-screen stretch), `ROGUESQ_VG_BENCH=bench/mission` (parser benchmark before the game, needs a capture in `bench/mission`).

## Building

See [progress.md](progress.md) (Romanian) for the VitaSDK build in `vita/` and the port's history.
