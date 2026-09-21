#!/usr/bin/env python3
"""Extract the production disc alias and segment mapping contracts."""

import argparse
from pathlib import Path
import re

from vo_android_frame import function


def generate(root, output):
    source = (root / "demux/demux_disc.c").read_text(encoding="utf-8")
    header = (root / "stream/stream.h").read_text(encoding="utf-8")
    types = ""
    for name in ("stream_dvd_stream", "stream_dvd_streams", "stream_decl_stream",
                 "stream_streams_req", "stream_stream_id", "stream_segment"):
        types += re.search(r"struct " + name + r" \{.*?\n\};", header, re.S)[0] + "\n"
    types += re.search(r"struct priv \{.*?\n\};", source, re.S)[0] + "\n"
    output.with_name("disc_segment_types.h").write_text(types, encoding="utf-8")
    functions = ""
    for name in ("clear_dvd_sub_holds", "arm_dvd_sub_holds", "get_ostate",
                 "dvd_alias_matches", "find_outer", "sync_dvd_alias",
                 "sync_dvd_stream", "selected_dvd_audio", "queue_dvd_sub_aliases",
                 "register_declared_streams", "reset_segments", "seg_by_id",
                 "find_segment", "segment_has_stream", "apply_presence",
                 "map_packet", "sync_streams", "deliver_dvd_sub",
                 "sync_initial_edition"):
        functions += function(source, name)
    output.write_text(functions, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate", required=True, type=Path)
    args = parser.parse_args()
    generate(Path(__file__).resolve().parents[1], args.generate)
