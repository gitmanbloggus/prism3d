#include "p3d_script.h"
#include "../engine/p3d_engine.h"
#include "../assets/p3d_image.h"
#include "../core/p3d_core.h"

#include "sebvm.h"
#include "seb.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SCRIPTS 64

typedef struct {
    int used, running;
    char path[256];
    p3d_engine *engine;
    seb_program *program;
    seb_vm *vm;
    seb_device device;
    p3d_object_id self, sel;
    p3d_texture_id texid;
    uint8_t *keys;          /* shared region prism.keys */
} script;

static script scripts[MAX_SCRIPTS];

/* ---- value conversion: the VM masks to the program width; device values are signed ---- */
static int64_t sval(const script *s, uint64_t v)
{
    unsigned w = s->program->width;
    if (w < 64 && (v & (UINT64_C(1) << (w - 1)))) v |= ~((UINT64_C(1) << w) - 1);
    return (int64_t)v;
}
static uint64_t uval(int64_t v) { return (uint64_t)v; }

static float milli(const script *s, uint64_t v) { return (float)sval(s, v) / 1000.0f; }
static int64_t to_milli(float f) { return (int64_t)floorf(f * 1000.0f + 0.5f); }
static float deg(const script *s, uint64_t v) { return (float)sval(s, v) / 1000.0f * P3D_PI / 180.0f; }
static int64_t to_mdeg(float r) { return (int64_t)floorf(r * 180.0f / P3D_PI * 1000.0f + 0.5f); }

static void rgb_from(uint64_t v, float *c)
{
    c[0] = (float)((v >> 16) & 255) / 255.0f;
    c[1] = (float)((v >> 8) & 255) / 255.0f;
    c[2] = (float)(v & 255) / 255.0f;
}
static uint64_t rgb_to(const float *c)
{
    uint64_t r = (uint64_t)(c[0] * 255.0f + 0.5f), g = (uint64_t)(c[1] * 255.0f + 0.5f), b = (uint64_t)(c[2] * 255.0f + 0.5f);
    return (r << 16) | (g << 8) | b;
}

static p3d_object *selected(script *s, char *msg, size_t msglen)
{
    p3d_object *o = p3d_scene_get(&s->engine->scene, s->sel);
    if (o == NULL) snprintf(msg, msglen, "no object %u is selected (prism.sel)", (unsigned)s->sel);
    return o;
}

/* ---- registers ---- */
#define DEV(d) script *s = (script *)(d)
#define NOSTR (void)str; (void)length

static int r_quit(void *d, uint64_t *v, char *m, size_t n) { DEV(d); (void)m; (void)n; *v = (uint64_t)s->engine->input.quit; return 0; }
static int r_ms(void *d, uint64_t *v, char *m, size_t n) { DEV(d); (void)m; (void)n; *v = s->engine->time_ms; return 0; }
static int r_dt(void *d, uint64_t *v, char *m, size_t n) { DEV(d); (void)m; (void)n; *v = s->engine->frame_ms; return 0; }
static int r_tier(void *d, uint64_t *v, char *m, size_t n) { (void)d; (void)m; (void)n; *v = (uint64_t)p3d_render_caps()->tier; return 0; }
static int r_self(void *d, uint64_t *v, char *m, size_t n) { DEV(d); (void)m; (void)n; *v = s->self; return 0; }
static int r_sel(void *d, uint64_t *v, char *m, size_t n) { DEV(d); (void)m; (void)n; *v = s->sel; return 0; }
static int r_texid(void *d, uint64_t *v, char *m, size_t n) { DEV(d); (void)m; (void)n; *v = s->texid; return 0; }
static int r_mousex(void *d, uint64_t *v, char *m, size_t n) { DEV(d); (void)m; (void)n; *v = uval(s->engine->input.mouse_x); return 0; }
static int r_mousey(void *d, uint64_t *v, char *m, size_t n) { DEV(d); (void)m; (void)n; *v = uval(s->engine->input.mouse_y); return 0; }
static int r_buttons(void *d, uint64_t *v, char *m, size_t n) { DEV(d); (void)m; (void)n; *v = s->engine->input.buttons; return 0; }

static int w_frame(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)
{
    DEV(d); NOSTR; (void)v; (void)m; (void)n;
    memcpy(s->keys, s->engine->input.keys, P3D_KEY_COUNT);   /* shared memory changes only here */
    return SEB_DEVICE_YIELD;
}

static int w_sel(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)
{
    DEV(d); NOSTR; (void)m; (void)n;
    s->sel = (p3d_object_id)v;
    return 0;
}

static int w_spawn(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)
{
    DEV(d); NOSTR;
    if (v < 1 || v >= P3D_PRIM_COUNT) { snprintf(m, n, "prism.spawn: unknown primitive %" PRIu64 " (1 cube, 2 sphere, 3 plane)", v); return SEB_DEVICE_ERROR; }
    s->sel = p3d_scene_spawn_primitive(&s->engine->scene, (p3d_primitive)v);
    if (!s->sel) { snprintf(m, n, "prism.spawn: the scene is full"); return SEB_DEVICE_ERROR; }
    return 0;
}

static int w_camera(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)
{
    DEV(d); NOSTR;
    s->sel = p3d_scene_spawn_camera(&s->engine->scene);
    if (!s->sel) { snprintf(m, n, "prism.camera: the scene is full"); return SEB_DEVICE_ERROR; }
    if (v == 1) s->engine->scene.active_camera = s->sel;
    return 0;
}

static int w_light(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)
{
    DEV(d); NOSTR;
    if (v > 1) { snprintf(m, n, "prism.light: unknown kind %" PRIu64 " (0 directional, 1 point)", v); return SEB_DEVICE_ERROR; }
    s->sel = p3d_scene_spawn_light(&s->engine->scene, v ? P3D_LIGHT_POINT : P3D_LIGHT_DIRECTIONAL);
    if (!s->sel) { snprintf(m, n, "prism.light: the scene is full"); return SEB_DEVICE_ERROR; }
    return 0;
}

static int w_destroy(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)
{
    DEV(d); NOSTR; (void)m; (void)n;
    p3d_scene_destroy(&s->engine->scene, (p3d_object_id)v);
    return 0;
}

static int w_active(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)
{
    DEV(d); NOSTR;
    p3d_object *o = p3d_scene_get(&s->engine->scene, (p3d_object_id)v);
    if (o == NULL || !(o->components & P3D_COMP_CAMERA)) { snprintf(m, n, "prism.active: object %" PRIu64 " is not a camera", v); return SEB_DEVICE_ERROR; }
    s->engine->scene.active_camera = (p3d_object_id)v;
    return 0;
}

/* transform registers: one read + one write function per field, generated */
#define XFORM(name, field, comp, from, to)                                                       \
static int r_##name(void *d, uint64_t *v, char *m, size_t n)                                     \
{ DEV(d); p3d_object *o = selected(s, m, n); if (!o) return SEB_DEVICE_ERROR;                    \
  *v = uval(to(o->field.comp)); return 0; }                                                      \
static int w_##name(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)   \
{ DEV(d); p3d_object *o; NOSTR; o = selected(s, m, n); if (!o) return SEB_DEVICE_ERROR;          \
  o->field.comp = from(s, v); return 0; }
XFORM(px, position, x, milli, to_milli) XFORM(py, position, y, milli, to_milli) XFORM(pz, position, z, milli, to_milli)
XFORM(rx, rotation, x, deg, to_mdeg)    XFORM(ry, rotation, y, deg, to_mdeg)    XFORM(rz, rotation, z, deg, to_mdeg)
XFORM(sx, scale, x, milli, to_milli)    XFORM(sy, scale, y, milli, to_milli)    XFORM(sz, scale, z, milli, to_milli)

static int r_color(void *d, uint64_t *v, char *m, size_t n)
{
    DEV(d); p3d_object *o = selected(s, m, n);
    if (!o) return SEB_DEVICE_ERROR;
    *v = rgb_to((o->components & P3D_COMP_LIGHT) ? o->light.color : o->mesh.material.color);
    return 0;
}

static int w_color(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)
{
    DEV(d); p3d_object *o; NOSTR;
    if (!(o = selected(s, m, n))) return SEB_DEVICE_ERROR;
    rgb_from(v, (o->components & P3D_COMP_LIGHT) ? o->light.color : o->mesh.material.color);
    return 0;
}

static p3d_object *selected_mesh(script *s, const char *reg, char *m, size_t n)
{
    p3d_object *o = selected(s, m, n);
    if (o && !(o->components & P3D_COMP_MESH)) { snprintf(m, n, "prism.%s: object %u has no mesh", reg, (unsigned)s->sel); return NULL; }
    return o;
}

static int w_shade(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)
{
    DEV(d); p3d_object *o; NOSTR;
    if (!(o = selected_mesh(s, "shade", m, n))) return SEB_DEVICE_ERROR;
    if (v > 3) { snprintf(m, n, "prism.shade: unknown shading %" PRIu64, v); return SEB_DEVICE_ERROR; }
    o->mesh.material.shading = (p3d_shading)v;
    return 0;
}

static int w_tex(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)
{
    DEV(d); p3d_object *o; NOSTR;
    if (!(o = selected_mesh(s, "tex", m, n))) return SEB_DEVICE_ERROR;
    o->mesh.material.texture = v == 1 ? s->engine->scene.checker : (p3d_texture_id)(v == 0 ? 0 : v);
    return 0;
}

static int w_texload(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)
{
    DEV(d);
    char path[256];
    p3d_image img;
    (void)v;
    if (length >= sizeof(path)) { snprintf(m, n, "prism.texload: path too long"); return SEB_DEVICE_ERROR; }
    memcpy(path, str, length);
    path[length] = '\0';
    s->texid = 0;   /* a missing file is not fatal: texid stays 0 and the log says why */
    if (!p3d_image_load(path, &img)) return 0;
    s->texid = p3d_render_texture_create(img.width, img.height, img.rgba);
    p3d_image_free(&img);
    return 0;
}

static int r_range(void *d, uint64_t *v, char *m, size_t n)
{
    DEV(d); p3d_object *o = selected(s, m, n);
    if (!o) return SEB_DEVICE_ERROR;
    *v = uval(to_milli(o->light.range));
    return 0;
}

static int w_range(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)
{
    DEV(d); p3d_object *o; NOSTR;
    if (!(o = selected(s, m, n))) return SEB_DEVICE_ERROR;
    o->light.range = milli(s, v);
    return 0;
}

static int w_clear(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)
{
    DEV(d); NOSTR; (void)m; (void)n;
    rgb_from(v, s->engine->scene.clear_color);
    return 0;
}

static int w_ambient(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)
{
    DEV(d); NOSTR; (void)m; (void)n;
    rgb_from(v, s->engine->scene.ambient);
    return 0;
}

static int w_log(void *d, uint64_t v, const uint8_t *str, size_t length, char *m, size_t n)
{
    DEV(d); (void)v; (void)m; (void)n;
    P3D_INFO("%s: %.*s", s->path, (int)length, (const char *)str);
    return 0;
}

#define R SEB_PORT_R
#define W SEB_PORT_W
static const seb_register registers[] = {
    { "frame", W, NULL, w_frame },
    { "quit", R, r_quit, NULL },
    { "ms", R, r_ms, NULL },
    { "dt", R, r_dt, NULL },
    { "tier", R, r_tier, NULL },
    { "self", R, r_self, NULL },
    { "sel", R | W, r_sel, w_sel },
    { "spawn", W, NULL, w_spawn },
    { "camera", W, NULL, w_camera },
    { "light", W, NULL, w_light },
    { "destroy", W, NULL, w_destroy },
    { "active", W, NULL, w_active },
    { "px", R | W, r_px, w_px }, { "py", R | W, r_py, w_py }, { "pz", R | W, r_pz, w_pz },
    { "rx", R | W, r_rx, w_rx }, { "ry", R | W, r_ry, w_ry }, { "rz", R | W, r_rz, w_rz },
    { "sx", R | W, r_sx, w_sx }, { "sy", R | W, r_sy, w_sy }, { "sz", R | W, r_sz, w_sz },
    { "color", R | W, r_color, w_color },
    { "shade", W, NULL, w_shade },
    { "tex", W, NULL, w_tex },
    { "texload", W | SEB_PORT_STR, NULL, w_texload },
    { "texid", R, r_texid, NULL },
    { "range", R | W, r_range, w_range },
    { "clear", W, NULL, w_clear },
    { "ambient", W, NULL, w_ambient },
    { "mousex", R, r_mousex, NULL },
    { "mousey", R, r_mousey, NULL },
    { "buttons", R, r_buttons, NULL },
    { "log", W | SEB_PORT_STR, NULL, w_log }
};
static const seb_shared_region regions[] = { { "prism.keys", P3D_KEY_COUNT } };

static int bind(void *d, seb_vm *vm, uint8_t *const *ptrs, char *err, size_t errlen)
{
    DEV(d); (void)vm; (void)err; (void)errlen;
    s->keys = ptrs[0];
    return 0;
}

/* console ports (print, printvar, printchar) go to the engine log's stdout */
static int host_write(void *ctx, int port, uint64_t value, const seb_program *program, char *err, size_t errlen)
{
    const uint8_t *bytes;
    size_t len;
    (void)ctx;
    switch (port) {
    case SEB_PORT_PUTC: putchar((int)(value & 0xFF)); break;
    case SEB_PORT_PUTNUM: {
        int64_t v = (int64_t)value;
        if (program->width < 64 && (value & (UINT64_C(1) << (program->width - 1)))) v = (int64_t)(value | ~((UINT64_C(1) << program->width) - 1));
        printf("%" PRId64 "\n", v);
        break;
    }
    case SEB_PORT_PUTS:
        if (seb_program_string(program, value, &bytes, &len)) printf("%.*s\n", (int)len, (const char *)bytes);
        break;
    default:
        snprintf(err, errlen, "port %s is not available to engine scripts", seb_program_port_name(program, port));
        return SEB_ERROR;
    }
    fflush(stdout);
    return SEB_OK;
}

static int host_read(void *ctx, int port, uint8_t width, uint64_t *value, int *soft, char *err, size_t errlen)
{
    (void)ctx; (void)width; (void)value; (void)soft;
    snprintf(err, errlen, "console input (port %d) is not available to engine scripts", port);
    return SEB_ERROR;
}

int p3d_script_attach(p3d_engine *engine, const char *path, p3d_object_id owner)
{
    script *s = NULL;
    seb_host host;
    uint8_t *data;
    size_t size;
    char err[512];
    int i;
    p3d_object *o;
    for (i = 0; i < MAX_SCRIPTS; ++i)
        if (!scripts[i].used) { s = &scripts[i]; break; }
    if (s == NULL) { P3D_ERROR("too many scripts"); return -1; }
    memset(s, 0, sizeof(*s));
    data = (uint8_t *)p3d_file_read(path, &size, NULL);
    if (data == NULL) { P3D_ERROR("can't read script %s", path); return -1; }
    if (seb_load(data, size, &s->program, err, sizeof(err)) != SEB_OK) {
        P3D_ERROR("%s: %s", path, err);
        free(data);
        return -1;
    }
    free(data);
    host.write = host_write;
    host.read = host_read;
    host.ctx = s;
    s->vm = seb_vm_new(s->program, &host);
    if (s->vm == NULL) { seb_program_free(s->program); P3D_ERROR("%s: out of memory", path); return -1; }
    s->device.name = "prism";
    s->device.registers = registers;
    s->device.register_count = P3D_ARRAY_LEN(registers);
    s->device.regions = regions;
    s->device.region_count = P3D_ARRAY_LEN(regions);
    s->device.bind = bind;
    s->device.dev = s;
    s->engine = engine;
    if (owner == 0) owner = p3d_scene_create(&engine->scene);
    s->self = s->sel = owner;
    if (seb_vm_add_device(s->vm, &s->device, err, sizeof(err)) != SEB_OK ||
        seb_vm_bind_devices(s->vm, err, sizeof(err)) != SEB_OK) {
        P3D_ERROR("%s: %s", path, err);
        seb_vm_free(s->vm);
        seb_program_free(s->program);
        return -1;
    }
    snprintf(s->path, sizeof(s->path), "%s", path);
    if ((o = p3d_scene_get(&engine->scene, owner)) != NULL) {
        o->components |= P3D_COMP_SCRIPT;
        o->script.script = i;
    }
    s->used = s->running = 1;
    return i;
}

void p3d_scripts_update(p3d_engine *engine)
{
    int i;
    (void)engine;
    for (i = 0; i < MAX_SCRIPTS; ++i) {
        script *s = &scripts[i];
        char err[512];
        int code = 0, status;
        if (!s->used || !s->running) continue;
        status = seb_vm_run(s->vm, P3D_SCRIPT_STEPS_PER_FRAME, &code, err, sizeof(err));
        if (status == SEB_YIELD) continue;
        s->running = 0;
        if (status == SEB_OK) P3D_INFO("%s: ended (kill %d)", s->path, code);
        else if (strncmp(err, "step limit", 10) == 0)
            P3D_ERROR("%s: ran %u steps without writing prism.frame; stopped", s->path, P3D_SCRIPT_STEPS_PER_FRAME);
        else P3D_ERROR("%s: %s", s->path, err[0] ? err : "stopped");
    }
}

int p3d_scripts_running(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_SCRIPTS; ++i) n += scripts[i].used && scripts[i].running;
    return n;
}

void p3d_scripts_shutdown(void)
{
    int i;
    for (i = 0; i < MAX_SCRIPTS; ++i)
        if (scripts[i].used) {
            seb_vm_free(scripts[i].vm);
            seb_program_free(scripts[i].program);
            scripts[i].used = 0;
        }
}
