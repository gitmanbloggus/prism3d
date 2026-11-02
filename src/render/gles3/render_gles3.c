/* render_gles3.c: tier T2 backend for OpenGL ES 3.0 / WebGL 2, with GPU raytracing.
 * Shaders are shared with the Vulkan backend (src/render/shaders), embedded by tools/shaders.py. */
#include "../p3d_render.h"
#include "../rt_gpu.h"
#include "../../core/p3d_core.h"
#include "shaders_gles.h"

#include <GLES3/gl3.h>
#include <string.h>

#define MAX_MESHES 1024

typedef struct { GLuint vao, vbo, ibo; GLsizei count; int used; } gl_mesh;

/* std140 mirrors of FrameBlock and DrawBlock in mesh_common.glsl */
typedef struct {
    float ambient[4];
    float light_info[4];
    float light_vec[P3D_MAX_LIGHTS][4];
    float light_color[P3D_MAX_LIGHTS][4];
} frame_block;
typedef struct { float mvp[16], model[16], color[4], flags[4]; } draw_block;

enum { BIND_FRAME = 0, BIND_DRAW = 1, BIND_RT = 2 };

static gl_mesh meshes[MAX_MESHES];
static GLuint mesh_program, rt_program, rt_vao;
static GLint mesh_u_tex;
static GLuint ubo_frame, ubo_draw, ubo_rt;
static GLuint white_texture;
static p3d_frame frame;
static p3d_mat4 view_proj;

#define SRC(name) name, (GLsizei)P3D_ARRAY_LEN(name)

static GLuint compile(GLenum type, const char *const *src, GLsizei lines)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, lines, src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        P3D_ERROR("gles3 shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link(const char *const *vs_src, GLsizei vs_lines, const char *const *fs_src, GLsizei fs_lines)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src, vs_lines), fs = compile(GL_FRAGMENT_SHADER, fs_src, fs_lines), p;
    GLint ok = 0;
    if (!vs || !fs) return 0;
    p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        P3D_ERROR("gles3 shader link failed: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static void bind_block(GLuint program, const char *name, GLuint binding)
{
    GLuint index = glGetUniformBlockIndex(program, name);
    if (index != GL_INVALID_INDEX) glUniformBlockBinding(program, index, binding);
}

static GLuint make_ubo(GLsizeiptr size, GLuint binding)
{
    GLuint b;
    glGenBuffers(1, &b);
    glBindBuffer(GL_UNIFORM_BUFFER, b);
    glBufferData(GL_UNIFORM_BUFFER, size, NULL, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, binding, b);
    return b;
}

static int gles3_init(p3d_caps *caps)
{
    static const uint8_t white[4] = { 255, 255, 255, 255 };
    GLint max_tex = 0, max_block = 0;
    P3D_INFO("GL: %s / %s", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    mesh_program = link(SRC(p3d_glsl_mesh_vert), SRC(p3d_glsl_mesh_frag));
    rt_program = link(SRC(p3d_glsl_rt_vert), SRC(p3d_glsl_rt_frag));
    if (!mesh_program) return 0;
    bind_block(mesh_program, "FrameBlock", BIND_FRAME);
    bind_block(mesh_program, "DrawBlock", BIND_DRAW);
    mesh_u_tex = glGetUniformLocation(mesh_program, "u_tex");
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &max_block);
    if (rt_program && max_block >= (GLint)sizeof(p3d_rt_gpu)) {
        bind_block(rt_program, "RTBlock", BIND_RT);
        glGenVertexArrays(1, &rt_vao);
        caps->raytracing = 1;
    } else P3D_WARN("gles3: raytracing unavailable (uniform blocks up to %d bytes)", max_block);
    ubo_frame = make_ubo(sizeof(frame_block), BIND_FRAME);
    ubo_draw = make_ubo(sizeof(draw_block), BIND_DRAW);
    ubo_rt = make_ubo(sizeof(p3d_rt_gpu), BIND_RT);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    caps->max_texture_size = max_tex;
    caps->npot_textures = 1;
    caps->max_lights = P3D_MAX_LIGHTS;
    caps->per_pixel_lighting = 1;
    glGenTextures(1, &white_texture);
    glBindTexture(GL_TEXTURE_2D, white_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    return 1;
}

static void gles3_shutdown(void)
{
    int i;
    for (i = 0; i < MAX_MESHES; ++i)
        if (meshes[i].used) {
            glDeleteVertexArrays(1, &meshes[i].vao);
            glDeleteBuffers(1, &meshes[i].vbo);
            glDeleteBuffers(1, &meshes[i].ibo);
            meshes[i].used = 0;
        }
    glDeleteBuffers(1, &ubo_frame);
    glDeleteBuffers(1, &ubo_draw);
    glDeleteBuffers(1, &ubo_rt);
    glDeleteVertexArrays(1, &rt_vao);
    glDeleteTextures(1, &white_texture);
    glDeleteProgram(mesh_program);
    glDeleteProgram(rt_program);
}

static p3d_mesh_id gles3_mesh_create(const p3d_vertex *v, int vertex_count, const uint16_t *idx, int index_count)
{
    gl_mesh *m;
    int i;
    for (i = 0; i < MAX_MESHES; ++i)
        if (!meshes[i].used) break;
    if (i == MAX_MESHES) return 0;
    m = &meshes[i];
    glGenVertexArrays(1, &m->vao);
    glBindVertexArray(m->vao);
    glGenBuffers(1, &m->vbo);
    glGenBuffers(1, &m->ibo);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(*v) * (size_t)vertex_count), v, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(*idx) * (size_t)index_count), idx, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(p3d_vertex), (const void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(p3d_vertex), (const void *)(sizeof(float) * 3));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(p3d_vertex), (const void *)(sizeof(float) * 6));
    glBindVertexArray(0);
    m->count = index_count;
    m->used = 1;
    return (p3d_mesh_id)(i + 1);
}

static void gles3_mesh_destroy(p3d_mesh_id id)
{
    gl_mesh *m;
    if (id == 0 || id > MAX_MESHES || !meshes[id - 1].used) return;
    m = &meshes[id - 1];
    glDeleteVertexArrays(1, &m->vao);
    glDeleteBuffers(1, &m->vbo);
    glDeleteBuffers(1, &m->ibo);
    m->used = 0;
}

static p3d_texture_id gles3_texture_create(int width, int height, const uint8_t *rgba)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return (p3d_texture_id)t;
}

static void gles3_texture_destroy(p3d_texture_id t)
{
    GLuint name = t;
    if (t) glDeleteTextures(1, &name);
}

static void gles3_begin_frame(const p3d_frame *f)
{
    frame_block fb;
    int i, n = f->light_count < P3D_MAX_LIGHTS ? f->light_count : P3D_MAX_LIGHTS;
    frame = *f;
    view_proj = p3d_m4_mul(f->proj, f->view);
    memset(&fb, 0, sizeof(fb));
    memcpy(fb.ambient, f->ambient, sizeof(float) * 3);
    fb.light_info[0] = (float)n;
    for (i = 0; i < n; ++i) {
        fb.light_vec[i][0] = f->lights[i].vector.x;
        fb.light_vec[i][1] = f->lights[i].vector.y;
        fb.light_vec[i][2] = f->lights[i].vector.z;
        fb.light_vec[i][3] = f->lights[i].kind == P3D_LIGHT_POINT ? 1.0f : 0.0f;
        memcpy(fb.light_color[i], f->lights[i].color, sizeof(float) * 3);
        fb.light_color[i][3] = f->lights[i].range;
    }
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_frame);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(fb), &fb);
    glViewport(0, 0, f->width, f->height);
    glClearColor(f->clear_color[0], f->clear_color[1], f->clear_color[2], 1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void gles3_draw(const p3d_draw *d)
{
    const gl_mesh *m;
    draw_block db;
    p3d_mat4 mvp;
    if (d->mesh == 0 || d->mesh > MAX_MESHES || !meshes[d->mesh - 1].used) return;
    m = &meshes[d->mesh - 1];
    mvp = p3d_m4_mul(view_proj, d->model);
    memcpy(db.mvp, mvp.m, sizeof(db.mvp));
    memcpy(db.model, d->model.m, sizeof(db.model));
    memcpy(db.color, d->material.color, sizeof(db.color));
    db.flags[0] = (float)d->material.shading;
    db.flags[1] = db.flags[2] = db.flags[3] = 0.0f;
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_draw);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(db), &db);
    glUseProgram(mesh_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->material.texture ? (GLuint)d->material.texture : white_texture);
    glUniform1i(mesh_u_tex, 0);
    glBindVertexArray(m->vao);
    glDrawElements(GL_TRIANGLES, m->count, GL_UNSIGNED_SHORT, (const void *)0);
    glBindVertexArray(0);
}

static void gles3_raytrace(const p3d_rt_scene *scene)
{
    static p3d_rt_gpu block;
    p3d_rt_pack(&frame, scene, &block);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_rt);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(block), &block);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(rt_program);
    glBindVertexArray(rt_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

static void gles3_end_frame(void) { p3d_platform_swap(); }

static int gles3_read_pixels(int width, int height, uint8_t *rgba)
{
    size_t row = (size_t)width * 4u;
    uint8_t tmp[4 * 4096];
    int y;
    if (width > 4096) return 0;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    for (y = 0; y < height / 2; ++y) {
        uint8_t *a = rgba + (size_t)y * row, *b = rgba + (size_t)(height - 1 - y) * row;
        memcpy(tmp, a, row);
        memcpy(a, b, row);
        memcpy(b, tmp, row);
    }
    return 1;
}

const p3d_render_backend p3d_backend_gles3 = {
    "gles3", P3D_TIER_SHADER, P3D_SURFACE_GLES3,
    gles3_init, gles3_shutdown,
    gles3_mesh_create, gles3_mesh_destroy,
    gles3_texture_create, gles3_texture_destroy,
    gles3_begin_frame, gles3_draw, gles3_raytrace, gles3_end_frame,
    gles3_read_pixels
};
