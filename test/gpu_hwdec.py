#!/usr/bin/env python3
"""Expose the production hardware plane copy to the Vulkan GPU fixture."""

import argparse
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--generate', type=Path, required=True)
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8')
    match = re.search(r'^static bool copy_hwdec_plane\(.*?^}',
                      source, re.M | re.S)
    if not match:
        raise ValueError('Missing production copy_hwdec_plane')
    args.generate.write_text(match.group() + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
