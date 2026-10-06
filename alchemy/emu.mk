# The alchemy-lab emulator (branch emu-custom):
#
#   make -C <alchemy-lab>/emu FW=custom FW_DIR=<this repo>/alchemy
#   make -C <alchemy-lab>/emu test-custom FW_DIR=<this repo>/alchemy   (emu-tests/*.emu)
#   make -C <alchemy-lab>/emu WEB=1 FW=custom FW_DIR=<this repo>/alchemy   (browser build)
#
# Described here rather than read from the Makefile because picker.cpp must
# stay out: the emulator stubs the SD picker itself (duplicate symbols).
# The SDK is taken by path, as the Makefile does (no submodules here).
EMU_TARGET       := stencil_alchemy
EMU_CPP_SOURCES  := src/stencil_alchemy.cpp
EMU_C_SOURCES    := ../core/stencil.c
EMU_C_INCLUDES   := -Isrc -I../core
EMU_C_DEFS       := -DSTENCIL_VERSION=\"emu\" -DSTENCIL_GIT_HASH=\"emu\"
EMU_ALCHEMY_DIR  ?= $(HOME)/tim-os/daisy-sdk/alchemy-sdk
EMU_LIBDAISY_DIR ?= $(HOME)/tim-os/daisy-sdk/alchemy-sdk/vendor/libDaisy
EMU_LABELS       := RATE|PATTERN|GATE|ATTACK|RELEASE|DEPTH
EMU_LABELS_ALT   := FILTER|STEPS/FIT|R OFFSET|SWING|MIX|TEMPO
