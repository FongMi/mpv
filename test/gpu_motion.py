#!/usr/bin/env python3
"""Expose production motion shaders to the GPU policy regression fixture."""

import argparse
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--vo-source', type=Path, required=True)
    parser.add_argument('--generate', type=Path, required=True)
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    shaders = []
    for name in ('sample_header', 'match_header', 'match_body', 'refine_header',
                 'translation_select_header', 'translation_select_body',
                 'synthesis_header', 'warp_body', 'synthesis_body'):
        match = re.search(r'^static const char ' + name +
                          r'\[\] = GLSL\(\n.*?^\);', source, re.M | re.S)
        if not match:
            raise ValueError('Missing shader: ' + name)
        shaders.append(match.group().replace(name + '[]', 'test_' + name + '[]', 1))
    vo = args.vo_source.read_text(encoding='utf-8')
    callback = re.search(r'^struct motion_mix \{.*?^\};\n\n'
                         r'static pl_tex mix_motion_views\(.*?^\}', vo, re.M | re.S)
    if not callback:
        raise ValueError('Missing production prepared-view callback')
    args.generate.write_text('#define GLSL(...) #__VA_ARGS__\n' +
                             '\n\n'.join(shaders) + '\n#undef GLSL\n\n' +
                             callback.group() + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
