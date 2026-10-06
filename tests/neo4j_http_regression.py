"""Exercise HTTP response fragmentation, chunking, persistence, and 202 query errors."""
import http.server
import json
import subprocess
import sys
import threading

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *args):
        pass
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert self.path == "/db/neo4j/query/v2"
        error = request["statement"] == "invalid"
        value = request["parameters"].get("value", 0)
        body = json.dumps({"errors": [{"code": "query failed"}]} if error else
                          {"data": {"fields": ["value"], "values": [[value]]}}).encode()
        self.send_response(202)
        if value == 1 and not error:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for part in (body[:13], body[13:]):
                self.wfile.write(f"{len(part):x};fixture=yes\r\n".encode() + part + b"\r\n")
                self.wfile.flush()
            self.wfile.write(b"0\r\nX-Fixture: complete\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            for part in (body[:7], body[7:]):
                self.wfile.write(part)
                self.wfile.flush()

with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    result = subprocess.run([sys.argv[1], str(server.server_port)], timeout=20)
    server.shutdown()
    sys.exit(result.returncode)
