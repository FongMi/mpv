#!/usr/bin/env python3
"""Extract decoder-flush ordering and the synchronous VO retirement owner."""

import argparse
from pathlib import Path
import re

from vo_android_frame import function


def generate(root, output):
    vo = (root / "video/out/vo.c").read_text(encoding="utf-8")
    header = (root / "video/out/vo.h").read_text(encoding="utf-8")
    core = (root / "player/playloop.c").read_text(encoding="utf-8")
    control = re.search(r"enum mp_voctrl \{.*?^\};", header, re.M | re.S)[0]
    maximum = re.search(r"^#define VO_MAX_REQ_FRAMES .*", header, re.M)[0]
    results = [re.search(r"^#define " + name + r" .*", header, re.M)[0]
               for name in ("VO_TRUE", "VO_FALSE", "VO_NOTIMPL")]
    names = ("forget_frames", "run_prepare_decoder_flush",
             "vo_prepare_decoder_flush", "vo_seek_reset")
    output.write_text("\n".join([control, maximum] + results +
                      [function(vo, name) for name in names] +
                      [function(core, "reset_playback_state")]), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=Path(__file__).resolve().parents[1])
    parser.add_argument("--generate", required=True, type=Path)
    args = parser.parse_args()
    generate(args.source, args.generate)
