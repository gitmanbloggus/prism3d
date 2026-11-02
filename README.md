# Prism3D

A small, portable C engine for low-end and old hardware, with SIPL/SEB scripting.
See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Build and run (Linux)

Needs: a C compiler, SDL2, libGLESv2 (Mesa or a vendor driver), Python 3 (SIPL compiler and shader generator);
for the Vulkan backend, the Vulkan headers/loader and `glslc` (drop `vulkan` from `RENDERERS` to build without).

```sh
git clone https://github.com/gitmanbloggus/SIPLaSEB siplaseb   # SEB VM + SIPL compiler (needs the yield patch)
make            # builds build/linux/prism-demo and compiles scripts/*.sipl
make run        # arrow keys move the sphere, Escape quits
build/linux/prism-demo scripts/raytrace.bin   # raytracing demo: R toggles raytrace/raster
build/linux/prism-demo --render soft          # pick a backend: vulkan, gles3, gles2, soft
make test       # every demo on every backend, screenshots in build/test/
```

Web: `make TARGET=web` with Emscripten on PATH (not yet verified).
