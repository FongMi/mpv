#!/usr/bin/env python3
"""Test live hardware-decoder policy updates using production control paths."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

from vo_android_frame import function


def generate(root, output):
    decoder = (root / "video/decode/vd_lavc.c").read_text(encoding="utf-8")
    wrapper = (root / "filters/f_decoder_wrapper.c").read_text(encoding="utf-8")
    header = (root / "filters/f_decoder_wrapper.h").read_text(encoding="utf-8")
    command = (root / "player/command.c").read_text(encoding="utf-8")
    types = re.search(r"struct hwdec_opts \{.*?\n\};", decoder, re.S)[0]
    types += "\n" + re.search(r"enum dec_ctrl \{.*?\n\};", header, re.S)[0]

    source = function(decoder, "update_hwdec_options")
    source += function(decoder, "handle_err")
    source += function(decoder, "control")
    source += function(wrapper, "mp_decoder_wrapper_control")
    callback = function(command, "mp_option_run_callback")
    # Keep the actual callback's policy, output and decoder update ordering.
    # Unrelated option handlers before/after this contiguous block are omitted.
    prefix = callback[:callback.index("    if (flags & UPDATE_TERM)")]
    begin = callback.index("    if (flags & UPDATE_HWDEC) {")
    end = callback.index("    if (flags & UPDATE_DVB_PROG) {")
    source += prefix + "    (void)opts;\n" + callback[begin:end] + "}\n"
    output.write_text("#ifdef HWDEC_POLICY_TYPES\n" + types +
                      "\n#else\n" + source + "#endif\n", encoding="utf-8")


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
    with tempfile.TemporaryDirectory(prefix="mpv-hwdec-policy-") as directory:
        work = Path(directory)
        generate(root, work / "hwdec_policy_functions.h")
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
