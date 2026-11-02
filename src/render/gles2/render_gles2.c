/* render_gles2.c: tier T2 backend for OpenGL ES 2.0 and WebGL 1.
 * Two shader programs: per-pixel and per-vertex lighting (unlit is a uniform switch). */
#include "../p3d_render.h"
#include "../../core/p3d_core.h"

#include <GLES2/gl2.h>
#include <math.h>
#include <string.h>

#define MAX_MESHES 1024

typedef struct { GLuint vbo, ibo; GLsizei count; int used; } gl_mesh;

typedef struct {
    GLuint program;
    GLint a_pos, a_normal, a_uv;
    GLint u_mvp, u_model, u_normal, u_color, u_tex, u_lit, u_ambient;
    GLint u_light_count, u_light_vec, u_light_color, u_light_range;
} gl_shader;

static gl_mesh meshes[MAX_MESHES];
static gl_shader shader_pixel, shader_vertex;
static GLuint white_texture;
static p3d_frame frame;
static p3d_mat4 view_proj;

static const char *const vs_common =
    "attribute vec3 a_pos;\n"
    "attribute vec3 a_normal;\n"
    "attribute vec2 a_uv;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_model;\n"
    "uniform mat3 u_normal;\n"
    "varying vec2 v_uv;\n"
    "varying vec3 v_world;\n"
    "varying vec3 v_n;\n"
    "varying vec3 v_light;\n";

static const char *const lighting_fn =
    "uniform int u_lit;\n"
    "uniform vec3 u_ambient;\n"
    "uniform int u_light_count;\n"
    "uniform vec3 u_light_vec[4];\n"
    "uniform vec3 u_light_color[4];\n"
    "uniform float u_light_range[4];\n"   /* 0 = directional */
    "vec3 light_at(vec3 p, vec3 n) {\n"
    "  vec3 sum = u_ambient;\n"
    "  for (int i = 0; i < 4; ++i) {\n"
    "    if (i >= u_light_count) break;\n"
    "    vec3 l; float att = 1.0;\n"
    "    if (u_light_range[i] > 0.0) {\n"
    "      vec3 d = u_light_vec[i] - p; float dist = length(d); l = d / max(dist, 0.0001);\n"
    "      att = clamp(1.0 - dist / u_light_range[i], 0.0, 1.0); att *= att;\n"
    "    } else { l = -u_light_vec[i]; }\n"
    "    sum += u_light_color[i] * max(dot(n, l), 0.0) * att;\n"
    "  }\n"
    "  return sum;\n"
    "}\n";

static const char *const vs_pixel_main =
    "void main() {\n"
    "  v_uv = a_uv;\n"
    "  v_world = (u_model * vec4(a_pos, 1.0)).xyz;\n"
    "  v_n = u_normal * a_normal;\n"
    "  v_light = vec3(1.0);\n"
    "  gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "}\n";

static const char *const vs_vertex_main =
    "void main() {\n"
    "  v_uv = a_uv;\n"
    "  v_world = (u_model * vec4(a_pos, 1.0)).xyz;\n"
    "  v_n = normalize(u_normal * a_normal);\n"
    "  v_light = u_lit == 1 ? light_at(v_world, v_n) : vec3(1.0);\n"
    "  gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "}\n";

static const char *const fs_head =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_color;\n"
    "varying vec2 v_uv;\n"
    "varying vec3 v_world;\n"
    "varying vec3 v_n;\n"
    "varying vec3 v_light;\n";

static const char *const fs_pixel_main =
    "void main() {\n"
    "  vec4 base = texture2D(u_tex, v_uv) * u_color;\n"
    "  vec3 light = u_lit == 1 ? light_at(v_world, normalize(v_n)) : vec3(1.0);\n"
    "  gl_FragColor = vec4(base.rgb * light, base.a);\n"
    "}\n";

static const char *const fs_vertex_main =
    "void main() {\n"
    "  vec4 base = texture2D(u_tex, v_uv) * u_color;\n"
    "  gl_FragColor = vec4(base.rgb * v_light, base.a);\n"
    "}\n";

static GLuint compile(GLenum type, const char *const *parts, int count)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, count, parts, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        P3D_ERROR("shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static int build_shader(gl_shader *sh, int per_pixel)
{
    const char *vs[4], *fs[4];
    GLuint v, f;
    GLint ok = 0;
    vs[0] = "precision highp float;\n";
    vs[1] = vs_common;
    vs[2] = lighting_fn;
    vs[3] = per_pixel ? vs_pixel_main : vs_vertex_main;
    fs[0] = fs_head;
    fs[1] = lighting_fn;
    fs[2] = per_pixel ? fs_pixel_main : fs_vertex_main;
    v = compile(GL_VERTEX_SHADER, vs, 4);
    f = compile(GL_FRAGMENT_SHADER, fs, 3);
    if (!v || !f) return 0;
    sh->program = glCreateProgram();
    glAttachShader(sh->program, v);
    glAttachShader(sh->program, f);
    glBindAttribLocation(sh->program, 0, "a_pos");
    glBindAttribLocation(sh->program, 1, "a_normal");
    glBindAttribLocation(sh->program, 2, "a_uv");
    glLinkProgram(sh->program);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(sh->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(sh->program, sizeof(log), NULL, log);
        P3D_ERROR("shader link failed: %s", log);
        return 0;
    }
    sh->a_pos = 0; sh->a_normal = 1; sh->a_uv = 2;
    sh->u_mvp = glGetUniformLocation(sh->program, "u_mvp");
    sh->u_model = glGetUniformLocation(sh->program, "u_model");
    sh->u_normal = glGetUniformLocation(sh->program, "u_normal");
    sh->u_color = glGetUniformLocation(sh->program, "u_color");
    sh->u_tex = glGetUniformLocation(sh->program, "u_tex");
    sh->u_lit = glGetUniformLocation(sh->program, "u_lit");
    sh->u_ambient = glGetUniformLocation(sh->program, "u_ambient");
    sh->u_light_count = glGetUniformLocation(sh->program, "u_light_count");
    sh->u_light_vec = glGetUniformLocation(sh->program, "u_light_vec");
    sh->u_light_color = glGetUniformLocation(sh->program, "u_light_color");
    sh->u_light_range = glGetUniformLocation(sh->program, "u_light_range");
    return 1;
}

static int gles2_init(p3d_caps *caps)
{
    static const uint8_t white[4] = { 255, 255, 255, 255 };
    GLint max_tex = 0;
    P3D_INFO("GL: %s / %s", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    if (!build_shader(&shader_pixel, 1) || !build_shader(&shader_vertex, 0)) return 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    caps->max_texture_size = max_tex;
    caps->npot_textures = 0;          /* GLES2 core: NPOT only with clamp + no mipmaps */
    caps->max_lights = P3D_MAX_LIGHTS;
    caps->per_pixel_lighting = 1;
    glGenTextures(1, &white_texture);
    glBindTexture(GL_TEXTURE_2D, white_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    return 1;
}

static void gles2_shutdown(void)
{
    int i;
    for (i = 0; i < MAX_MESHES; ++i)
        if (meshes[i].used) {
            glDeleteBuffers(1, &meshes[i].vbo);
            glDeleteBuffers(1, &meshes[i].ibo);
            meshes[i].used = 0;
        }
    glDeleteTextures(1, &white_texture);
    glDeleteProgram(shader_pixel.program);
    glDeleteProgram(shader_vertex.program);
}

static p3d_mesh_id gles2_mesh_create(const p3d_vertex *v, int vertex_count, const uint16_t *idx, int index_count)
{
    int i;
    for (i = 0; i < MAX_MESHES; ++i)
        if (!meshes[i].used) break;
    if (i == MAX_MESHES) return 0;
    glGenBuffers(1, &meshes[i].vbo);
    glGenBuffers(1, &meshes[i].ibo);
    glBindBuffer(GL_ARRAY_BUFFER, meshes[i].vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(*v) * (size_t)vertex_count), v, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, meshes[i].ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(*idx) * (size_t)index_count), idx, GL_STATIC_DRAW);
    meshes[i].count = index_count;
    meshes[i].used = 1;
    return (p3d_mesh_id)(i + 1);
}

static void gles2_mesh_destroy(p3d_mesh_id id)
{
    gl_mesh *m;
    if (id == 0 || id > MAX_MESHES || !meshes[id - 1].used) return;
    m = &meshes[id - 1];
    glDeleteBuffers(1, &m->vbo);
    glDeleteBuffers(1, &m->ibo);
    m->used = 0;
}

static int is_pow2(int n) { return n > 0 && (n & (n - 1)) == 0; }

static p3d_texture_id gles2_texture_create(int width, int height, const uint8_t *rgba)
{
    GLuint t;
    int pow2 = is_pow2(width) && is_pow2(height);
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (pow2) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return (p3d_texture_id)t;
}

static void gles2_texture_destroy(p3d_texture_id t)
{
    GLuint name = t;
    if (t) glDeleteTextures(1, &name);
}

static void gles2_begin_frame(const p3d_frame *f)
{
    frame = *f;
    view_proj = p3d_m4_mul(f->proj, f->view);
    glViewport(0, 0, f->width, f->height);
    glClearColor(f->clear_color[0], f->clear_color[1], f->clear_color[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* inverse transpose of the model's upper 3x3, for normals under non-uniform scale */
static void normal_matrix(const p3d_mat4 *m, float out[9])
{
    float a = m->m[0], b = m->m[4], c = m->m[8];
    float d = m->m[1], e = m->m[5], f = m->m[9];
    float g = m->m[2], h = m->m[6], i = m->m[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C;
    float s = fabsf(det) > 1e-12f ? 1.0f / det : 0.0f;
    /* column-major inverse transpose = cofactor matrix / det */
    out[0] = A * s; out[1] = B * s; out[2] = C * s;
    out[3] = -(b * i - c * h) * s; out[4] = (a * i - c * g) * s; out[5] = -(a * h - b * g) * s;
    out[6] = (b * f - c * e) * s; out[7] = -(a * f - c * d) * s; out[8] = (a * e - b * d) * s;
}

static void gles2_draw(const p3d_draw *d)
{
    const gl_shader *sh = d->material.shading == P3D_SHADE_PER_PIXEL ? &shader_pixel : &shader_vertex;
    const gl_mesh *m;
    p3d_mat4 mvp;
    float nm[9], vecs[12], colors[12], ranges[4];
    int i, n = frame.light_count < P3D_MAX_LIGHTS ? frame.light_count : P3D_MAX_LIGHTS;
    if (d->mesh == 0 || d->mesh > MAX_MESHES || !meshes[d->mesh - 1].used) return;
    m = &meshes[d->mesh - 1];
    mvp = p3d_m4_mul(view_proj, d->model);
    normal_matrix(&d->model, nm);
    for (i = 0; i < n; ++i) {
        vecs[i * 3] = frame.lights[i].vector.x;
        vecs[i * 3 + 1] = frame.lights[i].vector.y;
        vecs[i * 3 + 2] = frame.lights[i].vector.z;
        memcpy(colors + i * 3, frame.lights[i].color, sizeof(float) * 3);
        ranges[i] = frame.lights[i].kind == P3D_LIGHT_POINT ? frame.lights[i].range : 0.0f;
    }
    glUseProgram(sh->program);
    glUniformMatrix4fv(sh->u_mvp, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(sh->u_model, 1, GL_FALSE, d->model.m);
    glUniformMatrix3fv(sh->u_normal, 1, GL_FALSE, nm);
    glUniform4fv(sh->u_color, 1, d->material.color);
    glUniform1i(sh->u_lit, d->material.shading != P3D_SHADE_UNLIT);
    glUniform3fv(sh->u_ambient, 1, frame.ambient);
    glUniform1i(sh->u_light_count, n);
    if (n > 0) {
        glUniform3fv(sh->u_light_vec, n, vecs);
        glUniform3fv(sh->u_light_color, n, colors);
        glUniform1fv(sh->u_light_range, n, ranges);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->material.texture ? (GLuint)d->material.texture : white_texture);
    glUniform1i(sh->u_tex, 0);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(p3d_vertex), (const void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(p3d_vertex), (const void *)(sizeof(float) * 3));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(p3d_vertex), (const void *)(sizeof(float) * 6));
    glDrawElements(GL_TRIANGLES, m->count, GL_UNSIGNED_SHORT, (const void *)0);
}

static void gles2_end_frame(void) { p3d_platform_swap(); }

static int gles2_read_pixels(int width, int height, uint8_t *rgba)
{
    /* reads the back buffer that was just presented; valid with preserved/most drivers, fine for tests */
    int y;
    uint8_t tmp[4 * 4096];
    if (width > 4096) return 0;
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    for (y = 0; y < height / 2; ++y) {   /* GL is bottom row first */
        uint8_t *a = rgba + (size_t)y * (size_t)width * 4u, *b = rgba + (size_t)(height - 1 - y) * (size_t)width * 4u;
        memcpy(tmp, a, (size_t)width * 4u);
        memcpy(a, b, (size_t)width * 4u);
        memcpy(b, tmp, (size_t)width * 4u);
    }
    return 1;
}

const p3d_render_backend p3d_backend_gles2 = {
    "gles2", P3D_TIER_SHADER, P3D_SURFACE_GLES2,
    gles2_init, gles2_shutdown,
    gles2_mesh_create, gles2_mesh_destroy,
    gles2_texture_create, gles2_texture_destroy,
    gles2_begin_frame, gles2_draw, NULL, gles2_end_frame,
    gles2_read_pixels
};
