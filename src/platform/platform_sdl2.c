/* platform_sdl2.c: SDL2 platform layer (Linux, BSD, Windows XP+ with SDL 2.0.x, Emscripten). */
#include "p3d_platform.h"
#include "../core/p3d_core.h"

#include <SDL.h>
#ifdef P3D_WITH_VULKAN
#include <SDL_vulkan.h>
#endif
#include <string.h>

static SDL_Window *window;
static SDL_GLContext gl_context;
static p3d_surface_kind surface_kind;

int p3d_platform_init(const p3d_window_desc *desc)
{
    Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        P3D_ERROR("SDL_Init: %s", SDL_GetError());
        return 0;
    }
    surface_kind = desc->surface;
    if (surface_kind == P3D_SURFACE_GLES2 || surface_kind == P3D_SURFACE_GLES3) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, surface_kind == P3D_SURFACE_GLES3 ? 3 : 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        flags |= SDL_WINDOW_OPENGL;
    }
#ifdef P3D_WITH_VULKAN
    if (surface_kind == P3D_SURFACE_VULKAN) flags |= SDL_WINDOW_VULKAN;
#endif
    window = SDL_CreateWindow(desc->title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              desc->width, desc->height, flags);
    if (window == NULL) {
        P3D_ERROR("SDL_CreateWindow: %s", SDL_GetError());
        return 0;
    }
    if (surface_kind == P3D_SURFACE_GLES2 || surface_kind == P3D_SURFACE_GLES3) {
        gl_context = SDL_GL_CreateContext(window);
        if (gl_context == NULL) {
            P3D_ERROR("no OpenGL ES %d.0 context: %s", surface_kind == P3D_SURFACE_GLES3 ? 3 : 2, SDL_GetError());
            return 0;
        }
        SDL_GL_SetSwapInterval(1);
    }
    return 1;
}

void p3d_platform_shutdown(void)
{
    if (gl_context) SDL_GL_DeleteContext(gl_context);
    if (window) SDL_DestroyWindow(window);
    gl_context = NULL;
    window = NULL;
    SDL_Quit();
}

void p3d_platform_poll(p3d_input *input)
{
    SDL_Event e;
    const Uint8 *state;
    int n, x, y;
    Uint32 b;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT) input->quit = 1;
    state = SDL_GetKeyboardState(&n);
    if (n > P3D_KEY_COUNT) n = P3D_KEY_COUNT;
    memcpy(input->keys, state, (size_t)n);
    b = SDL_GetMouseState(&x, &y);
    input->mouse_x = x;
    input->mouse_y = y;
    input->buttons = (b & SDL_BUTTON_LMASK ? 1u : 0u) | (b & SDL_BUTTON_MMASK ? 2u : 0u) |
                     (b & SDL_BUTTON_RMASK ? 4u : 0u);
}

uint32_t p3d_platform_ticks_ms(void) { return SDL_GetTicks(); }

void p3d_platform_drawable_size(int *width, int *height)
{
    if (surface_kind == P3D_SURFACE_GLES2 || surface_kind == P3D_SURFACE_GLES3) SDL_GL_GetDrawableSize(window, width, height);
#ifdef P3D_WITH_VULKAN
    else if (surface_kind == P3D_SURFACE_VULKAN) SDL_Vulkan_GetDrawableSize(window, width, height);
#endif
    else SDL_GetWindowSize(window, width, height);
}

void p3d_platform_swap(void) { SDL_GL_SwapWindow(window); }

void p3d_platform_present_pixels(const uint32_t *pixels, int width, int height)
{
    SDL_Surface *dst = SDL_GetWindowSurface(window);
    SDL_Surface *src;
    if (dst == NULL) return;
    src = SDL_CreateRGBSurfaceWithFormatFrom((void *)pixels, width, height, 32, width * 4,
                                             SDL_PIXELFORMAT_ARGB8888);
    if (src == NULL) return;
    if (dst->w == width && dst->h == height) SDL_BlitSurface(src, NULL, dst, NULL);
    else SDL_BlitScaled(src, NULL, dst, NULL);
    SDL_FreeSurface(src);
    SDL_UpdateWindowSurface(window);
}

#ifdef P3D_WITH_VULKAN
const char *const *p3d_platform_vk_instance_extensions(unsigned *count)
{
    static const char *names[32];
    unsigned n = 32;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &n, names)) { *count = 0; return NULL; }
    *count = n;
    return names;
}

int p3d_platform_vk_create_surface(void *instance, void *surface)
{
    if (!SDL_Vulkan_CreateSurface(window, (VkInstance)instance, (VkSurfaceKHR *)surface)) {
        P3D_ERROR("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return 0;
    }
    return 1;
}
#else
const char *const *p3d_platform_vk_instance_extensions(unsigned *count) { *count = 0; return NULL; }
int p3d_platform_vk_create_surface(void *instance, void *surface) { (void)instance; (void)surface; return 0; }
#endif
