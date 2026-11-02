// mesh.frag: lit/textured meshes (gles3, vulkan). mesh_common.glsl comes first.
FS_IN(0) vec2 v_uv;
FS_IN(1) vec3 v_world;
FS_IN(2) vec3 v_n;
FS_IN(3) vec3 v_light;
#ifdef P3D_VK
layout(set = 1, binding = 0) uniform sampler2D u_tex;
#else
uniform sampler2D u_tex;
#endif
layout(location = 0) out vec4 out_color;

void main()
{
    int mode = int(flags.x + 0.5);
    vec4 base = texture(u_tex, v_uv) * color;
    vec3 light = mode == 3 ? light_at(v_world, normalize(v_n)) : v_light;
    out_color = vec4(base.rgb * light, base.a);
}
