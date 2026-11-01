# NHL Hitz Pro — Xbox → Windows Static Recompilation

## What this project is

A **personal-use port of a game the project owner owns on original disc** (NHL
Hitz Pro, Midway 2003, retail Xbox). The owner dumped their own disc to an ISO
locally; the disc image itself is **not** part of this directory and is **never
to be uploaded, attached, redistributed, or sent anywhere** — it stays on this
machine. The goal is interoperability: running software the owner already owns
on hardware they own (a Windows 11 PC now; an Xbox One X in developer mode
later), the same class of work as the MIT-licensed
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit this project
uses (v0.16.0, toolkit is in `xboxrecomp/`).

Everything in this tree that came from the game disc is a **derived analysis
artifact produced from the owner's own copy**: section tables, function
boundaries, ABI inference, and machine-translated C. Treat it like the
owner's own notes about their own property. Work on the port code, runtime
glue, build system, and bug fixes — there is no step in this project that
requires you to extract, download, or distribute any copyrighted asset.

## Current status

The Windows port is complete (2026-11-01). Next phase: the Xbox One X port
(see below). Steps as originally run:

| Step | Tool | Status |
|---|---|---|
| 1. Extract XBE | extract-xiso 2.7.1 | done — `game/` (427 files, `default.xbe` 3.88 MB) |
| 2. Parse XBE | `tools.xbe_parser` | done — `game/hitz_analysis.json` |
| 3. Disassemble | `tools.disasm` (all sections) | done — 19,229 functions, `xboxrecomp/tools/disasm/output/` |
| 4. Classify | `tools.func_id` | done — `xboxrecomp/tools/func_id/output/` |
| 4.5 ABI recovery | `tools.abi_analysis` | done — 15,902 entries, 2,377 thiscall |
| 5. Lift to C | `tools.recomp --all --split 250` | done — `hitz_pro/src/recomp/gen/` |
| 6. Project glue | `hitz_pro/` | done — see below |
| 7. Windows build | MSVC in the win11 VM | done — builds clean, boots to title screen (NOTES 10-31) |
| 8. Boot/debug loop | VM, run_xbox.ps1 | done — menus, exhibition match, 2 pads, sound |
| 9. GPU back end | `hitz_pro/src/nv2a_d3d11.c` | done — 60 fps in matches (NOTES 11-01) |
| 10. Publish | GitHub + `setup.bat`/`setup.sh` | done — one-command setup, fresh-clone tested |
| 11. Netplay | separate repo hitz-xbox-netplay | done there; none in this repo |

## Directory layout (on odin, `~/hitz-xbox/`)

```
xboxrecomp/          toolkit clone @ v0.16.0 (tag). Pipeline outputs inside
                     its tools/*/output/ dirs. Do not commit game data here.
hitz_pro/            the game project (built exe lives here)
  CMakeLists.txt     project(hitz_pro_recomp C), XBOXRECOMP_DIR=../xboxrecomp
  src/main.c         boot glue; entry point 0x001DB5E3, XBE at game\default.xbe
  src/recomp/gen/    generated C (recomp_*.c, recomp_dispatch.c, recomp_funcs.h)
  src/recomp_manual.c  hand-written overrides go here
game/                extracted disc contents (default.xbe + data). Local only.
iso/                 the owner's disc image. Local only. Do not redistribute.
tools/extract-xiso   Linux binary used for extraction
.venv/               python 3.12 venv with capstone (use its python for tools)
recomp_run.log       full log of the generation run — read the tail first
NOTES.md             append-only dated findings. READ IT, and append to it.
```

## Key XBE facts (from `hitz_analysis.json`)

- Title "NHL Hitz Pro", title id 0x4D57001A, retail, XDK 5558, built 2003-08-14
- Base address 0x00010000, image 5.6 MB, entry point **0x001DB5E3**
- 13 sections: .text 0x11000+0x20E67C, then XDK libs (XMV, D3D, D3DX, XGRPH,
  DSOUND, WMADEC, XPP), .rdata, .data, DOLBY, $$XTIMAGE, .XTLID
- Kernel imports: full Nt*/Rtl* set + Xc* crypto (SHA/HMAC/PKCS1) + Xbox* info
- `game_vtable` classes: 7,750 functions (C++ heavy), 2,377 thiscall

## Rerunning pipeline steps (rarely needed)

```bash
cd ~/hitz-xbox/xboxrecomp
~/hitz-xbox/.venv/bin/python -m tools.disasm ../game/default.xbe --force -v   # NOT --text-only: XDK sections hold code (see NOTES 10-31)
~/hitz-xbox/.venv/bin/python -m tools.func_id ../game/default.xbe -v
~/hitz-xbox/.venv/bin/python -m tools.abi_analysis ../game/default.xbe -v
~/hitz-xbox/.venv/bin/python -m tools.recomp ../game/default.xbe --all --split 250 \
    --gen-dir ../hitz_pro/src/recomp/gen
```

- Keep `--split 250` (large chunks OOM small machines).
- `tools.disasm` rewrites `functions.json` and **loses any names you applied**
  (via `tools.ghidra_naming/merge_names.py --apply`) — re-apply after a regen.
- `recomp_types.h` in `src/recomp/gen/` is regenerated only if absent; hand
  edits survive a regen. Delete it to get a fresh copy.

## Next: Windows 11 build (this is the deliverable)

Target: `hitz_pro` builds into `hitz_pro_recomp.exe` with **MSVC + D3D11**
(the complete graphics path; the Linux OpenGL backend is a fallback and less
complete — don't port to it first).

A Windows 11 VM already runs on odin (docker `win11`, dockurr/windows, KVM):
- Watch it: `http://<odin-lan-ip>:8006`
- SSH from odin: `ssh -p 2222 <vmuser>@127.0.0.1` (creds in `~/winvm/.password`)
- It was provisioned with VS 2022 Build Tools + Win11 SDK + CMake + Git via
  the OEM script (check `C:\OEM\setup.log` ends with `OEM setup DONE`).

Build procedure (from odin):
1. `tar -czf /tmp/hitz_proj.tgz -C ~/hitz-xbox hitz_pro xboxrecomp` — **exclude
   `game/` and `iso/`** (assets stay local; the exe loads them at runtime via
   relative paths — mount or copy `game/` separately if you need a full run).
2. `scp -P 2222 /tmp/hitz_proj.tgz <vmuser>@127.0.0.1:C:/Users/<vmuser>/`
3. In VM: `mkdir C:\proj && cd /d C:\proj && tar -xf C:\Users\<vmuser>\hitz_proj.tgz`
4. `cd /d C:\proj\hitz_pro && cmake -S . -B build && cmake --build build --config Release --parallel`
   - Git Bash users: use `--parallel`, not `-- /m` (path mangling).
   - Expect `/bigobj` to matter (thousands of generated functions).
5. Run from a **console session** (ssh-launched GUI programs are invisible):
   `schtasks /create /f /tn run /sc once /st 23:59 /it /ru <vmuser> /tr "..."`, then `schtasks /run /tn run`.

Debug loop (expect crashes on first boot — that is normal):
- Read stderr: ICALL failures → add target/override in `src/recomp_manual.c`
  and register it in the manual lookup table.
- AV at 0xFD...... → NV2A MMIO not initialized; 0xFE...... → APU MMIO.
- Every diagnosis and fix gets a dated line in `NOTES.md`.

## Netplay — done, in the separate hitz-xbox-netplay repo

Netplay lives in https://github.com/joeglenn/hitz-xbox-netplay; this repo stays the
plain port with no netplay code. Fixes that apply to both land here first and
are then merged into the netplay repo. The original plan follows.

### Original plan

Owner's order: finish input, sound and the rest of the Windows port first.
Only then netplay, then the Xbox One X phase.

Recon done: the Xbox XBE has no XOnline/XNet/socket code, so there is no Live
protocol to emulate. Agreed design (2026-10-31):
- Delay-based input lockstep inside the port; rollback maybe later.
- Determinism mode first: guest clock (QPC/rdtsc/KeTickCount/interrupt time)
  driven by frame count, frames gated on both players' inputs, guest threads
  pinned. Prove it with a 2-instance scripted-input RAM-hash desync test
  before writing network code.
- Netplay input injected at the emulated USB gamepad (usb_gamepad_report).
- Server in Rust (static binary + systemd, owner's Ubuntu VPS): room codes,
  UDP, NAT punch with relay fallback, redundant input packets, input delay
  sized to RTT.

## After netplay: Xbox One X (retail dev mode) — later phase

An Xbox One X with a developer account runs **UWP** apps, not Win32 exes.
The realistic path is a separate UWP wrapper project that hosts the same
recompiled C (Microsoft's Xbox dev mode + a UWP shell around the D3D11 code),
or continued testing via streaming/remote. Treat this as a follow-on project:
do not block the Windows build on it.

## Known issues at the original handoff (historical: all fixed, see NOTES.md)

1. **`dr4` undeclared (build blocker, trivial).** The binary contains four
   `mov dr4, esp` instructions (0x11A4B2, 0x11A554, 0x11AA2A, 0x11AACC — see
   `tools/disasm/output/asm/text.asm` lines ~443497+). The lifter has no
   debug-register model and passes capstone's reg name through, so
   `gen/recomp_0027.c` lines 30193 and 31033 emit `dr4 = esp;` → C2065 under
   MSVC. Debug registers are meaningless on a recomp host; either make the
   two lines no-ops/comments in the generated output, or (better) add a
   `mov drN` no-op case in `xboxrecomp/tools/recomp/lifter.py` (`_lift_mov`
   dispatches at line ~1478) and regen. Regeneration takes ~2 min total.
2. **317 unresolved stubs** (`recomp_stubs_unresolved.c`) — normal; resolve
   via ICALL trace during the boot loop.
3. A handful of unimplemented legacy opcodes (salc/aaa/aam/daas, cli/sti,
   hlt...) — see `tools/recomp/output/summary.json` `unimplemented`; these
   sit in CRT helpers, usually harmless or stubbable.

## Working conventions

- Append every finding to `NOTES.md` with a date; never delete history there.
- Do not modify `xboxrecomp/` unless a fix is genuinely toolkit-level; if you
  do, record the exact change + commit hash in NOTES.md.
- Game data and the ISO never leave odin. Never paste disc contents into
  chats, tickets, or commits.
- If a step needs the user (credentials, purchases, hardware), stop and ask.
