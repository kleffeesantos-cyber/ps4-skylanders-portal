# Builds skylanders_portal.prx (a GoldHEN plugin).
# Needs the OpenOrbis PS4 toolchain (OO_PS4_TOOLCHAIN) and the GoldHEN Plugins SDK next to this folder
# (../GoldHEN_Plugins_SDK) or at GOLDHEN_SDK.
TARGET      := skylanders_portal.prx
TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
GOLDHEN_SDK ?= $(CURDIR)/../GoldHEN_Plugins_SDK
LIBS        := -lSceLibcInternal -lkernel -lSceSysmodule
CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -c -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -isystem $(TOOLCHAIN)/include/orbis/_types
LDFLAGS     := -m elf_x86_64 -pie --export-dynamic --script $(TOOLCHAIN)/link.x -e _init --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS)

all: $(TARGET)

source/web.h: source/index.html tools/gen_web.py
	python3 tools/gen_web.py

$(TARGET): source/main.c source/portal.c source/portal.h source/web.h
	@mkdir -p build
	clang $(CFLAGS) -o build/main.o source/main.c
	clang $(CFLAGS) -o build/portal.o source/portal.c
	clang -target x86_64-pc-linux-gnu -ffreestanding -nostdlib -fno-builtin -fPIC -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -c $(GOLDHEN_SDK)/crt/crtprx.c -o build/crtprx.o
	ld.lld build/main.o build/portal.o build/crtprx.o -o build/plugin.elf $(LDFLAGS)
	$(TOOLCHAIN)/bin/linux/create-fself -in=build/plugin.elf -out=build/plugin.oelf --lib=$(TARGET) --paid 0x3800000000000011

# PC test of the portal protocol (no PS4 needed): make test
test:
	gcc -Wall -O1 -Isource -o test_lego source/test_lego.c && ./test_lego
clean:
	rm -rf build build_test $(TARGET) source/web.h

.PHONY: all test clean
