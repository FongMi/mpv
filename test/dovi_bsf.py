#!/usr/bin/env python3
"""Extract Dolby Vision bitstream-filter initialization and mode selection."""

import argparse
from pathlib import Path
import re

from vo_android_frame import function


def generate(root, output):
    source = (root / "demux/dovi_split.c").read_text(encoding="utf-8")
    header = (root / "demux/demux.h").read_text(encoding="utf-8")
    code = re.search(r"enum demux_dovi_profile7_mode \{.*?\n\};",
                     header, re.S)[0] + "\n"
    for declaration in ("enum dovi_filter_mode", "struct mp_dovi_split"):
        code += re.search(declaration + r" \{.*?\n\};", source, re.S)[0] + "\n"
    for name in ("init_bsf", "init_profile7_bsf"):
        code += function(source, name)
    output.write_text(code, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate", required=True, type=Path)
    args = parser.parse_args()
    generate(Path(__file__).resolve().parents[1], args.generate)
