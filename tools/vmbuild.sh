#!/bin/bash
# vmbuild.sh <n> [files...] : copy changed files (paths relative to ~/hitz-xbox) to the VM,
# kill a running game, build hitz_pro, print the tail and any errors. Exit 1 on error.
set -u
n=$1; shift
cd ~/hitz-xbox
for f in "$@"; do
    case "$f" in
        xboxrecomp/*|hitz_pro/*) scp -q -P 2222 "$f" "hitz@127.0.0.1:C:/proj/$f" || exit 1 ;;
        *) echo "vmbuild: path must start with xboxrecomp/ or hitz_pro/: $f"; exit 1 ;;
    esac
done
cat > /tmp/vmbuild.bat <<EOF
@echo off
taskkill /F /IM hitz_pro_recomp.exe >nul 2>&1
cd /d C:\\proj\\hitz_pro
call build_windows.bat > C:\\proj\\build$n.log 2>&1
EOF
scp -q -P 2222 /tmp/vmbuild.bat hitz@127.0.0.1:C:/proj/vmbuild.bat
ssh -o BatchMode=yes -p 2222 hitz@127.0.0.1 'cmd /c C:\proj\vmbuild.bat'
ssh -o BatchMode=yes -p 2222 hitz@127.0.0.1 "powershell -NoProfile -Command \"Get-Content C:\\proj\\build$n.log -Tail 1; Select-String -Path C:\\proj\\build$n.log -Pattern ' error ' | Select-Object -First 12\"" > /tmp/vmbuild.out
cat /tmp/vmbuild.out
scp -q -P 2222 hitz@127.0.0.1:C:/proj/hitz_pro/build/hitz_pro_recomp.map /tmp/hp/exe.map 2>/dev/null
grep -q " error " /tmp/vmbuild.out && exit 1
exit 0
