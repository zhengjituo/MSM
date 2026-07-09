import json, os, time, threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

STATUS_FILE = "/tmp/msm_live.json"
HTML_FILE = os.path.join(os.path.dirname(__file__), "e2e_dashboard.html")

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/api/data":
            try:
                with open(STATUS_FILE) as f:
                    data = json.load(f)
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(json.dumps(data, indent=2).encode())
            except:
                self.send_response(503)
                self.end_headers()
                self.wfile.write(b'{"error":"no data"}')
        else:
            try:
                with open(HTML_FILE) as f:
                    html = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(html.encode())
            except:
                self.send_response(404)
                self.end_headers()
                self.wfile.write(b"Not found")

if __name__ == "__main__":
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8088
    server = HTTPServer(("0.0.0.0", port), Handler)
    print(f"[LiveBridge] :{port} <- {STATUS_FILE}")
    server.serve_forever()
