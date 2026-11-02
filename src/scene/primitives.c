#include "p3d_scene.h"

#include <math.h>
#include <stdlib.h>

static p3d_vertex vtx(float px, float py, float pz, float nx, float ny, float nz, float u, float v)
{
    p3d_vertex r;
    r.px = px; r.py = py; r.pz = pz; r.nx = nx; r.ny = ny; r.nz = nz; r.u = u; r.v = v;
    return r;
}

static p3d_mesh_id cube(void)
{
    /* face normal, then the two in-plane axes (u, v) chosen so u x v = normal (CCW from outside) */
    static const float faces[6][9] = {
        {  1, 0, 0,   0, 0,-1,   0, 1, 0 }, { -1, 0, 0,   0, 0, 1,   0, 1, 0 },
        {  0, 1, 0,   1, 0, 0,   0, 0,-1 }, {  0,-1, 0,   1, 0, 0,   0, 0, 1 },
        {  0, 0, 1,   1, 0, 0,   0, 1, 0 }, {  0, 0,-1,  -1, 0, 0,   0, 1, 0 }
    };
    p3d_vertex v[24];
    uint16_t idx[36];
    int f, k;
    for (f = 0; f < 6; ++f) {
        const float *n = faces[f], *u = faces[f] + 3, *w = faces[f] + 6;
        for (k = 0; k < 4; ++k) {
            float su = (k == 1 || k == 2) ? 0.5f : -0.5f, sv = (k >= 2) ? 0.5f : -0.5f;
            v[f * 4 + k] = vtx(n[0] * 0.5f + u[0] * su + w[0] * sv, n[1] * 0.5f + u[1] * su + w[1] * sv,
                               n[2] * 0.5f + u[2] * su + w[2] * sv, n[0], n[1], n[2], su + 0.5f, 0.5f - sv);
        }
        idx[f * 6] = (uint16_t)(f * 4); idx[f * 6 + 1] = (uint16_t)(f * 4 + 1); idx[f * 6 + 2] = (uint16_t)(f * 4 + 2);
        idx[f * 6 + 3] = (uint16_t)(f * 4); idx[f * 6 + 4] = (uint16_t)(f * 4 + 2); idx[f * 6 + 5] = (uint16_t)(f * 4 + 3);
    }
    return p3d_render_mesh_create(v, 24, idx, 36);
}

static p3d_mesh_id sphere(void)
{
    enum { RINGS = 12, SEGS = 20 };
    p3d_vertex v[(RINGS + 1) * (SEGS + 1)];
    uint16_t idx[RINGS * SEGS * 6];
    int r, s, n = 0;
    for (r = 0; r <= RINGS; ++r) {
        float phi = P3D_PI * (float)r / RINGS;
        for (s = 0; s <= SEGS; ++s) {
            float theta = 2.0f * P3D_PI * (float)s / SEGS;
            float x = sinf(phi) * cosf(theta), y = cosf(phi), z = -sinf(phi) * sinf(theta);
            v[r * (SEGS + 1) + s] = vtx(x * 0.5f, y * 0.5f, z * 0.5f, x, y, z, (float)s / SEGS, (float)r / RINGS);
        }
    }
    for (r = 0; r < RINGS; ++r)
        for (s = 0; s < SEGS; ++s) {
            uint16_t a = (uint16_t)(r * (SEGS + 1) + s), b = (uint16_t)(a + SEGS + 1);
            idx[n++] = a; idx[n++] = b; idx[n++] = (uint16_t)(b + 1);
            idx[n++] = a; idx[n++] = (uint16_t)(b + 1); idx[n++] = (uint16_t)(a + 1);
        }
    return p3d_render_mesh_create(v, (int)(sizeof(v) / sizeof(v[0])), idx, n);
}

static p3d_mesh_id plane(void)
{
    /* 1x1 in XZ facing +Y, split 4x4 so per-vertex lighting and the soft tier look reasonable */
    enum { N = 4 };
    p3d_vertex v[(N + 1) * (N + 1)];
    uint16_t idx[N * N * 6];
    int i, j, n = 0;
    for (j = 0; j <= N; ++j)
        for (i = 0; i <= N; ++i)
            v[j * (N + 1) + i] = vtx((float)i / N - 0.5f, 0.0f, (float)j / N - 0.5f, 0, 1, 0,
                                     (float)i / N, (float)j / N);
    for (j = 0; j < N; ++j)
        for (i = 0; i < N; ++i) {
            uint16_t a = (uint16_t)(j * (N + 1) + i), b = (uint16_t)(a + N + 1);
            idx[n++] = a; idx[n++] = b; idx[n++] = (uint16_t)(b + 1);
            idx[n++] = a; idx[n++] = (uint16_t)(b + 1); idx[n++] = (uint16_t)(a + 1);
        }
    return p3d_render_mesh_create(v, (int)(sizeof(v) / sizeof(v[0])), idx, n);
}

p3d_mesh_id p3d_primitive_mesh(p3d_primitive kind)
{
    switch (kind) {
    case P3D_PRIM_CUBE: return cube();
    case P3D_PRIM_SPHERE: return sphere();
    case P3D_PRIM_PLANE: return plane();
    default: return 0;
    }
}
