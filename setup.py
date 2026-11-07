#!/usr/bin/env python3
"""One-shot setup for the NHL Hitz Pro Xbox recomp.

    setup.bat --iso "D:\\Games\\NHL Hitz Pro (USA).iso"      (Windows)
    ./setup.sh --iso "/path/NHL Hitz Pro (USA).iso"          (Linux/macOS)

Does everything, skipping steps that are already done, so it is safe to rerun:
  1. clone xboxrecomp, check out the base commit, apply the patch
  2. extract the game from the ISO (or copy it from --game DIR) into hitz_pro/game
  3. check that it is NHL Hitz Pro (USA)
  4. generate the C code (disasm, func_id, abi_analysis, recomp)
  5. build hitz_pro_recomp.exe (Windows only)
  6. start the game (--run)

Use the wrappers (setup.bat / setup.sh): they provide Python and capstone.

This module is meant to be run as a script (see main() and the module-level
constants below), not imported. It is also the thing argparse shows for
--help, so keep the text above in sync with what the script actually does.

Layout (all paths are derived from this file's own location, ROOT):
  ROOT/hitz_pro                   PROJ, the game project (this repo)
  ROOT/hitz_pro/game              GAME, the extracted Xbox disc (step 2)
  ROOT/hitz_pro/src/recomp/gen    GEN, the generated C code (step 4)
  ROOT/xboxrecomp                 TOOLKIT, the recompiler toolkit clone (step 1)
  ROOT/xboxrecomp-hitz.patch      PATCH, the toolkit patch this game needs

Idempotency: each step decides on its own whether it has already run and
skips itself if so, by checking for marker state on disk rather than a
single global flag:
  - step_toolkit: the toolkit is "done" once xboxrecomp/ is checked out at
    the commit named in hitz_pro/toolkit/BASE_COMMIT and the patch in this
    repo applies cleanly in reverse (i.e. it is already applied).
  - step_game: done once hitz_pro/game/default.xbe exists, unless the
    caller passed --iso or --game to force a re-extraction/copy.
  - step_generate: done once GEN/.setup_stamp holds the sha256 of the game
    xbe + the patch + recomp_manual.c, matching the current inputs, and
    GEN/recomp_funcs.h exists. Changing any of those three things changes
    the hash and triggers a regeneration; --regen forces one regardless.
  - step_build: always runs build_windows.bat, which is itself incremental
    (CMake/Ninja only rebuild what changed).

This script does not read any environment variables itself; it is normally
launched through setup.sh or setup.bat, which arrange a Python interpreter
with the capstone package installed (via a local venv or via uv) and then
exec this file. There is no threading: everything runs on the single main
thread, and external tools (git, the toolkit's generator scripts, the MSVC
build) are run synchronously with subprocess.run and are expected to do
their own job sequentially.
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJ = ROOT / "hitz_pro"
TOOLKIT = ROOT / "xboxrecomp"
GAME = PROJ / "game"
GEN = PROJ / "src" / "recomp" / "gen"
PATCH = ROOT / "xboxrecomp-hitz.patch"
TOOLKIT_URL = "https://github.com/sp00nznet/xboxrecomp.git"
# SDL2 runtime (one DLL) for non-Xbox controllers: 8BitDo, PlayStation,
# Switch Pro. Pinned and checked; the game runs without it (XInput only).
SDL2_URL = "https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10-win32-x64.zip"
SDL2_SHA256 = "6cf9706eefd0a4a06dc764007934d428afaf029fabdd408a9e646048c91e18fb"
TITLE_ID = 0x4D57001A
IS_WIN = os.name == "nt"


def say(msg):
    """Print a "=== msg" section banner to stdout, flushed immediately."""
    print("\n=== " + msg, flush=True)


def die(msg):
    """Print "[!] msg" to stderr and exit the process with status 1."""
    print("\n[!] " + msg, file=sys.stderr, flush=True)
    sys.exit(1)


def run(cmd, cwd=None, check=True, quiet=False):
    """Run cmd (a list) with subprocess.run, echoing it first unless quiet.

    With quiet=True, stdout/stderr are captured instead of inherited (used
    for git plumbing calls whose output we parse or only show on failure).
    With check=True (the default), a non-zero exit calls die(), which ends
    the whole setup; the captured output (if quiet) is printed first so the
    failure is not silent. Returns the CompletedProcess either way.
    """
    if not quiet:
        print("  $ " + " ".join(str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], cwd=cwd,
                       stdout=subprocess.PIPE if quiet else None,
                       stderr=subprocess.STDOUT if quiet else None, text=True)
    if check and r.returncode != 0:
        if quiet and r.stdout:
            print(r.stdout)
        die("command failed (exit %d): %s" % (r.returncode, " ".join(str(c) for c in cmd)))
    return r


def git(*args, check=True, quiet=True):
    """Run git against the TOOLKIT clone (git -C TOOLKIT <args>); see run()."""
    return run(["git", "-C", TOOLKIT] + list(args), check=check, quiet=quiet)


# ---------------------------------------------------------------- 1. toolkit
def step_toolkit():
    """Step 1: clone xboxrecomp if needed, check out BASE_COMMIT, apply PATCH.

    Safe to rerun: if the toolkit is already at BASE_COMMIT with PATCH
    applied (checked by reverse-applying it with --check), this is a no-op.
    If the tree has uncommitted changes that look like an older copy of our
    own patch (see remove_old_patch), they are reversed first so the new
    patch can go on cleanly; any other local change aborts the whole setup
    rather than risk clobbering someone's manual edits to the toolkit.
    """
    try:
        base = (PROJ / "toolkit" / "BASE_COMMIT").read_text().strip()
    except OSError:
        die("hitz_pro/toolkit/BASE_COMMIT is missing; the checkout is incomplete")
    say("Toolkit: xboxrecomp @ %s + patch" % base[:7])
    if shutil.which("git") is None:
        die("git not found. Install it from https://git-scm.com and rerun.")
    if not (TOOLKIT / ".git").exists():
        run(["git", "clone", TOOLKIT_URL, TOOLKIT])
    if git("cat-file", "-e", base + "^{commit}", check=False).returncode != 0:
        git("fetch", "origin", quiet=False)
    head = git("rev-parse", "HEAD").stdout.strip()
    applied = git("apply", "--reverse", "--check", PATCH, check=False).returncode == 0
    if head == base and applied:
        remember_patch()
        print("  already patched")
        return
    if git("status", "--porcelain", "--untracked-files=no").stdout.strip():
        # After a git pull the toolkit still carries the previous version of
        # our patch. Take it off again if it is one of ours; anything else is
        # someone's own edit and is left alone.
        if not remove_old_patch():
            die("xboxrecomp/ has local changes that are not from this repo's patch.\n"
                "    Commit or discard them (git -C xboxrecomp checkout -- .) and rerun.")
    git("checkout", "-q", base, quiet=False)
    if git("apply", "--check", PATCH, check=False).returncode != 0:
        die("the patch does not apply to %s; is xboxrecomp-hitz.patch from this repo?" % base[:7])
    git("apply", PATCH, quiet=False)
    remember_patch()
    print("  patched")


# The patch last applied, kept inside xboxrecomp/.git (not part of the tree).
def applied_copy():
    """Path of the saved copy of the last-applied patch (inside .git, untracked)."""
    return TOOLKIT / ".git" / "hitz-applied.patch"


def remember_patch():
    """Copy PATCH to applied_copy() so a later run can detect/undo this version."""
    try:
        shutil.copyfile(PATCH, applied_copy())
    except OSError:
        pass


def earlier_patches():
    """Earlier versions of the patch: the copy saved when it was applied, then
    every version in this repo's history (a ZIP download has none)."""
    if applied_copy().exists():
        yield "the last applied patch", applied_copy().read_bytes()
    if (ROOT / ".git").exists():
        log = run(["git", "-C", ROOT, "log", "--format=%H", "--", PATCH.name],
                  check=False, quiet=True)
        for commit in (log.stdout or "").split():
            show = subprocess.run(["git", "-C", str(ROOT), "show", "%s:%s" % (commit, PATCH.name)],
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            if show.returncode == 0 and show.stdout:
                yield "the patch from commit %s" % commit[:7], show.stdout


def remove_old_patch():
    """Reverse whichever earlier patch is applied; True if the tree is clean after."""
    tmp = ROOT / ".tools" / "old-toolkit.patch"
    tmp.parent.mkdir(parents=True, exist_ok=True)
    seen = set()
    try:
        for what, data in earlier_patches():
            h = hashlib.sha256(data).hexdigest()
            if h in seen:
                continue
            seen.add(h)
            tmp.write_bytes(data)
            if git("apply", "--reverse", "--check", tmp, check=False).returncode != 0:
                continue
            git("apply", "--reverse", tmp)
            if git("status", "--porcelain", "--untracked-files=no").stdout.strip():
                git("apply", tmp)                   # not the whole story: put it back
                continue
            print("  removed %s (updating to the new one)" % what)
            return True
        return False
    finally:
        if tmp.exists():
            tmp.unlink()


# ---------------------------------------------------------------- 2. game
SECTOR = 2048
XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets: trimmed xiso, then full redump images (XGD1, XGD2, XGD3).
PARTITIONS = (0, 0x18300000, 0xFD90000, 0x2080000)


def extract_iso(iso, dest):
    """Extract an XDVDFS (Xbox disc) image. No external tools needed.

    Scans PARTITIONS for the XDVDFS magic at sector 32 of each candidate
    offset, to support both a trimmed xiso (game data starts at byte 0) and
    a full redump dump (game partition starts further in, depending on the
    disc generation). Once the volume is found, walks its directory tree
    (an in-place binary tree per directory, 4-byte sector units) and copies
    every file into dest, which the caller is expected to already have
    prepared as scratch space. Calls die() on a missing volume or a
    truncated file; prints progress every 50 files.
    """
    with open(iso, "rb") as f:
        base = None
        for off in PARTITIONS:
            f.seek(off + 32 * SECTOR)
            if f.read(20) == XDVDFS_MAGIC:
                base = off
                break
        if base is None:
            die("%s is not an Xbox disc image (no XDVDFS volume found)" % iso)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        count = [0, 0]

        def read_at(sector, size):
            """Read size bytes at the given XDVDFS sector, relative to base."""
            f.seek(base + sector * SECTOR)
            return f.read(size)

        def walk_dir(sector, size, path):
            """Recursively extract one XDVDFS directory (its binary-tree table) into path."""
            if size == 0:
                return
            data = read_at(sector, size)
            stack = [0]
            seen = set()
            while stack:
                off = stack.pop()
                if off in seen or off + 14 > len(data):
                    continue
                seen.add(off)
                left, right, start, fsize, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
                if left == 0xFFFF and right == 0xFFFF and start == 0xFFFFFFFF:
                    continue
                name = data[off + 14: off + 14 + nlen].decode("latin-1")
                if left:
                    stack.append(left * 4)
                if right:
                    stack.append(right * 4)
                if not name or name in (".", "..") or "/" in name or "\\" in name:
                    continue
                out = path / name
                if attr & 0x10:
                    out.mkdir(parents=True, exist_ok=True)
                    walk_dir(start, fsize, out)
                else:
                    with open(out, "wb") as o:
                        f.seek(base + start * SECTOR)
                        left_b = fsize
                        while left_b:
                            chunk = f.read(min(left_b, 1 << 22))
                            if not chunk:
                                die("ISO is truncated (reading %s)" % out)
                            o.write(chunk)
                            left_b -= len(chunk)
                    count[0] += 1
                    count[1] += fsize
                    if count[0] % 50 == 0:
                        print("  %d files, %.0f MB" % (count[0], count[1] / 1e6), flush=True)

        dest.mkdir(parents=True, exist_ok=True)
        walk_dir(root_sector, root_size, dest)
        print("  extracted %d files, %.0f MB" % (count[0], count[1] / 1e6))


def xbe_title_id(xbe):
    """Read the title id out of an Xbox executable's certificate header.

    Returns the 32-bit title id, or None if the file is not an XBE ("XBEH"
    magic) or is too short/unreadable to parse. Offsets 0x104 and 0x118 are
    the XBE header's base address and certificate-address fields; the
    certificate's title id is 8 bytes into the certificate.
    """
    try:
        with open(xbe, "rb") as f:
            hdr = f.read(0x180)
            if hdr[:4] != b"XBEH":
                return None
            base, = struct.unpack_from("<I", hdr, 0x104)
            cert, = struct.unpack_from("<I", hdr, 0x118)
            f.seek(cert - base + 8)
            return struct.unpack("<I", f.read(4))[0]
    except (OSError, struct.error):
        return None                     # truncated or unreadable


def check_title(xbe):
    """Call die() unless xbe's title id matches TITLE_ID (NHL Hitz Pro USA); else return it."""
    tid = xbe_title_id(xbe)
    if tid != TITLE_ID:
        die("%s is not NHL Hitz Pro USA (title id %s, want 0x%08X)"
            % (xbe, "0x%08X" % tid if tid is not None else "unreadable", TITLE_ID))
    return tid


def replace_game(new_dir):
    """new_dir (checked) becomes hitz_pro/game; the old one goes only now."""
    if GAME.exists():
        shutil.rmtree(GAME)
    new_dir.rename(GAME)


def step_game(iso, game_dir):
    """Step 2: populate hitz_pro/game from --iso or --game, or leave it alone if already set up.

    Extraction/copy happens into a sibling "game.partial" directory first;
    only once check_title() has confirmed the result is the right game does
    replace_game() swap it in for the real "game" directory. That way a bad
    disc image or an interrupted extraction never destroys a working
    hitz_pro/game. Calls die() if neither --iso/--game was given and no game
    is present yet, or if the title id check fails.
    """
    say("Game files -> hitz_pro/game")
    xbe = GAME / "default.xbe"
    if xbe.exists() and not iso and not game_dir:
        print("  already present")
    elif iso:
        # Extract next to the old files, check the result, and only then
        # replace them: a wrong disc or a failed extraction leaves game/ alone.
        iso = Path(iso).expanduser()
        if not iso.is_file():
            die("ISO not found: %s" % iso)
        tmp = PROJ / "game.partial"
        if tmp.exists():
            shutil.rmtree(tmp)
        extract_iso(iso, tmp)
        check_title(tmp / "default.xbe")
        replace_game(tmp)
    elif game_dir:
        src = Path(game_dir).expanduser()
        if not (src / "default.xbe").is_file():
            die("%s has no default.xbe" % src)
        if src.resolve() != GAME.resolve():
            check_title(src / "default.xbe")
            tmp = PROJ / "game.partial"
            if tmp.exists():
                shutil.rmtree(tmp)
            shutil.copytree(src, tmp)
            replace_game(tmp)
            print("  copied")
    else:
        die("No game files yet. Pass the disc image:  --iso \"NHL Hitz Pro (USA).iso\"\n"
            "    or an extracted folder:  --game <folder with default.xbe>")
    tid = check_title(xbe)
    print("  NHL Hitz Pro (USA), title id 0x%08X" % tid)


# ---------------------------------------------------------------- 3. generate
def file_hash(*paths):
    """sha256 over the concatenated bytes of paths; used as the regeneration cache key."""
    h = hashlib.sha256()
    for p in paths:
        h.update(Path(p).read_bytes())
    return h.hexdigest()


def step_generate(force):
    """Step 4 (numbered 3 in the step comments): run the toolkit's code generation pipeline.

    Runs, in order inside TOOLKIT: xbe_parser (static analysis to JSON),
    disasm (full disassembly, including the XDK library sections -- never
    pass --text-only or about 3000 functions go missing), func_id, then
    abi_analysis, then recomp itself, which lifts x86 to C into GEN and
    skips any function overridden in recomp_manual.c (--exclude-manual),
    renaming the original to "..._gen" so the override can still call it.
    Skipped entirely if GEN/.setup_stamp already matches file_hash() of the
    game xbe, the toolkit PATCH and recomp_manual.c, and recomp_funcs.h
    exists, unless force (--regen) is set. On success, writes the new hash
    to the stamp file so the next run can skip again. Calls die() if
    capstone is not importable, or if recomp produced fewer than 50 C files
    or no recomp_funcs.h (a sign something upstream failed silently).
    """
    say("Generate C code (parse XBE, disasm, classify, ABI, lift)")
    try:
        import capstone  # noqa: F401
    except ImportError:
        die("Python module 'capstone' is missing. Run through setup.bat / setup.sh, "
            "or: %s -m pip install capstone" % sys.executable)
    stamp = GEN / ".setup_stamp"
    key = file_hash(GAME / "default.xbe", PATCH, PROJ / "src" / "recomp_manual.c")
    if not force and stamp.exists() and stamp.read_text() == key and (GEN / "recomp_funcs.h").exists():
        print("  up to date")
        return
    xbe = os.path.relpath(GAME / "default.xbe", TOOLKIT)
    py = sys.executable
    t0 = time.time()
    run([py, "-m", "tools.xbe_parser", xbe, "--json",
         os.path.relpath(GAME / "default_analysis.json", TOOLKIT)], cwd=TOOLKIT)
    print("  Disassembling the game: this is the slow part (several minutes, longer\n"
          "  on a slow PC or drive) and prints nothing until it finishes. It is not\n"
          "  stuck; leave the window open.", flush=True)
    t1 = time.time()
    run([py, "-m", "tools.disasm", xbe, "--force"], cwd=TOOLKIT)
    print("  disassembly took %.0f s" % (time.time() - t1), flush=True)
    run([py, "-m", "tools.func_id", xbe], cwd=TOOLKIT)
    run([py, "-m", "tools.abi_analysis", xbe], cwd=TOOLKIT)
    if GEN.exists():
        for p in GEN.glob("*.c"):
            p.unlink()
    GEN.mkdir(parents=True, exist_ok=True)
    run([py, "-m", "tools.recomp", xbe, "--all", "--split", "250",
         "--gen-dir", os.path.relpath(GEN, TOOLKIT),
         "--exclude-manual", os.path.relpath(PROJ / "src" / "recomp_manual.c", TOOLKIT)], cwd=TOOLKIT)
    n = len(list(GEN.glob("*.c")))
    if n < 50 or not (GEN / "recomp_funcs.h").exists():
        die("generation produced only %d files; see the output above" % n)
    stamp.write_text(key)
    print("  %d source files in %.0f s" % (n, time.time() - t0))


# ---------------------------------------------------------------- 4. build
def step_build():
    """Step 5: run hitz_pro/build_windows.bat (MSVC/CMake/Ninja) via cmd.exe.

    No-op (returns False) on non-Windows hosts: the build needs Visual
    Studio, so Linux/macOS runs are expected to stop here and be finished on
    a Windows machine, where rerunning setup.bat skips the already-done
    steps. Calls die() with build-tools/locked-exe hints if the batch file
    exits non-zero. Returns True if the build actually ran and succeeded.
    """
    say("Build hitz_pro_recomp.exe (MSVC)")
    if not IS_WIN:
        print("  skipped: building needs Windows + Visual Studio.\n"
              "  Copy this whole folder to a Windows PC and run setup.bat there;\n"
              "  the finished steps are skipped.")
        return False
    r = subprocess.run(["cmd", "/c", str(PROJ / "build_windows.bat")], cwd=PROJ)
    if r.returncode != 0:
        die("build failed. If it said cl.exe not found, install the C++ build tools:\n"
            "    winget install Microsoft.VisualStudio.2022.BuildTools --override "
            "\"--passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended\"\n"
            "  then rerun setup.bat. If it could not open the exe for writing, close the game first.")
    return True


def step_extras():
    """SDL2.dll next to the exe, and the launcher in hitz_pro/.

    Downloads the pinned SDL2 Windows DLL (needed for non-Xbox controllers;
    XInput pads work without it) into .tools/ the first time, verifying its
    sha256 against SDL2_SHA256 before trusting it, then unzips just
    SDL2.dll next to the built exe. Failure here is not fatal to setup: it
    prints a warning and leaves the game playable with XInput pads only.
    Also copies the built launcher exe to hitz_pro/HitzLauncher.exe so
    players have a stable, discoverable entry point.
    """
    import io
    import urllib.request
    import zipfile
    say("Controller support (SDL2) and the launcher")
    build = PROJ / "build"
    dll = build / "SDL2.dll"
    if dll.exists():
        print("  SDL2.dll already there")
    else:
        cache = ROOT / ".tools" / "SDL2-2.32.10-win32-x64.zip"
        try:
            if not cache.exists():
                print("  downloading " + SDL2_URL, flush=True)
                cache.parent.mkdir(parents=True, exist_ok=True)
                with urllib.request.urlopen(SDL2_URL, timeout=60) as r:
                    data = r.read()
                cache.write_bytes(data)
            data = cache.read_bytes()
            if hashlib.sha256(data).hexdigest() != SDL2_SHA256:
                cache.unlink()
                raise RuntimeError("checksum mismatch (download corrupted?)")
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                dll.write_bytes(z.read("SDL2.dll"))
            print("  SDL2.dll installed: 8BitDo, PlayStation and Switch Pro controllers work")
        except Exception as e:
            print("  [warning] could not get SDL2.dll (%s).\n"
                  "  The game still runs; only Xbox (XInput) controllers will work.\n"
                  "  Rerun setup.bat to try again." % e)
    exe = build / "hitz_launcher.exe"
    if exe.exists():
        try:
            shutil.copy2(exe, PROJ / "HitzLauncher.exe")
            print("  launcher: hitz_pro\\HitzLauncher.exe")
        except OSError as e:
            print("  [warning] could not copy the launcher (%s); close it and rerun setup.bat" % e)


def main():
    """Parse argv, then run the six steps in order (see the module docstring)."""
    ap = argparse.ArgumentParser(description="Set up, patch, generate and build the NHL Hitz Pro recomp.")
    ap.add_argument("--iso", help="NHL Hitz Pro (USA) disc image (.iso, xiso or full redump)")
    ap.add_argument("--game", help="folder with already-extracted game files (default.xbe ...)")
    ap.add_argument("--regen", action="store_true", help="regenerate the C code even if up to date")
    ap.add_argument("--no-build", action="store_true", help="stop after generating the code")
    ap.add_argument("--run", action="store_true", help="start the game when done")
    a = ap.parse_args()
    t0 = time.time()
    step_toolkit()
    step_game(a.iso, a.game)
    step_generate(a.regen)
    built = False if a.no_build else step_build()
    if built:
        step_extras()
    say("Done in %.0f min" % ((time.time() - t0) / 60))
    if built:
        print("  Play: hitz_pro\\HitzLauncher.exe  (settings, then Play)")
        print("    or: hitz_pro\\run_hitz.bat       (straight in, default settings)")
        if a.run:
            subprocess.Popen(["cmd", "/c", str(PROJ / "run_hitz.bat")], cwd=PROJ)


if __name__ == "__main__":
    main()
