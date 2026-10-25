<img width="512" height="512" alt="windfallicon" src="https://github.com/user-attachments/assets/dd91bb8a-1114-4129-b639-8676d139d194" />

# Windfall

Windfall is a PC port of The Legend of Zelda: The Wind Waker HD (Wii U, USA). It's a static recompilation: the
game's PowerPC code gets translated to C++ and compiled into a native Windows program, so there's no emulated CPU at
runtime. The graphics side uses Cemu's Vulkan renderer, adapted to run inside the port.

**You need your own copy of the game. Nothing from the game is included in this repo or in the releases.**

## Getting started

1. Download the latest release and unzip it anywhere.
2. Run `windfall.exe`.
3. Point it at your game folder: the decrypted dump with `code`, `content` and `meta` in it (from dumpling, or the
   folder Cemu uses). You can also drag the folder onto the window.
4. The first launch builds the game's code for your PC. It takes a few minutes (about 2 on an 8-core CPU), runs
   offline, and only happens again after an update that changes the recompiler.

Only one version of the game is supported, and Windfall checks it:

| | |
|---|---|
| Title ID | `0005000010143500` |
| Region | USA (`WUP-P-BCZE`) |
| Version | v0 |
| `code/cking.rpx` SHA-1 | `bdcbc4f815ae4c9b0a1f4a16238f96c197b38239` |

Saves and settings go in `%APPDATA%\WWHDRecomp`. The built game code goes in `%LOCALAPPDATA%\WWHDRecomp\game_module`.

## Controls

The game thinks it's talking to a Wii U Pro Controller. Xbox, PlayStation and Switch controllers all work, and face
buttons follow the labels printed on your controller.

| Pro Controller | Keyboard |
|---|---|
| Left stick | W A S D |
| Right stick (camera) | Arrow keys |
| A / B / X / Y | Space / Left Shift / E / Q |
| L / R | F / R |
| ZL / ZR | Left Ctrl / C |
| + / - | Enter / Backspace |
| D-pad | 1 / 2 / 3 / 4 |
| Stick clicks | V / B |
| Fast forward (hold) | ` |

Other keys:

| Key | What it does |
|---|---|
| F1, or Home/Guide on a controller | Settings |
| F2 | GamePad screen in a corner |
| Tab (hold) / Ctrl+Tab | Show / swap to the GamePad screen |
| F11 or Alt+Enter | Fullscreen |
| F12 | Screenshot (saved to Pictures\Windfall) |

When the game asks for a name, just type it and press Enter.

## Settings

Press F1 to open the settings. The game pauses while they're open. Mouse, keyboard and controller all work in the
menu (L and R switch tabs). Changes save right away.

Switching between 30 fps and anything higher needs a restart. Switching between the higher rates doesn't.

### Graphic packs

Windfall reads Cemu's community graphic packs for this game: Resolution, Shadows, Anisotropic Filtering, No SSAO,
Contrasty, Remove HUD and so on. It finds them in your Cemu install automatically (or set `WF_CEMU_DIR`), or you
can put them in `%APPDATA%\WWHDRecomp\graphicPacks`. Packs that patch the game's code (FPS Slowdown, Picto-Box Fix,
the Intel fixes) can't work on recompiled code, so they show up greyed out.

### Cheats

Put `.txt` files in `%APPDATA%\WWHDRecomp\cheats` and they show up under Enhancements. An enabled cheat rewrites its
values every frame:

```ini
[My Cheat]
description = What it does
0x10400004 = .short 999            ; .byte / .short / .int / .float, decimal or 0x hex
0x10400002 = .short @0x10400000    ; copy the value at another address
*0x10400000 + 0x24 = .byte 3       ; through a pointer (skipped while it's null)
```

## Co-op (Beta)

Open the Multiplayer tab, pick a name and a tunic color, and create or join a room with a code. Everyone in the room
sees each other on the same islands, and you can chat with Enter. Item sharing is on by default: anything one player
picks up lands in everyone's empty slots (rupees, hearts and ammo aren't shared). The public server is set by default. To run your own, use
`tools/mp_server/windfall_server.py` (plain Python, no dependencies) and enter its address in the tab.

## Randomizer (Super Beta)

The Randomizer tab runs [TWWHD Randomizer](https://github.com/SuperDude88/TWWHD-Randomizer) by SuperDude88 and
contributors, which ships in the `randomizer` folder. Choose your options and a seed, press Generate (takes about a
minute), turn on "Play the randomized game" and restart. Randomized runs have their own saves in
`%APPDATA%\WWHDRecomp\save_rando`, so your normal file stays as it is. There's a button to open the spoiler log.

## Building from source

You need Windows 10 or 11, Visual Studio 2022 with the Windows SDK, LLVM 21, CMake 3.25+, Ninja, the Vulkan SDK and
Python 3.10+. Running `build.bat` checks for all of that (and offers to install what's missing), finds your dump,
recompiles the game code and builds everything.

By hand:

```bash
git config core.hooksPath tools/githooks
python tools/recomp/recomp.py "<your dump>/code/cking.rpx"
python tools/recomp/test_semantics.py
powershell scripts/build.ps1
```

There are two outputs:

- `windfall.exe` is the runtime: the OS and hardware emulation, the renderer, audio, input and the menus. It has
  nothing from the game in it and builds without a dump.
- `windfall_game.dll` is the recompiled game code, built from `generated/`. It's freestanding (no C runtime) and
  connects to the exe by function name (`runtime/include/wf/game_module.h`). Players build their own on first launch
  with the same compiler flags (`runtime/src/core/game_build.cpp`).

`scripts/package.ps1` puts a release together in `dist/`: the exe, the randomizer, this README, the license, and the
toolchain players need to build the game code (the recompiler, an embedded Python 3.12, and clang/lld-link 21).

`generated/` and `windfall_game.dll` are translations of the game's code. They're git-ignored and never get committed
or shipped. The pre-commit hook (`tools/guard/check_no_game_data.py`) refuses game files and recompiled code, and the
packaging script runs the same check.

## License

GPL-3.0-or-later.

Parts of the code come from [Cemu](https://github.com/cemu-project/Cemu) (MPL-2.0) and
[decaf-emu](https://github.com/decaf-emu/decaf-emu) (GPL-3.0), and those files keep their original notices. The frame
interpolation follows the approach used by [Dusklight](https://github.com/TwilitRealm/dusklight) (CC0). The randomizer
is [TWWHD Randomizer](https://github.com/SuperDude88/TWWHD-Randomizer) (MIT, license in `randomizer/LICENSE.txt`).
Releases include [LLVM](https://llvm.org) clang and lld-link (Apache-2.0 with LLVM exceptions,
`toolchain/llvm/LICENSE.TXT`) and the embeddable [Python](https://www.python.org) (PSF license,
`toolchain/python/LICENSE.txt`).

## The Legend of Zelda and The Wind Waker/Wind Waker HD are trademarks of Nintendo. This project isn't affiliated with or endorsed by
Nintendo.
