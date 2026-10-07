#!/usr/bin/env python3
"""Build X11 monitor PAR publication contracts from production code."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from vo_android_frame import function


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", nargs="+", default=["cc"])
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="mpv-x11-window-geometry-") as directory:
        work = Path(directory)
        state = (root / "video/out/win_state.c").read_text(encoding="utf-8")
        x11 = (root / "video/out/x11_common.c").read_text(encoding="utf-8")
        source = function(state, "vo_apply_window_geometry")
        source += function(x11, "update_vo_size")
        source += function(x11, "vo_x11_config_vo_window")
        (work / "x11_window_geometry_functions.h").write_text(source, encoding="utf-8")
        output = work / ("test.exe" if os.name == "nt" else "test")
        fixture = Path(__file__).with_suffix(".c").resolve()
        if Path(args.cc[0]).stem.lower() in ("cl", "clang-cl"):
            flags = ["/nologo", "/std:c11", "/W4", "/WX", "/wd4100", f"/I{work}",
                     str(fixture), f"/Fe:{output}", f"/Fo:{work / 'test.obj'}"]
        else:
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                     "-I", str(work), str(fixture), "-o", str(output)]
        subprocess.run(args.cc + flags, cwd=work, check=True)
        subprocess.run([str(output)], cwd=work, check=True)


if __name__ == "__main__":
    main()
