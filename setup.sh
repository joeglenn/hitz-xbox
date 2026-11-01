#!/bin/sh
# NHL Hitz Pro recomp: one-step setup (patch, extract, generate). Building needs Windows.
#   ./setup.sh --iso "/path/NHL Hitz Pro (USA).iso"
#   ./setup.sh --help
set -e
cd "$(dirname "$0")"
VENV=.tools/venv
if [ ! -x "$VENV/bin/python" ]; then
    echo "=== Creating Python environment in $VENV"
    mkdir -p .tools
    if ! python3 -m venv "$VENV" 2>/dev/null; then
        rm -rf "$VENV"
        echo "[!] python3 -m venv failed. Install Python 3.10+ with venv (Debian/Ubuntu: sudo apt install python3-venv) and rerun."
        exit 1
    fi
fi
"$VENV/bin/python" -c "import capstone" 2>/dev/null || "$VENV/bin/python" -m pip install -q capstone
exec "$VENV/bin/python" setup.py "$@"
