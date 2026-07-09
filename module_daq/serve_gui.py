#!/usr/bin/env python3
"""Development server for MSM DAQ Web GUI."""

import http.server
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=SCRIPT_DIR, **kwargs)

    def log_message(self, fmt, *args):
        print(f"[GUI] {args[0]} {args[1]} {args[2]}")


if __name__ == "__main__":
    httpd = http.server.HTTPServer(("0.0.0.0", PORT), Handler)
    print(f"[GUI] Serving {SCRIPT_DIR} on http://localhost:{PORT}")
    print("[GUI] The MSM DAQ sim must be running on port 8080 for API calls")
    httpd.serve_forever()
