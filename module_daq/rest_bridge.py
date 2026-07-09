#!/usr/bin/env python3
"""Lightweight REST bridge: reads /tmp/msm_live.json, forwards MSG via IPC."""
import http.server
import json
import urllib.parse
import os
import socket

PORT = 8088
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
IPC_SOCK = "/tmp/msm_daq.sock"

def send_ipc(cmd):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(3)
        s.connect(IPC_SOCK)
        s.sendall((cmd + "\n").encode())
        resp = s.recv(4096).decode()
        s.close()
        return resp
    except Exception as e:
        return json.dumps({"error": str(e)})

def read_json():
    try:
        with open("/tmp/msm_live.json") as f:
            return f.read()
    except:
        return json.dumps({"error": "no data"})

class Handler(http.server.BaseHTTPRequestHandler):
    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path == "/api/data":
            body = read_json()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-cache")
        elif path == "/" or path == "":
            try:
                with open(os.path.join(SCRIPT_DIR, "e2e_dashboard.html"), "rb") as f:
                    body = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.wfile.write(body)
                return
            except:
                body = b"<html><body><h1>Not Found</h1></body></html>"
                self.send_response(404)
        else:
            body = json.dumps({"error": "not found"})
            self.send_response(404)
            self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        if isinstance(body, str):
            body = body.encode()
        self.wfile.write(body)

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode()
        if path == "/api/msg":
            resp = send_ipc("MSG " + body)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
        else:
            resp = json.dumps({"error": "not found"})
            self.send_response(404)
            self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(resp.encode())

print(f"REST bridge on http://localhost:{PORT}")
http.server.HTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
