#!/bin/sh
# Smoke test: run each demo script on every backend for a few frames and save screenshots.
# Uses SDL's offscreen video driver when there is no display. Backends that can't start here
# (no GPU, no driver) are reported as skipped, not failed.
BIN=${1:-build/linux/prism-demo}
OUT=build/test
mkdir -p "$OUT"
[ -n "$DISPLAY$WAYLAND_DISPLAY" ] || export SDL_VIDEODRIVER=offscreen
status=0
for script in demo raytrace; do
    for r in vulkan gles3 gles2 soft; do
        log="$OUT/$script-$r.log"
        if "$BIN" --render "$r" --frames 60 --screenshot "$OUT/$script-$r.ppm" --size 640x480 "scripts/$script.bin" >"$log" 2>&1; then
            if grep -q "wrote $OUT/$script-$r.ppm" "$log"; then echo "ok:   $script on $r"
            else echo "FAIL: $script on $r (no screenshot)"; status=1; fi
        elif grep -q "no render backend could start\|no render backend named" "$log"; then
            echo "skip: $script on $r (backend unavailable here)"
        else
            echo "FAIL: $script on $r (see $log)"; status=1
        fi
    done
done
exit $status
