# vapoursynth-mvc

A [VapourSynth](https://www.vapoursynth.com/) source plugin for **H.264/AVC and
MVC (3D)** video, built on the [edge264-mvc](https://github.com/jens-duttke/edge264-mvc)
software decoder.

Its reason to exist: there is **no dependency-free MVC (3D Blu-ray) source for
VapourSynth on Linux**. FFmpeg drops the MVC dependent view entirely, and the
existing options (DGMVCsourceVS, FRIMSource) are Windows-only and pull in
`libmfxsw64.dll`. `vapoursynth-mvc` decodes both views in pure, self-contained C:
edge264 is statically linked, so the built plugin is a single `.so`/`.dll` with
no external runtime dependency.

Intended workflow: decode a 3D Blu-ray's MVC stream in VapourSynth, frame-serve
the two views (e.g. as a top-and-bottom clip), interpolate (e.g. with
[vs-rife](https://github.com/HolyWu/vs-rife)) and re-encode - fully on Linux.

## Status

Working and verified end-to-end.

- [x] **Decode core** (`src/mvcsource.c`) - open, index, seek, and per-view /
  stacked frame assembly on top of edge264. **Verified bit-exact** against the
  edge264 reference output, with correct frame count, dimensions and
  frame-accurate seeking (see `tests/coretest.c`).
- [x] **VapourSynth glue** (`src/plugin.c`) - the API4 filter that exposes
  `mvc.Source`. Covered by `tests/mockhost.c` (drives the built plugin
  through the real API4) and confirmed **bit-exact in a real VapourSynth
  runtime** (Core R77): the plugin's frame output matches the edge264 reference.
- [ ] VUI frame-rate auto-detection; on-disk index cache for fast reopening.

## Usage

```python
import vapoursynth as vs
core = vs.core
core.std.LoadPlugin("/path/to/libvsmvc.so")

# base + dependent views stacked top-and-bottom (full resolution per eye)
clip = core.mvc.Source(r"movie.264", stack="tab")

# ... interpolate to 60000/1001 with vs-rife, then output/encode ...
```

`stack` selects the layout: `"base"` (left/2D), `"right"`, `"tab"`
(top-and-bottom) or `"sbs"` (side-by-side). On a 2D stream the stacked modes
fall back to the single view.

`swaplr=1` swaps the two views in any layout (base <-> dependent), so a stream
authored right-eye-first can be flipped to left-eye-first without re-authoring.

## Building

Requires a C compiler and an [edge264-mvc](https://github.com/jens-duttke/edge264-mvc)
source tree (pin a release tag for reproducible builds). The VapourSynth SDK
headers are vendored under `include/`. CI builds and bit-exact-verifies the plugin
against edge264-mvc `v2026.07.05`.

```sh
make EDGE264_SRC=/path/to/edge264-mvc          # builds the decode core + tests
./coretest movie.264 2                          # 0=base 1=right 2=tab 3=sbs
```

## Layout

- `src/mvcsource.{c,h}` - the VapourSynth-independent decode core (all the
  logic; unit-testable without a VapourSynth runtime).
- `tests/coretest.c` - standalone verification (info, sequential decode,
  seek == sequential, raw-frame dump for cross-checking).
- `include/` - vendored VapourSynth API4 SDK headers.

## Related projects

- **[Oku3D](https://oku3d.com)** - *watch everything in 3D.* A real-time 3D
  media player from the same author, built on the same
  [edge264-mvc](https://github.com/jens-duttke/edge264-mvc) decoder for native
  H.264 MVC (3D Blu-ray) playback - and it turns *any* other 2D video or photo
  into stereoscopic 3D on the fly with AI depth estimation, so your whole
  library plays in 3D, not just MVC discs.

## License

BSD-3-Clause (see [LICENSE](LICENSE)). Statically links edge264-mvc (BSD-3-Clause) and
builds against the VapourSynth SDK headers (LGPL-2.1-or-later).
