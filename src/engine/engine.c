#include "p3d_engine.h"
#include "../core/p3d_core.h"

#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

int p3d_engine_init(p3d_engine *engine, const p3d_engine_desc *desc)
{
    p3d_window_desc win;
    memset(engine, 0, sizeof(*engine));
    engine->backend = p3d_render_find(desc->renderer);
    if (engine->backend == NULL) {
        P3D_ERROR("no render backend named '%s'; this build has:", desc->renderer ? desc->renderer : "");
        p3d_render_list_backends();
        return 0;
    }
    engine->soft_width = desc->soft_width > 0 ? desc->soft_width : 320;
    engine->soft_height = desc->soft_height > 0 ? desc->soft_height : 240;
    win.title = desc->title ? desc->title : "Prism3D";
    win.width = desc->width > 0 ? desc->width : 960;
    win.height = desc->height > 0 ? desc->height : 720;
    win.surface = engine->backend->surface;
    if (!p3d_platform_init(&win)) return 0;
    if (!p3d_render_init(engine->backend)) { p3d_platform_shutdown(); return 0; }
    if (!p3d_scene_init(&engine->scene, desc->max_objects > 0 ? desc->max_objects : 1024)) return 0;
    p3d_modules_init(engine);
    engine->start_ms = engine->last_ms = p3d_platform_ticks_ms();
    engine->running = 1;
    engine->quit_when_scripts_end = 1;
    return 1;
}

int p3d_engine_frame(p3d_engine *engine)
{
    uint32_t now = p3d_platform_ticks_ms();
    int w, h, last;
    engine->frame_ms = now - engine->last_ms;
    engine->time_ms = now - engine->start_ms;
    engine->last_ms = now;
    p3d_platform_poll(&engine->input);

    p3d_scripts_update(engine);
    p3d_modules_update(engine, (float)engine->frame_ms / 1000.0f);

    if (engine->backend->tier == P3D_TIER_SOFT) { w = engine->soft_width; h = engine->soft_height; }
    else p3d_platform_drawable_size(&w, &h);
    ++engine->frame;
    last = engine->max_frames && engine->frame >= engine->max_frames;
    p3d_scene_render_begin(&engine->scene, w, h);
    if (last && engine->screenshot_path) {
        if (p3d_render_save_screenshot(engine->screenshot_path, w, h)) P3D_INFO("wrote %s", engine->screenshot_path);
        else P3D_WARN("could not write %s", engine->screenshot_path);
    }
    p3d_render_end_frame();

    if (engine->input.quit || last || (engine->quit_when_scripts_end && p3d_scripts_running() == 0))
        engine->running = 0;
    return engine->running;
}

#ifdef __EMSCRIPTEN__
static void web_tick(void *arg)
{
    p3d_engine *engine = (p3d_engine *)arg;
    if (!p3d_engine_frame(engine)) {
        emscripten_cancel_main_loop();
        p3d_engine_shutdown(engine);
    }
}
#endif

void p3d_engine_run(p3d_engine *engine)
{
#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(web_tick, engine, 0, 1);
#else
    while (p3d_engine_frame(engine)) {}
#endif
}

void p3d_engine_shutdown(p3d_engine *engine)
{
    p3d_scripts_shutdown();
    p3d_modules_shutdown(engine);
    p3d_scene_shutdown(&engine->scene);
    p3d_render_shutdown();
    p3d_platform_shutdown();
}
