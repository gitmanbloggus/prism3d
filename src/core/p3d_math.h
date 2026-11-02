/* p3d_math.h: small float vector/matrix library. Column-major 4x4 matrices (OpenGL convention),
 * right-handed world: +X right, +Y up, -Z forward. Angles in radians.
 * A fixed-point variant for FPU-less targets is planned behind the same function names. */
#ifndef P3D_MATH_H
#define P3D_MATH_H

#define P3D_PI 3.14159265358979323846f

typedef struct { float x, y, z; } p3d_vec3;
typedef struct { float x, y, z, w; } p3d_vec4;
typedef struct { float m[16]; } p3d_mat4;   /* m[col * 4 + row] */

p3d_vec3 p3d_v3(float x, float y, float z);
p3d_vec3 p3d_v3_add(p3d_vec3 a, p3d_vec3 b);
p3d_vec3 p3d_v3_sub(p3d_vec3 a, p3d_vec3 b);
p3d_vec3 p3d_v3_scale(p3d_vec3 a, float s);
float    p3d_v3_dot(p3d_vec3 a, p3d_vec3 b);
p3d_vec3 p3d_v3_cross(p3d_vec3 a, p3d_vec3 b);
p3d_vec3 p3d_v3_normalize(p3d_vec3 a);

p3d_mat4 p3d_m4_identity(void);
p3d_mat4 p3d_m4_mul(p3d_mat4 a, p3d_mat4 b);
p3d_mat4 p3d_m4_translate(p3d_vec3 t);
p3d_mat4 p3d_m4_scale(p3d_vec3 s);
p3d_mat4 p3d_m4_rotate_xyz(p3d_vec3 r);   /* applies X, then Y, then Z */
p3d_mat4 p3d_m4_trs(p3d_vec3 t, p3d_vec3 r, p3d_vec3 s);
p3d_mat4 p3d_m4_perspective(float fovy, float aspect, float znear, float zfar);
p3d_mat4 p3d_m4_inverse_rigid(p3d_mat4 m); /* inverse of rotation + translation (no scale) */
p3d_vec4 p3d_m4_mul_v4(p3d_mat4 m, p3d_vec4 v);
p3d_vec3 p3d_m4_mul_dir(p3d_mat4 m, p3d_vec3 v); /* upper 3x3 only */

#endif
