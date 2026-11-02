/* rt_gpu.h: the raytracing uniform block, laid out for std140, shared by the GPU backends.
 * Must match RTBlock in shaders/raytrace.frag. Everything is vec4/mat4 so C packing = std140. */
#ifndef P3D_RT_GPU_H
#define P3D_RT_GPU_H

#include "p3d_render.h"
#include <string.h>

typedef struct {
    float inv[16];
    float color[4];      /* rgb, reflect */
    float params[4];     /* kind, checker, 0, 0 */
} p3d_rt_gpu_prim;

typedef struct {
    float cam_to_world[16];
    float cam[4];        /* tan(fovy/2), aspect, width, height */
    float counts[4];     /* prims, lights, bounces, 0 */
    float ambient[4];
    float clear[4];
    float light_vec[P3D_MAX_LIGHTS][4];   /* xyz, w = 0 directional (direction of travel) / 1 point (position) */
    float light_color[P3D_MAX_LIGHTS][4]; /* rgb, w = range */
    p3d_rt_gpu_prim prims[P3D_RT_MAX_PRIMS];
} p3d_rt_gpu;

static void p3d_rt_pack(const p3d_frame *f, const p3d_rt_scene *s, p3d_rt_gpu *g)
{
    int i;
    memset(g, 0, sizeof(*g));
    memcpy(g->cam_to_world, s->camera_to_world.m, sizeof(g->cam_to_world));
    g->cam[0] = s->tan_half_fov_y;
    g->cam[1] = f->height > 0 ? (float)f->width / (float)f->height : 1.0f;
    g->cam[2] = (float)f->width;
    g->cam[3] = (float)f->height;
    g->counts[0] = (float)s->prim_count;
    g->counts[1] = (float)(f->light_count < P3D_MAX_LIGHTS ? f->light_count : P3D_MAX_LIGHTS);
    g->counts[2] = (float)(s->bounces < 0 ? 0 : s->bounces > 4 ? 4 : s->bounces);
    memcpy(g->ambient, f->ambient, sizeof(float) * 3);
    memcpy(g->clear, f->clear_color, sizeof(float) * 3);
    for (i = 0; i < (int)g->counts[1]; ++i) {
        const p3d_light *l = &f->lights[i];
        g->light_vec[i][0] = l->vector.x; g->light_vec[i][1] = l->vector.y; g->light_vec[i][2] = l->vector.z;
        g->light_vec[i][3] = l->kind == P3D_LIGHT_POINT ? 1.0f : 0.0f;
        memcpy(g->light_color[i], l->color, sizeof(float) * 3);
        g->light_color[i][3] = l->range;
    }
    for (i = 0; i < s->prim_count; ++i) {
        const p3d_rt_prim *p = &s->prims[i];
        memcpy(g->prims[i].inv, p->world_to_local.m, sizeof(g->prims[i].inv));
        memcpy(g->prims[i].color, p->color, sizeof(float) * 3);
        g->prims[i].color[3] = p->reflect;
        g->prims[i].params[0] = (float)p->kind;
        g->prims[i].params[1] = p->checker ? 1.0f : 0.0f;
    }
}

#endif
