#!/usr/bin/env python3
"""Test gpu-next mapping ownership using extracted production functions.

The fixture stubs GPU/codec APIs, not the parameter comparison or slot lifecycle.
It can run on a host without a GPU; it does not test Android buffer interop.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def extract_function(source, name):
    match = re.search(r'^[\w *]+\b' + name + r'\([^;{]*\)\s*\{',
                      source, re.M)
    if not match:
        raise ValueError(f'Missing function: {name}')
    end, depth = match.end(), 1
    # These functions contain no braces in strings or comments.
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def extract_structure(source, name):
    start = source.index('struct ' + name + ' {')
    return source[start:source.index('\n};', start) + 3]


def generate(source, contracts, functions):
    params = (source / 'video/mp_image.h').read_text(encoding='utf-8')
    hwdec = (source / 'video/out/gpu/hwdec.h').read_text(encoding='utf-8')
    image = (source / 'video/mp_image.c').read_text(encoding='utf-8')
    video = (source / 'video/out/vo_gpu_next.c').read_text(encoding='utf-8')
    contracts.write_text('\n\n'.join((
        extract_structure(params, 'mp_image_params'),
        extract_structure(hwdec, 'ra_hwdec_mapper'),
        extract_structure(video, 'hwdec_slot'),
    )), encoding='utf-8')
    functions.write_text('\n\n'.join(
        extract_function(text, name) for text, names in (
            (image, ('mp_image_params_equal', 'mp_image_params_static_equal')),
            (video, ('slot_unmap', 'slot_reconfig', 'slot_release_owner',
                     'slot_uninit', 'slot_preload')),
        ) for name in names
    ), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mpv', type=Path, required=True)
    parser.add_argument('--cc', nargs='+', default=['cc'])
    parser.add_argument('--generate', nargs=2, type=Path)
    args = parser.parse_args()
    if args.generate:
        generate(args.mpv, *args.generate)
        return

    fixture = Path(__file__).with_suffix('.c').resolve()
    with tempfile.TemporaryDirectory(prefix='mpv-gpu-next-') as directory:
        work = Path(directory)
        generate(args.mpv, work / 'vo_gpu_next_contracts.h',
                 work / 'vo_gpu_next_functions.h')
        executable = work / ('test.exe' if os.name == 'nt' else 'test')
        compiler = Path(args.cc[0]).stem.lower()
        if compiler in ('cl', 'clang-cl'):
            flags = ['/nologo', '/std:c11', '/W4', '/WX', f'/I{work}',
                     str(fixture), f'/Fe:{executable}', f'/Fo:{work / "test.obj"}']
            if compiler == 'clang-cl':
                flags += ['/clang:-Wno-sign-compare']
        else:
            # Match mpv's warning policy for the extracted production code.
            flags = ['-std=c11', '-Wall', '-Wextra', '-Werror',
                     '-Wno-sign-compare', '-I', str(work),
                     str(fixture), '-o', str(executable)]
        subprocess.run(args.cc + flags, cwd=work, check=True)
        subprocess.run([str(executable)], cwd=work, check=True)


if __name__ == '__main__':
    main()
