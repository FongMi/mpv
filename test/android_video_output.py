#!/usr/bin/env python3
"""Build Android video output lifecycle contracts from the production core."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from vo_android_frame import function


def generate(root, output):
    video = (root / "player/video.c").read_text(encoding="utf-8")
    source = ""
    for name in ("android_direct_output_surfaces_ready", "is_android_direct_output_forced",
                 "wants_android_direct_output", "should_use_android_direct_output"):
        source += function(video, name)
    command = (root / "player/command.c").read_text(encoding="utf-8")
    output.write_text(source + function(command, "update_video_output"), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", nargs="+", default=["cc"])
    parser.add_argument("--source-root", type=Path,
                        default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mpv-android-video-output-") as directory:
        work = Path(directory)
        generate(args.source_root, work / "android_video_output_functions.h")
        output = work / ("test.exe" if os.name == "nt" else "test")
        fixture = Path(__file__).with_suffix(".c").resolve()
        if Path(args.cc[0]).stem.lower() in ("cl", "clang-cl"):
            flags = ["/nologo", "/std:c11", "/W4", "/WX", "/wd4100", f"/I{work}",
                     str(fixture), f"/Fe:{output}", f"/Fo:{work / 'test.obj'}"]
        else:
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                     "-I", str(work), str(fixture), "-o", str(output)]
        subprocess.run(args.cc + flags, cwd=work, check=True, timeout=60)
        subprocess.run([str(output)], cwd=work, check=True, timeout=10)


if __name__ == "__main__":
    main()
