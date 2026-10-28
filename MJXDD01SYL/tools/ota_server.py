#!/usr/bin/env python3
"""Serve one firmware image over HTTP/1.1 for a stock Yeelight OTA.

The stock downloader does not finish a transfer from an HTTP/1.0 server.
Every path returns the same file. Default port is 80, which is the port a
miIO 0.0.6 lamp fetches when the URL has no port written in it.

Pass firmware.ota.bin, or the CRC image produced by append_crc.py.
firmware.factory.bin is refused.
"""

from http.server import BaseHTTPRequestHandler, HTTPServer
import pathlib
import re
import socketserver
import sys
import threading

APP_SLOT = 1966080
_lock = threading.Lock()


def build_handler(data: bytes):
    size = len(data)

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "nginx/1.24.0"
        sys_version = ""

        def log_message(self, fmt, *args):
            pass

        def _log_request(self):
            with _lock:
                print(
                    f"\n=== {self.client_address[0]}:{self.client_address[1]} "
                    f"{self.command} {self.path} {self.request_version} ===",
                    flush=True,
                )

        def do_HEAD(self):
            self._log_request()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()

        def do_GET(self):
            self._log_request()
            start, end, partial = 0, size - 1, False
            rng = self.headers.get("Range")
            if rng:
                match = re.match(r"bytes=(\d*)-(\d*)", rng.strip())
                if match:
                    if match.group(1):
                        start = int(match.group(1))
                    if match.group(2):
                        end = min(int(match.group(2)), size - 1)
                    partial = True
            body = data[start : end + 1]
            self.send_response(206 if partial else 200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Accept-Ranges", "bytes")
            if partial:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            sent = 0
            try:
                while sent < len(body):
                    wrote = self.wfile.write(body[sent : sent + 4096])
                    sent += wrote or 4096
                with _lock:
                    print(f"    --> sent {sent}/{len(body)} bytes", flush=True)
            except Exception as exc:  # noqa: BLE001
                with _lock:
                    print(
                        f"    --> sent {sent}/{len(body)} bytes then "
                        f"{type(exc).__name__}",
                        flush=True,
                    )

    return Handler


class Server(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: ota_server.py <firmware.ota.bin> [port]", file=sys.stderr)
        return 2
    path = pathlib.Path(sys.argv[1])
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 80
    if "factory" in path.name:
        print("refusing factory image", file=sys.stderr)
        return 2
    data = path.read_bytes()
    if not data.startswith(b"\xe9"):
        print("not an ESP-IDF application image", file=sys.stderr)
        return 2
    if len(data) > APP_SLOT + 4:
        print(
            f"image is {len(data)} bytes and does not fit the {APP_SLOT}-byte slot",
            file=sys.stderr,
        )
        return 2
    print(f"serving {path.name} ({len(data)} bytes) on port {port}", flush=True)
    Server(("0.0.0.0", port), build_handler(data)).serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
