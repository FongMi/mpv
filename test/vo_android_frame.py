#!/usr/bin/env python3
"""Extract production functions for focused host contract tests."""

import re


def function(source, name):
    match = re.search(r"(?m)^[\w *]+\b" + re.escape(name) + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise ValueError(f"Missing production function: {name}")
    start = match.start()
    body = match.end() - 1
    end = body + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"
