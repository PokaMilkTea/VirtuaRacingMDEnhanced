# The GPGX core is built by its own upstream Makefile (sdl/Makefile.sdl2);
# our tools link those objects, minus the SDL frontend's main.o.

GPGX     = third_party/genplus-gx
GPGX_SDL = $(GPGX)/sdl
GPGX_OBJ = $(GPGX_SDL)/build_sdl2

# Must match DEFINES in $(GPGX_SDL)/Makefile.sdl2, or struct layouts will differ.
DEFINES  = -DLSB_FIRST -DUSE_16BPP_RENDERING -DUSE_LIBTREMOR -DUSE_LIBCHDR -DMAXROMSIZE=33554432 \
           -DHAVE_YM3438_CORE -DHAVE_OPLL_CORE -DENABLE_SUB_68K_ADDRESS_ERROR_EXCEPTIONS \
           -DZ7_ST -DZSTD_DISABLE_ASM -DHAVE_ALLOCA_H
INCLUDES = $(addprefix -I$(GPGX)/core, / /z80 /m68k /sound /sound/minimp3 /sound/tremor /input_hw /cart_hw \
             /cart_hw/svp /cd_hw /ntsc /cd_hw/libchdr/include /cd_hw/libchdr/deps/lzma-24.05/include) \
           -I$(GPGX_SDL) -I$(GPGX_SDL)/sdl2
CFLAGS   = -O2 -Wall -std=c99

# OpenGL: a framework on macOS, libGL elsewhere (Linux). Under -std=c99 glibc hides the POSIX
# functions (getopt, fseeko in GPGX's libchdr) unless asked for them with _DEFAULT_SOURCE; GPGX's
# Makefile leaves its DEFINES out of some rules, so the GPGX build gets it through CC.
ifeq ($(shell uname -s),Darwin)
GL_LIBS  = -framework OpenGL
else
GL_LIBS  = -lGL
POSIX    = -D_DEFAULT_SOURCE
CFLAGS  += $(POSIX)
GPGX_CC  = CC="$(CC) $(POSIX)"
endif

all: build/svptrace build/vrr

gpgx:
	$(MAKE) -C $(GPGX_SDL) -f Makefile.sdl2 $(GPGX_CC)

build/svptrace: src/svptrace.c src/core.c src/core.h gpgx
	@mkdir -p build
	$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) src/svptrace.c src/core.c \
		$$(ls $(GPGX_OBJ)/*.o | grep -v '/main\.o$$') -lz -lm -o $@

VRR_SOURCES = src/vrr.c src/core.c src/hle.c src/layers.c src/settings.c
VRR_HEADERS = src/core.h src/gl.h src/hle.h src/layers.h src/settings.h src/version.h

build/vrr: $(VRR_SOURCES) $(VRR_HEADERS) gpgx
	@mkdir -p build
	$(CC) $(CFLAGS) $$(sdl2-config --cflags) $(DEFINES) $(INCLUDES) $(VRR_SOURCES) \
		$$(ls $(GPGX_OBJ)/*.o | grep -v '/main\.o$$') $$(sdl2-config --libs) $(GL_LIBS) -lz -lm -o $@

# ---- Windows: vrr.exe cross-compiled with llvm-mingw against SDL2's MinGW development files,
# both downloaded into deps/ by win-deps. The core is built by GPGX's own Makefile as for
# Windows (OS=Windows_NT: no alloca.h), into its build_win; zlib from the copy GPGX carries.

LLVM_MINGW  = llvm-mingw-20260922-ucrt-macos-universal
SDL2_MINGW  = SDL2-devel-2.32.10-mingw
WIN_CC      = $(CURDIR)/deps/llvm-mingw/bin/x86_64-w64-mingw32-clang
WIN_SDL     = $(CURDIR)/deps/SDL2/x86_64-w64-mingw32
WIN_GPGX_OBJ = $(GPGX_SDL)/build_win
WIN_DEFINES = $(filter-out -DHAVE_ALLOCA_H,$(DEFINES))
ZLIB        = $(GPGX)/core/cd_hw/libchdr/deps/zlib-1.3.1
WIN_CFLAGS  = -O2 -Wall -Wno-strict-aliasing -std=c99 -I$(WIN_SDL)/include/SDL2 -I$(CURDIR)/$(ZLIB)
ZLIB_OBJ    = $(addprefix build/win/zlib/,adler32.o crc32.o gzclose.o gzlib.o gzread.o inffast.o inflate.o inftrees.o zutil.o)

win: build/win/vrr.exe

win-deps:
	@mkdir -p deps
	[ -d deps/llvm-mingw ] || (curl -L https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/$(LLVM_MINGW).tar.xz \
		| tar xJ -C deps && mv deps/$(LLVM_MINGW) deps/llvm-mingw)
	[ -d deps/SDL2 ] || (curl -L https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/$(SDL2_MINGW).tar.gz \
		| tar xz -C deps && mv deps/SDL2-2.32.10 deps/SDL2)

win-gpgx: win-deps
	$(MAKE) -C $(GPGX_SDL) -f Makefile.sdl2 -f $(CURDIR)/gpgx-objects.mk OS=Windows_NT OBJDIR=./build_win \
		CC=$(WIN_CC) CFLAGS="$(WIN_CFLAGS)" gpgx-objects

build/win/zlib/%.o: $(ZLIB)/%.c | win-deps            # reading only: no gzip writer
	@mkdir -p build/win/zlib
	$(WIN_CC) -O2 -DNO_GZCOMPRESS -c $< -o $@

build/win/vrr_rc.o: src/vrr.rc src/vrr.manifest src/version.h | win-deps     # manifest and version information
	@mkdir -p build/win
	$(CURDIR)/deps/llvm-mingw/bin/x86_64-w64-mingw32-windres -I src src/vrr.rc -o $@

build/win/vrr.exe: $(VRR_SOURCES) $(VRR_HEADERS) win-gpgx $(ZLIB_OBJ) build/win/vrr_rc.o
	$(WIN_CC) $(WIN_CFLAGS) $(WIN_DEFINES) $(INCLUDES) $(VRR_SOURCES) $(WIN_GPGX_OBJ)/*.o $(ZLIB_OBJ) build/win/vrr_rc.o \
		-L$(WIN_SDL)/lib -lmingw32 -lSDL2main -lSDL2 -lopengl32 -o $@
	cp $(WIN_SDL)/bin/SDL2.dll build/win/

# self-checks of logic that is not covered by vrr -d
test: src/settings.c src/settings.h
	@mkdir -p build
	$(CC) $(CFLAGS) -DSETTINGS_TEST src/settings.c -o build/settings_test && build/settings_test

clean:
	rm -rf build $(WIN_GPGX_OBJ)
	$(MAKE) -C $(GPGX_SDL) -f Makefile.sdl2 clean

.PHONY: all gpgx test clean win win-deps win-gpgx
