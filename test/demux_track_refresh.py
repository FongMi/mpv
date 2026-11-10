#!/usr/bin/env python3
"""Extract the production track refresh and its cache accounting."""

from pathlib import Path
import sys

from vo_android_frame import function


source = Path(sys.argv[1]).read_text(encoding="utf-8")
names = ("get_forward_buffered_bytes", "initiate_refresh_seek",
         "drop_refresh_packet")
functions = "\n".join(function(source, name) for name in names)
Path(sys.argv[2]).write_text(functions, encoding="utf-8")
