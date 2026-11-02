// raytrace.frag: analytic raytracer for the scene's primitives (unit sphere, box, plane in local
// space), with shadows and mirror reflections. One source for GLSL ES 3.00 (gles3) and GLSL 450
// (vulkan, compiled with -DP3D_VK). The block layout must match rt_gpu.h.
#ifdef P3D_VK
#define UBO layout(std140, set = 0, binding = 2) uniform
layout(location = 0) out vec4 out_color;
#else
precision highp float;
precision highp int;
#define UBO layout(std140) uniform
out vec4 out_color;
#endif

struct Prim { mat4 inv; vec4 color; vec4 params; };

UBO RTBlock {
    mat4 cam_to_world;
    vec4 cam;          // tan(fovy/2), aspect, width, height
    vec4 counts;       // prims, lights, bounces
    vec4 ambient;
    vec4 clear_color;
    vec4 light_vec[4];
    vec4 light_color[4];
    Prim prims[64];
};

const float FAR = 1e20;
const float EPS = 1e-3;

// ray vs one primitive in its local space; returns t (FAR if missed) and the local normal
float hit_prim(int i, vec3 ro, vec3 rd, out vec3 n_local, out vec3 p_local)
{
    vec3 o = (prims[i].inv * vec4(ro, 1.0)).xyz;
    vec3 d = (prims[i].inv * vec4(rd, 0.0)).xyz;   // not normalized: t stays a world-space t
    int kind = int(prims[i].params.x + 0.5);
    float t = FAR;
    n_local = vec3(0.0, 1.0, 0.0);
    if (kind == 1) {                       // sphere, radius 0.5
        float a = dot(d, d), b = dot(o, d), c = dot(o, o) - 0.25;
        float disc = b * b - a * c;
        if (disc >= 0.0) {
            float s = sqrt(disc);
            float t0 = (-b - s) / a, t1 = (-b + s) / a;
            t = t0 > EPS ? t0 : (t1 > EPS ? t1 : FAR);
            n_local = o + d * t;
        }
    } else if (kind == 2) {                // box, -0.5..0.5
        vec3 inv_d = 1.0 / d;
        vec3 ta = (vec3(-0.5) - o) * inv_d, tb = (vec3(0.5) - o) * inv_d;
        vec3 tmin = min(ta, tb), tmax = max(ta, tb);
        float t0 = max(max(tmin.x, tmin.y), tmin.z), t1 = min(min(tmax.x, tmax.y), tmax.z);
        if (t0 <= t1 && t1 > EPS) {
            t = t0 > EPS ? t0 : t1;
            vec3 p = o + d * t;
            vec3 ap = abs(p);
            n_local = ap.x > ap.y && ap.x > ap.z ? vec3(sign(p.x), 0.0, 0.0)
                    : ap.y > ap.z ? vec3(0.0, sign(p.y), 0.0) : vec3(0.0, 0.0, sign(p.z));
        }
    } else if (kind == 3) {                // plane y = 0, -0.5..0.5 in x and z, two-sided
        if (abs(d.y) > 1e-8) {
            float tp = -o.y / d.y;
            vec3 p = o + d * tp;
            if (tp > EPS && abs(p.x) <= 0.5 && abs(p.z) <= 0.5) {
                t = tp;
                n_local = vec3(0.0, d.y < 0.0 ? 1.0 : -1.0, 0.0);
            }
        }
    }
    p_local = o + d * t;
    return t;
}

float trace(vec3 ro, vec3 rd, out int id, out vec3 n_world, out vec3 p_local)
{
    float best = FAR;
    id = -1;
    int count = int(counts.x + 0.5);
    for (int i = 0; i < 64; ++i) {
        if (i >= count) break;
        vec3 n, p;
        float t = hit_prim(i, ro, rd, n, p);
        if (t < best) {
            best = t;
            id = i;
            // normals go to world space with the inverse transpose of the model matrix
            n_world = normalize(transpose(mat3(prims[i].inv)) * n);
            p_local = p;
        }
    }
    return best;
}

bool shadowed(vec3 ro, vec3 rd, float max_t)
{
    int count = int(counts.x + 0.5);
    for (int i = 0; i < 64; ++i) {
        if (i >= count) break;
        vec3 n, p;
        if (hit_prim(i, ro, rd, n, p) < max_t) return true;
    }
    return false;
}

// the built-in checker (8 cells per unit of uv), 150/255 vs 255/255 like the raster path
float checker(int kind, vec3 p, vec3 n_local)
{
    vec2 uv;
    if (kind == 1) uv = vec2(atan(p.z, p.x) / 6.2831853 * 2.0, acos(clamp(p.y * 2.0, -1.0, 1.0)) / 3.14159265);
    else if (kind == 3) uv = p.xz + 0.5;
    else {
        vec3 a = abs(n_local);
        uv = (a.x > 0.5 ? p.zy : a.y > 0.5 ? p.xz : p.xy) + 0.5;
    }
    vec2 c = floor(uv * 8.0);
    return mod(c.x + c.y, 2.0) < 0.5 ? 150.0 / 255.0 : 1.0;
}

vec3 sky(vec3 rd)
{
    return mix(clear_color.rgb, clear_color.rgb * 1.6 + vec3(0.05, 0.07, 0.1), clamp(rd.y * 0.5 + 0.5, 0.0, 1.0));
}

void main()
{
#ifdef P3D_VK
    vec2 frag = vec2(gl_FragCoord.x, cam.w - gl_FragCoord.y);   // make +y up like GL
#else
    vec2 frag = gl_FragCoord.xy;
#endif
    vec2 ndc = frag / cam.zw * 2.0 - 1.0;
    vec3 rd = normalize(mat3(cam_to_world) * vec3(ndc.x * cam.x * cam.y, ndc.y * cam.x, -1.0));
    vec3 ro = cam_to_world[3].xyz;
    vec3 result = vec3(0.0), throughput = vec3(1.0);
    int lights = int(counts.y + 0.5), bounces = int(counts.z + 0.5);

    for (int bounce = 0; bounce < 5; ++bounce) {
        int id;
        vec3 n, pl;
        float t = trace(ro, rd, id, n, pl);
        if (id < 0) { result += throughput * sky(rd); break; }
        vec3 p = ro + rd * t;
        if (dot(n, rd) > 0.0) n = -n;
        int kind = int(prims[id].params.x + 0.5);
        vec3 base = prims[id].color.rgb;
        if (prims[id].params.y > 0.5) {
            vec3 nl = normalize((prims[id].inv * vec4(n, 0.0)).xyz);
            base *= checker(kind, pl, nl);
        }
        vec3 lit = ambient.rgb;
        vec3 spec = vec3(0.0);
        for (int l = 0; l < 4; ++l) {
            if (l >= lights) break;
            vec3 dir;
            float att = 1.0, max_t = FAR;
            if (light_vec[l].w > 0.5) {
                vec3 to = light_vec[l].xyz - p;
                max_t = length(to);
                dir = to / max(max_t, 1e-4);
                att = clamp(1.0 - max_t / light_color[l].w, 0.0, 1.0);
                att *= att;
            } else dir = -normalize(light_vec[l].xyz);
            float ndl = dot(n, dir);
            if (ndl <= 0.0 || att <= 0.0) continue;
            if (shadowed(p + n * EPS * 4.0, dir, max_t)) continue;
            lit += light_color[l].rgb * ndl * att;
            spec += light_color[l].rgb * pow(max(dot(reflect(-dir, n), -rd), 0.0), 48.0) * att * 0.4;
        }
        float refl = clamp(prims[id].color.a, 0.0, 1.0);
        result += throughput * ((base * lit + spec) * (1.0 - refl));
        if (refl <= 0.0 || bounce >= bounces) break;
        throughput *= refl * mix(vec3(1.0), base, 0.25);   // slightly tinted mirrors
        ro = p + n * EPS * 4.0;
        rd = reflect(rd, n);
    }
    out_color = vec4(result, 1.0);
}
