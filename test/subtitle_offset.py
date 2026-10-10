#!/usr/bin/env python3
"""Test production rendered-subtitle offsets with a host C compiler."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from vo_android_frame import function


def generate(root, output):
    source = (root / "sub/img_convert.c").read_text(encoding="utf-8")
    output.write_text(function(source, "mp_sub_bitmaps_bb")
                      + function(source, "mp_sub_bitmaps_shift_y"), encoding="utf-8")


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
    with tempfile.TemporaryDirectory(prefix="mpv-subtitle-offset-") as directory:
        work = Path(directory)
        generate(root, work / "subtitle_offset_functions.h")
        output = args.output.resolve() if args.output else work / (
            "test.exe" if os.name == "nt" else "test")
        fixture = Path(__file__).with_suffix(".c").resolve()
        if Path(args.cc[0]).stem.lower() in ("cl", "clang-cl"):
            flags = ["/nologo", "/std:c11", "/W4", "/WX", f"/I{work}",
                     str(fixture), f"/Fe:{output}", f"/Fo:{work / 'test.obj'}"]
        else:
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(work),
                     str(fixture), "-o", str(output), "-lm"]
        subprocess.run(args.cc + flags + args.cflag, cwd=work, check=True)
        if not args.output:
            subprocess.run([str(output)], cwd=work, check=True)


if __name__ == "__main__":
    main()
