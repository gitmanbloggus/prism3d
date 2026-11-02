/* p3d_render.h: renderer front end and the backend interface.
 *
 * Capability tiers: every backend declares the highest tier it implements. Content (materials) asks
 * for features and gets the best fallback the active tier supports; games query p3d_render_caps()
 * rather than backend names.
 *
 *   T0  P3D_TIER_SOFT    CPU rasterizer: flat/Gouraud, affine textures, z-buffer
 *   T1  P3D_TIER_FIXED   fixed-function GPUs: GL 1.x, D3D7, PS2 GS, Dreamcast PVR
 *   T2  P3D_TIER_SHADER  programmable: GLES2/3, WebGL, GL 2.1, D3D9
 *   T3  P3D_TIER_MODERN  explicit/custom pipelines: Vulkan 1.0, Wii GX + TEV
 */
#ifndef P3D_RENDER_H
#define P3D_RENDER_H

#include "../core/p3d_math.h"
#include "../platform/p3d_platform.h"
#include <stdint.h>

typedef enum { P3D_TIER_SOFT = 0, P3D_TIER_FIXED = 1, P3D_TIER_SHADER = 2, P3D_TIER_MODERN = 3 } p3d_tier;

typedef struct {
    p3d_tier tier;
    int max_texture_size;
    int npot_textures;       /* non-power-of-two textures allowed */
    int max_lights;
    int per_pixel_lighting;
} p3d_caps;

typedef struct {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
} p3d_vertex;

typedef uint32_t p3d_mesh_id;      /* 0 = none */
typedef uint32_t p3d_texture_id;   /* 0 = none (white) */

/* Lighting the material asks for. A tier that can't do it falls back one step down:
 * PER_PIXEL -> PER_VERTEX -> FLAT. UNLIT is always available. */
typedef enum { P3D_SHADE_UNLIT, P3D_SHADE_FLAT, P3D_SHADE_PER_VERTEX, P3D_SHADE_PER_PIXEL } p3d_shading;

typedef struct {
    float color[4];
    p3d_texture_id texture;
    p3d_shading shading;
} p3d_material;

#define P3D_MAX_LIGHTS 4
typedef enum { P3D_LIGHT_DIRECTIONAL, P3D_LIGHT_POINT } p3d_light_kind;
typedef struct {
    p3d_light_kind kind;
    p3d_vec3 vector;      /* direction the light travels (directional) or world position (point) */
    float color[3];
    float range;          /* point lights */
} p3d_light;

typedef struct {
    p3d_mat4 view, proj;
    float clear_color[3];
    float ambient[3];
    p3d_light lights[P3D_MAX_LIGHTS];
    int light_count;
    int width, height;
} p3d_frame;

typedef struct {
    p3d_mesh_id mesh;
    p3d_mat4 model;
    p3d_material material;   /* shading already resolved for the active tier */
} p3d_draw;

/* A render backend. Backends register themselves in render.c's table; with the dynamic module loader
 * a backend can also live in a shared library exporting this struct. */
typedef struct p3d_render_backend {
    const char *name;
    p3d_tier tier;
    p3d_surface_kind surface;
    int  (*init)(p3d_caps *caps);
    void (*shutdown)(void);
    p3d_mesh_id (*mesh_create)(const p3d_vertex *vertices, int vertex_count, const uint16_t *indices, int index_count);
    void (*mesh_destroy)(p3d_mesh_id mesh);
    p3d_texture_id (*texture_create)(int width, int height, const uint8_t *rgba);
    void (*texture_destroy)(p3d_texture_id texture);
    void (*begin_frame)(const p3d_frame *frame);
    void (*draw)(const p3d_draw *draw);
    void (*end_frame)(void);
    /* Copy the last frame as RGBA8, top row first (screenshots/tests). May be NULL. */
    int  (*read_pixels)(int width, int height, uint8_t *rgba);
} p3d_render_backend;

/* ---- front end ---- */
const p3d_render_backend *p3d_render_find(const char *name);    /* NULL = best available */
void p3d_render_list_backends(void);
int  p3d_render_init(const p3d_render_backend *backend);         /* after the platform window exists */
void p3d_render_shutdown(void);
const p3d_caps *p3d_render_caps(void);
const char *p3d_render_backend_name(void);
p3d_shading p3d_render_resolve_shading(p3d_shading wanted);

p3d_mesh_id    p3d_render_mesh_create(const p3d_vertex *v, int vertex_count, const uint16_t *idx, int index_count);
void           p3d_render_mesh_destroy(p3d_mesh_id mesh);
p3d_texture_id p3d_render_texture_create(int width, int height, const uint8_t *rgba);
void           p3d_render_texture_destroy(p3d_texture_id texture);
void p3d_render_begin_frame(const p3d_frame *frame);
void p3d_render_draw(const p3d_draw *draw);
void p3d_render_end_frame(void);
int  p3d_render_save_screenshot(const char *path, int width, int height);   /* binary PPM */

#endif
