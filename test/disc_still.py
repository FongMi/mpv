#!/usr/bin/env python3
"""Extract the disc demux read owner and DVD/BD still presentation paths."""

import argparse
from pathlib import Path

from vo_android_frame import function


def generate(root, output):
    bluray = (root / "stream/stream_bluray.c").read_text(encoding="utf-8")
    disc = (root / "demux/demux_disc.c").read_text(encoding="utf-8")
    source = function(bluray, "clear_still") + function(bluray, "handle_still_time")
    start = bluray.index("        case BD_EVENT_STILL:")
    end = bluray.index("        case BD_EVENT_PLAYLIST:", start)
    source += ("static void still_event(struct bluray_priv_s *b, int event, unsigned length)\n"
               "{\n    struct { unsigned param; } ev = {length};\n    switch (event) {\n" +
               bluray[start:end] + "    }\n}\n")
    player = (root / "player/discnav.c").read_text(encoding="utf-8")
    source += function(bluray, "skip_still") + function(player, "run_timed_still")
    source += function(disc, "d_read_packet")
    output.write_text(source, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate", required=True, type=Path)
    args = parser.parse_args()
    generate(Path(__file__).resolve().parents[1], args.generate)
