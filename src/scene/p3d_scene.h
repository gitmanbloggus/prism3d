/* p3d_scene.h: objects and built-in components.
 *
 * An object is a transform plus a set of components. Built in: Mesh (a primitive or loaded mesh +
 * material), Camera, Light, Script. Textures are resources a Mesh's material points at. Anything else
 * (physics, audio, particles...) is a module that keeps its own per-object data keyed by object id. */
#ifndef P3D_SCENE_H
#define P3D_SCENE_H

#include "../render/p3d_render.h"

typedef uint32_t p3d_object_id;   /* index + 1; 0 = none */

typedef enum {
    P3D_PRIM_NONE = 0, P3D_PRIM_CUBE = 1, P3D_PRIM_SPHERE = 2, P3D_PRIM_PLANE = 3, P3D_PRIM_COUNT
} p3d_primitive;

enum {
    P3D_COMP_MESH   = 1u << 0,
    P3D_COMP_CAMERA = 1u << 1,
    P3D_COMP_LIGHT  = 1u << 2,
    P3D_COMP_SCRIPT = 1u << 3
};

typedef struct { p3d_mesh_id mesh; p3d_material material; } p3d_mesh_comp;
typedef struct { float fov_y, znear, zfar; } p3d_camera_comp;
typedef struct { p3d_light_kind kind; float color[3]; float range; } p3d_light_comp;
typedef struct { int script; } p3d_script_comp;  /* index into the script system */

typedef struct {
    int used;
    unsigned components;
    p3d_vec3 position, rotation, scale;   /* rotation: XYZ euler, radians */
    p3d_mesh_comp mesh;
    p3d_camera_comp camera;
    p3d_light_comp light;
    p3d_script_comp script;
} p3d_object;

typedef struct {
    p3d_object *objects;
    int capacity;
    p3d_object_id active_camera;
    float clear_color[3];
    float ambient[3];
    p3d_mesh_id primitives[P3D_PRIM_COUNT];
    p3d_texture_id checker;               /* built-in texture, id 1 for scripts */
} p3d_scene;

int  p3d_scene_init(p3d_scene *scene, int capacity);   /* after p3d_render_init */
void p3d_scene_shutdown(p3d_scene *scene);
p3d_object *p3d_scene_get(p3d_scene *scene, p3d_object_id id);

p3d_object_id p3d_scene_create(p3d_scene *scene);
void p3d_scene_destroy(p3d_scene *scene, p3d_object_id id);
p3d_object_id p3d_scene_spawn_primitive(p3d_scene *scene, p3d_primitive kind);
p3d_object_id p3d_scene_spawn_camera(p3d_scene *scene);
p3d_object_id p3d_scene_spawn_light(p3d_scene *scene, p3d_light_kind kind);

void p3d_scene_render(p3d_scene *scene, int width, int height);
/* render without presenting, then call p3d_render_end_frame yourself (screenshots) */
void p3d_scene_render_begin(p3d_scene *scene, int width, int height);

/* primitives.c: unit-sized meshes centred on the origin, CCW front faces */
p3d_mesh_id p3d_primitive_mesh(p3d_primitive kind);

#endif
