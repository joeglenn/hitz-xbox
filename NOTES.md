# NOTES — append-only, dated findings

## 2026-10-31 — Initial pipeline run (Igor/Hermes)

- Source: retail disc image dumped by the owner, extracted with extract-xiso
  2.7.1 → `game/` (427 files). default.xbe 3,883,008 bytes.
- XBE identity: "NHL Hitz Pro", title 0x4D57001A, retail, XDK 5558,
  build path `d:\project\hitz2004\xbox\out\hitzr.exe`, built 2003-08-14.
  Base 0x00010000, entry 0x001DB5E3, kernel thunk table 0x002FEA00.
  TLS: index at 0x00596AE8, zero-fill 144 bytes.
- Disassembly: 130.5 s, **15,902 functions** (xrefs + labels + strings DBs in
  `xboxrecomp/tools/disasm/output/`).
- func_id: engine is NOT RenderWare (RW modules = 0). Heavy C++ use:
  7,750 game_vtable functions; 4,441 vtable_thunks + 2,721 vtable_scan +
  588 vtable_ctor. Midway in-house engine, XDK sections for D3D/D3DX/XGRPH/
  DSOUND/WMADEC/DOLBY linked statically.
- abi_analysis: 15,902 entries, **2,377 thiscall** — C++ this-heavy code,
  recomp must consume `abi_functions.json` (it warns once and silently falls
  back to cdecl/0-args otherwise — check the recomp log for that warning).
- recomp: `--all --split 250` into `hitz_pro/src/recomp/gen/`. Log:
  `~/hitz-xbox/recomp_run.log`. 15,952 functions loaded / 15,902 analyzed.
- Project glue configured: `hitz_pro/src/main.c` entry 0x001DB5E3,
  XBE path `game\default.xbe`, dir `game`; CMake project name
  `hitz_pro_recomp`; `XBOXRECOMP_DIR=../xboxrecomp` (siblings in ~/hitz-xbox).
- Xbox version note: unlike the PS2 build, no overlay system — plain PE/XBE
  sections. WMADEC + DOLBY sections suggest WMA/XMA audio via in-XBE decoders.
- win11 VM on odin (docker `win11`) already up and provisioned for the PS2
  Hitz project; VS2022 Build Tools + SDK should be present — verify via
  `C:\OEM\setup.log` before first MSVC build.

## 2026-10-31 — First Windows compile attempt (VM, MSVC 14.44)
- Configure OK (Ninja, VS2022 BuildTools, C:\proj\hitz_pro). Gen code compiles
  through 96/152 objects, stops on C2065 dr4 (2 sites, recomp_0027.c
  30193/31033) — documented in CLAUDE.md "Known issues". All other warnings so
  far: C4293 shift-count (benign).

## 2026-10-31 — dr4 blocker fixed at toolkit level (Igor/Hermes)
- `xboxrecomp/tools/recomp/lifter.py` `_lift_mov` (uncommitted diff on top of
  tag v0.16.0 / f0f8a46, +8 lines; see `git -C xboxrecomp diff`): `mov drN/crN, x`
  now emits a no-op comment; `mov x, drN/crN` writes 0. Backup of original:
  odin `/tmp/lifter.py.bak`.
- Regen (44.6 s, log `recomp_run2.log`): 0 failed, only `recomp_0027.c` changed
  (lines 30193/31033 now `/* mov dr4, ...: system register write ignored */`).
  Pre-fix gen kept in `/tmp/gen.pre-drfix`.
- Pushed recomp_0027.c to VM `C:\proj\hitz_pro`, rebuild → `C:\proj\build2.log`.

## 2026-10-31 — Boot #1 (Windows VM) + full-section disassembly (Igor/Hermes)
- Copied `game/` into VM `C:\proj\hitz_pro\game` (local VM on odin). Runner
  `C:\proj\run_xbox.ps1` (schtasks /it, screenshots q10s → `C:\proj\run\`).
  Logs pulled to `~/hitz-xbox/runs/boot001/`.
- Boot #1: XBE load, memory map, 158/158 kernel thunks, XAPI init all OK
  (DVD auth MODE SENSE, TDATA/UDATA 4d57001a, TitleMeta/TitleImage, cache
  partition, contentmeta.xbx not-found = normal). Then ICALL "Failed to resolve"
  for 0x2A6BCC/0x2A6BD8/0x2A6BE4 (XGRPH), 0x2BCA68/0x2BCA73/0x2C04FD/0x2C0508
  (DSOUND), 0x2F8322 (XPP) → ~200k null-ICALL skips (ordinals 277/294 crit-sec
  spin) → AV reading guest VA 0xFFFFFFFD, exit after 10 s.
- ROOT CAUSE: pipeline was run with `tools.disasm --text-only`, so the XDK
  library sections had only 286 functions in total. Re-ran disasm WITHOUT
  --text-only (610 s, sections .text,XMV,D3D,D3DX,XGRPH,DSOUND,WMADEC,XPP,DOLBY):
  **19,229 functions** (was 15,902). func_id + abi_analysis + recomp re-run:
  19,245/19,245 lifted, 0 failed, 77 files, 1.73M lines, **16 unresolved stubs
  (was 317)**. 628 unimplemented opcodes now (outsb/arpl/insd... = data lifted
  as code in library sections, emitted as no-op comments; watch for them only if
  a trace lands there).
- Old text-only outputs backed up in `~/hitz-xbox/backup_textonly/`.
- Logs: pipe_disasm.log, pipe_funcid.log, pipe_abi.log, recomp_run3.log.

## 2026-10-31 — Boots to title screen on Windows (Igor/Hermes)
- Boot #3 (watchdog): hang in D3D CreateDevice, fence wait sub_00254000
  (D3D_BlockOnTime; device ptr 0x00269608, compares dev+0x2C vs *dev+0x30),
  NV2A USER PUT=GET, executor not running.
- Boot #4 (RECOMP_PB_EXEC=1): executor ran, saw 0x1D70 semaphore releases,
  but had no target → still hung.
- FIX: `hitz_pro/src/recomp_manual.c` defines `sub_00254000` wrapper → hands
  `*(dev+0x30)` to `nv2a_pb_set_semaphore_target()` (re-checked each call), then
  calls `sub_00254000_gen`. Regen now uses `--exclude-manual
  ../hitz_pro/src/recomp_manual.c` (log: "1 wrapped as sub_X_gen"). Backup:
  recomp_manual.c.pre-fence. Semaphore = 0x80001000, device 0x00269610.
- Boot #5: past D3D, loads loading_image*.xbt, sound.hdr/dat, fe/*.db, Heads.
  Rendering in guest memory but no host window (RECOMP_FB_WINDOW unset).
- Boot #6 (runs/boot006): env RECOMP_PB_EXEC=1 RECOMP_VBLANK=paced
  RECOMP_AC97_READY=1 RECOMP_FB_WINDOW=1 RECOMP_D3D_PRESENT=1 → window
  "NHL Hitz Pro", D3D11 present, attract intro then **NHL HITZ PRO title
  screen, correct colours, ~53 fps**, 120 s with no crash. Software D3D (VM has
  no GPU).
- Remaining known: ICALL fail 0x002F673C (XPP section, once at D3D init),
  `wbinvd` no-op, 149 distinct unhandled NV2A methods (no visible effect yet),
  APU DSP in passthrough. Next: input (keyboard/XInput → pad) to get into
  menus + a game; audio check.
- Launcher with those env defaults: `hitz_pro/run_hitz.bat`.
- Online recon: XBE has NO XOnline/XNet/socket code (only the stock
  `xonlinedash.xbe` XAPI string and an unused `h_selectteams_online.fng` FE
  leftover from the shared PS2 codebase). The Xbox release has no Live/System
  Link mode, so there is no Live protocol to emulate.

## 2026-10-31 — Input working, match loads, audio produced (Igor/Hermes)
Toolkit (`xboxrecomp/`) changes, uncommitted on v0.16.0 (`git -C xboxrecomp diff`):
- `src/usb/ohci.c`: `bus_resolve_desc()` for ED/TD/HCCA (rd32/wr32): a page
  currently allocated in the contiguous window is the window. Hitz's contig
  physical numbers (0x402000...) overlap the XBE image range, and the
  image-first rule read the control ED out of .data as zeroes → enumeration
  stalled after port reset. Data buffers (`bus_resolve`): an in-image address is
  the image only if MmGetPhysicalAddress named it so, or no live window block
  covers it (else SETUP packets read as zeroes).
- `src/kernel/xbox_memory_layout.c` + `kernel_memory.c`: `xbox_PhysNoteImageName`
  / `xbox_PhysImageNamed`, recorded in MmGetPhysicalAddress.
- `src/kernel/nv2a_pb_exec.c`: latch method 0x0310/subch5 into PGRAPH 0x400B10
  also when a semaphore target is set (not only RECOMP_PB_WALK=dma). Hitz XDK
  5558 KickOff = (pb<<5)|(fence&0x1F); sub_00253C70 spins on
  (latch ^ sem) & 0x1F → froze on match load.
- `src/kernel/kernel_file.c`: XBOX_FILE_NO_INTERMEDIATE_BUFFERING no longer
  mapped to FILE_FLAG_NO_BUFFERING (Win32 then needs sector-aligned buffers;
  Hitz reads to 0x0226A626 → err 87 → "problem with the disc" screen). Plus a
  logged bounce-buffer retry when ReadFile fails with ERROR_NOACCESS.
Project:
- `hitz_pro/src/main.c` now calls `xbox_OhciInit()` (nothing in the toolkit
  does; without it RECOMP_USB has no effect). Backup main.c.pre-ohci.
- Env additions: RECOMP_USB=1. Pad enumerates on HC0 port 3, ~250 reports/s;
  RECOMP_PAD_SCRIPT drives menus → title → team select → match loading
  (Red Wings vs Oilers). Keyboard path via the FB window: not yet tested.
- Audio: APU produces signal (RECOMP_AUDIO_WAV capture, runs/boot008/audio.wav,
  RMS ~400-1200 from t=30 s). VM has no audio device (waveOutOpen err 2), so
  live playback is untested; quality to be checked by ear.
- Runner: `~/hitz-xbox/tools/runvm.sh <name> <secs>` (env from /tmp/run_env.txt
  → C:\proj\run_env.txt; results in runs/<name>/).
OPEN (boot016): attract-mode in-game demo starts, but the 3D view is black
(only the 2D "DEMO" overlay draws), ~5-10 fps, then AV at 131 s:
sub_000142C0 → D3D sub_002501D0 (DrawIndexedVerticesUP-style index copy)
with index ptr [esi+8] = 0xC4F6B459 from mesh data in sub_00015C60
(edi=[ecx+0x2C], esi=[edi+0x4C]). Corrupt or un-fixed-up model data; suspect
asset loading/relocation or a lifter bug. Next step.

## 2026-10-31 — Attract demo renders the 3D rink (async-loader ordering fixed)

**Symptom:** demo match showed only the "DEMO" overlay over black, ran at 5-10 fps and
crashed at ~131 s in index copying (`sub_00016140` via `sub_000874C0`, mesh table `[0x36E924]`).

**Cause 1, empty async packs.** `sub_0015C2A0` queues a pack read on the game's I/O layer
(`sub_001CA200`, completion `sub_0015C3D0(fileobj, job)`) and only after that call returns
writes the buffer header pointer `job+0x21C` that the parser (`sub_0015C120` -> `sub_0015CDD0`)
reads. On the console the DVD read is still pending at that point. Here NtReadFile completes
synchronously and the completion ran inside the submit, so the parser saw data=8/hdr=0 and
every async .xbx/anim pack came out empty. `HITZ_SYNC_LOAD=1` (forces the sync path through
flag byte 0x37B5CD) confirmed it: the packs filled and the crash moved.

**Cause 2, crowd loaded 20 times.** The match-load handler `sub_0002A4C0` queues batches through
`sub_00028AC0` (pending counter `0x3FCDFC`++). Each completion `sub_00028A70` decrements it and
posts msg 0x117 at 0, and the 0x117 handler loads `stadium\NewCrowd.pck` (msg 0x116 on done).
With per-file completion the counter hit 0 after every file: ~20 crowd loads exhausted the
64 MB contiguous arena (`frees=0`, alloc refused for caller 0x001DD802), then an AV on the null result.

**Fix (recomp_manual.c):** thread-local deferral. Completions (`sub_0015C3D0`) that arrive while
the thread is inside `sub_0015C2A0` or `sub_0002A4C0` are queued and run when the outermost one
returns. `HITZ_NO_DEFER=1` disables it.

**Result, boot035 (build24):** crowd loaded once, contig peak 32 MB, no crash in 260 s. The demo
renders the Islanders rink, boards, crowd and ice logos, then returns to the title (normal
attract loop).

**Open:** the 3D scene runs at ~1 fps (4,300 draws/frame). The NV2A executor rasterises on the CPU
(4 raster threads in the 8-core VM), and no D3D11 back end is registered (`nv2a_backend.h`
supports one; only a test back end exists). Next: profile vertex-program interpretation
against rasterisation, then decide on a D3D11 `Nv2aBackend`. Diagnostic wrappers
(MESH/DRAWIDX/PACK/ASYNC/PARSE/C120/MSG/LOADCNT) are still in recomp_manual.c; remove them.

## 2026-10-31 — CPU geometry profile in the demo; vertex cache added

Added `[PERF]` per-report timing in `nv2a_pb_exec.c` (`raster_batch_program`). Demo scene (boot036):
about 1M indices per frame and about 1 fps. Per 10 s window: vertex programs 2.0 s, raster 6.0 s.
Added a per-batch post-transform vertex cache (direct-mapped by index; `RECOMP_NO_VCACHE=1` turns it off).
boot037: vertex programs 1.1 s per window, raster still 6.7 s, fps still about 1, no crash.
**The CPU rasteriser is the bottleneck.** Next: a D3D11 `Nv2aBackend` (`nv2a_backend.h`), so triangles
go to the GPU (the present path already uses D3D11). Vertex programs stay on the CPU for now (about 100 ms/frame).

## 2026-10-31 — Host profile of the 3D demo: CPU pixel shading is the wall

Added `hitz_pro/src/hostprof_win.c` (`HITZ_HOSTPROF=<file>`). It suspends threads about every 1 ms and records RIP into a
histogram, rewritten every 10 s. Linked with `/MAP`; resolved offline by `hostprof_resolve.py` in the scratch dir
(diff two snapshots to isolate a phase). boot040, 3D demo phase:
- NV2A thread: about 78% per-pixel work (sample_tex 17%, nv2a_rc_eval 11%, read_rgb 7%, xf_rows 6%, rc_unpack 5%,
  put_pixel 4%). nv2a_vsh_run about 6%. The rest is waiting.
- 3 raster workers: about 55% busy on the same functions.
- Guest D3D thread: sub_00253E30 (waiting on the GPU).
Workload: about 4.0M pixels written plus 0.7M depth-fails per frame (13x the 640x480 screen; offscreen 640x480 targets
0x00404000 and 0x00788000 are drawn every frame, likely ice reflection/effects), about 4,300 draws and 1M indices.
The CPU shades about 4M combiner pixels/s, so about 1 fps. 60 fps needs about 240M px/s, which is out of reach for the
interpreter. **Full speed needs a GPU renderer.** The VM's adapter is "Red Hat VirtIO GPU DOD" (display only, so D3D11 =
WARP), which means GPU speed can't be measured in the VM.

## 2026-11-01 — GPU passthrough + D3D11 back end (HITZ_GPU=1) working

**GPU passthrough** (details in skill reference windows-vm-testing.md §4): odin's 8060S runs in the
win11 VM on a one-shot vfio boot entry (`sudo tools/gpu-vfio-boot.sh once && sudo reboot`, then
`tools/win11-gpu.sh gpu`; a normal reboot returns the GPU to the host). Runtime amdgpu unbind
oopses on 6.17-oem (isp_genpd_remove_device), so don't use it. Code 43 was cleared by: a PCIe root port
with multifunction GPU+audio, AMDGopDriver.rom on the audio function, and a guest VFCT from
`tools/mk_vfct.py` (guest 01:00.0). Driver: Adrenalin 26.9.2 display INF via pnputil.
dxdiag reports 8060S, FL 12_2.

**Toolkit (nv2a_backend.h / nv2a_pb_exec.c):** new `draw_x` callback with full-state batches
(Nv2aBatchX: Nv2aXVertex = pos, d0, d1, fog factor, tex[4][4]; four Nv2aStage; combiner state;
render state), extended Nv2aSurface (clip origin, format, swizzle, zeta bits), and
`nv2a_backend_decode_stage()`. The screen-space path is widened into it too.

**Project: hitz_pro/src/nv2a_d3d11.c** (registered by `HITZ_GPU=1` from main.c):
- D3D11 device on the first non-Microsoft adapter (`HITZ_GPU_ADAPTER` overrides).
- Pixel shader: a direct HLSL port of nv2a_rc_eval (uber-shader; combiner words in a cbuffer),
  plus stage modes NONE/2D/3D/cube/passthru/clip-plane and alpha test.
- Render targets and depth (D32S8) on the GPU, keyed by guest address. A texture at a recently
  drawn render target samples the target directly. Textures are cached by key plus a per-frame
  content hash.
- Blend, depth, stencil and sampler state objects are cached.
- At flip, dirty linear targets are read back into guest memory, so the presenter and screenshots
  are unchanged.
- HLSL is checked offline with fxc in the VM (C:\proj\fxc.ps1).
- Stats are printed as `[D3D11]`.

**Results (boot043/045):** the demo frame matches the CPU renderer visually (arena, crowd, lighting,
overlay). 3D demo: 1 fps (CPU) -> 2-3 fps; menus 300+ fps.

**Bottleneck now (host profile with module names, boot046):** the executor thread spends ~3.0 s per
10 s in CPU vertex programs (~1M vertices/frame) and ~2.5 s building per-triangle Nv2aXVertex
(~180 MB memcpy/frame). The rest is idle waiting on the guest (the guest's main thread waits in the
D3D kick/flush loop sub_00253E30). **Next: run the vertex programs on the GPU** (port
nv2a_vsh_interp to an HLSL interpreter; upload raw attribute ranges, indices, program and
constants per batch; hardware clipping with DepthClip on). The CPU path stays for FFP and
`cxt_write` programs.

## 2026-11-01 — Vertex programs on the GPU (draw_vsh)

**Toolkit:**
- `Nv2aBatchVsh` / `draw_vsh`: raw attribute ranges for the batch's index range, a triangle-list index
  array relative to `first`, the program from the start slot, the 192 constants, fog parameters and AA scale.
- The executor fast path is `backend_vsh_batch()` in raster_batch_program. It falls back to the CPU for
  inline arrays, cxt_write programs and attributes outside guest memory.
- Accessors added: `nv2a_vsh_program_words` / `nv2a_vsh_constants` / `nv2a_vsh_cxt_write`.

**Back end:**
- `vs_prog` is an HLSL port of nv2a_vsh_run, including the colour clamp, the w=0 rule, AA scale and the fog unit.
  Attributes are fetched from a raw ByteAddressBuffer in NV097 types 0/1/2/4/5/6.
- Overlapping (interleaved) attribute ranges are merged and copied once into a 64 MB ring; indices go into a 16 MB ring.
- Program constants are uploaded per draw; the program only when its hash changes.
- Hardware depth-clip does the near-plane clipping.
- `HITZ_GPU_VSH=0` keeps vertex programs on the CPU.

**HLSL pitfalls (fxc):**
- A loop that writes `float4[i]` with a dynamic `i` must be unrollable: use a fixed bound plus a guard.
- Don't `[unroll]` a loop around a function that contains a `[loop]`.
- `pass` is reserved.
- Switch cases need break/return.

**Results (boot047):**
- 3D demo at 5.6 fps (from 2.7). 4,296 of 4,377 draws/frame take the GPU vertex path.
- CPU vertex time: about 3,000 ms down to 40 ms per 10 s window. Back-end draw CPU: 80 down to 11 ms/frame.
- The image matches the CPU renderer (arena, scoreboard, crowd, lights).
- Now limited by the 41 ms/frame flip readback (Map waits on the GPU, then a copy to guest memory) and by
  the guest<->executor handshake. The executor is idle ~85% while the guest spins in the D3D kick/flush
  loop sub_00253E30.

**Next:**
1. Present directly from the GPU render target (no per-flip readback; read back only when the CPU needs the memory).
2. Find out why kickoffs wait (flip hold, ack latency).
3. Add GPU timestamp queries for real GPU frame time.

## 2026-11-01 — Toward 60 fps: kick/fence handshake, GPU notify interrupt, logging

**Async readback + GPU timers (nv2a_d3d11.c):**
- Render targets are copied at a flip and written to guest memory one flip later (`HITZ_GPU_SYNC_READBACK=1` restores the old behaviour).
- GPU frame time comes from timestamp queries. `HITZ_GPU_PROF=1` adds per-draw GPU time and pipeline statistics.

**Specialised pixel shaders:**
- Each distinct combiner/stage/alpha-test state compiles to a literal-constant pixel shader (`ps_spec`, cached, ~27 per run, ~700 ms total compile).
- The uber-shader is about 1,760 instructions with indexable temps; a specialised one is about 18. GPU draw time dropped from ~45 to ~15 ms/frame.
- `HITZ_GPU_UBER=1` forces the uber-shader.
- Constant-buffer ring (D3D11.1 offsets) added; on its own it made no measurable difference.

**D3D kickoff (sub_00253E30):**
- Every kick set the PFB flush bit (0x100410 |= 0x10000) and spun until the executor's tick cleared it.
- The executor clears it only between segment walks, so each of ~150 kicks per frame waited ~5 ms.
- `src/recomp_kick.c` is the generated body with the flush completing at once, plus `_mm_mfence` before PUT. `HITZ_KICK_WAIT=1` runs the original.
- sub_00253CF0 has the same spin; it was not changed yet.

**GPU notify interrupt:**
- Once kicks stopped waiting, the game ran ahead into D3D's fence wait sub_00254000. For a fence the GPU is far behind on, D3D writes WAIT_FOR_IDLE plus NO_OPERATION(5) and sleeps on the KEVENT at device+0x1DBC, which its GPU ISR sets. Nothing set it, so the intro froze.
- My first fix (poll the semaphore and kick from the wrapper) was wrong. It called KickOff from inside D3D's own KickOff/MakeSpace, publishing PUT mid-update, so the walker desynced and the game crashed or exited at 131 s.
- Correct model (toolkit):
  - `nv2a_pb_set_swmethod_callback()` runs on the executor thread when a NO_OPERATION with a non-zero parameter is reached.
  - `xbox_SignalGuestEvent(va)` sets a title-built KEVENT (`RECOMP_TITLE_KEVENTS=1`) or a KeInitializeEvent shadow.
  - Project: `hitz_gpu_swmethod()` sets device+0x1DBC on parameter 5. RECOMP_TITLE_KEVENTS now defaults to on in main.c.

**Walker bounds:**
- pb_walk_segment now drops a segment whose walk leaves guest RAM, instead of reading host memory (it was crashing at guest VA 0x8F0x0100).
- Still seen about twice per 4 minutes (e.g. a CALL target 0x85AA05A8). Not yet explained.

**Logging was the bottleneck:**
- stderr is unbuffered on Windows; every line was a synchronous write to the VM disk under the CRT lock.
- The executor spent 42% of its time and the guest 16% blocked in fprintf. ~17.5k lines per 4 min came from `[GPU] draw #`, `[PATH]`, `[READ]` and the DIAG wrappers.
- Now: 1 MB buffered stderr with a 250 ms flusher thread (`HITZ_LOG_UNBUFFERED=1` reverts), and toolkit traces gated by `RECOMP_DRAW_LOG`, `RECOMP_PATH_LOG`, `RECOMP_READ_LOG`.

**Frame pacing:** `RECOMP_FLIP_PACING=1` gives menus at ~56–60 fps. Without it the game ran uncapped (1,100+ fps), which breaks game speed and pad-script timing.

**Result (boot074):**
- Heavy 3D demo (~3,200 draws, 630–750k triangles per frame) at 32–35 fps; it was 8–15 fps.
- No crashes over 240 s. Image correct.

**Tools:**
- `tools/rva2sym.py` resolves the crash handler's RVAs (the crash handler now prints the exe base and RVAs; ASLR had defeated the old fixed-range filter).
- hostprof now does a real x64 unwind (RtlVirtualUnwind) to the first exe frame (`(wait) in X`). `HITZ_HOSTPROF_NOSTACK=1` turns that off; `HITZ_HOSTPROF_HEAVY=1` samples only frames with more than 1,000 draws.

## 2026-11-01 (later) — Stable, paced, playable: ready for the netplay add-on

**Hang fixed (heavy 3D froze about 3 times in 4 on the segment scan):**
- The default pushbuffer path (segment scan) set DMA_GET to PUT before D3D had finished writing the fence plus WAIT_FOR_IDLE plus NO_OPERATION(5) sequence. Fence waits then missed their notify, and Present slept forever on device+0x1DBC.
- The DMA walk (`RECOMP_PB_WALK=dma`, which follows JUMPs and publishes GET per command) together with the software-method interrupt has no such race.
- The flip hold stops the DMA walk at a held FLIP_STALL (`nv2a_pb_exec_flip_held()`).
- The ack thread resumes the walk after the vblank even if PUT did not move (`nv2a_pb_dma_pending()`), and publishes the walk's own GET (`nv2a_pb_dma_get()`).
- Without the resume the game freezes at boot. Without the stop, `RECOMP_FLIP_PACING` is ignored and menus run at about 2,000 fps.
- main.c now defaults `RECOMP_PB_WALK=dma`, `RECOMP_FLIP_PACING=1` and `RECOMP_TITLE_KEVENTS=1`. An explicit environment setting wins.

**2D screens black on the GPU (title, menus, legal screens):**
- My draw-queue `ring_alloc` aligned with a power-of-two mask, but draw_x asked for alignment `sizeof(Nv2aXVertex)*16` = 1856. Vertex offsets were not multiples of the stride, so `Draw(start = off/stride)` read garbage.
- Now it aligns the absolute offset with any alignment.
- Found with `HITZ_GPU_FLATPS=1`: the full-screen white quad was missing, so the problem was geometry, not shading.
- Tool: `HITZ_GPU_TRACE=1` dumps every draw and clear of one light frame every ~5 s.

**Draw queue (nv2a_d3d11.c):**
- Draws are recorded into CPU-side windows of the GPU rings (constants, draw_x vertices, raw attributes, indices) and replayed at flush with redundant state filtered.
- It flushes on clear, flip, and on any texture, RT or DS release.
- Texture cache uses bucket chains. Readback never blocks (`DO_NOT_WAIT`), and targets sampled as textures are not copied out.
- `RECOMP_GPU_REPORT=1` gates the executor's 10 s survey.

**Input:**
- Xbox A is analog; the pad script sends 0xFF for 150 ms, which works.
- Team select needs the stick pushed toward Away or Home before A assigns that controller.
- Two pads: `RECOMP_USB_PADS=2`, script steps prefixed `p2-`. USER2 assigned to Home and played (boot100, boot101).
- Once (boot098) the p2 steps never fired and I could not explain it. The next runs, with a first-report log line added, were fine.
- Working path into a match: start, start, a (Exhibition), lleft + p2-lright, a, then a's through jerseys and loading.

**Results:**
- Exhibition match playable. The game clock runs at real time (10 game seconds per 10 real seconds); gameplay holds 58–60 fps and the faceoff, live play and fight cutscene render correctly.
- Attract demo heavy 3D at 48–60 fps.
- Soaks (boot086/087/094–101): no crashes, no hangs, no pushbuffer walks out of guest RAM.

**Diagnostics:** the bring-up wrappers in recomp_manual.c (mesh/pack/drawidx/message) now run only with `HITZ_DIAG=1`.

**Tools:** `tools/vmbuild.sh <n> files...` builds and fetches the map. `tools/runsum.sh <boot>...` prints a one-line summary per run. Only one VM run at a time: runs share the VM output folder.

**Netplay starting point:** two local pads already drive the game through `usb_gamepad_report(pad, ...)`. The pad-script hook (`pad_script_apply`) is where remote input gets injected. Game time is paced by the guest vblank clock (`RECOMP_VBLANK=paced` + flip hold), which is the frame boundary for exchanging inputs.

## 2026-11-01 (night)
Ice smear fixed: inline-array layout sized short attributes at 4 bytes/component (draw_inline_array now uses attr_elem_bytes, dword padded). Layout seen: s16x2 pos + D3DCOLOR + ub2 = 12 bytes (was laid out as 16). Same bug class as xemu #1850. Verified on an identical replay: smear in ~half of in-match shots before, none in 24 after; 60 fps.

## 2026-11-01 (late): smooth motion, non-Xbox controllers, launcher, performance log

- **Stall every 2 s found and fixed.** The perf log (below) showed a 45-180 ms freeze about every 2 s in every match, followed by catch-up frames; the 1-second fps average hid it (it always read 60). The toolkit's kernel summary (7 stderr lines every 2 s, written from the game thread) was the cause: stderr was unbuffered (WinMain's `setvbuf(_IONBF)` undid `hitz_log_buffering`), several threads printed the summary at once, and each write waited on the log's reader. Fix: fd 2 is now an in-memory pipe drained by a background thread (`hitz_log_async` in main.c; crash handler drains it); the summary is printed by one thread. Same 150 s scripted match: hitches after 30 s 89 -> 5, in-match frames 15.3-17.6 ms.
- **Display-locked frame clock.** The presenter's display-clock thread (IDXGIOutput::WaitForVBlank) reports each vblank; when the refresh is a 60 Hz multiple, `xbox_VblankDiscipline` steers the vblank schedule onto it (period = display period x N, edges 1.5 ms after a display vblank, an eighth of the error per report, max 0.2 ms). RECOMP_PACING=timer disables. Not testable in the VM: its virtual display measures 62.5 Hz (not lockable), so the log there says free-running.
- **Presenter:** flip-model swap chain (FLIP_DISCARD, latency 1; VRR works in borderless fullscreen), each game frame presented once on its flip (event from `xbox_FramebufferWindowPresent`; free-running fallback after 100 ms without flips, for movies). Fullscreen refresh switch: on a non-60-multiple display, ChangeDisplaySettingsEx(CDS_FULLSCREEN) to the highest 60 Hz multiple at the same resolution; restored on leaving fullscreen/exit.
- **Shaders:** specialised pixel shaders compile on a background thread (draws use the general shader until ready) and are kept in `cache\hitz_shaders.bin` (keyed by a hash of the shader source). Cold run: 5 compiled in the background, no stalls; warm run: 5 loaded in 1 ms, 0 compiled.
- **Controllers:** `sdl_pads_win.c` binds SDL2.dll at run time (no build dependency); one thread owns SDL and publishes snapshots; Xbox-type pads are left to XInput. Ports are assigned in connection order across XInput and SDL (so a pad in XInput slot 2 is still player 1) by a 1 s scanner thread, off the input path (the old code polled every empty XInput slot on every read). setup.py downloads SDL2 2.32.10 (sha256-pinned). Untested with a real non-Xbox pad (the VM has none).
- **Performance log** `hitz_perf.log` (toolkit `platform/perf_log.c`): header (OS, CPU, GPU, settings), HITCH lines (frame > 1.5x the recent median and > 25 ms, with shaders/textures/files during it), JUDDER (frame statistics, when available), 5 s summaries, slow kernel calls (> 8 ms, non-wait ordinals), F9 MARK lines.
- **Launcher** `HitzLauncher.exe` (src/launcher/hitz_launcher.c, Win32/comctl32): settings -> hitz_settings.ini -> env vars -> game with stderr to hitz_stderr.log; hides while the game runs. Tested in the VM: window lays out cleanly, Play started the game with the INI's settings (perf log header shows them).

## 2026-11-02: frozen "LOADING" picture before a match; GPU auto-pick

- Reported: after picking teams the window stayed on LOADING while the game (and music) ran. Reproduced in the VM's attract demo with both the old and new builds (~40 s of identical frames, then the match). The pre-game intro samples the full-screen frame it presents; be_flip treated every sampled target as GPU-only (no readback), so guest memory, which the window shows, kept the last loading frame. Full-screen targets (>= 640x448) are now always read back. VM attract run: every 10 s shot differs through the intro.
- Default GPU: the hardware adapter with the most dedicated video memory, not the first listed (laptops list the iGPU first; the launcher's empty HITZ_GPU_ADAPTER also matched adapter 0). Each adapter's VRAM is logged.
- Jitter in matches on an RTX 4090 laptop: the async readback polled with DO_NOT_WAIT and missed on ~half the in-match flips (late 213-310 per 10 s), each miss dropping or repeating a displayed frame while the game ran at 60. Copies are now flushed when queued and mapped with a wait one flip later (HITZ_GPU_READBACK_POLL=1 = old polling). VM attract run: late 0 throughout, map wait 0.1 ms/frame, 60 fps.
- Perf log: per-frame time breakdown (tex convert+upload, texture change hashing, draw building, GPU submit + readback, kernel calls > 0.5 ms, rest = game code + waits) on HITCH lines and as avg/max in the 5 s summaries. VM attract run: texture conversion is a real hitch cost (1.1 MB -> ~8-9 ms, 7.4 MB -> 43 ms).

## 2026-11-03: code review of both repos

Four read-only reviewers over the pushed heads (toolkit patch, project + setup, launchers, netplay + relay), then fixes:
- Toolkit: the kernel timer subsystem's first-use init was unsynchronised (two guest threads setting a timer at once could create the lock twice and start two timer threads) -> interlocked once-init. Frame buffer window: three present buffers instead of two (a window thread stalled 2+ flips -- modal move/resize, occlusion -- read a buffer the flip was overwriting: a torn frame); the window title is now written and read under an SRWLOCK. GPU presenter: a half-created device is released before the blit-model retry; shutdown waits up to 2 s for the display-clock thread (WaitForVBlank can take longer than the old 200 ms, which could leave two clock threads after a re-init).
- main.c: the log pump thread is started before stderr is redirected (if it could not start, every log write after 4 MB would have blocked the game forever); the exe started on its own now carries the same defaults run_hitz.bat and the launcher set (GPU, window, sound, two pads) instead of the CPU rasteriser without a window.
- nv2a_d3d11.c: a shader-cache file with a damaged tail (killed mid-write) is cut back to its last whole record, so the shaders compiled later are loadable again (they used to sit behind the garbage forever); dead constant-ring code removed; duplicate WIN32_LEAN_AND_MEAN guarded (was a build warning). CMake links d3dcompiler explicitly.
- setup.py: `--iso`/`--game` check the title id of the *new* files before the old game/ folder is removed (a wrong disc or a failed extraction no longer leaves you with nothing); a truncated XBE reads as "unreadable" instead of a traceback; missing BASE_COMMIT is a clean error.
- Launcher: CB_GETLBTEXT length-checked (a long adapter name could overflow the 128-char buffer); `--set` rejects values with line breaks (INI corruption); path truncation is an error instead of a wrong path; WM_DPICHANGED re-lays out the controls and font when the window moves between monitors with different scale factors; "Open log folder" opens the folder directly.
- Docs: README gains HITZ_GPU_CULL, HITZ_GPU_READBACK_POLL, the pad-port default; CLAUDE.md phase table gains the polish row.
- Verified in the VM: full rebuild 0 warnings in our files; attract demo 150 s (60 fps, late 0, every 10 s screenshot differs); launcher Play -> game running with its INI; the bare exe with no environment comes up on the GPU in a window at 60 fps.

## 2026-11-04: in-match texture hitches

Player's perf log (RTX 4090 laptop, 280 s fullscreen): 12 in-match hitches of 26-44 ms, `tex` 8-26 ms of each -- new textures streamed during play, 1-3 MB at a time. The renderer decoded every texture texel by texel through the toolkit's sampler (wrap, border and format switch per texel).
- DXT1/3/5 (most of the game's textures) now go to the GPU as BC1/2/3 unchanged: no CPU decode, 1/4-1/8 the upload. Top level must be whole 4x4 blocks, else the old path.
- The uncompressed formats still seen (swizzled A1R5G5B5/A8R8G8B8 in matches, linear YUY2 movie frames, LIN_A8R8G8B8) get a row-at-a-time decoder: separable Morton tables for the swizzle, the same channel expansion and BT.601 maths as the toolkit. `HITZ_TEX_VERIFY=1` compared it with the toolkit's decoder: identical for every format seen (0 texels differ).
- VM attract demo, 220 s: hitches 18 -> 12; in-match `tex` 45.5/8.0/12.2/8.7 ms -> 0.9/0.1/6.0/3.9 ms; movie frames 1.7 -> 0.7 ms each. Screenshots: ice markings, board ads, crowd, players, logos and the DEMO overlay's transparency all correct. What remains in those frames is the game's own loading code (`rest`).

## 2026-11-04: Debug logging toggle

The launcher's Diagnostics box is now **Debug logging** (`debug` in hitz_settings.ini, `HITZ_DEBUG`), off by default, replacing the performance-log box.
- Off: `src/hitz_log_gate.h`, force-included (`/FI`, set from CMakeLists.txt) into the toolkit libraries and our own sources but not the generated code, turns every print to stderr/stdout (`fprintf`, `vfprintf`, `fputs`, `fputc`, `fwrite`, `printf`, `vprintf`, `puts`) into one test of `g_hitz_log` before any formatting. The log pipe and its pump thread are not started, there is no `hitz_perf.log` (the kernel-call timing behind it is already gated on it), and the renderer's timers and GPU timestamp queries are off. The launcher hands the game NUL instead of truncating `hitz_stderr.log`; `run_hitz.bat` starts the exe without a redirect. A crash still writes `hitz_crash.log` (the fault handler reopens stderr there).
- On: exactly the old logs. `HITZ_PERF_LOG=0/1` still overrides the perf log on its own.
- No toolkit patch change, so updating needs no regeneration, only a rebuild of the toolkit libraries and our files.
- VM: `run_hitz.bat` off: no log files, 60 fps; on: both logs as before. Launcher off: INI `debug=0`, no logs, game running; on (`--set debug=1`): 53 KB stderr log + perf log with `HITZ_DEBUG=1` in its header. Caption fits the window.
