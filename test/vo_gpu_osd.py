#!/usr/bin/env python3
"""Check the legacy GPU OSD vertex attributes used to build its shader."""

import argparse
from pathlib import Path
import re

from vo_android_frame import function
from vo_gpu_dumb import structure


def generate(root, output):
    gpu = root / "video/out/gpu"
    ra = (gpu / "ra.h").read_text(encoding="utf-8")
    osd = (gpu / "osd.c").read_text(encoding="utf-8")
    shader_cache = (gpu / "shader_cache.c").read_text(encoding="utf-8")
    source = """#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MP_ASSERT_UNREACHABLE() abort()
"""
    source += re.search(r"enum ra_vartype \{.*?\n\};", ra, re.S)[0] + "\n"
    source += structure(ra, "ra_renderpass_input") + "\n"
    source += structure(osd, "vertex") + "\n"
    source += re.search(
        r"static const struct ra_renderpass_input vertex_vao\[\] = \{.*?\n\};",
        osd, re.S)[0] + "\n"
    source += function(shader_cache, "vao_glsl_type")
    source += """
int main(void)
{
    assert(strcmp(vao_glsl_type(&vertex_vao[0]), "vec2") == 0);
    assert(strcmp(vao_glsl_type(&vertex_vao[1]), "vec2") == 0);
    assert(strcmp(vao_glsl_type(&vertex_vao[2]), "vec4") == 0);
    assert(vertex_vao[0].type == RA_VARTYPE_FLOAT && vertex_vao[0].dim_m == 1);
    assert(vertex_vao[1].type == RA_VARTYPE_FLOAT && vertex_vao[1].dim_m == 1);
    assert(vertex_vao[2].type == RA_VARTYPE_BYTE_UNORM && vertex_vao[2].dim_m == 1);
    assert(vertex_vao[0].offset == offsetof(struct vertex, position));
    assert(vertex_vao[1].offset == offsetof(struct vertex, texcoord));
    assert(vertex_vao[2].offset == offsetof(struct vertex, ass_color));
    return 0;
}
"""
    output.write_text(source, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate", type=Path, required=True)
    args = parser.parse_args()
    generate(Path(__file__).resolve().parents[1], args.generate)
