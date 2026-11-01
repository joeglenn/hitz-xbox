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
    base = (PROJ / "toolkit" / "BASE_COMMIT").read_text().strip()
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
        print("  already patched")
        return
    dirty = git("status", "--porcelain", "--untracked-files=no").stdout.strip()
    if dirty:
        die("xboxrecomp/ has local changes that are not this patch.\n"
            "    Commit or discard them (git -C xboxrecomp checkout -- .) and rerun.")
    git("checkout", "-q", base, quiet=False)
    if git("apply", "--check", PATCH, check=False).returncode != 0:
        die("the patch does not apply to %s; is xboxrecomp-hitz.patch from this repo?" % base[:7])
    git("apply", PATCH, quiet=False)
    print("  patched")


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
    with open(xbe, "rb") as f:
        hdr = f.read(0x180)
        if hdr[:4] != b"XBEH":
            return None
        base, = struct.unpack_from("<I", hdr, 0x104)
        cert, = struct.unpack_from("<I", hdr, 0x118)
        f.seek(cert - base + 8)
        return struct.unpack("<I", f.read(4))[0]


def step_game(iso, game_dir):
    say("Game files -> hitz_pro/game")
    xbe = GAME / "default.xbe"
    if xbe.exists() and not iso and not game_dir:
        print("  already present")
    elif iso:
        iso = Path(iso).expanduser()
        if not iso.is_file():
            die("ISO not found: %s" % iso)
        if GAME.exists():
            shutil.rmtree(GAME)
        tmp = PROJ / "game.partial"
        if tmp.exists():
            shutil.rmtree(tmp)
        extract_iso(iso, tmp)
        tmp.rename(GAME)
    elif game_dir:
        src = Path(game_dir).expanduser()
        if not (src / "default.xbe").is_file():
            die("%s has no default.xbe" % src)
        if src.resolve() != GAME.resolve():
            if GAME.exists():
                shutil.rmtree(GAME)
            shutil.copytree(src, GAME)
            print("  copied")
    else:
        die("No game files yet. Pass the disc image:  --iso \"NHL Hitz Pro (USA).iso\"\n"
            "    or an extracted folder:  --game <folder with default.xbe>")
    tid = xbe_title_id(xbe)
    if tid != TITLE_ID:
        die("hitz_pro/game/default.xbe is not NHL Hitz Pro USA (title id %s, want 0x%08X)"
            % ("0x%08X" % tid if tid is not None else "unreadable", TITLE_ID))
    print("  NHL Hitz Pro (USA), title id 0x%08X" % tid)


# ---------------------------------------------------------------- 3. generate
def file_hash(*paths):
    h = hashlib.sha256()
    for p in paths:
        h.update(Path(p).read_bytes())
    return h.hexdigest()


def step_generate(force):
    say("Generate C code (disasm, classify, ABI, lift)")
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
    run([py, "-m", "tools.disasm", xbe, "--force"], cwd=TOOLKIT)
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
    say("Done in %.0f min" % ((time.time() - t0) / 60))
    if built:
        print("  Play: hitz_pro\\run_hitz.bat")
        if a.run:
            subprocess.Popen(["cmd", "/c", str(PROJ / "run_hitz.bat")], cwd=PROJ)


if __name__ == "__main__":
    main()
