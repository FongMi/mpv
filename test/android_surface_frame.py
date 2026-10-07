#!/usr/bin/env python3
"""Build Android Surface frame synchronization contracts from production code."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

from vo_android_frame import function


def generate(root, output):
    common = (root / "video/out/android_common.c").read_text(encoding="utf-8")
    vo = (root / "video/out/vo.h").read_text(encoding="utf-8")
    source = re.search(r"struct vo_android_surface_frame \{.*?\n\};", vo, re.S)[0]
    source += "\n" + re.search(r"struct vo_android_state \{.*?\n\};", common, re.S)[0]
    for name in ("vo_android_set_native_window", "get_surface_frame_request",
                 "vo_android_native_window", "vo_android_has_native_window",
                 "vo_android_surface_size",
                 "surface_frames_equal", "vo_android_surface_frame_drawn",
                 "vo_android_surface_frame_presented", "vo_android_get_surface_frame"):
        source += "\n" + function(common, name)
    command = (root / "player/command.c").read_text(encoding="utf-8")
    source += function(command, "cmd_android_surface_frame")
    core = (root / "video/out/vo.c").read_text(encoding="utf-8")
    source += function(core, "vo_has_rendered_frame")
    gpu = (root / "video/out/vo_gpu_next.c").read_text(encoding="utf-8")
    source += function(gpu, "flip_page").replace("flip_page(", "gpu_next_flip_page(", 1)
    direct = (root / "video/out/vo_mediacodec_embed.c").read_text(encoding="utf-8")
    source += function(direct, "update_surface_frame")
    source += function(direct, "control").replace("control(", "direct_control(", 1)
    source += function(direct, "reconfig").replace("reconfig(", "direct_reconfig(", 1)
    source += function(direct, "flip_page").replace("flip_page(", "direct_flip_page(", 1)
    egl = (root / "video/out/opengl/context_android.c").read_text(encoding="utf-8")
    source += function(egl, "android_swap_buffers").replace(
        "struct priv *", "struct android_priv *", 1)
    output.write_text(source, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", nargs="+", default=["cc"])
    parser.add_argument("--generate", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if args.generate:
        generate(root, args.generate)
        return
    with tempfile.TemporaryDirectory(prefix="mpv-android-surface-frame-") as directory:
        work = Path(directory)
        generate(root, work / "android_surface_frame_functions.h")
        output = work / ("test.exe" if os.name == "nt" else "test")
        fixture = Path(__file__).with_suffix(".c").resolve()
        if Path(args.cc[0]).stem.lower() in ("cl", "clang-cl"):
            # mpv driver callbacks intentionally leave some API parameters unused.
            flags = ["/nologo", "/std:c11", "/W4", "/WX", "/wd4100", f"/I{work}",
                     str(fixture), f"/Fe:{output}", f"/Fo:{work / 'test.obj'}"]
        else:
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                     "-I", str(work),
                     str(fixture), "-o", str(output)]
        subprocess.run(args.cc + flags, cwd=work, check=True)
        subprocess.run([str(output)], cwd=work, check=True)


if __name__ == "__main__":
    main()
