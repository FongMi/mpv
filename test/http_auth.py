import base64
import hashlib
import io
import re
import subprocess
import sys
import threading
import unittest
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def md5(value):
    return hashlib.md5(value.encode()).hexdigest()


def audio_sample():
    data = io.BytesIO()
    with wave.open(data, "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(8000)
        output.writeframes(b"\0\0" * 8000)
    return data.getvalue()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    sample = audio_sample()

    def log_message(self, *args):
        pass

    def do_GET(self):
        authorization = self.headers.get("Authorization", "")
        with self.server.lock:
            self.server.requests.append(authorization)
        basic = "Basic " + base64.b64encode(b"fixture:password").decode()
        accepted = self.server.mode in ("basic", "both") and authorization == basic
        if self.server.mode == "bearer":
            accepted = authorization == "Bearer selected"
        if self.server.mode in ("digest", "both"):
            accepted = accepted or self.valid_digest(authorization)
        if not accepted:
            body = b"Unauthorized response body"
            self.send_response(401)
            if self.server.mode in ("basic", "both"):
                self.send_header("WWW-Authenticate", 'Basic realm="mpv-test"')
            if self.server.mode in ("digest", "both", "bearer"):
                self.send_header("WWW-Authenticate", 'Digest realm="mpv-test", nonce="nonce", qop="auth", algorithm=MD5')
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(self.sample)))
        self.end_headers()
        try:
            self.wfile.write(self.sample)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def valid_digest(self, authorization):
        if not authorization.startswith("Digest "):
            return False
        params = {
            key: quoted or plain
            for key, quoted, plain in re.findall(
                r'(\w+)=(?:"([^"]*)"|([^,\s]+))', authorization[7:]
            )
        }
        expected = {
            "username": "fixture",
            "realm": "mpv-test",
            "nonce": "nonce",
            "uri": self.path,
            "qop": "auth",
        }
        if any(params.get(key) != value for key, value in expected.items()):
            return False
        if not params.get("nc") or not params.get("cnonce"):
            return False
        ha1 = md5("fixture:mpv-test:password")
        ha2 = md5("GET:" + self.path)
        response = md5(f"{ha1}:nonce:{params['nc']}:{params['cnonce']}:auth:{ha2}")
        return params.get("response") == response


class HttpAuthTest(unittest.TestCase):
    executable = sys.argv[1]

    def setUp(self):
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.mode = "digest"
        self.server.requests = []
        self.server.lock = threading.Lock()
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def load(self, credentials="fixture:password", success=True, header=None):
        url = f"http://{credentials}@127.0.0.1:{self.server.server_port}/audio.wav"
        command = [self.executable, url, "success" if success else "failure"]
        if header is not None:
            command.append(header)
        result = subprocess.run(command, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_digest_challenge_with_body(self):
        self.load()
        self.assertTrue(any(header.startswith("Digest ") for header in self.server.requests))

    def test_basic_challenge_with_body(self):
        self.server.mode = "basic"
        self.load()
        self.assertTrue(any(header.startswith("Basic ") for header in self.server.requests))

    def test_digest_is_preferred_when_both_are_available(self):
        self.server.mode = "both"
        self.load()
        authenticated = [header for header in self.server.requests if header]
        self.assertTrue(authenticated)
        self.assertTrue(all(header.startswith("Digest ") for header in authenticated))

    def test_wrong_password_fails(self):
        self.load(credentials="fixture:wrong", success=False)
        self.assertTrue(any(header.startswith("Digest ") for header in self.server.requests))
        self.assertLessEqual(len(self.server.requests), 3)

    def test_explicit_header_overrides_url_credentials(self):
        self.server.mode = "bearer"
        self.load(credentials="other:wrong", header="authorization: Bearer selected")
        self.assertEqual(self.server.requests, ["Bearer selected"])

    def test_explicit_header_failure_does_not_use_url_credentials(self):
        self.server.mode = "basic"
        self.load(success=False, header="Authorization: Bearer wrong")
        self.assertEqual(self.server.requests, ["Bearer wrong"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
