#!/bin/sh
# Smoke test: run the demo headless-ish for a few frames on every backend and save screenshots.
# Uses SDL's offscreen video driver when there is no display.
set -e
BIN=${1:-build/linux/prism-demo}
OUT=build/test
mkdir -p "$OUT"
[ -n "$DISPLAY$WAYLAND_DISPLAY" ] || export SDL_VIDEODRIVER=offscreen
for r in soft gles2; do
    if "$BIN" --render "$r" --frames 30 --screenshot "$OUT/$r.ppm" --size 640x480; then
        echo "ok: $r -> $OUT/$r.ppm"
    else
        echo "FAIL: $r"
        status=1
    fi
done
exit ${status:-0}
