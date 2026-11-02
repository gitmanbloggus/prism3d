#include "p3d_render.h"
#include "../core/p3d_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef P3D_WITH_GLES2
extern const p3d_render_backend p3d_backend_gles2;
#endif
#ifdef P3D_WITH_SOFT
extern const p3d_render_backend p3d_backend_soft;
#endif

/* Best first. */
static const p3d_render_backend *const backends[] = {
#ifdef P3D_WITH_GLES2
    &p3d_backend_gles2,
#endif
#ifdef P3D_WITH_SOFT
    &p3d_backend_soft,
#endif
    NULL
};

static const p3d_render_backend *active;
static p3d_caps caps;

const p3d_render_backend *p3d_render_find(const char *name)
{
    size_t i;
    if (name == NULL || name[0] == '\0') return backends[0];
    for (i = 0; backends[i] != NULL; ++i)
        if (strcmp(backends[i]->name, name) == 0) return backends[i];
    return NULL;
}

void p3d_render_list_backends(void)
{
    size_t i;
    for (i = 0; backends[i] != NULL; ++i) printf("  %-8s tier T%d\n", backends[i]->name, (int)backends[i]->tier);
}

int p3d_render_init(const p3d_render_backend *backend)
{
    memset(&caps, 0, sizeof(caps));
    if (backend == NULL || !backend->init(&caps)) return 0;
    caps.tier = backend->tier;
    active = backend;
    P3D_INFO("renderer: %s (tier T%d, %d lights, max texture %d)", backend->name, (int)caps.tier,
             caps.max_lights, caps.max_texture_size);
    return 1;
}

void p3d_render_shutdown(void)
{
    if (active) active->shutdown();
    active = NULL;
}

const p3d_caps *p3d_render_caps(void) { return &caps; }
const char *p3d_render_backend_name(void) { return active ? active->name : "none"; }

p3d_shading p3d_render_resolve_shading(p3d_shading wanted)
{
    if (wanted == P3D_SHADE_PER_PIXEL && !caps.per_pixel_lighting) wanted = P3D_SHADE_PER_VERTEX;
    if (wanted != P3D_SHADE_UNLIT && caps.max_lights == 0) wanted = P3D_SHADE_UNLIT;
    return wanted;
}

p3d_mesh_id p3d_render_mesh_create(const p3d_vertex *v, int vertex_count, const uint16_t *idx, int index_count)
{
    return active->mesh_create(v, vertex_count, idx, index_count);
}
void p3d_render_mesh_destroy(p3d_mesh_id mesh) { active->mesh_destroy(mesh); }
p3d_texture_id p3d_render_texture_create(int width, int height, const uint8_t *rgba)
{
    return active->texture_create(width, height, rgba);
}
void p3d_render_texture_destroy(p3d_texture_id texture) { active->texture_destroy(texture); }
void p3d_render_begin_frame(const p3d_frame *frame) { active->begin_frame(frame); }
void p3d_render_draw(const p3d_draw *draw) { active->draw(draw); }
void p3d_render_end_frame(void) { active->end_frame(); }

int p3d_render_save_screenshot(const char *path, int width, int height)
{
    uint8_t *rgba;
    FILE *f;
    int i, ok;
    if (active == NULL || active->read_pixels == NULL) return 0;
    rgba = (uint8_t *)malloc((size_t)width * (size_t)height * 4u);
    if (rgba == NULL) return 0;
    ok = active->read_pixels(width, height, rgba);
    f = ok ? fopen(path, "wb") : NULL;
    if (f != NULL) {
        fprintf(f, "P6\n%d %d\n255\n", width, height);
        for (i = 0; i < width * height; ++i) fwrite(rgba + i * 4, 1, 3, f);
        ok = fclose(f) == 0;
    } else ok = 0;
    free(rgba);
    return ok;
}
