#!/usr/bin/env python3
"""Extract the disc demux read owner and Blu-ray still identity helpers."""

import argparse
from pathlib import Path

from vo_android_frame import function


def generate(root, output):
    bluray = (root / "stream/stream_bluray.c").read_text(encoding="utf-8")
    disc = (root / "demux/demux_disc.c").read_text(encoding="utf-8")
    source = function(bluray, "hold_still") + function(bluray, "clear_still")
    source += function(disc, "d_read_packet")
    output.write_text(source, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate", required=True, type=Path)
    args = parser.parse_args()
    generate(Path(__file__).resolve().parents[1], args.generate)
