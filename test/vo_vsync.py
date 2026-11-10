#!/usr/bin/env python3
"""Extract production VO presentation timing and lifecycle functions."""
import argparse
from pathlib import Path
import re

from vo_android_frame import function


def generate(source, header, output):
    text = source.read_text(encoding="utf-8")
    names = ("reset_vsync_timings", "vsync_stddef", "check_estimated_display_fps",
             "vsync_skip_detection", "update_vsync_timing_after_swap",
             "get_current_frame_end", "get_display_synced_frame_end",
             "advance_vsyncs", "schedule_vsync_slot", "render_frame",
             "still_displaying", "vo_get_delay", "vo_discard_timing_info",
             "vo_set_paused", "vo_seek_reset")
    contracts = header.read_text(encoding="utf-8")
    start = contracts.index("struct vo_vsync_info {")
    functions = [contracts[start:contracts.index("\n};", start) + 3]]
    for name in ("MAX_VSYNC_SAMPLES", "DELAY_VSYNC_SAMPLES"):
        match = re.search(r"^#define " + name + r" .*$", text, re.M)
        if not match:
            raise ValueError("Missing timing constant: " + name)
        functions.append(match[0])
    functions += [function(text, name) for name in names]
    output.write_text("\n".join(functions), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--generate", type=Path, required=True)
    args = parser.parse_args()
    generate(args.source, args.header, args.generate)


if __name__ == "__main__":
    main()
