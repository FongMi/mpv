#!/usr/bin/env python3
"""Extract filter option transactions and command error propagation."""

import argparse
from pathlib import Path

from vo_android_frame import function


def generate(root, output):
    command = (root / "player/command.c").read_text(encoding="utf-8")
    source = ""
    for name in ("reinit_filters", "set_filters", "access_options", "cmd_change_list"):
        source += function(command, name)
    output.write_text(source, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate", required=True, type=Path)
    args = parser.parse_args()
    generate(Path(__file__).resolve().parents[1], args.generate)
