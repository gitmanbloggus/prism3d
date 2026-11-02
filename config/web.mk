# Browser build: Emscripten, SDL2 port, WebGL 1 (= GLES2). The engine owns the loop through
# emscripten_set_main_loop and scripts yield with prism.frame, so no ASYNCIFY is needed.
PLATFORM        := sdl2
CC              := emcc
AR              := emar
DEMO_BIN        := prism-demo.html
PLATFORM_CFLAGS := -sUSE_SDL=2
PLATFORM_LIBS   := -sUSE_SDL=2 -sMIN_WEBGL_VERSION=1 -sMAX_WEBGL_VERSION=1 -sALLOW_MEMORY_GROWTH=1 \
                   --preload-file scripts/demo.bin@scripts/demo.bin
RUN             := emrun
RENDERERS       := gles3 gles2 soft   # WebGL 2 / WebGL 1 / canvas
