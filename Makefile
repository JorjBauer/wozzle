MACFLAGS=-mmacosx-version-min=10.15

include wozcore.mk

CFLAGS = -Wall -g $(WOZCORE_CFLAGS) -I/opt/homebrew/opt/readline/include -D_FILE_OFFSET_BITS=64

# wozfuse needs FUSE 3 (libfuse, or macFUSE on a Mac), and is only built
# when pkg-config can find it
FUSE_CFLAGS := $(shell pkg-config --cflags fuse3 2>/dev/null)
FUSE_LIBS := $(shell pkg-config --libs fuse3 2>/dev/null)
CXXFLAGS = $(CFLAGS)

# The disk image core (src/, listed in wozcore.mk) is built once into a
# static library that every tool links against
LIBWOZCORE = libwozcore.a

WOZSRCS=wozzle.cpp
WOZOBJS=wozzle.o

FUSESRCS=wozfuse.cpp
FUSEOBJS=wozfuse.o

WOZITSRCS=clireport.cpp wozit.cpp
WOZITOBJS=clireport.o wozit.o

WOZMOSISSRCS=wozmosis.cpp
WOZMOSISOBJS=wozmosis.o

# The WebAssembly build (make wasm), for the GUI: the library plus
# wasm/wozjs.cpp, as an ES module that wasm/wozzle.mjs wraps. Needs
# Emscripten (emcc and em++).
EMCC ?= emcc
EMXX ?= em++
WASM_BUILD = wasm/build
WASM_OBJS = $(addprefix $(WASM_BUILD)/obj/,$(notdir $(addsuffix .o,$(basename $(WOZCORE_SRCS))))) \
	$(WASM_BUILD)/obj/wozjs.o
WASM_FUNCS = wozMalloc wozFree malloc free wozLastError wozClearError \
	wozImageOpen wozImageClose wozImageDetect wozImageFsType wozImageProbe \
	wozImageIsDirty wozImageSave wozImageReadFile wozImageListBasic \
	wozImageWriteFile wozImageDelete wozImageRemoveDirectory \
	wozImageMakeDirectory wozImageRenameVolume wozImageInstallBootBlocks \
	wozImageKrunch wozImageResize wozCreateBlankImage wozConvertImage \
	wozParseSize wozJsInit wozJsVolumeInfo wozJsList wozJsStat wozJsCopy
comma := ,
space := $(subst ,, )
WASM_EXPORTS = $(subst $(space),$(comma),$(strip $(addprefix _,$(WASM_FUNCS))))
WASM_RUNTIME = ccall,FS,UTF8ToString,stringToNewUTF8,HEAPU8,HEAPU32

.PHONY: test clean distclean unit-tests wasm

all: wozzle wozit wozmosis $(if $(FUSE_LIBS),wozfuse)

$(LIBWOZCORE): $(WOZCORE_OBJS)
	rm -f $@
	$(AR) rcs $@ $(WOZCORE_OBJS)

UNIT_BINS = tests/unit-intbas tests/unit-applesoft tests/unit-nib tests/unit-host tests/unit-capi tests/unit-dosref

unit-tests: $(UNIT_BINS)

tests/unit-intbas: tests/unit-intbas.cpp $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -I. tests/unit-intbas.cpp $(LIBWOZCORE) -o $@

tests/unit-applesoft: tests/unit-applesoft.cpp $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -I. tests/unit-applesoft.cpp $(LIBWOZCORE) -o $@

tests/unit-nib: tests/unit-nib.cpp $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -I. tests/unit-nib.cpp $(LIBWOZCORE) -o $@

tests/unit-host: tests/unit-host.cpp $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -I. tests/unit-host.cpp $(LIBWOZCORE) -o $@

# Compiled as C, to show wozlib.h works from C; linked as C++
tests/unit-capi: tests/unit-capi.c $(LIBWOZCORE)
	$(CC) $(CFLAGS) -std=c99 -c tests/unit-capi.c -o tests/unit-capi.o
	$(CXX) $(CFLAGS) tests/unit-capi.o $(LIBWOZCORE) -o $@

tests/unit-dosref: tests/unit-dosref.c $(LIBWOZCORE)
	$(CC) $(CFLAGS) -std=c99 -c tests/unit-dosref.c -o tests/unit-dosref.o
	$(CXX) $(CFLAGS) tests/unit-dosref.o $(LIBWOZCORE) -o $@

wozzle: $(WOZOBJS) $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -o wozzle $(WOZOBJS) $(LIBWOZCORE)

wozfuse.o: wozfuse.cpp
	$(CXX) $(CXXFLAGS) $(FUSE_CFLAGS) -c wozfuse.cpp -o $@

wozfuse: $(FUSEOBJS) $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -o wozfuse $(FUSEOBJS) $(LIBWOZCORE) $(FUSE_LIBS)

wozit: $(WOZITOBJS) $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -L/opt/homebrew/opt/readline/lib -o wozit $(WOZITOBJS) $(LIBWOZCORE) $(READLINE_LIBS) $(if $(filter win32,$(HOST_OS)),,-lncurses)

wozmosis: $(WOZMOSISOBJS) $(LIBWOZCORE)
	$(CXX) $(CFLAGS) -o wozmosis $(WOZMOSISOBJS) $(LIBWOZCORE)

wasm: $(WASM_BUILD)/wozcore.mjs

$(WASM_BUILD)/obj/%.o: src/%.cpp src/*.h
	@mkdir -p $(WASM_BUILD)/obj
	$(EMXX) -O2 $(WOZCORE_CFLAGS) -c $< -o $@

$(WASM_BUILD)/obj/%.o: src/%.c src/*.h
	@mkdir -p $(WASM_BUILD)/obj
	$(EMCC) -O2 $(WOZCORE_CFLAGS) -c $< -o $@

$(WASM_BUILD)/obj/wozjs.o: wasm/wozjs.cpp src/*.h
	@mkdir -p $(WASM_BUILD)/obj
	$(EMXX) -O2 $(WOZCORE_CFLAGS) -c $< -o $@

$(WASM_BUILD)/wozcore.mjs: $(WASM_OBJS)
	$(EMXX) -O2 $(WASM_OBJS) -o $@ -sMODULARIZE -sEXPORT_ES6 \
	  -sEXPORT_NAME=createWozModule -sALLOW_MEMORY_GROWTH \
	  -sENVIRONMENT=web,worker,node \
	  -sEXPORTED_FUNCTIONS=$(WASM_EXPORTS) \
	  -sEXPORTED_RUNTIME_METHODS=$(WASM_RUNTIME)

depend: .depend

.depend: $(WOZCORE_SRCS) $(WOZSRCS) $(FUSESRCS) $(WOZITSRCS) $(WOZMOSISSRCS)
	rm -f ./.depend
	for f in $^; do $(CC) $(CFLAGS) -MM -MT $${f%.*}.o $$f; done > ./.depend

# Cleaning doesn't need the dependencies, and including .depend would
# make it first, only for distclean to delete it again
ifeq ($(filter clean distclean,$(MAKECMDGOALS)),)
include .depend
endif

clean:
	rm -f $(WOZCORE_OBJS) $(LIBWOZCORE) $(WOZOBJS) $(FUSEOBJS) $(WOZITOBJS) $(WOZMOSISOBJS) wozzle wozit wozmosis wozfuse *~ $(UNIT_BINS)
	rm -rf *.dSYM tests/*.dSYM $(WASM_BUILD)
	rm -f tests/nib-test tests/test-pattern tests/make-test-disk tests/unit-capi.o tests/unit-dosref.o
	rm -f tests/pattern.dsk tests/pattern-out.dsk
	rm -rf dist gui/dist gui/build gui/src-tauri/gen/schemas
	rmdir gui/src-tauri/gen 2>/dev/null || true

# clean, and the things a build fetches or generates along the way: the
# GUI's npm packages (make app installs them again) and .depend
distclean: clean
	rm -rf gui/node_modules .depend

test: all unit-tests
	./run-tests.sh

# ---- release builds ----
#
#   make version    check the version is the same everywhere it's written
#   make tools      the command line tools, built for release, in dist/build/bin/
#   make app        the GUI app for this computer, in gui/dist/
#   make run        make app, then open it
#   make dev        run the GUI with its front end reloading as you edit
#   make dist       both, packed for a release: the tools into dist/release/,
#                   the app into gui/dist/
#   make release    publish those as a GitHub release (see tools/release.sh)
#
# The GUI's transient files (Vite's output, Rust's build) are in gui/build.
#
# Releases are built for the computer you're on: a Mac builds the Mac
# files, and a Linux or Windows machine adds its own to the same release
# with "sh tools/release.sh add". GitHub does that for both once a release
# is published (.github/workflows/build.yml). Windows builds under MSYS2's
# UCRT64 shell, with MinGW for the tools and Rust's MSVC toolchain for
# the app.
#
# The version is set in one place, library.properties. Two copies of it
# have to exist as files: src/wozversion.h, because the Arduino IDE and
# the other projects using src/ compile it without make, and the GUI's
# package.json, which Tauri reads (tauri.conf.json points at it, and
# Cargo.toml has none; npm keeps package-lock.json's in step). make
# writes both from library.properties before anything that uses them,
# and make version always writes them.

NPM ?= npm
VERSION   := $(shell sed -n 's/^version=//p' library.properties)
HOST_OS   := $(shell uname -s | sed -e 's/Darwin/darwin/' -e 's/Linux/linux/' -e 's/MINGW.*/win32/' -e 's/UCRT.*/win32/' -e 's/CLANG.*/win32/' -e 's/MSYS.*/win32/')
HOST_ARCH := $(shell uname -m | sed -e 's/x86_64/x64/' -e 's/aarch64/arm64/')

# MSYS2 has gcc and g++ but no cc. They're set here rather than on the
# command line, since make hands command-line variables to everything it
# runs, and Rust's build would then compile Tauri's C++ with MinGW, which
# the MSVC linker can't read.
ifeq ($(HOST_OS),win32)
ifeq ($(origin CC),default)
CC := gcc
endif
ifeq ($(origin CXX),default)
CXX := g++
endif
endif
REL_BUILD := dist/build
REL_OUT   := dist/release
GUI_BUILD := gui/build
GUI_OUT   := gui/dist

# The tools are built apart from the everyday ones (their own objects, in
# dist/build), optimized, and on a Mac for both Apple Silicon and Intel.
# wozit links the system's readline (libedit on a Mac) rather than
# Homebrew's, so it runs on a Mac that doesn't have Homebrew. wozfuse
# still needs macFUSE (or libfuse3) installed wherever it runs.
# macOS 12 is the oldest macFUSE 5 (and so wozfuse) runs on; the current
# SDK's C++ library won't go back past 11 anyway.
#
# On Windows the tools are linked statically, so they don't need MinGW's
# DLLs next to them, and MinGW's readline wants termcap after it.
ifeq ($(HOST_OS),darwin)
REL_ARCHFLAGS := -arch arm64 -arch x86_64 -mmacosx-version-min=12.0
TOOLS_ARCH    := universal
else
REL_ARCHFLAGS :=
TOOLS_ARCH    := $(HOST_ARCH)
endif
ifeq ($(HOST_OS),win32)
EXE           := .exe
REL_LDFLAGS   := -static
READLINE_LIBS := -lreadline -ltermcap
else
EXE           :=
REL_LDFLAGS   :=
READLINE_LIBS := -lreadline
endif
REL_CFLAGS := -O2 -Wall $(WOZCORE_CFLAGS) -D_FILE_OFFSET_BITS=64 $(REL_ARCHFLAGS)
REL_OBJ    := $(REL_BUILD)/obj
REL_CORE   := $(addprefix $(REL_OBJ)/,$(notdir $(addsuffix .o,$(basename $(WOZCORE_SRCS)))))
REL_TOOLS  := $(addprefix $(REL_BUILD)/bin/,$(addsuffix $(EXE),wozzle wozit wozmosis $(if $(FUSE_LIBS),wozfuse)))
TOOLS_PKG  := wozzle-tools-$(VERSION)-$(HOST_OS)-$(TOOLS_ARCH)
# Everything that goes in the tools tarball, and nothing else
TOOLS_SHIP := $(REL_TOOLS) README.md LICENSE

$(REL_OBJ)/%.o: src/%.cpp src/*.h
	@mkdir -p $(REL_OBJ)
	$(CXX) $(REL_CFLAGS) -c $< -o $@

$(REL_OBJ)/%.o: src/%.c src/*.h
	@mkdir -p $(REL_OBJ)
	$(CC) $(REL_CFLAGS) -c $< -o $@

$(REL_OBJ)/%.o: %.cpp src/*.h
	@mkdir -p $(REL_OBJ)
	$(CXX) $(REL_CFLAGS) $(if $(filter wozfuse.cpp,$<),$(FUSE_CFLAGS)) -c $< -o $@

$(REL_BUILD)/bin/wozzle$(EXE): $(REL_OBJ)/wozzle.o $(REL_CORE)
	@mkdir -p $(dir $@)
	$(CXX) $(REL_CFLAGS) $(REL_LDFLAGS) -o $@ $^

$(REL_BUILD)/bin/wozit$(EXE): $(REL_OBJ)/clireport.o $(REL_OBJ)/wozit.o $(REL_CORE)
	@mkdir -p $(dir $@)
	$(CXX) $(REL_CFLAGS) $(REL_LDFLAGS) -o $@ $^ $(READLINE_LIBS)

$(REL_BUILD)/bin/wozmosis$(EXE): $(REL_OBJ)/wozmosis.o $(REL_CORE)
	@mkdir -p $(dir $@)
	$(CXX) $(REL_CFLAGS) $(REL_LDFLAGS) -o $@ $^

$(REL_BUILD)/bin/wozfuse$(EXE): $(REL_OBJ)/wozfuse.o $(REL_CORE)
	@mkdir -p $(dir $@)
	$(CXX) $(REL_CFLAGS) -o $@ $^ $(FUSE_LIBS)

# The GUI is built for this computer's architecture. A universal Mac app
# needs Rust's x86_64-apple-darwin target as well (rustup can add it); then
# APP_TARGET=universal-apple-darwin builds one.
APP_TARGET ?=
APP_ARCH   := $(if $(filter universal-apple-darwin,$(APP_TARGET)),universal,$(HOST_ARCH))
APP_TARGET_FLAG := $(if $(APP_TARGET),--target $(APP_TARGET))
APP_BUNDLE_DIR  := $(GUI_BUILD)/target/$(if $(APP_TARGET),$(APP_TARGET)/)release/bundle
APP_PKG    := Wozzle-$(VERSION)-$(HOST_OS)-$(APP_ARCH)
# Which of Tauri's packages to make: the .app on a Mac (zipped by
# rel-app), the NSIS installer on Windows, and an AppImage and a .deb on
# Linux
APP_BUNDLES := $(if $(filter darwin,$(HOST_OS)),app,$(if $(filter win32,$(HOST_OS)),nsis,appimage$(comma)deb))

# The app gets the front end's index.html, its favicon, and Vite's assets/
# (the code, styles and wasm it built from src/), and nothing else. Tauri
# would embed all of Vite's output, which also has a copy of whatever is in
# gui/public, so a stray file there (notes, a .env) could end up inside a
# public release; instead only these are staged, and that's what's
# embedded. gui-ship-check stops the build if index.html refers to a
# top-level file the list leaves out, since the app would then go without
# it. Widen the list first.
GUI_SHIP     := index.html favicon.svg assets
GUI_WEB      := $(GUI_BUILD)/web
GUI_STAGE    := $(GUI_BUILD)/frontend
# Which files the finished Mac app may hold: what Tauri itself puts there
APP_CONTENTS := Contents/Info.plist Contents/MacOS/wozzle-gui \
	Contents/Resources/icon.icns Contents/_CodeSignature/CodeResources

.PHONY: version tools app run dev dist release rel-tools rel-app gui-stage gui-ship-check

# The header is only replaced when the version in it changes, since
# everything includes it and a newer copy would rebuild it all (and a
# prebuilt wasm/build, which a Windows build can't remake). npm version
# sets package-lock.json's copy along with package.json's.
SET_H_VERSION   = sed 's/"Wozzle v[^"]*"/"Wozzle v$(VERSION)"/' src/wozversion.h > src/wozversion.h.new && \
	{ cmp -s src/wozversion.h.new src/wozversion.h && rm -f src/wozversion.h.new || mv src/wozversion.h.new src/wozversion.h; }
SET_GUI_VERSION = cd gui && $(NPM) version $(VERSION) --no-git-tag-version --allow-same-version >/dev/null

src/wozversion.h: library.properties
	$(SET_H_VERSION)

gui/package.json: library.properties
	$(SET_GUI_VERSION)

# Not by the files' times: a checkout, or an edit undone within the
# second, can leave a copy looking newer than library.properties
version:
	@$(SET_H_VERSION)
	@$(SET_GUI_VERSION)
	@grep -q '"Wozzle v$(VERSION)"' src/wozversion.h && \
	  grep -q '^  "version": "$(VERSION)",' gui/package.json || \
	  { echo "couldn't write version $(VERSION) into src/wozversion.h and gui/package.json" >&2; exit 1; }
	@echo "version $(VERSION)"

tools: $(REL_TOOLS)

# A CI build sets NPM_INSTALL=ci, which installs exactly what
# package-lock.json says and never rewrites it
NPM_INSTALL ?= install

gui/node_modules: gui/package.json
	cd gui && $(NPM) $(NPM_INSTALL)
	@touch $@

# Tauri signs the program inside the app but not the bundle around it, so
# it gets an ad hoc signature (no certificate) as a whole, which is what
# an Apple Silicon Mac wants before it'll launch a downloaded app at all.
gui-stage: version wasm gui/node_modules
	cd gui && $(NPM) run build
	@$(MAKE) -s gui-ship-check
	rm -rf $(GUI_STAGE)
	mkdir -p $(GUI_STAGE)
	cd $(GUI_WEB) && cp -R $(GUI_SHIP) "$(CURDIR)/$(GUI_STAGE)/"

gui-ship-check:
	@missing=$$(grep -oE '(href|src)="/[^"]*"' $(GUI_WEB)/index.html | \
	  sed -E 's#^[a-z]+="/##; s#[/"?].*##' | sort -u | \
	  while read -r f; do \
	    case " $(GUI_SHIP) " in (*" $$f "*) ;; (*) echo "$$f" ;; esac; \
	  done); \
	if [ -n "$$missing" ]; then \
	  echo "$(GUI_WEB)/index.html uses $$missing, but GUI_SHIP only ships $(GUI_SHIP); widen it" >&2; \
	  exit 1; \
	fi

# The front end's already built and staged, so Tauri is told not to build
# it again and to embed the staged copy rather than all of $(GUI_WEB).
# Tauri on Windows is a Windows program, so it gets C:/... rather than
# MSYS2's /c/...
APP_ROOT   := $(if $(filter win32,$(HOST_OS)),$(shell cygpath -m "$(CURDIR)"),$(CURDIR))
APP_CONFIG := {"build":{"beforeBuildCommand":"","frontendDist":"$(APP_ROOT)/$(GUI_STAGE)"}}

app: gui-stage
	cd gui && env -u CC -u CXX npx tauri build $(APP_TARGET_FLAG) --bundles $(APP_BUNDLES) \
	  --config '$(APP_CONFIG)'
ifeq ($(HOST_OS),darwin)
	codesign --force --deep --sign - "$(APP_BUNDLE_DIR)/macos/Wozzle.app"
	@extra=$$(cd "$(APP_BUNDLE_DIR)/macos/Wozzle.app" && find . -type f | sed 's#^\./##' | \
	  while read -r f; do \
	    case " $(APP_CONTENTS) " in (*" $$f "*) ;; (*) echo "$$f" ;; esac; \
	  done); \
	if [ -n "$$extra" ]; then \
	  echo "Wozzle.app holds files that aren't in APP_CONTENTS: $$extra" >&2; \
	  exit 1; \
	fi
	@mkdir -p $(GUI_OUT)
	rm -rf "$(GUI_OUT)/Wozzle.app"
	ditto "$(APP_BUNDLE_DIR)/macos/Wozzle.app" "$(GUI_OUT)/Wozzle.app"
endif

ifeq ($(HOST_OS),darwin)
run: app
	open "$(GUI_OUT)/Wozzle.app"
else
run: app
	"$(GUI_BUILD)/target/$(if $(APP_TARGET),$(APP_TARGET)/)release/wozzle-gui"
endif

# tauri dev builds a debug copy of the Rust side and serves the front end
# from Vite, so an edit to gui/src shows up without a rebuild
dev: version wasm gui/node_modules
	cd gui && npx tauri dev

dist: version rel-tools rel-app

rel-tools: tools
	@mkdir -p $(REL_OUT)
	rm -rf "$(REL_BUILD)/$(TOOLS_PKG)"
	mkdir -p "$(REL_BUILD)/$(TOOLS_PKG)"
	cp $(TOOLS_SHIP) "$(REL_BUILD)/$(TOOLS_PKG)/"
ifeq ($(HOST_OS),win32)
	rm -f "$(CURDIR)/$(REL_OUT)/$(TOOLS_PKG).zip"
	cd $(REL_BUILD) && zip -qr "$(CURDIR)/$(REL_OUT)/$(TOOLS_PKG).zip" "$(TOOLS_PKG)"
else
	tar -C $(REL_BUILD) -czf "$(REL_OUT)/$(TOOLS_PKG).tar.gz" "$(TOOLS_PKG)"
endif

# ditto keeps the Mac bundle's symlinks and signature intact. On Linux
# and Windows, Tauri's own packages are already single files.
rel-app: app
	@mkdir -p $(GUI_OUT)
ifeq ($(HOST_OS),darwin)
	rm -f "$(GUI_OUT)/$(APP_PKG).zip"
	ditto -c -k --keepParent "$(GUI_OUT)/Wozzle.app" "$(GUI_OUT)/$(APP_PKG).zip"
else ifeq ($(HOST_OS),win32)
	cp $(APP_BUNDLE_DIR)/nsis/*-setup.exe "$(GUI_OUT)/$(APP_PKG)-setup.exe"
else
	for f in $(APP_BUNDLE_DIR)/appimage/*.AppImage $(APP_BUNDLE_DIR)/deb/*.deb; do \
	  [ -f "$$f" ] && cp "$$f" "$(GUI_OUT)/$(APP_PKG).$${f##*.}"; \
	done; true
endif

release:
	sh tools/release.sh check
	rm -rf $(REL_OUT) $(GUI_OUT)
	$(MAKE) wasm
	$(MAKE) test dist
	sh tools/release.sh

# DO NOT DELETE
