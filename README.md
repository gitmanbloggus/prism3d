# Prism3D

A small, portable C engine for low-end and old hardware, with SIPL/SEB scripting.
See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Build and run (Linux)

Needs: a C compiler, SDL2, libGLESv2 (Mesa or a vendor driver), Python 3 (for the SIPL compiler).

```sh
git clone https://github.com/gitmanbloggus/SIPLaSEB siplaseb   # SEB VM + SIPL compiler (needs the yield patch)
make            # builds build/linux/prism-demo and compiles scripts/*.sipl
make run        # arrow keys move the sphere, Escape quits
build/linux/prism-demo --render soft    # software tier (T0)
make test       # renders 30 frames on each backend, writes build/test/*.ppm
```

Web: `make TARGET=web` with Emscripten on PATH (not yet verified).
