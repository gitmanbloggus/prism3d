/* p3d_engine.h: ties the layers together and owns the main loop. */
#ifndef P3D_ENGINE_H
#define P3D_ENGINE_H

#include "../platform/p3d_platform.h"
#include "../render/p3d_render.h"
#include "../scene/p3d_scene.h"
#include "../module/p3d_module.h"
#include "../script/p3d_script.h"

typedef struct {
    const char *title;
    int width, height;          /* window size */
    const char *renderer;       /* backend name, NULL = best available */
    int soft_width, soft_height;/* internal resolution of the software tier (0 = 320x240) */
    int max_objects;            /* 0 = 1024 */
} p3d_engine_desc;

typedef struct p3d_engine {
    p3d_scene scene;
    p3d_input input;
    const p3d_render_backend *backend;
    int soft_width, soft_height;
    uint32_t start_ms, last_ms;
    uint32_t time_ms, frame_ms;     /* since start, last frame */
    uint64_t frame;
    int running;
    /* testing hooks */
    uint64_t max_frames;            /* 0 = unlimited */
    const char *screenshot_path;    /* written on the last frame when max_frames is set */
    int quit_when_scripts_end;
} p3d_engine;

int  p3d_engine_init(p3d_engine *engine, const p3d_engine_desc *desc);
int  p3d_engine_frame(p3d_engine *engine);   /* one tick; returns 0 when the engine should stop */
void p3d_engine_run(p3d_engine *engine);     /* native loop, or the browser's animation frame */
void p3d_engine_shutdown(p3d_engine *engine);

#endif
