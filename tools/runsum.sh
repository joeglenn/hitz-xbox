#!/bin/bash
# runsum.sh <boot>... : one-line health summary per run
for b in "$@"; do
    d=~/hitz-xbox/runs/$b
    [ -d "$d" ] || { echo "== $b (missing)"; continue; }
    cd "$d"
    st=$(tail -c 60 status.txt 2>/dev/null | iconv -f utf-16 -t utf-8 2>/dev/null | tr -d '\r\n')
    echo "== $b: $st | crash $(grep -ac 'CRASH\]' stderr.log) | pbdrop $(grep -ac 'PB\] walk left' stderr.log) | lines $(wc -l < stderr.log)"
    grep -a "D3D11\] .*fps |" stderr.log | awk '{printf "%ds %s/%s  ", NR*10, $2, $5}'
    echo
done
