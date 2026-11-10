from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from subprocess import run
from sys import argv
from threading import Thread


class RangeServer(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        spec = self.headers["Range"]
        self.server.ranges.append(spec)
        start, end = spec.partition("=")[2].split("-")
        start = int(start)
        end = int(end) if end else len(self.server.data) - 1
        self.send_response(206)
        self.send_header("Content-Range", f"bytes {start}-{end}/{len(self.server.data)}")
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        try:
            self.wfile.write(self.server.data[start:end + 1])
        except OSError:
            # The reader may close the connection before consuming the range.
            pass


with ThreadingHTTPServer(("127.0.0.1", 0), RangeServer) as server:
    server.data = b"".join(bytes([block]) * (128 * 1024) for block in range(4))
    server.ranges = []
    Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_port}/media"
    try:
        cases = [
            ("cancel", []),
            ("read", ["bytes=131072-"]),
            ("seek", ["bytes=393216-"]),
            ("bounded", ["bytes=131072-393215", "bytes=393216-524287"]),
        ]
        for action, followup in cases:
            server.ranges.clear()
            run([argv[1], url, action], check=True, timeout=10)
            expected = ["bytes=0-131071", *followup]
            assert server.ranges == expected, (action, server.ranges, expected)
    finally:
        server.shutdown()
