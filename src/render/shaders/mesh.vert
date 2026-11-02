// mesh.vert: lit/textured meshes (gles3, vulkan). mesh_common.glsl comes first.
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
VS_OUT(0) vec2 v_uv;
VS_OUT(1) vec3 v_world;
VS_OUT(2) vec3 v_n;
VS_OUT(3) vec3 v_light;

void main()
{
    int mode = int(flags.x + 0.5);
    v_uv = a_uv;
    v_world = (model * vec4(a_pos, 1.0)).xyz;
    v_n = transpose(inverse(mat3(model))) * a_normal;
    v_light = (mode == 1 || mode == 2) ? light_at(v_world, normalize(v_n)) : vec3(1.0);
    gl_Position = mvp * vec4(a_pos, 1.0);
#ifdef P3D_VK
    gl_Position.y = -gl_Position.y;                          // Vulkan clip space: y down,
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;   // z in 0..1
#endif
}
