#!/bin/bash
# usage: runvm.sh <name> <seconds>   (env from C:\proj\run_env.txt)
#
# Runs the game unattended inside the win11 VM for a fixed duration (used
# for timed smoke tests / pad-script runs) and pulls the results back to
# ~/hitz-xbox/runs/<name>/ on the host: run_xbox.ps1 (not in this repo) does
# the actual launch+wait+kill in the guest, scheduled via schtasks so it
# runs even without an interactive session. Blocks until the guest writes
# C:\proj\run\finished.txt, polling every 5 seconds once the expected
# duration plus a safety margin has passed.
N=$1; S=${2:-120}
W="ssh -o BatchMode=yes -p 2222 hitz@127.0.0.1"
# /tmp/run_env.txt (if present) carries environment variables the next run
# should use, e.g. HITZ_* or RECOMP_* overrides set by the caller.
[ -f /tmp/run_env.txt ] && scp -q -P 2222 /tmp/run_env.txt hitz@127.0.0.1:C:/proj/run_env.txt
$W "schtasks /create /f /tn runx /sc once /st 23:59 /it /ru hitz /tr \"powershell -NoProfile -ExecutionPolicy Bypass -File C:\\proj\\run_xbox.ps1 -Seconds $S\" >nul && schtasks /run /tn runx >nul"
sleep $((S+15))
until $W "if exist C:\\proj\\run\\finished.txt (exit 0) else (exit 1)"; do sleep 5; done
D=~/hitz-xbox/runs/$N; rm -rf $D; mkdir -p $D
scp -q -P 2222 "hitz@127.0.0.1:C:/proj/run/*" $D/
# status.txt is UTF-16 (PowerShell's default encoding); show it as UTF-8.
iconv -f utf-16 -t utf-8 $D/status.txt
