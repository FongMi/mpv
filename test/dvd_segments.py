#!/usr/bin/env python3
"""Extract DVD VM selection, segment production and seek landing paths."""

import argparse
from pathlib import Path

from vo_android_frame import function


def generate(root, output):
    source = (root / "stream/stream_dvdnav.c").read_text(encoding="utf-8")
    functions = ""
    for name in ("dvd_audio_logical", "set_active_audio", "set_active_sub",
                 "drop_segments", "bump_discontinuity", "record_vobu"):
        functions += function(source, name)
    functions += "static int process_event(stream_t *s, int event, void *buf, int len);\n"
    functions += function(source, "land_on_picture")
    start = source.index("    case STREAM_CTRL_GET_SEGMENTS:")
    end = source.index("    case ", start + 9)
    functions += ("static int take_segments(stream_t *stream, void *arg)\n{\n"
                  "    struct priv *priv = stream->priv;\n"
                  "    switch (STREAM_CTRL_GET_SEGMENTS) {\n" + source[start:end] +
                  "    }\n    return -1;\n}\n")
    output.write_text(functions, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate", required=True, type=Path)
    args = parser.parse_args()
    generate(Path(__file__).resolve().parents[1], args.generate)
