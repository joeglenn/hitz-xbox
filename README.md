# hitz-xbox

A static recompilation of **NHL Hitz Pro** (original Xbox, USA, title ID `0x4D57001A`) to a native Windows x64 program. It is built on the [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit.

The game's x86 code is translated to C ahead of time and compiled with MSVC. Xbox hardware (NV2A GPU, APU audio, USB pads, kernel) is emulated by the toolkit runtime. Graphics run on the host GPU through a D3D11 back end in this repo.

**Status:** boots, menus work, and an exhibition match is playable at 60 fps, single player or two players on one machine, with sound.

This repo is the plain port. Online play lives in a separate edition, [hitz-xbox-netplay](https://github.com/joeglenn/hitz-xbox-netplay): the same port plus lockstep netplay and its relay server.

> **No game data is included.** You need your own copy of NHL Hitz Pro. The generated C code is derived from the game executable, so it is not in this repo either; you rebuild it from your copy (step 4).

---

## Repo layout

```
hitz_pro/                  the game project
  CMakeLists.txt           builds hitz_pro_recomp.exe against ../xboxrecomp
  build_windows.bat        MSVC + Ninja build (finds Visual Studio on its own)
  HitzLauncher.exe         settings window + Play (copied here by setup)
  run_hitz.bat             starts the game straight away with the defaults
  src/main.c               entry point, title defaults, crash handler
  src/recomp_manual.c      hand-written overrides of recompiled functions
  src/recomp_kick.c        D3D KickOff without the PFB-flush spin
  src/nv2a_d3d11.c         NV2A -> D3D11 renderer (vertex programs, combiners on the GPU)
  src/hostprof_win.c       sampling profiler (HITZ_HOSTPROF=1)
  src/recomp/gen/          generated code -- NOT in the repo, you create it (step 4)
  game/                    your game files -- NOT in the repo (step 3)
  toolkit/BASE_COMMIT      the xboxrecomp commit the patch applies to
  toolkit/xboxrecomp-hitz.patch   toolkit changes this game needs
xboxrecomp-hitz.patch      same patch (top-level copy)
tools/                     helper scripts used during development (see below)
NOTES.md                   dated engineering log: every bug and fix
CLAUDE.md                  phase plan / project overview
```

The launcher shows the box art (`hitz_pro\cover.jpg`) on the left; replace the file to use another picture.

---

## Requirements

**Windows 10/11 x64** (to build and play):
- [Git for Windows](https://git-scm.com/download/win)
- Visual Studio 2022 or the free *Build Tools for Visual Studio 2022*, with the C++ workload. From a command prompt:
  ```
  winget install Microsoft.VisualStudio.2022.BuildTools --override "--passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
  ```
- A D3D11 GPU (feature level 11.1). Tested on an AMD Radeon 8060S.
- An XInput controller, or the keyboard.
- About 6 GB of free disk space, and an internet connection on the first run.

You do not need to install Python: the setup script downloads its own copy into `.tools\`.

---

## Quick start: one command

```bat
git clone https://github.com/joeglenn/hitz-xbox.git
cd hitz-xbox
setup.bat --iso "D:\Games\NHL Hitz Pro (USA).iso"
```

Then play with **`hitz_pro\HitzLauncher.exe`** (pick your settings, press Play), or `hitz_pro\run_hitz.bat` to start straight away with the defaults. Add `--run` to the setup command to start the game when it finishes.

`setup.bat` does every step in the next section:
1. Fetches the toolkit, checks out the right commit and applies the patch.
2. Extracts the disc image. A trimmed xiso or a full redump image both work, and no extra tools are needed.
3. Checks that it is the USA release (title ID `0x4D57001A`).
4. Generates the C code.
5. Builds the exe.

The first run took 16 minutes in testing (8-core Windows VM): about 15 for code generation and 1 for the build. The script can be rerun safely: finished steps are skipped, and the code is only regenerated when the game, the patch or `recomp_manual.c` changes.

Options:

| Option | What it does |
|---|---|
| `--iso FILE` | disc image to extract into `hitz_pro\game\` |
| `--game DIR` | use an already-extracted folder (with `default.xbe`) instead |
| `--regen` | regenerate the C code even if it looks up to date |
| `--no-build` | stop after generating the code |
| `--run` | start the game when done |

**Updating:** `git pull`, then run `setup.bat` again. It regenerates the code and rebuilds only if the patch or the overrides changed.

**Linux/macOS:** `./setup.sh --iso "NHL Hitz Pro (USA).iso"` does steps 1–4. It needs Python 3.10+ with venv; on Ubuntu, `sudo apt install python3-venv`. Building needs Windows, so copy the whole folder to a Windows PC and run `setup.bat` there; the finished steps are skipped.

---

## Manual steps (what setup does)

You only need this section to understand or debug the setup script. The manual build additionally needs Python 3.10+ with `capstone` (`pip install capstone`).

### 1. Clone this repo

```sh
git clone https://github.com/joeglenn/hitz-xbox.git
cd hitz-xbox
```

When you are done, the tree looks like this:

```
hitz-xbox/
  hitz_pro/
    game/default.xbe ...   (step 3)
    src/recomp/gen/ ...    (step 4)
  xboxrecomp/              (step 2)
```

### 2. Get the toolkit and apply the patch

```sh
git clone https://github.com/sp00nznet/xboxrecomp.git
cd xboxrecomp
git checkout $(cat ../hitz_pro/toolkit/BASE_COMMIT)    # f0f8a46, v0.16.0
git apply --check ../xboxrecomp-hitz.patch              # dry run, should print nothing
git apply ../xboxrecomp-hitz.patch
cd ..
```

On Windows cmd, use `git checkout f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b` instead of `$(cat ...)`.

The patch has to go in **before** you generate code, because it also fixes the lifter (`tools/recomp/lifter.py`). In summary, it changes:
- **Lifter:** debug-register moves (`mov drN`).
- **NV2A:** pushbuffer DMA walk with flip-hold stop and resume; walk bounds checks; back-end hooks (`draw_x`, `draw_vsh`) used by the D3D11 renderer; the software-method (notify) interrupt; log gating.
- **Kernel:** `xbox_SignalGuestEvent` for D3D's GPU-interrupt event; file and memory fixes.
- **USB:** the OHCI controller with up to 4 pads and the pad-script tool.

If `git apply` fails, you are not on the base commit. Run `git status` and check it out again.

### 3. Put the game files in `hitz_pro/game/`

(setup.py has its own extractor.) Extract the disc so that `hitz_pro/game/default.xbe` exists next to all the data (`*.pck`, `FE/`, `AI/`, and so on). Any XISO extractor works, for example [extract-xiso](https://github.com/XboxDev/extract-xiso):

```sh
extract-xiso -x "NHL Hitz Pro (USA).iso" -d hitz_pro/game
```

### 4. Generate the C code

Run from inside `xboxrecomp/`:

```sh
cd xboxrecomp
python -m tools.xbe_parser   ../hitz_pro/game/default.xbe --json ../hitz_pro/game/default_analysis.json
python -m tools.disasm       ../hitz_pro/game/default.xbe --force -v
python -m tools.func_id      ../hitz_pro/game/default.xbe -v
python -m tools.abi_analysis ../hitz_pro/game/default.xbe -v
python -m tools.recomp       ../hitz_pro/game/default.xbe --all --split 250 \
    --gen-dir ../hitz_pro/src/recomp/gen \
    --exclude-manual ../hitz_pro/src/recomp_manual.c
cd ..
```

Notes:
- **Do not pass `--text-only` to the disassembler.** The XDK library sections (D3D, DSOUND, XGRPH, and so on) contain code, and without them you lose about 3,000 functions.
- **Do not skip `--exclude-manual`.** It makes the generator rename every function overridden in `recomp_manual.c` to `sub_XXXXXXXX_gen`, so the override wraps it. The log should report the wrapped functions. Without it, the link fails with duplicate symbols.
- Expect about 19,200 functions, about 65 MB of C in `hitz_pro/src/recomp/gen/`, and a few minutes of run time.

### 5. Build (Windows)

```bat
cd hitz_pro
build_windows.bat
```

The script loads the MSVC environment through `vswhere`, then runs CMake and Ninja in Release mode. The first build compiles 16k+ generated functions and takes about 10 minutes on a fast machine; later builds are incremental. The output is `hitz_pro\build\hitz_pro_recomp.exe`.

If the link fails with "cannot open file", the game is still running. Close it, or run `taskkill /F /IM hitz_pro_recomp.exe`.

### 6. Run

Double-click `hitz_pro\HitzLauncher.exe` (or `run_hitz.bat`; the exe itself also runs on its own with the same defaults). Either starts the exe with `hitz_pro\` as the working directory, which is where it finds `game\default.xbe`. With **Debug logging** ticked in the launcher (or `HITZ_DEBUG=1`), log output goes to `hitz_pro\hitz_stderr.log` (fps every 10 s) and `hitz_pro\hitz_perf.log`. It is off by default: then nothing is logged at all, and a crash still leaves `hitz_pro\hitz_crash.log`.

**The launcher** has the settings players need, saved in `hitz_pro\hitz_settings.ini`:

- **Display:** windowed or fullscreen, window size, smooth or sharp picture, frame pacing, the fullscreen refresh switch, and which GPU to use.
- **Controls:** number of players, keyboard as player 1, non-Xbox controllers on/off, and a list of the controllers it can see.
- **Diagnostics:** Debug logging on/off (`hitz_stderr.log` and `hitz_perf.log`; off by default, and off means no logging code runs while you play), and a button that opens the folder with the logs. Tick it before playing a session you want to send logs from.

### Smooth motion

The game runs at 60 frames a second. To show every frame for the same length of time:

- On a 60 Hz monitor (or 120/240 Hz), the game's frame clock locks to the monitor's refresh, so frames are never doubled or skipped.
- On a 144/165 Hz monitor, 60 doesn't divide evenly into the refresh rate. With G-Sync/FreeSync, fullscreen is smooth anyway. Without it, the launcher's **refresh switch** (on by default) switches the monitor to 120 or 60 Hz while the game is fullscreen and back when you leave.
- F11 or Alt+Enter toggles fullscreen.

### Performance log and F9

`hitz_pro\hitz_perf.log` (on by default) records stutter so it can be tracked down later: your PC, monitor and settings at the top, then a `HITCH` line for any frame that took clearly longer than the ones around it (with what happened during it: shaders, textures, files loaded), `JUDDER` when a frame stayed on screen for an uneven time, and a summary every 5 seconds. Times are wall clock.

**When something feels laggy, press F9.** It writes a numbered `MARK` into the log (the window title shows "F9 mark N saved"), so the moment can be found afterwards.

---

## Controls

**Xbox controllers** (360/One/Series, and anything in XInput mode) map one-to-one to the Xbox pad.

**Other controllers** (8BitDo in Switch or D-input mode, PlayStation, Switch Pro, most USB/Bluetooth pads) work through SDL2, which setup downloads (`build\SDL2.dll`). Buttons go by position: the bottom face button is A, as on an Xbox pad.

Players are numbered in the order controllers connect, whatever kind they are, and they can be plugged in or out at any time. With Debug logging on, `hitz_stderr.log` and `hitz_perf.log` name each controller and its player (`[PAD] player 1: ...`).

For two players on one machine, plug in two controllers. `run_hitz.bat` sets `RECOMP_USB_PADS=2`. On the team-select screen each player pushes the stick toward **Away** or **Home**, then presses **A**.

**Keyboard** (`set RECOMP_KEYBOARD=1` before running, player 1):

| Key | Pad |
|---|---|
| W A S D / keypad | left stick |
| Arrow keys | D-pad |
| Z / X / C / V | A / B / X / Y |
| Q / E | White / Black |
| Enter | Start |
| Backspace | Back |

---

## Settings (environment variables)

You only need these to change behaviour or to debug. `main.c` and `run_hitz.bat` already set the ones the game needs. Anything you set yourself overrides the default.

**Needed (set by default):**

| Variable | Default | What it does |
|---|---|---|
| `RECOMP_PB_EXEC` | 1 | run the GPU pushbuffer executor |
| `RECOMP_PB_WALK` | dma | follow the pushbuffer like the hardware. The older segment scan hangs in heavy scenes. |
| `RECOMP_VBLANK` | paced | real-time 60 Hz vblank |
| `RECOMP_FLIP_PACING` | 1 | flips wait for vblank: 60 fps and correct game speed |
| `RECOMP_TITLE_KEVENTS` | 1 | lets the GPU signal D3D's interrupt event |
| `RECOMP_AC97_READY` | 1 | audio codec ready |
| `RECOMP_FB_WINDOW`, `RECOMP_D3D_PRESENT` | 1 | open the game window and present to it |
| `RECOMP_USB` | 1 | emulated USB controllers |
| `HITZ_GPU` | 1 | D3D11 renderer. 0 = the toolkit's CPU rasteriser, about 1 fps. |

**Optional:**

| Variable | What it does |
|---|---|
| `RECOMP_USB_PADS=1..4` | number of controller ports (default 2; the launcher's Players setting) |
| `RECOMP_KEYBOARD=1` | keyboard as pad 1 |
| `HITZ_GPU_CULL=1` | back-face culling on the GPU (the CPU path never culled; try it if a scene looks wrong) |
| `HITZ_GPU_BC=0` | decode DXT textures on the CPU instead of uploading them compressed (debug) |
| `HITZ_GPU_FASTDECODE=0` | use the toolkit's per-texel decoder for uncompressed textures (debug) |
| `HITZ_TEX_VERIFY=1` | check the fast decoder against the toolkit's for the first textures of each format |
| `HITZ_GPU_READBACK_POLL=1` | old non-waiting frame readback (dropped/repeated frames on some drivers) |
| `HITZ_PACING=timer` | don't lock the frame clock to the display (the old free-running 60 Hz) |
| `HITZ_REFRESH_SWITCH=0` | never change the monitor's refresh rate in fullscreen |
| `HITZ_DEBUG=1` | debug logging: `hitz_stderr.log` (from the launcher or `run_hitz.bat`) and `hitz_perf.log`. Default 0: no log output at all |
| `HITZ_PERF_LOG=0/1` | `hitz_perf.log` regardless of `HITZ_DEBUG` |
| `HITZ_SDL_PADS=0` | only Xbox (XInput) controllers |
| `HITZ_GPU_ASYNC_SHADERS=0` | compile new shaders on the render thread (stalls a frame each, as before) |
| `HITZ_SHADER_CACHE=0` | don't keep compiled shaders in `hitz_pro\cache\` |
| `HITZ_LOG_DIRECT=1` | write the log directly instead of through the background writer |
| `HITZ_GPU_ADAPTER=<text>` | pick the GPU whose name contains the text |
| `HITZ_GPU_VSH=0` | vertex programs on the CPU instead of the GPU |
| `HITZ_GPU_UBER=1` | one general pixel shader instead of per-state ones (slower; for checking) |
| `HITZ_GPU_SYNC_READBACK=1` | old synchronous render-target readback |

**Debugging:**

| Variable | What it does |
|---|---|
| `HITZ_GPU_PROF=1` | per-draw GPU timings |
| `HITZ_GPU_TRACE=1` | dump all draws of one light frame every 5 s |
| `HITZ_GPU_FLATPS=1` | vertex colour only (separates geometry bugs from shading bugs) |
| `HITZ_HOSTPROF=1` | sampling profiler writing `C:\proj\hprof.txt`; resolve it with `tools/hostprof_resolve.py` |
| `HITZ_DIAG=1` | bring-up diagnostics in `recomp_manual.c` |
| `RECOMP_DRAW_LOG`, `RECOMP_PATH_LOG`, `RECOMP_READ_LOG`, `RECOMP_GPU_REPORT` | verbose toolkit logs (slow) |
| `RECOMP_WATCHDOG_SECS=N` | dump guest state if the game hasn't exited after N seconds |
| `RECOMP_PAD_SCRIPT=<ms>:<button>[:hold],...` | scripted input for unattended tests, e.g. `58000:start,64000:start,72000:a`. `p2-` targets pad 2. |

---

## Changing things

- **Overriding a game function:** define `void sub_XXXXXXXX(void)` in `hitz_pro/src/recomp_manual.c`, call `sub_XXXXXXXX_gen()` for the original, then rerun step 4 (with `--exclude-manual`) and rebuild. Guest registers are the globals `g_eax`, `g_esp` and so on; guest memory is `MEM32(addr)`.
- **Toolkit changes:** edit in `xboxrecomp/`, then refresh both copies of the patch:
  ```sh
  cd xboxrecomp && git diff > ../xboxrecomp-hitz.patch && cp ../xboxrecomp-hitz.patch ../hitz_pro/toolkit/
  ```
- **Crashes:** the crash handler prints the exe base and RVAs. Map them to function names with
  `python tools/rva2sym.py hitz_pro/build/hitz_pro_recomp.map <rva> ...`.
- Read `NOTES.md` before changing anything in D3D, the pushbuffer or the fences; it explains why each fix exists.

## tools/

The scripts in `tools/` come from the original dev setup: a Windows VM on Linux with GPU passthrough. They are machine-specific; use them as references.

| Script | What it does |
|---|---|
| `vmbuild.sh`, `runvm.sh`, `runsum.sh` | build in the VM, do a timed run, summarise the run |
| `hostprof_resolve.py`, `hp_diff.py`, `rva2sym.py` | profiling and crash symbolisation (portable) |
| `gpu-vfio-boot.sh`, `win11-gpu.sh`, `mk_vfct.py` | AMD iGPU passthrough to a dockurr/windows VM |

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nznet and contributors. The patch here is a derivative of it; see its licence.
- NHL Hitz Pro © Midway / NHL / NHLPA. This repo contains no game code or assets.
