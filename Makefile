# Prism3D: plain GNU Make. Pick a target with `make TARGET=<name>` (config/<name>.mk).
#   make                      native Linux/BSD build (TARGET=linux)
#   make TARGET=web           Emscripten build (needs emcc on PATH)
#   make scripts              compile SIPL scripts to SEB (.bin) with siplc.py
#   make run / make test

TARGET   ?= linux
SEB_DIR  ?= siplaseb
BUILD    := build/$(TARGET)

# Engine modules compiled in. Render backends are named render_<x>.
RENDERERS ?= gles2 soft

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
ALL_CFLAGS := $(CFLAGS) $(WARN) $(P3D_DEFS) $(PLATFORM_CFLAGS) -I$(SEB_DIR)/c/src

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

# engine: C99
$(BUILD)/%.o: %.c
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
