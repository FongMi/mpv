#!/usr/bin/env python3
"""Extract the production display-sync scheduler and playback status."""
import argparse
from pathlib import Path

from vo_android_frame import function


def generate(source, header, output):
    text = header.read_text(encoding="utf-8")
    start = text.index("enum playback_status {")
    status = text[start:text.index("};", start) + 2]
    body = function(source.read_text(encoding="utf-8"),
                    "handle_display_sync_frame")
    output.write_text(status + "\n" + body, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--generate", type=Path, required=True)
    args = parser.parse_args()
    generate(args.source, args.header, args.generate)


if __name__ == "__main__":
    main()
