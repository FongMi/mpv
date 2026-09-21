#!/usr/bin/env python3
"""Check target hint black-point provenance and reference-white ordering."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def function(text, name):
    match = re.search(r'^[\w *]+\b' + name + r'\([^;{}]*?\)\n\{.*?^\}', text, re.M | re.S)
    if not match:
        raise ValueError(f'Missing production function: {name}')
    return match[0]


def generate(root, output):
    video = (root / 'video/out/gpu/video.c').read_text(encoding='utf-8')
    header = (root / 'video/out/gpu/video.h').read_text(encoding='utf-8')
    next_video = (root / 'video/out/vo_gpu_next.c').read_text(encoding='utf-8')
    declaration = re.search(r'enum gl_target_hint_status gl_video_prepare_target_hint\([^;]+;', header)
    if not declaration:
        raise ValueError('Missing target hint declaration')
    source = declaration[0] + '\n' + function(video, 'gl_video_prepare_target_hint')
    source += '\n' + function(next_video, 'use_ref_luma')
    start = next_video.index('        if (target_ref_luma && use_ref_luma(&hint, &target_csp))')
    end = next_video.index('        // Always set maxCLL,', start)
    block = next_video[start:end]
    source += '\nstatic void apply_reference_hint(struct pl_color_space *result, '
    source += 'const struct pl_color_space *target, float target_ref_luma, bool min_luma_known)\n{\n'
    source += '    struct pl_color_space hint = *result;\n    struct pl_color_space target_csp = *target;\n'
    source += block + '    *result = hint;\n}\n'
    output.write_text(source, encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', nargs='+', default=['cc'])
    parser.add_argument('--generate', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if args.generate:
        generate(root, args.generate)
        return
    with tempfile.TemporaryDirectory(prefix='mpv-target-hint-') as directory:
        work = Path(directory)
        generate(root, work / 'target_hint_functions.h')
        fixture = Path(__file__).with_suffix('.c')
        output = work / ('test.exe' if os.name == 'nt' else 'test')
        if Path(args.cc[0]).stem.lower() in ('cl', 'clang-cl'):
            flags = ['/nologo', '/std:c11', '/W4', '/WX', f'/I{work}', str(fixture),
                     f'/Fe:{output}', f'/Fo:{work / "test.obj"}']
        else:
            flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(work),
                     str(fixture), '-o', str(output), '-lm']
        subprocess.run(args.cc + flags, cwd=work, check=True)
        subprocess.run([str(output)], cwd=work, check=True)


if __name__ == '__main__':
    main()
