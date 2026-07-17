# mvc-source - VapourSynth and AviSynth+ source plugins (over one reusable decode
# core) for H.264/AVC and MVC (3D) built on the edge264-mvc decoder.
#
# edge264 is statically linked, so each resulting plugin (libvsmvc.so for
# VapourSynth, libavsmvc.so for AviSynth+) has no external runtime dependency -
# exactly what a "no dependencies on Linux" MVC source needs.
#
# Parameters:
#   EDGE264_SRC  path to an edge264-mvc source tree (default: ../edge264).
#                Pin a release tag for reproducible builds.
#   TEST_FILE    an .264/.h264 sample used by `make check`.
#   CC, CFLAGS   compiler and flags.

CC        ?= cc
# MinGW dlltool for the Windows cross-build's import library (see $(AVS_DLL)).
DLLTOOL   ?= x86_64-w64-mingw32-dlltool
# Preference flags only (a user/CI CFLAGS override replaces these). The
# semantically-required -fPIC for the shared object lives in the $(PLUGIN) rule,
# so an override cannot silently drop it. Everything here is C - both plugins and
# the tests - so no C++ compiler is needed.
CFLAGS    ?= -O2 -std=gnu11 -Wall -Wextra
EDGE264_SRC ?= ../edge264

EDGE264_A := $(EDGE264_SRC)/libedge264.a
# Extra args passed to the edge264 sub-make; set OS=windows (+ a MinGW CC) for a
# Windows cross-build, e.g. EDGE264_MAKE="OS=windows CC=x86_64-w64-mingw32-gcc".
EDGE264_MAKE ?=
INCLUDES  := -Isrc -I$(EDGE264_SRC) -Iinclude
PLUGIN    := libvsmvc.so
AVS_PLUGIN := libavsmvc.so
AVS_DLL   := libavsmvc.dll
VS_DLL    := libvsmvc.dll

.PHONY: all clean check check-bitexact check-avs
all: coretest mockhost seektest enomemtest allocfailtest poctest stalltest cachetest budgettest avsnulltest $(PLUGIN) $(AVS_PLUGIN)

# edge264 as a self-contained static library. FORCE so the sub-make always runs
# and decides up-to-dateness itself: a bare file target with no prerequisites is
# treated as up to date once it exists, which would link a stale .a after the
# edge264 tree changes (a git pull / edit). The sub-make is fast when idle.
$(EDGE264_A): FORCE
	$(MAKE) -C $(EDGE264_SRC) STATIC=yes BUILDTEST=no $(EDGE264_MAKE)

FORCE: ;

# The VapourSynth plugin: only VapourSynthPluginInit2 is exported; edge264's
# symbols are hidden so the plugin cannot clash with another that links it too.
$(PLUGIN): src/plugin.c src/mvcsource.c src/mvcsource.h $(EDGE264_A)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden $(INCLUDES) -shared \
	    src/plugin.c src/mvcsource.c $(EDGE264_A) -pthread \
	    -Wl,--exclude-libs,ALL -o $@

# The AviSynth+ plugin: the same decode core plus C-interface glue. Both are C,
# so a single $(CC) invocation builds the shared object. The C interface (not the
# C++ API) is used so the identical source cross-compiles to a Windows .dll that
# loads in the MSVC-built AviSynth+. Only avisynth_c_plugin_init is exported (it
# carries visibility=default in the source; everything else is hidden) and
# edge264's statically-linked symbols are hidden.
# -Wno-missing-field-initializers: the vendored avisynth_c.h defines
# `avs_void = {'v'}` (first field only) by design; silence that upstream warning
# here rather than patch a vendored file.
$(AVS_PLUGIN): src/avisynth_plugin.c src/mvcsource.c src/mvcsource.h $(EDGE264_A)
	$(CC) $(CFLAGS) -Wno-missing-field-initializers -fPIC -fvisibility=hidden $(INCLUDES) -shared \
	    src/avisynth_plugin.c src/mvcsource.c $(EDGE264_A) -pthread \
	    -Wl,--exclude-libs,ALL -o $@

# Windows cross-build of the AviSynth+ plugin (.dll). Invoke with the MinGW-w64
# toolchain and a Windows edge264 build, e.g.:
#   make libavsmvc.dll CC=x86_64-w64-mingw32-gcc DLLTOOL=x86_64-w64-mingw32-dlltool \
#        EDGE264_SRC=/path/to/edge264-win \
#        EDGE264_MAKE="OS=windows CC=x86_64-w64-mingw32-gcc"
# The C interface is used precisely so this MinGW-built .dll loads in the official
# MSVC-built AviSynth+ (the C++ vtable ABI does not match across GCC/MSVC). The
# import library for AviSynth's C API is generated from a .def with dlltool, so no
# AviSynth.dll/.lib is needed at build time; the .dll's import table references
# AviSynth.dll, which the host resolves at load time. __declspec(dllexport) on
# avisynth_c_plugin_init exports only that entry; edge264 stays hidden. -static*
# link libgcc/winpthread in, so the .dll depends only on KERNEL32/msvcrt/AviSynth.
$(AVS_DLL): src/avisynth_plugin.c src/mvcsource.c src/mvcsource.h src/avisynth_win.def $(EDGE264_A)
	$(DLLTOOL) -d src/avisynth_win.def -D AviSynth.dll -l avisynth.dll.a
	$(CC) $(CFLAGS) -Wno-missing-field-initializers $(INCLUDES) -shared \
	    src/avisynth_plugin.c src/mvcsource.c $(EDGE264_A) avisynth.dll.a \
	    -static -static-libgcc -pthread -o $@
	rm -f avisynth.dll.a

# Windows cross-build of the VapourSynth plugin (.dll). Same MinGW invocation as
# libavsmvc.dll (CC/EDGE264_SRC/EDGE264_MAKE). Unlike the AviSynth DLL the plugin
# imports nothing from the host - VapourSynth passes its API into
# VapourSynthPluginInit2 - so there is no import .def / dlltool step; the export
# comes from VS_EXTERNAL_API's __declspec(dllexport) on Windows. -static* fold
# libgcc/winpthread in, so the .dll depends only on KERNEL32/msvcrt.
$(VS_DLL): src/plugin.c src/mvcsource.c src/mvcsource.h $(EDGE264_A)
	$(CC) $(CFLAGS) $(INCLUDES) -shared \
	    src/plugin.c src/mvcsource.c $(EDGE264_A) \
	    -static -static-libgcc -pthread -o $@

# Standalone decode-core test (no VapourSynth needed).
coretest: tests/coretest.c src/mvcsource.c src/mvcsource.h $(EDGE264_A)
	$(CC) $(CFLAGS) $(INCLUDES) tests/coretest.c src/mvcsource.c $(EDGE264_A) -pthread -o $@

# Windows cross-build of the core test (statically linked PE), for exercising the
# Windows I/O shim under Wine (CI) or Windows without an AviSynth+ install. Built
# with the same MinGW invocation as libavsmvc.dll (CC/EDGE264_SRC/EDGE264_MAKE).
coretest.exe: tests/coretest.c src/mvcsource.c src/mvcsource.h $(EDGE264_A)
	$(CC) $(CFLAGS) $(INCLUDES) tests/coretest.c src/mvcsource.c $(EDGE264_A) -static -pthread -o $@

# Mock VapourSynth API4 host that dlopens the plugin and drives it end-to-end.
mockhost: tests/mockhost.c
	$(CC) $(CFLAGS) $(INCLUDES) tests/mockhost.c -ldl -o $@

# End-to-end AviSynth+ host: loads the built plugin through a *real* AviSynth+
# runtime via its C API (the counterpart to the VapourSynth vspipe run). Needs a
# libavisynth to link/run against, so it is exercised by `make check-avs` and the
# avisynth CI job - not by the AviSynth-free `make check`. Override AVS_LIBS to
# point at an uninstalled build, e.g.
#   make check-avs AVS_LIBS="-L$HOME/avsplus/build -lavisynth" \
#        LD_LIBRARY_PATH=$HOME/avsplus/build TEST_FILE=movie.264
# -Wno-missing-field-initializers: the vendored avisynth_c.h defines
# `avs_void = {'v'}` (first field only) by design; the warning is the upstream
# header's, not ours, so silence it just here rather than patch a vendored file.
AVS_LIBS ?= -lavisynth
avshost: tests/avshost.c
	$(CC) $(CFLAGS) -Wno-missing-field-initializers $(INCLUDES) tests/avshost.c $(AVS_LIBS) -o $@

# The same host under AddressSanitizer/LeakSanitizer, for the createVideoFilter-
# failure cleanup check (the plugin's leak on that path is only observable here).
mockhost-asan: tests/mockhost.c
	$(CC) $(CFLAGS) -fsanitize=address -g $(INCLUDES) tests/mockhost.c -ldl -o $@

# Random-access seek regression test on headerless-GOP / AUD-headed topologies
# (uses a committed fixture, so it needs no TEST_FILE and no encoder).
seektest: tests/seektest.c src/mvcsource.c src/mvcsource.h src/h264poc.h $(EDGE264_A)
	$(CC) $(CFLAGS) $(INCLUDES) tests/seektest.c src/mvcsource.c $(EDGE264_A) -pthread -o $@

# ENOMEM-handling test: --wrap intercepts edge264_decode_NAL to inject ENOMEM on
# a chosen slice, so the caller loop's data-loss handling is checked without real
# memory pressure. Committed fixture, no TEST_FILE.
enomemtest: tests/enomemtest.c src/mvcsource.c src/mvcsource.h $(EDGE264_A)
	$(CC) $(CFLAGS) $(INCLUDES) -Wl,--wrap=edge264_decode_NAL \
	    tests/enomemtest.c src/mvcsource.c $(EDGE264_A) -pthread -o $@

# Decoder-allocation-failure test: --wrap intercepts edge264_alloc to fail the
# seek-time reallocation, checking the alloc-failure error and retry recovery.
allocfailtest: tests/allocfailtest.c src/mvcsource.c src/mvcsource.h $(EDGE264_A)
	$(CC) $(CFLAGS) $(INCLUDES) -Wl,--wrap=edge264_alloc \
	    tests/allocfailtest.c src/mvcsource.c $(EDGE264_A) -pthread -o $@

# Display-order tripwire test: --wrap intercepts edge264_get_frame to force a
# non-monotone DisplayPoc, checking the output-count divergence is caught.
poctest: tests/poctest.c src/mvcsource.c src/mvcsource.h $(EDGE264_A)
	$(CC) $(CFLAGS) $(INCLUDES) -Wl,--wrap=edge264_get_frame \
	    tests/poctest.c src/mvcsource.c $(EDGE264_A) -pthread -o $@

# ENOBUFS-stall test: --wrap intercepts both edge264 entry points to hold the
# decoder in a persistent-ENOBUFS / no-frame state (a DPB stuck full of incomplete
# pictures), checking the caller loop's progress guard fails loudly instead of
# spinning forever. Committed fixture, no TEST_FILE.
stalltest: tests/stalltest.c src/mvcsource.c src/mvcsource.h $(EDGE264_A)
	$(CC) $(CFLAGS) $(INCLUDES) -Wl,--wrap=edge264_decode_NAL -Wl,--wrap=edge264_get_frame \
	    tests/stalltest.c src/mvcsource.c $(EDGE264_A) -pthread -o $@

# On-disk index-cache regression: cache hit == fresh scan, and a corrupt / stale
# sidecar is ignored rather than trusted. Committed fixture, no TEST_FILE.
cachetest: tests/cachetest.c src/mvcsource.c src/mvcsource.h $(EDGE264_A)
	$(CC) $(CFLAGS) $(INCLUDES) tests/cachetest.c src/mvcsource.c $(EDGE264_A) -pthread -o $@

# AviSynth+ get_frame callback regression: a failed host frame allocation
# (avs_new_video_frame_a -> NULL) must be reported as a catchable error, never
# dereferenced. The AviSynth C host functions are stubbed at link time (POSIX
# resolves them as plain extern-"C" symbols), so no libavisynth is needed. The
# glue source is #included, hence -Wno-missing-field-initializers for the
# vendored header's `avs_void = {'v'}`. Committed fixture, no TEST_FILE.
avsnulltest: tests/avsnulltest.c src/avisynth_plugin.c src/mvcsource.c src/mvcsource.h $(EDGE264_A)
	$(CC) $(CFLAGS) -Wno-missing-field-initializers $(INCLUDES) \
	    tests/avsnulltest.c src/mvcsource.c $(EDGE264_A) -pthread -o $@

# Cache-budget sizing: the MiB->byte budget must never wrap (guards the 32-bit
# size_t `mb << 20` overflow the clamp permits, mb up to 16384). Pure arithmetic
# over the shared cache_budget.h - no edge264 - so it also builds as a 32-bit
# binary (see `check`), where the wrap actually bites. Committed, no TEST_FILE.
budgettest: tests/budgettest.c src/cache_budget.h
	$(CC) $(CFLAGS) $(INCLUDES) tests/budgettest.c -o $@

check: coretest mockhost mockhost-asan seektest enomemtest allocfailtest poctest stalltest cachetest budgettest avsnulltest $(PLUGIN) $(AVS_PLUGIN)
	@echo "== makefile behaviour (edge264 sub-make always delegated) =="
	sh tests/mkcheck.sh "$(EDGE264_SRC)"
	@echo "== seek regression (headerless-GOP / AUD-headed / open-GOP, committed fixtures) =="
	./seektest tests/fixtures/base_multigop.264 tests/fixtures/base_opengop.264
	@echo "== ENOMEM handling (injected via --wrap) =="
	./enomemtest tests/fixtures/base_multigop.264
	@echo "== decoder alloc-failure handling (injected via --wrap) =="
	./allocfailtest tests/fixtures/base_multigop.264
	@echo "== display-order tripwire (injected via --wrap) =="
	./poctest tests/fixtures/base_multigop.264
	@echo "== ENOBUFS-stall progress guard (injected via --wrap) =="
	./stalltest tests/fixtures/base_multigop.264
	@echo "== on-disk index cache (miss/hit + corrupt/stale, committed fixture) =="
	./cachetest tests/fixtures/base_multigop.264
	@echo "== AviSynth+ get_frame OOM handling (stubbed host, committed fixture) =="
	./avsnulltest tests/fixtures/base_multigop.264
	@echo "== cache-budget sizing (64-bit, + 32-bit if -m32 is available) =="
	./budgettest
	@if printf 'int main(void){return 0;}' | $(CC) -m32 -x c - -o /dev/null 2>/dev/null; then \
	    echo "  building 32-bit variant (where the size_t wrap bites):"; \
	    $(CC) $(CFLAGS) $(INCLUDES) -m32 tests/budgettest.c -o budgettest32 && ./budgettest32; \
	else echo "  (32-bit build unavailable: no -m32 / gcc-multilib; skipping)"; fi
	@echo "== test.vpy argument guards (stub VapourSynth) =="
	python3 tests/vpycheck.py
ifndef TEST_FILE
	@echo "set TEST_FILE=<file.264> to run the suite, e.g. make check TEST_FILE=movie.264"
	@exit 1
endif
	@echo "== decode core =="
	./coretest "$(TEST_FILE)" 0
	./coretest "$(TEST_FILE)" 1
	./coretest "$(TEST_FILE)" 2
	./coretest "$(TEST_FILE)" 3
	./coretest "$(TEST_FILE)" 4
	@echo "== coretest dump-mode error handling =="
	sh tests/dumperr.sh ./coretest "$(TEST_FILE)"
	@echo "== plugin (mock VapourSynth host) =="
	./mockhost ./$(PLUGIN) "$(TEST_FILE)" tab
	@echo "== plugin failure-path leak check (LeakSanitizer) =="
	ASAN_OPTIONS=detect_leaks=1 ./mockhost-asan ./$(PLUGIN) "$(TEST_FILE)" __leakcheck__

# Bit-exact cross-check: the decode core's frame must match edge264's own decode
# of the same frame. The reference is regenerated from edge264 on the fly (no
# committed MD5, which would rot across edge264 versions / test files). Needs
# edge264_test (built here) and ffmpeg. The CI additionally runs the equivalent
# check through a real vspipe.
BITEXACT_FRAME ?= 5
check-bitexact: coretest
ifndef TEST_FILE
	@echo "set TEST_FILE=<file.264> to run the bit-exact check, e.g. make check-bitexact TEST_FILE=movie.264"
	@exit 1
endif
	$(MAKE) -C $(EDGE264_SRC) STATIC=yes BUILDTEST=no edge264_test
	@set -e; \
	  core=$$(mktemp); ref=$$(mktemp); \
	  trap 'rm -f "$$core" "$$ref"' EXIT; \
	  ./coretest "$(TEST_FILE)" 0 $(BITEXACT_FRAME) "$$core"; \
	  sh tests/mkref.sh "$(EDGE264_SRC)/edge264_test" "$(TEST_FILE)" $(BITEXACT_FRAME) "$$ref"; \
	  a=$$(md5sum < "$$core"); b=$$(md5sum < "$$ref"); \
	  echo "core=$$a"; echo "ref =$$b"; \
	  [ "$$a" = "$$b" ] && echo "bit-exact vs edge264: OK" || { echo "MISMATCH"; exit 1; }

# Full end-to-end AviSynth+ check: load the built plugin through a real AviSynth+
# runtime (properties, layouts, error paths), then bit-exact vs an edge264
# reference regenerated on the fly - the same shared extraction check-bitexact
# uses. Needs a libavisynth (see AVS_LIBS above), ffmpeg, and TEST_FILE.
check-avs: $(AVS_PLUGIN) avshost
ifndef TEST_FILE
	@echo "set TEST_FILE=<file.264> to run the AviSynth+ check, e.g. make check-avs TEST_FILE=movie.264"
	@exit 1
endif
	@echo "== AviSynth+ end-to-end (real runtime: properties, layouts, error paths) =="
	./avshost ./$(AVS_PLUGIN) "$(TEST_FILE)" tab
	@echo "== bit-exact vs edge264 through AviSynth+ =="
	$(MAKE) -C $(EDGE264_SRC) STATIC=yes BUILDTEST=no edge264_test
	@set -e; \
	  avs=$$(mktemp); ref=$$(mktemp); \
	  trap 'rm -f "$$avs" "$$ref"' EXIT; \
	  ./avshost ./$(AVS_PLUGIN) "$(TEST_FILE)" base $(BITEXACT_FRAME) "$$avs"; \
	  sh tests/mkref.sh "$(EDGE264_SRC)/edge264_test" "$(TEST_FILE)" $(BITEXACT_FRAME) "$$ref"; \
	  a=$$(md5sum < "$$avs"); b=$$(md5sum < "$$ref"); \
	  echo "avs (AviSynth+): $$a"; echo "edge264 ref:     $$b"; \
	  [ "$$a" = "$$b" ] && echo "bit-exact vs edge264: OK" || { echo "MISMATCH"; exit 1; }

clean:
	rm -f coretest mockhost mockhost-asan seektest enomemtest allocfailtest poctest stalltest cachetest budgettest budgettest32 avsnulltest avshost \
	    $(PLUGIN) $(AVS_PLUGIN) $(AVS_DLL) $(VS_DLL) *.exe src/*.o
