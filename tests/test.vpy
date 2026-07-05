# vapoursynth-mvc smoke test for a real VapourSynth runtime.
#
# vspipe emits raw planar frames on stdout; add `-c y4m` so ffmpeg can probe the
# stream (without it, `ffmpeg -i -` fails with "Invalid data found"). Inspect or
# compare like:
#   vspipe -c y4m --arg plugin=./libvsmvc.so --arg source=movie.264 --arg stack=tab \
#          tests/test.vpy - | ffmpeg -i - -frames:v 1 -f framemd5 -
#
# Args (via vspipe --arg): plugin (path to the .so), source (path to the .264),
# stack (base|right|tab|sbs|alt), swaplr (0|1, swap the two views).
# Capture the args vspipe injected as globals *before* our own imports add names,
# so a mistyped --arg name (e.g. stak=sbs, which would otherwise vanish into the
# script default) can be reported instead of silently ignored.
_injected_args = {k for k in globals() if not k.startswith("_")}

import sys
import vapoursynth as vs

core = vs.core

_KNOWN_ARGS = {"plugin", "source", "stack", "swaplr"}
_STACKS = ("base", "right", "tab", "sbs", "alt")

plugin = globals().get("plugin", "./libvsmvc.so")
source = globals().get("source")
stack = globals().get("stack", "tab")
swaplr = globals().get("swaplr")
if not source:
    raise ValueError("pass --arg source=<file.264>")
if stack not in _STACKS:
    raise ValueError(f"unknown stack {stack!r} (use {'/'.join(_STACKS)})")
_unexpected = _injected_args - _KNOWN_ARGS
if _unexpected:
    print("vapoursynth-mvc: warning: ignoring unexpected --arg(s): "
          + ", ".join(sorted(_unexpected)), file=sys.stderr)

core.std.LoadPlugin(plugin)
# swaplr is only forwarded when given, so the default (off) is exercised too.
_kwargs = {"swaplr": int(swaplr)} if swaplr is not None else {}
clip = core.mvc.Source(source=source, stack=stack, **_kwargs)

print(f"vapoursynth-mvc: {clip.width}x{clip.height}  {clip.num_frames} frames  "
      f"{clip.fps_num}/{clip.fps_den} fps  format={clip.format.name}", file=sys.stderr)

clip.set_output(0)
