/* Prism3D milestone 1 demo: runs one SIPL script that builds and animates the scene. */
#include "../../src/engine/p3d_engine.h"
#include "../../src/core/p3d_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    printf("usage: prism-demo [options] [script.bin]\n"
           "  --render NAME      render backend; this build has:\n");
    p3d_render_list_backends();
    printf("  --size WxH         window size (default 960x720)\n"
           "  --soft-res WxH     software tier resolution (default 320x240)\n"
           "  --frames N         stop after N frames\n"
           "  --screenshot FILE  with --frames: save the last frame as a PPM\n");
}

int main(int argc, char **argv)
{
    static p3d_engine engine;      /* static: on the web it outlives main() */
    p3d_engine_desc desc;
    const char *script = "scripts/demo.bin", *shot = NULL;
    unsigned long frames = 0;
    int i;
    memset(&desc, 0, sizeof(desc));
    desc.title = "Prism3D demo";
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--render") && i + 1 < argc) desc.renderer = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &desc.width, &desc.height);
        else if (!strcmp(argv[i], "--soft-res") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &desc.soft_width, &desc.soft_height);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (argv[i][0] != '-') script = argv[i];
        else { usage(); return 2; }
    }
    if (!p3d_engine_init(&engine, &desc)) return 1;
    engine.max_frames = frames;
    engine.screenshot_path = shot;
    if (p3d_script_attach(&engine, script, 0) < 0) { p3d_engine_shutdown(&engine); return 1; }
    p3d_engine_run(&engine);
#ifndef __EMSCRIPTEN__
    p3d_engine_shutdown(&engine);
#endif
    return 0;
}
