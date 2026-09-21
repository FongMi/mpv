#!/usr/bin/env python3
"""Extract decoder initialization and lazy-stream failure ownership."""

import argparse
from pathlib import Path

from vo_android_frame import function


def generate(root, output):
    video = (root / "player/video.c").read_text(encoding="utf-8")
    audio = (root / "player/audio.c").read_text(encoding="utf-8")
    source = function(video, "init_video_decoder")
    source += function(audio, "init_audio_decoder")
    source += function(video, "uninit_video_decoder")
    source += function(audio, "uninit_audio_decoder")
    source += function(video, "handle_video_init_failure")
    source += function(video, "reinit_video_decoder")
    source += function(audio, "reinit_audio_decoder")
    source += function((root / "player/playloop.c").read_text(encoding="utf-8"),
                       "handle_lazy_streams")
    output.write_text(source, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate", required=True, type=Path)
    args = parser.parse_args()
    generate(Path(__file__).resolve().parents[1], args.generate)
