/* p3d_platform.h: window, input, time. One implementation per OS/console (platform_<name>.c).
 * Milestone 1 ships platform_sdl2.c, which covers Linux, Windows and the web (Emscripten). */
#ifndef P3D_PLATFORM_H
#define P3D_PLATFORM_H

#include <stdint.h>

/* What the renderer needs from the window. */
typedef enum {
    P3D_SURFACE_PIXELS,   /* CPU framebuffer, for the software tier (T0) */
    P3D_SURFACE_GLES2,    /* OpenGL ES 2.0 / WebGL 1 context (T2) */
    P3D_SURFACE_GLES3,    /* OpenGL ES 3.0 / WebGL 2 context (T2) */
    P3D_SURFACE_VULKAN    /* a window a Vulkan surface can be made for (T3) */
} p3d_surface_kind;

typedef struct {
    const char *title;
    int width, height;
    p3d_surface_kind surface;
} p3d_window_desc;

/* Key codes are USB HID usage ids (the same numbers as SDL scancodes): 4 = A, 26 = W, 41 = Escape,
 * 44 = Space, 79-82 = Right/Left/Down/Up. Platforms without USB keyboards map onto these. */
#define P3D_KEY_COUNT 512
typedef struct {
    uint8_t keys[P3D_KEY_COUNT];  /* 1 = held */
    int mouse_x, mouse_y;
    unsigned buttons;             /* bit 0 left, bit 1 middle, bit 2 right */
    int quit;
} p3d_input;

int      p3d_platform_init(const p3d_window_desc *desc);
void     p3d_platform_shutdown(void);
void     p3d_platform_poll(p3d_input *input);   /* pump events, update input */
uint32_t p3d_platform_ticks_ms(void);
void     p3d_platform_drawable_size(int *width, int *height);
void     p3d_platform_swap(void);               /* P3D_SURFACE_GLES2/3 */
/* P3D_SURFACE_VULKAN. Typed as void* so this header doesn't need vulkan.h:
 * instance is a VkInstance, surface points to a VkSurfaceKHR. */
const char *const *p3d_platform_vk_instance_extensions(unsigned *count);
int      p3d_platform_vk_create_surface(void *instance, void *surface);
/* P3D_SURFACE_PIXELS: show a 0xAARRGGBB framebuffer, scaled to the window. */
void     p3d_platform_present_pixels(const uint32_t *pixels, int width, int height);

#endif
