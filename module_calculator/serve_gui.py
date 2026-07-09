#!/usr/bin/env python3
"""Serve the MSM dashboard HTML and proxy REST API with CORS headers."""

import http.server
import http.client
import os
import json

REST_HOST = "localhost"
REST_PORT = 8080
PROXY_PORT = 8000
DIR = os.path.dirname(os.path.abspath(__file__))


def rest_request(method, path, body=None):
    conn = http.client.HTTPConnection(REST_HOST, REST_PORT, timeout=5)
    try:
        headers = {"Connection": "close"}
        if body is not None:
            headers["Content-Type"] = "text/plain"
        conn.request(method, path, body=body, headers=headers)
        resp = conn.getresponse()
        data = resp.read()
        return resp.status, data
    finally:
        conn.close()


class Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/api/"):
            status, data = rest_request("GET", self.path)
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        if self.path == "/" or self.path == "/index.html":
            self.path = "/msm_dashboard.html"
        return super().do_GET()

    def do_PUT(self):
        if self.path.startswith("/api/"):
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length) if length else b""
            status, data = rest_request("PUT", self.path, body)
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        self.send_error(405)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, PUT, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Max-Age", "86400")
        self.end_headers()

    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    os.chdir(DIR)
    server = http.server.HTTPServer(("0.0.0.0", PROXY_PORT), Handler)
    print(f"MSM GUI at http://localhost:{PROXY_PORT}")
    server.serve_forever()
