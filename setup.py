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
    print("\n=== " + msg, flush=True)


def die(msg):
    print("\n[!] " + msg, file=sys.stderr, flush=True)
    sys.exit(1)


def run(cmd, cwd=None, check=True, quiet=False):
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
    return run(["git", "-C", TOOLKIT] + list(args), check=check, quiet=quiet)


# ---------------------------------------------------------------- 1. toolkit
def step_toolkit():
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
    return TOOLKIT / ".git" / "hitz-applied.patch"


def remember_patch():
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
    """Extract an XDVDFS (Xbox disc) image. No external tools needed."""
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
            f.seek(base + sector * SECTOR)
            return f.read(size)

        def walk_dir(sector, size, path):
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
    h = hashlib.sha256()
    for p in paths:
        h.update(Path(p).read_bytes())
    return h.hexdigest()


def step_generate(force):
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
    """SDL2.dll next to the exe, and the launcher in hitz_pro/."""
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
