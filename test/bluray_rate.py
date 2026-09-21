#!/usr/bin/env python3
"""Extract the production Blu-ray rate control and deferred application paths."""

import argparse
from pathlib import Path


def generate(root, output):
    source = (root / "stream/stream_bluray.c").read_text(encoding="utf-8")
    start = source.index("    case STREAM_CTRL_SET_PLAYBACK_RATE: {")
    end = source.index("    case STREAM_CTRL_GET_CURRENT_TITLE:", start)
    control = source[start:end]
    start = source.index("        bool pending_rate = b->pending_rate_set")
    start = source.rindex("        mp_mutex_lock(&b->overlay_lock);", 0, start)
    end = source.index("        if (n > 0)", start)
    apply = source[start:end]
    output.write_text(
        "static int set_rate(struct bluray_priv_s *b, double rate)\n"
        "{\n    void *arg = &rate;\n    switch (STREAM_CTRL_SET_PLAYBACK_RATE) {\n" +
        control + "    }\n    return STREAM_UNSUPPORTED;\n}\n\n" +
        "static void apply_pending(struct bluray_priv_s *b)\n{\n" + apply + "}\n",
        encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate", required=True, type=Path)
    args = parser.parse_args()
    generate(Path(__file__).resolve().parents[1], args.generate)
