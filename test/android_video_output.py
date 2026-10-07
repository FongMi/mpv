#!/usr/bin/env python3
"""Build Android video output lifecycle contracts from the production core."""

import argparse
import os
import re
from pathlib import Path
import subprocess
import tempfile

from vo_android_frame import function


def generate(root, output):
    video = (root / "player/video.c").read_text(encoding="utf-8")
    source = ""
    for name in ("android_direct_output_surfaces_ready", "is_android_direct_output_forced",
                 "wants_android_direct_output", "should_use_android_direct_output"):
        source += function(video, name)
    source += function(video, "android_video_output_needs_surface")
    source += function(video, "is_android_video_output_waiting_for_surface")
    source += function(video, "reset_video_state")
    source += function(video, "uninit_video_chain")
    source += function(video, "uninit_video_out")
    playloop = (root / "player/playloop.c").read_text(encoding="utf-8")
    window = function(playloop, "handle_force_window")
    source += window.split("    if (mpctx->opts->force_vo != 2 && !act)", 1)[0]
    source += "    test_force_window_init(mpctx);\n    return 0;\n}\n"
    # The remaining init path owns real VO/decoder resources. Observe entry
    # through the production Surface prerequisite before substituting that tail.
    entry = function(video, "reinit_video_chain_src")
    source += entry.split("    bool use_dovi_direct =", 1)[0]
    source += "    test_video_output_init(mpctx, track);\n}\n"
    loadfile = (root / "player/loadfile.c").read_text(encoding="utf-8")
    start = loadfile.index("    reinit_video_chain(mpctx);", loadfile.index("static void play_current_file"))
    end = loadfile.index("    // For lavfi-complex mode", start)
    source += "static void test_prepare_outputs(struct MPContext *mpctx)\n{\n"
    source += loadfile[start:end] + "}\n"
    switch = function(loadfile, "mp_switch_track_n")
    start = switch.index("    mpctx->current_track[order][type] = track;")
    end = switch.index("    mp_notify(mpctx, MP_EVENT_TRACK_SWITCHED", start)
    source += ("static void test_finish_track_switch(struct MPContext *mpctx, "
               "int order, int type, struct track *track)\n{\n")
    source += switch[start:end] + "}\n"
    check = re.search(
        r'(?m)^    if \(([^{}]*?)\)\s*\{\n'
        r'        MP_FATAL\(mpctx, "No video or audio streams selected', loadfile)
    if not check:
        raise ValueError("Missing production playable-stream check")
    source += "static bool test_has_playable_stream(struct MPContext *mpctx)\n{\n"
    source += "    struct MPOpts *opts = mpctx->opts;\n"
    source += "    return !(" + check[1] + ");\n}\n"
    command = (root / "player/command.c").read_text(encoding="utf-8")
    output.write_text(source + function(command, "update_video_output"), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", nargs="+", default=["cc"])
    parser.add_argument("--source-root", type=Path,
                        default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mpv-android-video-output-") as directory:
        work = Path(directory)
        generate(args.source_root, work / "android_video_output_functions.h")
        output = work / ("test.exe" if os.name == "nt" else "test")
        fixture = Path(__file__).with_suffix(".c").resolve()
        if Path(args.cc[0]).stem.lower() in ("cl", "clang-cl"):
            flags = ["/nologo", "/std:c11", "/W4", "/WX", "/wd4100", f"/I{work}",
                     str(fixture), f"/Fe:{output}", f"/Fo:{work / 'test.obj'}"]
        else:
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                     "-I", str(work), str(fixture), "-o", str(output)]
        subprocess.run(args.cc + flags, cwd=work, check=True, timeout=60)
        subprocess.run([str(output)], cwd=work, check=True, timeout=10)


if __name__ == "__main__":
    main()
