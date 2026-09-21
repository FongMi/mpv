#!/usr/bin/env python3
"""Test minimal GPU rendering coordinates using production functions.

GPU sampling and shader emission are captured by the fixture. Image view sizing,
rotation, chroma alignment, and crop transforms run without a GPU.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

from vo_android_frame import function


def structure(source, name):
    match = re.search(r"\bstruct " + name + r" \{.*?\n\};", source, re.S)
    if not match:
        raise ValueError(f"Missing production structure: {name}")
    return match[0]


def generate(root, output):
    video = (root / "video/out/gpu/video.c").read_text(encoding="utf-8")
    utils = (root / "video/out/gpu/utils.h").read_text(encoding="utf-8")
    types = structure(utils, "gl_transform")
    types += "\n" + re.search(
        r"static const struct gl_transform identity_trans = \{.*?\n\};",
        utils, re.S)[0]
    types += "\n" + re.search(r"enum plane_type \{.*?\n\};", video, re.S)[0]
    for name in ("texplane", "video_image", "image"):
        types += "\n" + structure(video, name)
    output.with_name("vo_gpu_dumb_types.h").write_text(types, encoding="utf-8")

    source = function(utils, "gl_transform_vec")
    utils = (root / "video/out/gpu/utils.c").read_text(encoding="utf-8")
    source += function(utils, "gl_transform_trans")
    for name in ("get_transform", "chroma_upsize", "merge_plane_types",
                 "pass_get_images", "compute_src_transform", "chroma_realign",
                 "pass_render_frame_dumb"):
        source += function(video, name)
    output.write_text(source, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mpv", type=Path)
    parser.add_argument("--cc", nargs="+", default=["cc"])
    parser.add_argument("--cflag", action="append", default=[])
    parser.add_argument("--generate", type=Path)
    args = parser.parse_args()
    root = args.mpv or Path(__file__).resolve().parents[1]
    if args.generate:
        generate(root, args.generate)
        return
    with tempfile.TemporaryDirectory(prefix="mpv-gpu-dumb-") as directory:
        work = Path(directory)
        generate(root, work / "vo_gpu_dumb_functions.h")
        output = work / ("test.exe" if os.name == "nt" else "test")
        fixture = Path(__file__).with_suffix(".c").resolve()
        compiler = Path(args.cc[0]).stem.lower()
        if compiler in ("cl", "clang-cl"):
            flags = ["/nologo", "/std:c11", "/W4", "/WX", f"/I{work}",
                     str(fixture), f"/Fe:{output}", f"/Fo:{work / 'test.obj'}"]
            if compiler == "clang-cl":
                flags += ["/clang:-Wno-missing-field-initializers"]
        else:
            # Match mpv's policy for intentionally zero-initialized transforms.
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror",
                     "-Wno-missing-field-initializers", "-I", str(work),
                     str(fixture), "-o", str(output), "-lm"]
        subprocess.run(args.cc + flags + args.cflag, cwd=work, check=True)
        subprocess.run([str(output)], cwd=work, check=True)


if __name__ == "__main__":
    main()
