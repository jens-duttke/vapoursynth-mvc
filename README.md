# mvc-source - AviSynth+ & VapourSynth MVC (3D Blu-ray) source plugins

Source plugins for **H.264/AVC and MVC (3D Blu-ray, stereo)** video, built on
the [edge264-mvc](https://github.com/jens-duttke/edge264-mvc) software decoder.
One shared decode core drives two thin host plugins:

- **VapourSynth** (API4) - `core.mvc.Source(...)`, built as `libvsmvc.so`
- **AviSynth+** - `MVCSource(...)`, built as `libavsmvc.so`

Its reason to exist: there is **no dependency-free MVC (3D Blu-ray) source for
these frameservers on Linux**. FFmpeg drops the MVC dependent view entirely, and
the existing options (DGMVCsourceVS, FRIMSource) are Windows-only and pull in
`libmfxsw64.dll`. `mvc-source` decodes both views in pure, self-contained C:
edge264 is statically linked, so each built plugin is a single shared library
(`.so` on Linux, `.dll` on Windows) with no external runtime dependency.

Intended workflow: decode a 3D Blu-ray's MVC stream, frame-serve the two views
(e.g. as a top-and-bottom clip), interpolate (e.g. with
[vs-rife](https://github.com/HolyWu/vs-rife)) and re-encode - fully on Linux.

## Status

Working and verified end-to-end, **bit-exact against the edge264 reference on
both hosts** (correct frame count, dimensions, frame-accurate seeking).

- [x] **Decode core** (`src/mvcsource.c`) - open, index, seek, and per-view /
  stacked frame assembly on top of edge264. Host-independent; unit-tested
  without any frameserver runtime (`tests/coretest.c`).
- [x] **VapourSynth glue** (`src/plugin.c`) - the API4 filter `mvc.Source`.
  Covered by `tests/mockhost.c` (drives the built plugin through the real API4)
  and confirmed bit-exact in a real VapourSynth runtime (Core R77).
- [x] **AviSynth+ glue** (`src/avisynth_plugin.c`) - the `MVCSource` filter,
  written to the AviSynth **C interface** (not the C++ API) so a MinGW-cross-built
  `.dll` loads in the official MSVC-built Windows AviSynth+; the C++ vtable ABI
  does not match across GCC and MSVC. Loaded through a real AviSynth+ by
  `tests/avshost.c` and confirmed bit-exact (AviSynth+ 3.7.3).
- [x] **Windows build** - the AviSynth+ plugin cross-compiles with MinGW-w64 to a
  self-contained `.dll` (only `avisynth_c_plugin_init` exported, edge264 hidden;
  imports only `KERNEL32`/`msvcrt`/`AviSynth.dll`). Verified **bit-exact in the
  official MSVC-built Windows AviSynth+ 3.7.3** end-to-end - both the Win32
  file-mapping I/O path and the C-interface glue.
- [x] **On-disk index cache** - a reopen of an unchanged stream skips the
  full-file NAL scan (which must otherwise read the whole file to count frames):
  the scan result is cached in a sidecar `<source>.mvcidx` next to the source,
  keyed on the source's size + last-write time. Measured on a 1.4 GB MVC stream:
  reopen dropped from a full-file scan to ~25 ms (the sidecar plus the first GOP),
  bit-identical output. Writing is best-effort (a read-only directory simply
  means no cache, never a failed open); a stale or corrupt sidecar is detected
  and a fresh scan runs.
- [x] **Random-access seeking + decoded-frame cache** - a seek decodes forward
  from the nearest preceding IDR (the only guaranteed H.264 random-access point:
  it resets the DPB, so a cold decode from it is bit-exact). A bounded
  decoded-frame cache (`cachesize`, default 512 MiB, modelled on BestSource's
  `cachesize`) holds independent frame copies that survive a decoder reset, so
  backward / repeat / `Reverse()` access is served from RAM (~0.2 ms) instead of
  re-decoding: AviSynth `Reverse()` went from never displaying a frame to
  real-time playback, bit-exact. The cache also spans a long GOP, so a backward
  pass triggers one cold re-decode per cache-window rather than one per frame -
  the dominant cost of `Reverse()`. Frame-accurate and bit-exact against a
  sequential decode across the full JVT conformance corpus and on real 3D
  Blu-rays. Some 3D Blu-rays run 600+ frames between IDRs, and a cold seek into
  such a GOP re-decodes from its IDR; multithreaded decode (`threads`, default
  `-1`) makes that re-decode several times faster and a larger `cachesize` spans
  the whole GOP so subsequent nearby seeks are cache hits. Measured on a real
  multi-thousand-frame 3D Blu-ray, a backward pass near the end sped up roughly
  20x at the defaults over the previous single-thread / small-cache behaviour, and
  more with a GOP-spanning `cachesize`.
- [ ] VUI frame-rate auto-detection.

## Usage

Both plugins expose the same layouts and options; only the call syntax differs
per host.

`stack` selects the layout: `"base"` (left/2D), `"right"`, `"tab"`
(top-and-bottom), `"sbs"` (side-by-side) or `"alt"` (alternating frames: base,
dependent, base, dependent ... - twice the frames at twice the frame rate). On a
2D stream the stacked/alternating modes fall back to the single view. `swaplr`
swaps the two views in any layout (base <-> dependent), so a stream authored
right-eye-first can be flipped without re-authoring.

### VapourSynth

```python
import vapoursynth as vs
core = vs.core
core.std.LoadPlugin("/path/to/libvsmvc.so")

# base + dependent views stacked top-and-bottom (full resolution per eye)
clip = core.mvc.Source(r"movie.264", stack="tab")

# ... interpolate to 60000/1001 with vs-rife, then output/encode ...
```

Signature: `core.mvc.Source(source, stack="base", threads=-1, fpsnum=..., fpsden=..., swaplr=0, cachesize=512)`.
`threads` is edge264's internal decode parallelism (`-1` auto-detect cores, `0`
single-thread, or an explicit count); `cachesize` is the decoded-frame cache
ceiling in MiB (raise it for smoother backward / `Reverse()` seeking on
long-GOP streams, lower it to save memory).

### AviSynth+

```avisynth
LoadPlugin("/path/to/libavsmvc.so")

# base + dependent views stacked top-and-bottom (full resolution per eye)
MVCSource("movie.264", stack="tab")
```

Signature: `MVCSource(source, stack="base", threads=-1, fpsnum=..., fpsden=..., swaplr=false, cachesize=512)`.
`threads` and `cachesize` behave as for the VapourSynth signature above.

`fpsnum`/`fpsden` must be given together (edge264's public API does not expose
the VUI rate); the default is 24000/1001.

## Building

Requires a C compiler and an
[edge264-mvc](https://github.com/jens-duttke/edge264-mvc) source tree (pin a
release tag for reproducible builds). The VapourSynth API4 and AviSynth+ SDK
headers are vendored under `include/`, so no host install is needed to build.

```sh
make EDGE264_SRC=/path/to/edge264-mvc     # builds both plugins + the core tests
./coretest movie.264 2                     # 0=base 1=right 2=tab 3=sbs 4=alt
```

Individual targets: `make libvsmvc.so` (VapourSynth), `make libavsmvc.so`
(AviSynth+). Tested on Linux; CI builds and bit-exact-verifies both plugins
against edge264-mvc `v2026.07.13` and AviSynth+ `v3.7.3`.

The **released** binaries target a portable `x86-64-v2` floor (SSE4.2, runs on
~2009-and-later CPUs), with edge264's runtime dispatch lifting the parser to AVX2
(`x86-64-v3`) where the CPU supports it - so one binary runs across CPU
generations at full speed on modern hardware. A plain local `make` instead builds
for the build machine (`-march=native`); to reproduce the portable release ISA
pass `EDGE264_MAKE="VARIANTS=x86-64-v2,x86-64-v3 CFLAGS=-march=x86-64-v2"`.

### Windows cross-build (MinGW-w64)

The AviSynth+ plugin cross-compiles from Linux to a self-contained Windows `.dll`
that loads in the official MSVC-built AviSynth+ - it uses the AviSynth C interface
precisely so the GCC/MSVC C++ ABI mismatch does not apply. Needs
`gcc-mingw-w64-x86-64` (and its `binutils` for `dlltool`); no `AviSynth.dll` is
needed at build time - the import library is generated from
`src/avisynth_win.def`.

```sh
# edge264 for Windows - a separate tree keeps its objects apart from a Linux build:
cp -r ../edge264 ../edge264-win && make -C ../edge264-win clean
make -C ../edge264-win OS=windows CC=x86_64-w64-mingw32-gcc STATIC=yes BUILDTEST=no
# the plugin DLL:
make libavsmvc.dll CC=x86_64-w64-mingw32-gcc DLLTOOL=x86_64-w64-mingw32-dlltool \
    EDGE264_SRC=../edge264-win EDGE264_MAKE="OS=windows CC=x86_64-w64-mingw32-gcc"
```

The `windows-cross` CI job runs exactly this, verifies the DLL's exports, and runs
the core under Wine bit-exact vs an edge264 reference. `make coretest.exe` (same
MinGW flags) builds the standalone core test as a Windows binary. The DLL was
additionally verified bit-exact in a real MSVC-built Windows AviSynth+.

## Testing

```sh
make check TEST_FILE=movie.264       # core (all layouts, seek==sequential) +
                                     # VapourSynth glue via the mock API4 host
make check-bitexact TEST_FILE=movie.264   # core frame md5 == an edge264 reference

# end-to-end through a real AviSynth+ (needs libavisynth + ffmpeg); properties,
# all layouts, error paths, then a bit-exact frame md5 vs edge264:
make check-avs TEST_FILE=movie.264
```

`make check` needs no frameserver install. The real-runtime proofs
(`vspipe` for VapourSynth, `check-avs` for AviSynth+) run in CI.

## Layout

- `src/mvcsource.{c,h}` - the host-independent decode core (all the logic;
  shared by both plugins, unit-testable without a frameserver runtime).
- `src/plugin.c` - VapourSynth API4 glue (`mvc.Source`).
- `src/avisynth_plugin.c` - AviSynth+ C-interface glue (`MVCSource`);
  `src/avisynth_win.def` lists its C-API imports for the Windows cross-build.
- `tests/coretest.c` - standalone core verification (info, sequential decode,
  seek == sequential, raw-frame dump for cross-checking).
- `tests/mockhost.c` - a mock VapourSynth API4 host driving the built plugin.
- `tests/avshost.c` - loads the built AviSynth+ plugin through a real AviSynth+.
- `include/` - vendored VapourSynth API4 and AviSynth+ SDK headers.

## Related projects

- **[Oku3D](https://oku3d.com)** - *watch everything in 3D.* A real-time 3D
  media player from the same author, built on the same
  [edge264-mvc](https://github.com/jens-duttke/edge264-mvc) decoder for native
  H.264 MVC (3D Blu-ray) playback - and it turns *any* other 2D video or photo
  into stereoscopic 3D on the fly with AI depth estimation, so your whole
  library plays in 3D, not just MVC discs.

## License

BSD-3-Clause (see [LICENSE](LICENSE)). Statically links edge264-mvc
(BSD-3-Clause) and builds against vendored SDK headers: the VapourSynth SDK
(LGPL-2.1-or-later) and the AviSynth+ headers (GPL-2.0-or-later, with the
standard exception permitting independent plugins that use only the documented
interfaces - which is exactly how this plugin uses them).
