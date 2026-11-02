#include "p3d_math.h"

#include <math.h>

p3d_vec3 p3d_v3(float x, float y, float z) { p3d_vec3 v; v.x = x; v.y = y; v.z = z; return v; }
p3d_vec3 p3d_v3_add(p3d_vec3 a, p3d_vec3 b) { return p3d_v3(a.x + b.x, a.y + b.y, a.z + b.z); }
p3d_vec3 p3d_v3_sub(p3d_vec3 a, p3d_vec3 b) { return p3d_v3(a.x - b.x, a.y - b.y, a.z - b.z); }
p3d_vec3 p3d_v3_scale(p3d_vec3 a, float s) { return p3d_v3(a.x * s, a.y * s, a.z * s); }
float p3d_v3_dot(p3d_vec3 a, p3d_vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
p3d_vec3 p3d_v3_cross(p3d_vec3 a, p3d_vec3 b)
{
    return p3d_v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
p3d_vec3 p3d_v3_normalize(p3d_vec3 a)
{
    float len = sqrtf(p3d_v3_dot(a, a));
    return len > 1e-8f ? p3d_v3_scale(a, 1.0f / len) : a;
}

p3d_mat4 p3d_m4_identity(void)
{
    p3d_mat4 r = { { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 } };
    return r;
}

p3d_mat4 p3d_m4_mul(p3d_mat4 a, p3d_mat4 b)
{
    p3d_mat4 r;
    int c, row, k;
    for (c = 0; c < 4; ++c)
        for (row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

p3d_mat4 p3d_m4_translate(p3d_vec3 t)
{
    p3d_mat4 r = p3d_m4_identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

p3d_mat4 p3d_m4_scale(p3d_vec3 s)
{
    p3d_mat4 r = p3d_m4_identity();
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
    return r;
}

p3d_mat4 p3d_m4_rotate_xyz(p3d_vec3 a)
{
    float cx = cosf(a.x), sx = sinf(a.x), cy = cosf(a.y), sy = sinf(a.y), cz = cosf(a.z), sz = sinf(a.z);
    p3d_mat4 rx = p3d_m4_identity(), ry = p3d_m4_identity(), rz = p3d_m4_identity();
    rx.m[5] = cx; rx.m[6] = sx; rx.m[9] = -sx; rx.m[10] = cx;
    ry.m[0] = cy; ry.m[2] = -sy; ry.m[8] = sy; ry.m[10] = cy;
    rz.m[0] = cz; rz.m[1] = sz; rz.m[4] = -sz; rz.m[5] = cz;
    return p3d_m4_mul(rz, p3d_m4_mul(ry, rx));
}

p3d_mat4 p3d_m4_trs(p3d_vec3 t, p3d_vec3 r, p3d_vec3 s)
{
    return p3d_m4_mul(p3d_m4_translate(t), p3d_m4_mul(p3d_m4_rotate_xyz(r), p3d_m4_scale(s)));
}

p3d_mat4 p3d_m4_perspective(float fovy, float aspect, float n, float f)
{
    p3d_mat4 r = { { 0 } };
    float t = 1.0f / tanf(fovy * 0.5f);
    r.m[0] = t / aspect;
    r.m[5] = t;
    r.m[10] = (f + n) / (n - f);
    r.m[11] = -1.0f;
    r.m[14] = 2.0f * f * n / (n - f);
    return r;
}

p3d_mat4 p3d_m4_inverse_rigid(p3d_mat4 m)
{
    p3d_mat4 r = p3d_m4_identity();
    int i, j;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j) r.m[i * 4 + j] = m.m[j * 4 + i];
    for (i = 0; i < 3; ++i)
        r.m[12 + i] = -(r.m[i] * m.m[12] + r.m[4 + i] * m.m[13] + r.m[8 + i] * m.m[14]);
    return r;
}

p3d_mat4 p3d_m4_inverse_trs(p3d_vec3 t, p3d_vec3 r, p3d_vec3 s)
{
    /* (T R S)^-1 = S^-1 R^T T^-1 */
    p3d_mat4 rt = p3d_m4_inverse_rigid(p3d_m4_rotate_xyz(r));
    p3d_vec3 inv_s = p3d_v3(s.x != 0.0f ? 1.0f / s.x : 0.0f, s.y != 0.0f ? 1.0f / s.y : 0.0f, s.z != 0.0f ? 1.0f / s.z : 0.0f);
    return p3d_m4_mul(p3d_m4_scale(inv_s), p3d_m4_mul(rt, p3d_m4_translate(p3d_v3_scale(t, -1.0f))));
}

p3d_vec4 p3d_m4_mul_v4(p3d_mat4 m, p3d_vec4 v)
{
    p3d_vec4 r;
    r.x = m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12] * v.w;
    r.y = m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13] * v.w;
    r.z = m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14] * v.w;
    r.w = m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15] * v.w;
    return r;
}

p3d_vec3 p3d_m4_mul_dir(p3d_mat4 m, p3d_vec3 v)
{
    return p3d_v3(m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z,
                  m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z,
                  m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z);
}
