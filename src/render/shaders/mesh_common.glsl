// mesh_common.glsl: prepended to mesh.vert and mesh.frag. GLSL ES 3.00 or 450 (-DP3D_VK).
#ifdef P3D_VK
#define UBO(n) layout(std140, set = 0, binding = n) uniform
#define VS_OUT(n) layout(location = n) out
#define FS_IN(n) layout(location = n) in
#else
precision highp float;
#define UBO(n) layout(std140) uniform
#define VS_OUT(n) out
#define FS_IN(n) in
#endif

UBO(0) FrameBlock {
    vec4 ambient;
    vec4 light_info;       // x = light count
    vec4 light_vec[4];     // xyz, w = 0 directional (direction of travel) / 1 point (position)
    vec4 light_color[4];   // rgb, w = range
};

UBO(1) DrawBlock {
    mat4 mvp;
    mat4 model;
    vec4 color;
    vec4 flags;            // x = shading: 0 unlit, 1 flat, 2 per vertex, 3 per pixel
};

vec3 light_at(vec3 p, vec3 n)
{
    vec3 sum = ambient.rgb;
    int count = int(light_info.x + 0.5);
    for (int i = 0; i < 4; ++i) {
        if (i >= count) break;
        vec3 l;
        float att = 1.0;
        if (light_vec[i].w > 0.5) {
            vec3 d = light_vec[i].xyz - p;
            float dist = length(d);
            l = d / max(dist, 0.0001);
            att = clamp(1.0 - dist / light_color[i].w, 0.0, 1.0);
            att *= att;
        } else l = -light_vec[i].xyz;
        sum += light_color[i].rgb * max(dot(n, l), 0.0) * att;
    }
    return sum;
}
