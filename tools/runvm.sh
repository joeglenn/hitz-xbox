#!/bin/bash
# usage: runvm.sh <name> <seconds>   (env from C:\proj\run_env.txt)
N=$1; S=${2:-120}
W="ssh -o BatchMode=yes -p 2222 hitz@127.0.0.1"
[ -f /tmp/run_env.txt ] && scp -q -P 2222 /tmp/run_env.txt hitz@127.0.0.1:C:/proj/run_env.txt
$W "schtasks /create /f /tn runx /sc once /st 23:59 /it /ru hitz /tr \"powershell -NoProfile -ExecutionPolicy Bypass -File C:\\proj\\run_xbox.ps1 -Seconds $S\" >nul && schtasks /run /tn runx >nul"
sleep $((S+15))
until $W "if exist C:\\proj\\run\\finished.txt (exit 0) else (exit 1)"; do sleep 5; done
D=~/hitz-xbox/runs/$N; rm -rf $D; mkdir -p $D
scp -q -P 2222 "hitz@127.0.0.1:C:/proj/run/*" $D/
iconv -f utf-16 -t utf-8 $D/status.txt
