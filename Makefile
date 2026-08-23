RACK_DIR ?= Rack-SDK

ER301 := er-301

# ER-301 include paths (src/ first so our HAL/emu overrides take priority)
FLAGS += -Isrc
FLAGS += -I$(ER301)
FLAGS += -I$(ER301)/emu
FLAGS += -I$(ER301)/arch/darwin
FLAGS += -I$(ER301)/libs/lua54
FLAGS += -I$(ER301)/libs/lodepng
FLAGS += -I$(ER301)/libs/miniz

# Glue includes (for string_vector_wrapper.h etc.)
FLAGS += -I$(ER301)/emu/od/glue

# doomgeneric (pulled in by od/graphics/screensavers/Doom.cpp). The
# firmware's own emu build (scripts/emu.mk) compiles this whole dir and
# links SDL2 the same way -- i_system.c includes SDL.h directly.
FLAGS += -I$(ER301)/libs/doomgeneric
# NOTE: `brew --prefix sdl2` can resolve to a stale/non-existent
# "sdl2-compat" alias path on some Homebrew installs even though the
# real keg lives at $(brew --prefix)/opt/sdl2. Prefer the opt symlink
# directly and only fall back to `brew --prefix sdl2` if it's missing.
SDL2_PREFIX := $(shell test -d "$(shell brew --prefix)/opt/sdl2" && echo "$(shell brew --prefix)/opt/sdl2" || brew --prefix sdl2 2>/dev/null || echo /opt/homebrew)
FLAGS += -I$(SDL2_PREFIX)/include -I$(SDL2_PREFIX)/include/SDL2
LDFLAGS += -L$(SDL2_PREFIX)/lib -lSDL2

# Architecture flags
ifeq ($(shell uname -m),arm64)
  FLAGS += -march=armv8.2-a
else
  FLAGS += -msse4
endif

# ER-301 build flags
FLAGS += -DBUILDOPT_TESTING
FLAGS += -DFIRMWARE_VERSION=\"0.7.0-vcv\"
FLAGS += -DBUILD_PROFILE=\"testing\"
FLAGS += -ffast-math -ftree-vectorize
FLAGS += -Wno-c++11-narrowing -Wno-sign-compare -Wno-unused-variable

# FFTW
FFTW_PREFIX := $(shell brew --prefix fftw 2>/dev/null || echo /opt/homebrew)
FLAGS += -I$(FFTW_PREFIX)/include
LDFLAGS += -L$(FFTW_PREFIX)/lib -lfftw3f

# C standard for .c files
CFLAGS += -std=gnu11

# Plugin sources
SOURCES += src/plugin.cpp
SOURCES += src/ER301Module.cpp

# VCV HAL replacements
SOURCES += src/hal/audio.c
SOURCES += src/hal/display.cpp
SOURCES += src/hal/gpio.c
SOURCES += src/hal/encoder.cpp
SOURCES += src/hal/timing.c
SOURCES += src/hal/log.c
SOURCES += src/hal/card.cpp
SOURCES += src/hal/modulation.c
SOURCES += src/hal/pwm.c
SOURCES += src/hal/rng.c
SOURCES += src/hal/uart.c
SOURCES += src/hal/usb.cpp
SOURCES += src/hal/adc.c
SOURCES += src/hal/i2cSlave.c
SOURCES += src/hal/dir.c
SOURCES += src/hal/fft.c
SOURCES += src/hal/modulemap.cpp
SOURCES += src/hal/concurrency/Mutex.cpp
SOURCES += src/hal/concurrency/Thread.cpp
SOURCES += src/hal/concurrency/EventFlags.cpp

# ER-301 core engine (od/)
ER301_OD_CPP := $(shell find $(ER301)/od -name '*.cpp' ! -path '*/glue/*_swig*')
ER301_OD_C := $(shell find $(ER301)/od -name '*.c')
SOURCES += $(ER301_OD_CPP)
SOURCES += $(ER301_OD_C)

# HAL (non-platform-specific parts)
SOURCES += $(ER301)/hal/pump/pump.cpp
SOURCES += $(ER301)/hal/pump/resample4.c
SOURCES += $(ER301)/hal/pump/rfifo4.c
SOURCES += $(ER301)/hal/pump/pidcontrol.c
SOURCES += $(ER301)/hal/events.cpp
SOURCES += $(ER301)/hal/simd.c

# Architecture-specific (darwin)
SOURCES += $(shell find $(ER301)/arch/darwin -name '*.c' -o -name '*.cpp')

# TLS (our own pthread-based replacement)
SOURCES += src/tls.c

# Lua 5.4 library
LUA_SOURCES := $(filter-out %/lua.c %/luac.c %/luaoslib.c, $(wildcard $(ER301)/libs/lua54/*.c))
SOURCES += $(LUA_SOURCES)

# lodepng
SOURCES += $(ER301)/libs/lodepng/lodepng.cpp

# miniz
SOURCES += $(ER301)/libs/miniz/miniz.c

# doomgeneric (Doom.cpp screensaver dependency; mirrors scripts/emu.mk)
SOURCES += $(wildcard $(ER301)/libs/doomgeneric/*.c)

# SWIG-generated bindings (pre-generate)
SOURCES += build/swig/app_swig.cpp

# Pre-generate SWIG bindings
SWIG ?= swig
SWIGFLAGS := -lua -no-old-metatable-bindings -fvirtual -fcompact -c++
SWIGFLAGS += -I$(ER301) -I$(ER301)/emu -Isrc -I$(ER301)/libs/lua54

build/swig/app_swig.cpp: $(ER301)/od/glue/app.cpp.swig
	@mkdir -p $(@D)
	$(SWIG) $(SWIGFLAGS) -o $@ $<

# Ensure SWIG runs before compilation
$(shell mkdir -p build/swig)

DISTRIBUTABLES += res

include $(RACK_DIR)/plugin.mk

# Direct install (bypasses zstd packaging)
PLUGINS_DIR ?= $(HOME)/Library/Application Support/Rack2/plugins-mac-arm64
direct-install: all
	mkdir -p dist/ER-301
	cp plugin.dylib plugin.json dist/ER-301/
	rsync -a res dist/ER-301/
	rsync -a dist/ER-301/ "$(PLUGINS_DIR)/ER-301/"
	@echo "Installed to $(PLUGINS_DIR)/ER-301/"
