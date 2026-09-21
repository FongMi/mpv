#!/usr/bin/env python3
"""Extract the AImageReader mapper, including submission and pending ownership."""
import argparse
from pathlib import Path
import re


def generate(source, output, acquire=False):
    text = source.read_text(encoding="utf-8")
    names = ("delete_image", "acquire_latest_image") if acquire else ("mapper_map",)
    functions = []
    for name in names:
        match = re.search(r"^static (?:void|int|media_status_t) " + name +
                          r"\(.*?^}\n", text, re.M | re.S)
        if not match:
            raise ValueError("Missing production function: " + name)
        functions.append(match[0])
    output.write_text("\n".join(functions), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1] /
                        "video/out/hwdec/hwdec_aimagereader.c")
    parser.add_argument("--generate", type=Path, required=True)
    parser.add_argument("--acquire", action="store_true")
    args = parser.parse_args()
    generate(args.source, args.generate, args.acquire)


if __name__ == "__main__":
    main()
