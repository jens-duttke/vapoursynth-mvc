# vapoursynth-mvc - a VapourSynth source plugin (and reusable decode core) for
# H.264/AVC and MVC (3D) built on the edge264-mvc decoder.
#
# edge264 is statically linked, so the resulting plugin (libvsmvc.so) has no
# external runtime dependency - exactly what a "no dependencies on Linux" MVC
# source needs.
#
# Parameters:
#   EDGE264_SRC  path to an edge264-mvc source tree (default: ../edge264).
#                Pin a release tag for reproducible builds.
#   TEST_FILE    an .264/.h264 sample used by `make check`.
#   CC, CFLAGS   compiler and flags.

CC        ?= cc
# Preference flags only (a user/CI CFLAGS override replaces these). The
# semantically-required -fPIC for the shared object lives in the $(PLUGIN) rule,
# so an override cannot silently drop it.
CFLAGS    ?= -O2 -std=gnu11 -Wall -Wextra
EDGE264_SRC ?= ../edge264

EDGE264_A := $(EDGE264_SRC)/libedge264.a
INCLUDES  := -Isrc -I$(EDGE264_SRC) -Iinclude
PLUGIN    := libvsmvc.so

.PHONY: all clean check check-bitexact
all: coretest mockhost seektest enomemtest allocfailtest poctest $(PLUGIN)

# edge264 as a self-contained static library. FORCE so the sub-make always runs
# and decides up-to-dateness itself: a bare file target with no prerequisites is
# treated as up to date once it exists, which would link a stale .a after the
# edge264 tree changes (a git pull / edit). The sub-make is fast when idle.
$(EDGE264_A): FORCE
	$(MAKE) -C $(EDGE264_SRC) STATIC=yes BUILDTEST=no

FORCE: ;

# The VapourSynth plugin: only VapourSynthPluginInit2 is exported; edge264's
# symbols are hidden so the plugin cannot clash with another that links it too.
$(PLUGIN): src/plugin.c src/mvcsource.c src/mvcsource.h $(EDGE264_A)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden $(INCLUDES) -shared \
	    src/plugin.c src/mvcsource.c $(EDGE264_A) -pthread \
	    -Wl,--exclude-libs,ALL -o $@

# Standalone decode-core test (no VapourSynth needed).
coretest: tests/coretest.c src/mvcsource.c src/mvcsource.h $(EDGE264_A)
	$(CC) $(CFLAGS) $(INCLUDES) tests/coretest.c src/mvcsource.c $(EDGE264_A) -pthread -o $@

# Mock VapourSynth API4 host that dlopens the plugin and drives it end-to-end.
mockhost: tests/mockhost.c
	$(CC) $(CFLAGS) $(INCLUDES) tests/mockhost.c -ldl -o $@

# The same host under AddressSanitizer/LeakSanitizer, for the createVideoFilter-
# failure cleanup check (the plugin's leak on that path is only observable here).
mockhost-asan: tests/mockhost.c
	$(CC) $(CFLAGS) -fsanitize=address -g $(INCLUDES) tests/mockhost.c -ldl -o $@

# Random-access seek regression test on headerless-GOP / AUD-headed topologies
# (uses a committed fixture, so it needs no TEST_FILE and no encoder).
seektest: tests/seektest.c src/mvcsource.c src/mvcsource.h $(EDGE264_A)
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

check: coretest mockhost mockhost-asan seektest enomemtest allocfailtest poctest $(PLUGIN)
	@echo "== makefile behaviour (edge264 sub-make always delegated) =="
	sh tests/mkcheck.sh "$(EDGE264_SRC)"
	@echo "== seek regression (headerless-GOP / AUD-headed, committed fixture) =="
	./seektest tests/fixtures/base_multigop.264
	@echo "== ENOMEM handling (injected via --wrap) =="
	./enomemtest tests/fixtures/base_multigop.264
	@echo "== decoder alloc-failure handling (injected via --wrap) =="
	./allocfailtest tests/fixtures/base_multigop.264
	@echo "== display-order tripwire (injected via --wrap) =="
	./poctest tests/fixtures/base_multigop.264
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

clean:
	rm -f coretest mockhost mockhost-asan seektest enomemtest allocfailtest poctest $(PLUGIN) src/*.o
