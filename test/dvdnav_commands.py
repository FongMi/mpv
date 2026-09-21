#!/usr/bin/env python3
"""Extract the production DVD command acceptance and jump ownership."""

import argparse
from pathlib import Path

from vo_android_frame import function


def generate(root, output):
    header = (root / "stream/stream.h").read_text(encoding="utf-8")
    start = header.index("enum stream_nav_action {")
    end = header.index("// Release the finite", start)
    source = header[start:end]
    dvd = (root / "stream/stream_dvdnav.c").read_text(encoding="utf-8")
    for name in ("note_activation", "select_neighbour_button", "clear_still",
                 "vm_moved", "release_still", "do_nav_cmd",
                 "bump_discontinuity", "handle_nav_cmd"):
        source += function(dvd, name)
    output.write_text(source, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate", required=True, type=Path)
    args = parser.parse_args()
    generate(Path(__file__).resolve().parents[1], args.generate)
