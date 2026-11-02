/* render_soft.c: tier T0 CPU rasterizer. Gouraud lighting, perspective-correct textures, float
 * z-buffer, near-plane clipping. Renders at the frame size the engine asks for (default 320x240)
 * and the platform scales it up. Plain C, no SIMD, so it ports to anything with a framebuffer. */
#include "../p3d_render.h"
#include "../../core/p3d_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MESHES 1024
#define MAX_TEXTURES 256

typedef struct { p3d_vertex *v; uint16_t *idx; int vcount, icount; } soft_mesh;
typedef struct { uint8_t *rgba; int w, h; } soft_texture;

/* a vertex after transform: clip position, light, uv */
typedef struct { float x, y, z, w; float r, g, b; float u, v; } cvert;

static soft_mesh meshes[MAX_MESHES];
static soft_texture textures[MAX_TEXTURES];
static uint32_t *color_buf;
static float *depth_buf;
static int fb_w, fb_h, fb_cap;
static p3d_frame frame;
static p3d_mat4 view_proj;

static int soft_init(p3d_caps *caps)
{
    caps->max_texture_size = 1024;
    caps->npot_textures = 1;
    caps->max_lights = P3D_MAX_LIGHTS;
    caps->per_pixel_lighting = 0;
    return 1;
}

static void soft_shutdown(void)
{
    int i;
    for (i = 0; i < MAX_MESHES; ++i) { free(meshes[i].v); free(meshes[i].idx); meshes[i].v = NULL; meshes[i].idx = NULL; }
    for (i = 0; i < MAX_TEXTURES; ++i) { free(textures[i].rgba); textures[i].rgba = NULL; }
    free(color_buf); free(depth_buf);
    color_buf = NULL; depth_buf = NULL; fb_cap = 0;
}

static p3d_mesh_id soft_mesh_create(const p3d_vertex *v, int vertex_count, const uint16_t *idx, int index_count)
{
    int i;
    for (i = 0; i < MAX_MESHES; ++i)
        if (meshes[i].v == NULL) break;
    if (i == MAX_MESHES) return 0;
    meshes[i].v = (p3d_vertex *)malloc(sizeof(*v) * (size_t)vertex_count);
    meshes[i].idx = (uint16_t *)malloc(sizeof(*idx) * (size_t)index_count);
    if (!meshes[i].v || !meshes[i].idx) { free(meshes[i].v); free(meshes[i].idx); meshes[i].v = NULL; return 0; }
    memcpy(meshes[i].v, v, sizeof(*v) * (size_t)vertex_count);
    memcpy(meshes[i].idx, idx, sizeof(*idx) * (size_t)index_count);
    meshes[i].vcount = vertex_count;
    meshes[i].icount = index_count;
    return (p3d_mesh_id)(i + 1);
}

static void soft_mesh_destroy(p3d_mesh_id id)
{
    if (id == 0 || id > MAX_MESHES) return;
    free(meshes[id - 1].v); free(meshes[id - 1].idx);
    meshes[id - 1].v = NULL; meshes[id - 1].idx = NULL;
}

static p3d_texture_id soft_texture_create(int w, int h, const uint8_t *rgba)
{
    int i;
    for (i = 0; i < MAX_TEXTURES; ++i)
        if (textures[i].rgba == NULL) break;
    if (i == MAX_TEXTURES) return 0;
    textures[i].rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    if (textures[i].rgba == NULL) return 0;
    memcpy(textures[i].rgba, rgba, (size_t)w * (size_t)h * 4u);
    textures[i].w = w;
    textures[i].h = h;
    return (p3d_texture_id)(i + 1);
}

static void soft_texture_destroy(p3d_texture_id id)
{
    if (id == 0 || id > MAX_TEXTURES) return;
    free(textures[id - 1].rgba);
    textures[id - 1].rgba = NULL;
}

static void soft_begin_frame(const p3d_frame *f)
{
    int i, n = f->width * f->height;
    uint32_t clear = 0xFF000000u | ((uint32_t)(f->clear_color[0] * 255.0f) << 16) |
                     ((uint32_t)(f->clear_color[1] * 255.0f) << 8) | (uint32_t)(f->clear_color[2] * 255.0f);
    frame = *f;
    view_proj = p3d_m4_mul(f->proj, f->view);
    if (n > fb_cap) {
        free(color_buf); free(depth_buf);
        color_buf = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)n);
        depth_buf = (float *)malloc(sizeof(float) * (size_t)n);
        fb_cap = (color_buf && depth_buf) ? n : 0;
    }
    fb_w = f->width;
    fb_h = f->height;
    if (fb_cap == 0) return;
    for (i = 0; i < n; ++i) { color_buf[i] = clear; depth_buf[i] = 1.0f; }
}

static void light_vertex(const p3d_material *mat, p3d_vec3 p, p3d_vec3 n, float out[3])
{
    int i;
    out[0] = frame.ambient[0]; out[1] = frame.ambient[1]; out[2] = frame.ambient[2];
    if (mat->shading == P3D_SHADE_UNLIT) { out[0] = out[1] = out[2] = 1.0f; return; }
    for (i = 0; i < frame.light_count && i < P3D_MAX_LIGHTS; ++i) {
        const p3d_light *l = &frame.lights[i];
        p3d_vec3 dir;
        float att = 1.0f, ndl;
        if (l->kind == P3D_LIGHT_POINT) {
            p3d_vec3 d = p3d_v3_sub(l->vector, p);
            float dist = sqrtf(p3d_v3_dot(d, d));
            dir = p3d_v3_scale(d, 1.0f / (dist > 1e-4f ? dist : 1e-4f));
            att = 1.0f - dist / l->range;
            att = att < 0.0f ? 0.0f : att * att;
        } else dir = p3d_v3_scale(l->vector, -1.0f);
        ndl = p3d_v3_dot(n, dir);
        if (ndl > 0.0f) {
            out[0] += l->color[0] * ndl * att;
            out[1] += l->color[1] * ndl * att;
            out[2] += l->color[2] * ndl * att;
        }
    }
}

static cvert lerp_cv(const cvert *a, const cvert *b, float t)
{
    cvert r;
    r.x = a->x + (b->x - a->x) * t; r.y = a->y + (b->y - a->y) * t;
    r.z = a->z + (b->z - a->z) * t; r.w = a->w + (b->w - a->w) * t;
    r.r = a->r + (b->r - a->r) * t; r.g = a->g + (b->g - a->g) * t; r.b = a->b + (b->b - a->b) * t;
    r.u = a->u + (b->u - a->u) * t; r.v = a->v + (b->v - a->v) * t;
    return r;
}

static uint32_t sample(const soft_texture *t, float u, float v)
{
    int x, y;
    const uint8_t *p;
    if (t == NULL) return 0xFFFFFFFFu;
    x = (int)floorf(u * (float)t->w) % t->w; if (x < 0) x += t->w;
    y = (int)floorf(v * (float)t->h) % t->h; if (y < 0) y += t->h;
    p = t->rgba + ((size_t)y * (size_t)t->w + (size_t)x) * 4u;
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* screen-space vertex: pixel x/y, depth, 1/w and attributes pre-divided by w */
typedef struct { float x, y, z, iw, r, g, b, u, v; } svert;

static void raster(const svert *a, const svert *b, const svert *c, const float color[4], const soft_texture *tex)
{
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    int minx, maxx, miny, maxy, x, y;
    if (area >= 0.0f) return;   /* counter-clockwise in NDC becomes negative with y flipped: back faces cull */
    minx = (int)floorf(fminf(a->x, fminf(b->x, c->x))); maxx = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    miny = (int)floorf(fminf(a->y, fminf(b->y, c->y))); maxy = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > fb_w - 1) maxx = fb_w - 1;
    if (maxy > fb_h - 1) maxy = fb_h - 1;
    for (y = miny; y <= maxy; ++y) {
        float py = (float)y + 0.5f;
        for (x = minx; x <= maxx; ++x) {
            float px = (float)x + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px)) / area;
            float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px)) / area;
            float w2 = 1.0f - w0 - w1;
            float z, iw, persp, r, g, bl;
            uint32_t texel;
            size_t i;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            z = w0 * a->z + w1 * b->z + w2 * c->z;
            i = (size_t)y * (size_t)fb_w + (size_t)x;
            if (z >= depth_buf[i]) continue;
            iw = w0 * a->iw + w1 * b->iw + w2 * c->iw;
            persp = 1.0f / iw;
            texel = sample(tex, (w0 * a->u + w1 * b->u + w2 * c->u) * persp,
                           (w0 * a->v + w1 * b->v + w2 * c->v) * persp);
            r = (w0 * a->r + w1 * b->r + w2 * c->r) * persp * color[0] * (float)((texel >> 16) & 255);
            g = (w0 * a->g + w1 * b->g + w2 * c->g) * persp * color[1] * (float)((texel >> 8) & 255);
            bl = (w0 * a->b + w1 * b->b + w2 * c->b) * persp * color[2] * (float)(texel & 255);
            if (r > 255.0f) r = 255.0f;
            if (g > 255.0f) g = 255.0f;
            if (bl > 255.0f) bl = 255.0f;
            depth_buf[i] = z;
            color_buf[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
        }
    }
}

static svert to_screen(const cvert *v)
{
    svert s;
    float iw = 1.0f / v->w;
    s.x = (v->x * iw * 0.5f + 0.5f) * (float)fb_w;
    s.y = (1.0f - (v->y * iw * 0.5f + 0.5f)) * (float)fb_h;
    s.z = v->z * iw * 0.5f + 0.5f;
    s.iw = iw;
    s.r = v->r * iw; s.g = v->g * iw; s.b = v->b * iw;
    s.u = v->u * iw; s.v = v->v * iw;
    return s;
}

/* clip a triangle against the near plane (z > -w), then fan-rasterize */
static void draw_triangle(const cvert *tri, const float color[4], const soft_texture *tex)
{
    cvert out[4];
    svert s[4];
    int n = 0, i;
    for (i = 0; i < 3; ++i) {
        const cvert *p = &tri[i], *q = &tri[(i + 1) % 3];
        float dp = p->z + p->w, dq = q->z + q->w;
        if (dp >= 0.0f) out[n++] = *p;
        if ((dp >= 0.0f) != (dq >= 0.0f)) out[n++] = lerp_cv(p, q, dp / (dp - dq));
    }
    if (n < 3) return;
    for (i = 0; i < n; ++i) s[i] = to_screen(&out[i]);
    for (i = 1; i + 1 < n; ++i) raster(&s[0], &s[i], &s[i + 1], color, tex);
}

static void soft_draw(const p3d_draw *d)
{
    const soft_mesh *m;
    const soft_texture *tex = NULL;
    p3d_mat4 mvp;
    cvert *cv;
    int i;
    if (fb_cap == 0 || d->mesh == 0 || d->mesh > MAX_MESHES || meshes[d->mesh - 1].v == NULL) return;
    m = &meshes[d->mesh - 1];
    if (d->material.texture && d->material.texture <= MAX_TEXTURES && textures[d->material.texture - 1].rgba)
        tex = &textures[d->material.texture - 1];
    mvp = p3d_m4_mul(view_proj, d->model);
    cv = (cvert *)malloc(sizeof(cvert) * (size_t)m->vcount);
    if (cv == NULL) return;
    for (i = 0; i < m->vcount; ++i) {
        const p3d_vertex *v = &m->v[i];
        p3d_vec4 p4, c;
        p3d_vec3 world, n;
        float light[3];
        p4.x = v->px; p4.y = v->py; p4.z = v->pz; p4.w = 1.0f;
        c = p3d_m4_mul_v4(mvp, p4);
        p4 = p3d_m4_mul_v4(d->model, p4);
        world = p3d_v3(p4.x, p4.y, p4.z);
        n = p3d_v3_normalize(p3d_m4_mul_dir(d->model, p3d_v3(v->nx, v->ny, v->nz)));
        light_vertex(&d->material, world, n, light);
        cv[i].x = c.x; cv[i].y = c.y; cv[i].z = c.z; cv[i].w = c.w;
        cv[i].r = light[0]; cv[i].g = light[1]; cv[i].b = light[2];
        cv[i].u = v->u; cv[i].v = v->v;
    }
    for (i = 0; i + 2 < m->icount; i += 3) {
        cvert tri[3];
        tri[0] = cv[m->idx[i]]; tri[1] = cv[m->idx[i + 1]]; tri[2] = cv[m->idx[i + 2]];
        draw_triangle(tri, d->material.color, tex);
    }
    free(cv);
}

static void soft_end_frame(void)
{
    if (fb_cap) p3d_platform_present_pixels(color_buf, fb_w, fb_h);
}

static int soft_read_pixels(int width, int height, uint8_t *rgba)
{
    int i;
    if (width != fb_w || height != fb_h || fb_cap == 0) return 0;
    for (i = 0; i < width * height; ++i) {
        uint32_t c = color_buf[i];
        rgba[i * 4] = (uint8_t)(c >> 16); rgba[i * 4 + 1] = (uint8_t)(c >> 8);
        rgba[i * 4 + 2] = (uint8_t)c; rgba[i * 4 + 3] = 255;
    }
    return 1;
}

const p3d_render_backend p3d_backend_soft = {
    "soft", P3D_TIER_SOFT, P3D_SURFACE_PIXELS,
    soft_init, soft_shutdown,
    soft_mesh_create, soft_mesh_destroy,
    soft_texture_create, soft_texture_destroy,
    soft_begin_frame, soft_draw, NULL, soft_end_frame,
    soft_read_pixels
};
