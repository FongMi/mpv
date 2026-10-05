#!/usr/bin/env python3
"""Build VO frame-retention contracts using the production core functions."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    match = re.search(r"(?m)^[\w *]+\b" + re.escape(name) + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise ValueError(f"Missing production function: {name}")
    start = match.start()
    body = match.end() - 1
    end = body + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def generate(root, output):
    vo = (root / "video/out/vo.c").read_text(encoding="utf-8")
    source = ""
    for name in ("read_opts", "forget_frames", "run_reconfig", "vo_queue_frame",
                 "vo_seek_reset", "vo_has_video_frame", "render_frame", "do_redraw"):
        source += function(vo, name)
    playloop = (root / "player/playloop.c").read_text(encoding="utf-8")
    source += function(playloop, "handle_force_window")
    output.write_text(source, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", nargs="+", default=["cc"])
    parser.add_argument("--cflag", action="append", default=[])
    output = parser.add_mutually_exclusive_group()
    output.add_argument("--output", type=Path)
    output.add_argument("--generate", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if args.generate:
        generate(root, args.generate)
        return
    with tempfile.TemporaryDirectory(prefix="mpv-vo-android-frame-") as directory:
        work = Path(directory)
        generate(root, work / "vo_android_frame_functions.h")
        output = args.output.resolve() if args.output else work / (
            "test.exe" if os.name == "nt" else "test")
        fixture = Path(__file__).with_suffix(".c").resolve()
        compiler = Path(args.cc[0]).stem.lower()
        if compiler in ("cl", "clang-cl"):
            flags = ["/nologo", "/std:c11", "/W4", "/WX", f"/I{work}",
                     str(fixture), f"/Fe:{output}", f"/Fo:{work / 'test.obj'}"]
        else:
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(work),
                     str(fixture), "-o", str(output)]
        subprocess.run(args.cc + flags + args.cflag, cwd=work, check=True)
        if not args.output:
            subprocess.run([str(output)], cwd=work, check=True)


if __name__ == "__main__":
    main()
