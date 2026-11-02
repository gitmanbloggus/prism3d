# Native Linux / BSD with SDL2 and OpenGL ES 2 (Mesa provides libGLESv2 on desktop GPUs).
PLATFORM        := sdl2
CC              ?= cc
AR              ?= ar
DEMO_BIN        := prism-demo
PLATFORM_CFLAGS := $(shell pkg-config --cflags sdl2) -DP3D_DYNAMIC_MODULES
PLATFORM_LIBS   := $(shell pkg-config --libs sdl2) $(shell pkg-config --libs glesv2 2>/dev/null || echo -lGLESv2) -lm -ldl
RUN             :=
