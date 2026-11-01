#!/usr/bin/env bash
set -uo pipefail
cd "$(dirname "$0")"

echo "[1/3] Checking build dependencies..."
for tool in gcc make pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Missing required tool: $tool"; exit 2; }
done
pkg-config --exists xft || { echo "Missing Xft development package (pkg-config xft)."; exit 2; }
pkg-config --exists x11 || { echo "Missing X11 development package (pkg-config x11)."; exit 2; }

echo "[2/3] Building isolated test harness..."
CFLAGS="-std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -g"
XFLAGS="$(pkg-config --cflags xft x11)"
XLIBS="$(pkg-config --libs xft x11)"
gcc $CFLAGS $XFLAGS -o terminator_tester tester.c shell.c parser.c builtins.c hotkeys.c multiwatch.c $XLIBS || exit 2

echo "[3/3] Running tests..."
if [[ -n "${DISPLAY:-}" ]]; then
    ./terminator_tester
elif command -v xvfb-run >/dev/null 2>&1; then
    xvfb-run -a ./terminator_tester
else
    echo "No DISPLAY and xvfb-run is unavailable."
    echo "On Arch: sudo pacman -S --needed xorg-server-xvfb"
    echo "Then rerun: ./run_tests.sh"
    exit 2
fi
