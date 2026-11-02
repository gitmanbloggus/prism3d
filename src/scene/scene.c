#include "p3d_scene.h"
#include "../core/p3d_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static p3d_texture_id make_checker(void)
{
    uint8_t px[64 * 64 * 4];
    int x, y;
    for (y = 0; y < 64; ++y)
        for (x = 0; x < 64; ++x) {
            uint8_t c = (((x >> 3) ^ (y >> 3)) & 1) ? 255 : 150;
            uint8_t *p = px + (y * 64 + x) * 4;
            p[0] = c; p[1] = c; p[2] = c; p[3] = 255;
        }
    return p3d_render_texture_create(64, 64, px);
}

int p3d_scene_init(p3d_scene *scene, int capacity)
{
    int k;
    memset(scene, 0, sizeof(*scene));
    scene->objects = (p3d_object *)calloc((size_t)capacity, sizeof(p3d_object));
    if (scene->objects == NULL) return 0;
    scene->capacity = capacity;
    scene->clear_color[0] = 0.10f; scene->clear_color[1] = 0.12f; scene->clear_color[2] = 0.16f;
    scene->ambient[0] = scene->ambient[1] = scene->ambient[2] = 0.15f;
    for (k = 1; k < P3D_PRIM_COUNT; ++k) scene->primitives[k] = p3d_primitive_mesh((p3d_primitive)k);
    scene->checker = make_checker();
    scene->rt_bounces = 2;
    return 1;
}

void p3d_scene_shutdown(p3d_scene *scene)
{
    int k;
    for (k = 1; k < P3D_PRIM_COUNT; ++k)
        if (scene->primitives[k]) p3d_render_mesh_destroy(scene->primitives[k]);
    if (scene->checker) p3d_render_texture_destroy(scene->checker);
    free(scene->objects);
    memset(scene, 0, sizeof(*scene));
}

p3d_object *p3d_scene_get(p3d_scene *scene, p3d_object_id id)
{
    if (id == 0 || (int)id > scene->capacity || !scene->objects[id - 1].used) return NULL;
    return &scene->objects[id - 1];
}

p3d_object_id p3d_scene_create(p3d_scene *scene)
{
    int i;
    for (i = 0; i < scene->capacity; ++i)
        if (!scene->objects[i].used) {
            p3d_object *o = &scene->objects[i];
            memset(o, 0, sizeof(*o));
            o->used = 1;
            o->scale = p3d_v3(1, 1, 1);
            return (p3d_object_id)(i + 1);
        }
    P3D_WARN("scene is full (%d objects)", scene->capacity);
    return 0;
}

void p3d_scene_destroy(p3d_scene *scene, p3d_object_id id)
{
    p3d_object *o = p3d_scene_get(scene, id);
    if (o == NULL) return;
    if (scene->active_camera == id) scene->active_camera = 0;
    o->used = 0;
}

p3d_object_id p3d_scene_spawn_primitive(p3d_scene *scene, p3d_primitive kind)
{
    p3d_object_id id;
    p3d_object *o;
    if (kind <= P3D_PRIM_NONE || kind >= P3D_PRIM_COUNT) return 0;
    id = p3d_scene_create(scene);
    if ((o = p3d_scene_get(scene, id)) == NULL) return 0;
    o->components |= P3D_COMP_MESH;
    o->mesh.mesh = scene->primitives[kind];
    o->mesh.material.color[0] = o->mesh.material.color[1] = o->mesh.material.color[2] = o->mesh.material.color[3] = 1.0f;
    o->mesh.material.shading = P3D_SHADE_PER_PIXEL;
    return id;
}

p3d_object_id p3d_scene_spawn_camera(p3d_scene *scene)
{
    p3d_object_id id = p3d_scene_create(scene);
    p3d_object *o = p3d_scene_get(scene, id);
    if (o == NULL) return 0;
    o->components |= P3D_COMP_CAMERA;
    o->camera.fov_y = 60.0f * P3D_PI / 180.0f;
    o->camera.znear = 0.1f;
    o->camera.zfar = 200.0f;
    if (scene->active_camera == 0) scene->active_camera = id;
    return id;
}

p3d_object_id p3d_scene_spawn_light(p3d_scene *scene, p3d_light_kind kind)
{
    p3d_object_id id = p3d_scene_create(scene);
    p3d_object *o = p3d_scene_get(scene, id);
    if (o == NULL) return 0;
    o->components |= P3D_COMP_LIGHT;
    o->light.kind = kind;
    o->light.color[0] = o->light.color[1] = o->light.color[2] = 1.0f;
    o->light.range = 10.0f;
    return id;
}

void p3d_scene_render_begin(p3d_scene *scene, int width, int height)
{
    p3d_frame f;
    const p3d_object *cam = p3d_scene_get(scene, scene->active_camera);
    int i;
    memset(&f, 0, sizeof(f));
    f.width = width;
    f.height = height;
    memcpy(f.clear_color, scene->clear_color, sizeof(f.clear_color));
    memcpy(f.ambient, scene->ambient, sizeof(f.ambient));
    if (cam != NULL) {
        f.view = p3d_m4_inverse_rigid(p3d_m4_trs(cam->position, cam->rotation, p3d_v3(1, 1, 1)));
        f.proj = p3d_m4_perspective(cam->camera.fov_y, height > 0 ? (float)width / (float)height : 1.0f,
                                    cam->camera.znear, cam->camera.zfar);
    } else {
        f.view = p3d_m4_translate(p3d_v3(0, 0, -5));
        f.proj = p3d_m4_perspective(1.0f, height > 0 ? (float)width / (float)height : 1.0f, 0.1f, 100.0f);
    }
    for (i = 0; i < scene->capacity && f.light_count < P3D_MAX_LIGHTS; ++i) {
        const p3d_object *o = &scene->objects[i];
        p3d_light *l;
        if (!o->used || !(o->components & P3D_COMP_LIGHT)) continue;
        l = &f.lights[f.light_count++];
        l->kind = o->light.kind;
        memcpy(l->color, o->light.color, sizeof(l->color));
        l->range = o->light.range;
        /* directional lights shine along the object's local -Z, like the camera looks */
        l->vector = o->light.kind == P3D_LIGHT_POINT ? o->position
                  : p3d_v3_normalize(p3d_m4_mul_dir(p3d_m4_rotate_xyz(o->rotation), p3d_v3(0, 0, -1)));
    }
    p3d_render_begin_frame(&f);
    if (scene->raytrace && p3d_render_caps()->raytracing) {
        static p3d_rt_scene rt;    /* ~5 KB: keep it off small stacks */
        rt.camera_to_world = p3d_m4_inverse_rigid(f.view);
        rt.tan_half_fov_y = tanf((cam ? cam->camera.fov_y : 1.0f) * 0.5f);
        rt.bounces = scene->rt_bounces;
        rt.prim_count = 0;
        for (i = 0; i < scene->capacity && rt.prim_count < P3D_RT_MAX_PRIMS; ++i) {
            const p3d_object *o = &scene->objects[i];
            p3d_rt_prim *p;
            int k;
            if (!o->used || !(o->components & P3D_COMP_MESH)) continue;
            for (k = 1; k < P3D_PRIM_COUNT; ++k)
                if (o->mesh.mesh == scene->primitives[k]) break;
            if (k == P3D_PRIM_COUNT) continue;   /* only primitives can be traced for now */
            p = &rt.prims[rt.prim_count++];
            p->kind = k == P3D_PRIM_CUBE ? P3D_RT_BOX : k == P3D_PRIM_SPHERE ? P3D_RT_SPHERE : P3D_RT_PLANE;
            p->world_to_local = p3d_m4_inverse_trs(o->position, o->rotation, o->scale);
            memcpy(p->color, o->mesh.material.color, sizeof(p->color));
            p->reflect = o->mesh.material.reflect;
            p->checker = o->mesh.material.texture != 0 && o->mesh.material.texture == scene->checker;
        }
        p3d_render_raytrace(&rt);
        return;
    }
    for (i = 0; i < scene->capacity; ++i) {
        const p3d_object *o = &scene->objects[i];
        p3d_draw d;
        if (!o->used || !(o->components & P3D_COMP_MESH)) continue;
        d.mesh = o->mesh.mesh;
        d.model = p3d_m4_trs(o->position, o->rotation, o->scale);
        d.material = o->mesh.material;
        d.material.shading = p3d_render_resolve_shading(d.material.shading);
        p3d_render_draw(&d);
    }
}

void p3d_scene_render(p3d_scene *scene, int width, int height)
{
    p3d_scene_render_begin(scene, width, height);
    p3d_render_end_frame();
}
