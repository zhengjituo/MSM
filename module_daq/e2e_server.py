#!/usr/bin/env python3
"""Server: receives binary frames, serves JSON to browser."""
import http.server
import json
import urllib.parse
import os
import struct
from socketserver import ThreadingMixIn

class ThreadingServer(ThreadingMixIn, http.server.HTTPServer):
    allow_reuse_address = True

PORT = 8088
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
latest_bin = b''
received_count = 0
prev_t_us = 0
batch_interval_us = 0


def parse_binary(data):
    """Parse binary MSM1 format → JSON-serializable dict."""
    if len(data) < 28:
        return {}
    magic = data[0:4]
    if magic != b'MSM1':
        return {}
    hdr_len = struct.unpack_from('<H', data, 4)[0]
    num_ch = struct.unpack_from('<H', data, 6)[0]
    batch_size = struct.unpack_from('<I', data, 8)[0]
    sent_seq = struct.unpack_from('<Q', data, 12)[0]
    t_us = struct.unpack_from('<Q', data, 20)[0]

    result = {
        "sent_seq": sent_seq,
        "t_us": t_us,
        "batch_size": batch_size,
        "channels": []
    }

    off = hdr_len
    for ch in range(num_ch):
        ch_data = {"ch": ch}
        if off + 2 > len(data):
            break
        num_vars = struct.unpack_from('<H', data, off)[0]
        off += 2
        for v in range(num_vars):
            if off + 2 > len(data):
                break
            vtype = data[off]
            name_len = data[off + 1]
            off += 2
            if off + name_len > len(data):
                break
            name = data[off:off + name_len].decode()
            off += name_len
            if vtype == 0:  # U32
                cnt = min((len(data) - off) // 4, batch_size)
                arr = list(struct.unpack_from(f'<{cnt}I', data, off))
                off += cnt * 4
                ch_data[name] = arr
            elif vtype == 1:  # U64
                cnt = min((len(data) - off) // 8, batch_size)
                arr = list(struct.unpack_from(f'<{cnt}Q', data, off))
                off += cnt * 8
                ch_data[name] = arr
            elif vtype == 2:  # DBL
                cnt = min((len(data) - off) // 8, batch_size)
                arr = list(struct.unpack_from(f'<{cnt}d', data, off))
                off += cnt * 8
                ch_data[name] = arr
        result["channels"].append(ch_data)
    return result


class Handler(http.server.BaseHTTPRequestHandler):
    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS")
        self.end_headers()

    def do_POST(self):
        global latest_bin, received_count, prev_t_us, batch_interval_us
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        latest_bin = body

        # Parse header for timing only
        if len(body) >= 28 and body[0:4] == b'MSM1':
            t_us = struct.unpack_from('<Q', body, 20)[0]
            if prev_t_us and t_us:
                batch_interval_us = t_us - prev_t_us
            prev_t_us = t_us

        received_count += 1
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(b'{"ok": true}')

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path == "/api/data":
            parsed = parse_binary(latest_bin) if latest_bin else {}
            parsed["received_count"] = received_count
            parsed["batch_interval_us"] = batch_interval_us
            body = json.dumps(parsed).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(body)
        elif path == "/api/data.bin":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(latest_bin)
        elif path == "/":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            with open(os.path.join(SCRIPT_DIR, "e2e_dashboard.html"), "rb") as f:
                self.wfile.write(f.read())
        else:
            self.send_response(404)
            self.end_headers()


print(f"Server on http://localhost:{PORT}")
ThreadingServer(("0.0.0.0", PORT), Handler).serve_forever()
