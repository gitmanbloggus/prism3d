# Prism3D: plain GNU Make. Pick a target with `make TARGET=<name>` (config/<name>.mk).
#   make                      native Linux/BSD build (TARGET=linux)
#   make TARGET=web           Emscripten build (needs emcc on PATH)
#   make scripts              compile SIPL scripts to SEB (.bin) with siplc.py
#   make run / make test

TARGET   ?= linux
SEB_DIR  ?= siplaseb
BUILD    := build/$(TARGET)

# Render backends compiled in (src/render/<x>/render_<x>.c), tried best first at startup.
RENDERERS ?= vulkan gles3 gles2 soft

include config/$(TARGET).mk

P3D_SRC := src/core/core.c src/core/math.c \
           src/platform/platform_$(PLATFORM).c \
           src/render/render.c $(foreach r,$(RENDERERS),src/render/$(r)/render_$(r).c) \
           src/scene/scene.c src/scene/primitives.c \
           src/assets/image.c \
           src/module/module.c \
           src/script/script_seb.c \
           src/engine/engine.c
SEB_SRC := $(SEB_DIR)/c/src/sebvm.c $(SEB_DIR)/c/src/sebload.c
DEMO_SRC := games/demo/main.c

P3D_DEFS := $(foreach r,$(RENDERERS),-DP3D_WITH_$(shell echo $(r) | tr a-z A-Z))
CFLAGS   ?= -O2 -g
WARN     := -Wall -Wextra -Wpedantic -Wno-unused-parameter
ALL_CFLAGS := $(CFLAGS) $(WARN) $(P3D_DEFS) $(PLATFORM_CFLAGS) -I$(SEB_DIR)/c/src -I$(BUILD)/gen

# Shaders (src/render/shaders) are shared by gles3 and vulkan; tools/shaders.py embeds them.
SHADER_SRC := $(wildcard src/render/shaders/*)
GEN_HDRS := $(if $(filter gles3,$(RENDERERS)),$(BUILD)/gen/shaders_gles.h) \
            $(if $(filter vulkan,$(RENDERERS)),$(BUILD)/gen/shaders_vk.h)
PLATFORM_LIBS += $(if $(filter vulkan,$(RENDERERS)),$(VULKAN_LIBS)) $(if $(filter gles3,$(RENDERERS)),$(GLES_LIBS),$(if $(filter gles2,$(RENDERERS)),$(GLES_LIBS)))

OBJ_P3D  := $(P3D_SRC:%.c=$(BUILD)/%.o)
OBJ_SEB  := $(SEB_SRC:$(SEB_DIR)/%.c=$(BUILD)/seb/%.o)
OBJ_DEMO := $(DEMO_SRC:%.c=$(BUILD)/%.o)

SIPLC    ?= python3 $(SEB_DIR)/siplc.py
SCRIPTS  := $(patsubst %.sipl,%.bin,$(wildcard scripts/*.sipl))

.PHONY: all scripts run test clean
all: $(BUILD)/$(DEMO_BIN) scripts

$(BUILD)/libprism.a: $(OBJ_P3D) $(OBJ_SEB)
	$(AR) rcs $@ $^

$(BUILD)/$(DEMO_BIN): $(OBJ_DEMO) $(BUILD)/libprism.a $(SCRIPTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJ_DEMO) $(BUILD)/libprism.a $(PLATFORM_LIBS)

$(BUILD)/gen/shaders_gles.h: $(SHADER_SRC) tools/shaders.py
	python3 tools/shaders.py gles $@
$(BUILD)/gen/shaders_vk.h: $(SHADER_SRC) tools/shaders.py
	python3 tools/shaders.py vk $@

# engine: C99
$(BUILD)/%.o: %.c | $(GEN_HDRS)
	@mkdir -p $(dir $@)
	$(CC) -std=c99 $(ALL_CFLAGS) -MMD -MP -c -o $@ $<

# SEB VM: C11 (as upstream)
$(BUILD)/seb/%.o: $(SEB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(CFLAGS) $(PLATFORM_CFLAGS) -w -MMD -MP -c -o $@ $<

scripts: $(SCRIPTS)
scripts/%.bin: scripts/%.sipl scripts/lib/prism.def $(wildcard scripts/*.def)
	$(SIPLC) $< -L scripts/lib -o $@

run: all
	$(RUN) $(BUILD)/$(DEMO_BIN)

test: all
	sh tests/smoke.sh $(BUILD)/$(DEMO_BIN)

clean:
	rm -rf build scripts/*.bin

-include $(OBJ_P3D:.o=.d) $(OBJ_SEB:.o=.d) $(OBJ_DEMO:.o=.d)
