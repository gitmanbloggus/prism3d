# Prism3D architecture

A low-level 3D engine in C for hardware from the fixed-function era up to Vulkan, plus custom console
pipelines such as the Wii's GX. It aims to be flexible through **capability tiers** and **modules**, not
through abstraction layers stacked on top of each other.

## Decisions

| Topic | Decision |
|---|---|
| Language | C99 for the engine (the SEB VM stays C11, as upstream). No C++ and no required libraries beyond the platform's. |
| Build | Plain GNU Make: one `config/<target>.mk` per target, chosen with `make TARGET=…`. |
| Rendering | Capability tiers T0–T3. Backends declare a tier; materials fall back per tier. |
| Modules | One `p3d_module` interface, linked statically or loaded as a shared library (`P3D_DYNAMIC_MODULES`). |
| Scripting | SIPL compiled to SEB, one VM per script component, talking to the engine through the `prism` device. |
| Assets | Dev builds load standard formats directly (TGA/PPM now, glTF/PNG next). Release builds use cooked per-platform packs (planned). |
| Built-in scene | Primitives (cube, sphere, plane) and components (Mesh, Camera, Light, Script). Textures are resources. Everything else is a module. |
| Coordinates | Right-handed, +Y up, cameras look down −Z, column-major matrices. |

## Layers

```
games/<game>/          game entry point (C) + scripts/*.sipl
src/engine/            main loop: poll → scripts → modules → render
src/script/            SEB VMs + the `prism` device (scripts/lib/prism.def)
src/scene/             objects, built-in components, primitive meshes
src/module/            module registry (static + dlopen)
src/assets/            dev-mode loaders (later: cooked-pack reader)
src/render/            front end + backends/<name>/render_<name>.c
src/platform/          window, input, time: platform_<name>.c
src/core/              logging, arenas, file IO, math
```

The dependencies only point downward, and nothing below `engine/` knows about scripts.

## Render tiers

| Tier | Meaning | Backends (✓ = exists) |
|---|---|---|
| T0 `P3D_TIER_SOFT` | CPU rasterizer | ✓ `soft`: Gouraud, perspective-correct textures, z-buffer, near clipping |
| T1 `P3D_TIER_FIXED` | fixed-function GPUs | planned: GL 1.x, D3D7, PS2 GS, Dreamcast PVR |
| T2 `P3D_TIER_SHADER` | programmable | ✓ `gles2`: GLES2/WebGL 1, per-pixel and per-vertex programs |
| T3 `P3D_TIER_MODERN` | explicit / custom | planned: Vulkan 1.0, Wii GX + TEV |

- A backend fills in a `p3d_render_backend` struct and reports its `p3d_caps`.
- Materials ask for a shading level. `p3d_render_resolve_shading` steps it down to what the tier can do:
  per-pixel → per-vertex → flat → unlit.
- Games branch on `p3d_render_caps()`, never on backend names. Scripts read `prism.tier`.

To add a backend:
1. Write `src/render/<x>/render_<x>.c` that exports `p3d_backend_<x>`.
2. Add `<x>` to `RENDERERS` in the target config.
3. Add one line to the table in `render.c`.

## Scripts and the main loop

SEB programs own their own loop (`jump (id 1)`). To let the engine own the frame instead (needed for the
browser, and for running many scripts), the SEB VM gained a **yield**:

- A device register's write callback returns `SEB_DEVICE_YIELD`.
- `seb_vm_run` then returns `SEB_YIELD`, and the next call resumes after that instruction.
- `max_steps` is now a budget per call.

In SIPL a script just writes `prism.frame = 1` at the end of each frame. A script that runs 5M steps
without yielding is stopped and reported.

Every script gets its own VM and its own `prism` device instance, bound to the object it is attached to
(`prism.self`). See `scripts/lib/prism.def` for every register and its units: thousandths for distances,
thousandths of a degree for angles, ms for time, 0xRRGGBB for colours.

Keyboard state is the shared region `prism.keys`, indexed by USB HID usage. In keeping with SEB's
determinism rule it is only written during the `prism.frame` doorbell.

## Roadmap

1. **M1 (this): done.** GLES2 + software tiers, SDL2 platform, primitives and components, SIPL scripting, Linux build.
   Web config written but **not yet built or run** (no emcc here).
2. M2: run the web build; glTF + PNG dev loading; the asset cooker (`tools/cook`) and pack format; fixed-point math option.
3. M3: T1 backend (GL 1.x) and a Windows XP build (MinGW + SDL 2.0.x).
4. M4: in-engine editor module; script hot reload.
5. Later: Vulkan 1.0 and Wii GX (T3), retro platform layers (DOS/DJGPP, PS2, Dreamcast), audio and physics modules.
