#!/bin/bash
# vmbuild.sh <n> [files...] : copy changed files (paths relative to ~/hitz-xbox) to the VM,
# kill a running game, build hitz_pro, print the tail and any errors. Exit 1 on error.
#
# Dev-loop helper: n tags this build's log (C:\proj\buildN.log) and the exe
# map it fetches back (/tmp/hp/exe.map, used by rva2sym.py/hostprof_resolve.py).
# Only paths under xboxrecomp/ or hitz_pro/ may be copied, since those are
# the only two trees that exist both here and at C:\proj in the guest.
set -u
n=$1; shift
cd ~/hitz-xbox
for f in "$@"; do
    case "$f" in
        xboxrecomp/*|hitz_pro/*) scp -q -P 2222 "$f" "hitz@127.0.0.1:C:/proj/$f" || exit 1 ;;
        *) echo "vmbuild: path must start with xboxrecomp/ or hitz_pro/: $f"; exit 1 ;;
    esac
done
# The game locks its own exe while running, so any build would fail to
# relink it; always kill it first. The batch file is built locally then
# copied over and run with cmd /c so its own output capture (build$n.log)
# happens inside the guest regardless of the SSH session's own redirection.
cat > /tmp/vmbuild.bat <<EOF
@echo off
taskkill /F /IM hitz_pro_recomp.exe >nul 2>&1
cd /d C:\\proj\\hitz_pro
call build_windows.bat > C:\\proj\\build$n.log 2>&1
EOF
scp -q -P 2222 /tmp/vmbuild.bat hitz@127.0.0.1:C:/proj/vmbuild.bat
ssh -o BatchMode=yes -p 2222 hitz@127.0.0.1 'cmd /c C:\proj\vmbuild.bat'
# Only the last log line plus any " error " lines are pulled back, to keep
# the terminal output short; the full build$n.log stays in the guest.
ssh -o BatchMode=yes -p 2222 hitz@127.0.0.1 "powershell -NoProfile -Command \"Get-Content C:\\proj\\build$n.log -Tail 1; Select-String -Path C:\\proj\\build$n.log -Pattern ' error ' | Select-Object -First 12\"" > /tmp/vmbuild.out
cat /tmp/vmbuild.out
scp -q -P 2222 hitz@127.0.0.1:C:/proj/hitz_pro/build/hitz_pro_recomp.map /tmp/hp/exe.map 2>/dev/null
grep -q " error " /tmp/vmbuild.out && exit 1
exit 0
