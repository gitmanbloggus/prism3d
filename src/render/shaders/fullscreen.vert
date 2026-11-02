// fullscreen.vert: one triangle covering the screen. GLSL ES 3.00 (gles3) or 450 (vulkan, -DP3D_VK).
#ifdef P3D_VK
#define VERTEX_ID gl_VertexIndex
#else
#define VERTEX_ID gl_VertexID
#endif
void main()
{
    vec2 p = vec2(float((VERTEX_ID << 1) & 2), float(VERTEX_ID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
