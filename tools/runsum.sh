#!/bin/bash
# runsum.sh <boot>... : one-line health summary per run
#
# Companion to runvm.sh: feed it the same run names runvm.sh was given, and
# it prints, for each one, the final status line, how many crashes and
# pushbuffer-walk drops the stderr log recorded, the log's line count, and
# the fps/frame-time samples the D3D11 renderer logged every 10 seconds.
# Reads only files already collected under ~/hitz-xbox/runs/<name>/ (by
# runvm.sh); does not touch the VM itself.
for b in "$@"; do
    d=~/hitz-xbox/runs/$b
    [ -d "$d" ] || { echo "== $b (missing)"; continue; }
    cd "$d"
    # status.txt is written by the guest in UTF-16 (PowerShell's default); convert
    # to UTF-8 and strip the CR/LF so it prints on the same line as the summary.
    st=$(tail -c 60 status.txt 2>/dev/null | iconv -f utf-16 -t utf-8 2>/dev/null | tr -d '\r\n')
    echo "== $b: $st | crash $(grep -ac 'CRASH\]' stderr.log) | pbdrop $(grep -ac 'PB\] walk left' stderr.log) | lines $(wc -l < stderr.log)"
    grep -a "D3D11\] .*fps |" stderr.log | awk '{printf "%ds %s/%s  ", NR*10, $2, $5}'
    echo
done
