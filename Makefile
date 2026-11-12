# LEGO Dimensions Toy Pad - GoldHEN plugin for the PS4
#
#   make         builds lego_toypad.prx (needs OO_PS4_TOOLCHAIN and the GoldHEN Plugins SDK)
#   make test    runs the protocol tests on this PC (no PS4 SDK needed)
#   make sim     runs the whole plugin on this PC with a fake game (test/host_sim.c)
#   make web     regenerates source/web.h from web/index.html (only after changing the page)
#
# The GoldHEN SDK is looked up at ../GoldHEN_Plugins_SDK, or at GOLDHEN_SDK if you set it.

TARGET      := lego_toypad.prx
TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
GOLDHEN_SDK ?= $(CURDIR)/../GoldHEN_Plugins_SDK
LIBS        := -lSceLibcInternal -lkernel -lSceSysmodule
CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -c -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -isystem $(TOOLCHAIN)/include/orbis/_types
LDFLAGS     := -m elf_x86_64 -pie --export-dynamic --script $(TOOLCHAIN)/link.x -e _init --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS)
HOST_CC     ?= cc
HOST_CFLAGS ?= -Wall -Wextra -O1 -g -fsanitize=address,undefined

.DEFAULT_GOAL := all
.PHONY: all test sim web clean

all: $(TARGET)

web: source/web.h

source/web.h: web/index.html tools/gen_web.py
	python3 tools/gen_web.py

$(TARGET): source/main.c source/portal.c source/portal.h source/web.h
	@mkdir -p build
	clang $(CFLAGS) -o build/main.o source/main.c
	clang $(CFLAGS) -o build/portal.o source/portal.c
	clang -target x86_64-pc-linux-gnu -ffreestanding -nostdlib -fno-builtin -fPIC -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -c $(GOLDHEN_SDK)/crt/crtprx.c -o build/crtprx.o
	ld.lld build/main.o build/portal.o build/crtprx.o -o build/plugin.elf $(LDFLAGS)
	$(TOOLCHAIN)/bin/linux/create-fself -in=build/plugin.elf -out=build/plugin.oelf --lib=$(TARGET) --paid 0x3800000000000011

# PC test of the protocol engine: 90 checks
test: source/portal.c test/test_portal.c
	$(HOST_CC) $(HOST_CFLAGS) -Isource -o build_test source/portal.c test/test_portal.c && ./build_test

# whole plugin on this PC with a fake game: 60 checks
sim:
	sh test/run_host_sim.sh

clean:
	rm -rf build build_test $(TARGET)
