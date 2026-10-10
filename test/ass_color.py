#!/usr/bin/env python3
"""Test ASS color conversion before either LIBASS or BGRA atlas packing."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

from vo_android_frame import function


def structure(source, name):
    return re.search(r"struct " + name + r" \{.*?\n\};", source, re.S)[0] + "\n"


def generate(root, output):
    osd = (root / "sub/osd.h").read_text(encoding="utf-8")
    packer = (root / "sub/packer.c").read_text(encoding="utf-8")
    source = structure(osd, "sub_bitmap") + structure(osd, "sub_bitmaps")
    source += structure(packer, "mp_sub_packer")
    output.with_name("ass_color_types.h").write_text(source, encoding="utf-8")

    csp = (root / "video/csputils.c").read_text(encoding="utf-8")
    # These production matrices intentionally zero initialize the omitted c.
    source = """
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
"""
    for name in ("mp_get_csp_mul", "luma_coeffs", "mp_get_csp_matrix",
                 "mp_map_fixp_color"):
        source += function(csp, name)
    source += """
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
"""
    for name in ("fill_padding_1", "fill_padding_4", "draw_ass_rgba",
                 "pack_libass", "pack_rgba", "mp_sub_packer_pack_ass"):
        source += function(packer, name)
    ass = (root / "sub/sd_ass.c").read_text(encoding="utf-8")
    converter = (root / "sub/img_convert.c").read_text(encoding="utf-8")
    source += function(converter, "mp_sub_bitmaps_bb")
    source += function(converter, "mp_sub_bitmaps_shift_y")
    source += function(ass, "mangle_colors")
    source += function(ass, "control")
    render = function(ass, "get_bitmaps")
    start = render.index("    int changed;\n")
    source += """
static struct sub_bitmaps *render_subtitles(struct sd *sd, int format,
                                          bool converted)
{
    struct sd_ass_priv *ctx = sd->priv;
    struct mp_subtitle_opts *opts = sd->opts;
    struct mp_subtitle_shared_opts *shared_opts = sd->shared_opts;
    ASS_Renderer *renderer = ctx->ass_renderer;
    ASS_Track *track = ctx->ass_track;
    long long ts = 0;
    struct sub_bitmaps *res = &(struct sub_bitmaps){0};
    struct mp_osd_res dim = {.h = 1000};
    bool transform_layout = false;
    if (!renderer)
        goto done;
""" + render[start:]
    output.write_text(source, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", nargs="+", default=["cc"])
    parser.add_argument("--cflag", action="append", default=[])
    output = parser.add_mutually_exclusive_group()
    output.add_argument("--output", type=Path)
    output.add_argument("--generate", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if args.generate:
        generate(root, args.generate)
        return
    with tempfile.TemporaryDirectory(prefix="mpv-ass-color-") as directory:
        work = Path(directory)
        generate(root, work / "ass_color_functions.h")
        output = args.output.resolve() if args.output else work / (
            "test.exe" if os.name == "nt" else "test")
        fixture = Path(__file__).with_suffix(".c").resolve()
        if Path(args.cc[0]).stem.lower() in ("cl", "clang-cl"):
            flags = ["/nologo", "/std:c11", "/W4", "/WX", f"/I{work}",
                     str(fixture), f"/Fe:{output}", f"/Fo:{work / 'test.obj'}"]
        else:
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(work),
                     str(fixture), "-o", str(output), "-lm"]
        subprocess.run(args.cc + flags + args.cflag, cwd=work, check=True)
        if not args.output:
            subprocess.run([str(output)], cwd=work, check=True)


if __name__ == "__main__":
    main()
