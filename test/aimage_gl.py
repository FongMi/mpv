#!/usr/bin/env python3
"""Test AImageReader GL release ownership using production helpers."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

def generate(root, output):
    source = (root / "video/out/hwdec/hwdec_aimagereader.c").read_text(
        encoding="utf-8")
    functions = []
    for name in ("init_gl_release_fence", "release_gl_mapping", "replace_gl_mapping"):
        match = re.search(r"^static bool " + name + r"\(.*?^}\n",
                          source, re.M | re.S)
        if not match:
            raise ValueError(f"Missing production function: {name}")
        functions.append(match[0])
    output.write_text("\n".join(functions), encoding="utf-8")


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
    with tempfile.TemporaryDirectory(prefix="mpv-aimage-gl-") as directory:
        work = Path(directory)
        generate(root, work / "aimage_gl_functions.h")
        executable = args.output.resolve() if args.output else work / (
            "test.exe" if os.name == "nt" else "test")
        fixture = Path(__file__).with_suffix(".c").resolve()
        compiler = Path(args.cc[0]).stem.lower()
        if compiler in ("cl", "clang-cl"):
            flags = ["/nologo", "/std:c11", "/W4", "/WX", f"/I{work}",
                     str(fixture), f"/Fe:{executable}", f"/Fo:{work / 'test.obj'}"]
        else:
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(work),
                     str(fixture), "-o", str(executable)]
        subprocess.run(args.cc + flags + args.cflag, cwd=work, check=True)
        if not args.output:
            subprocess.run([str(executable)], cwd=work, check=True)


if __name__ == "__main__":
    main()
