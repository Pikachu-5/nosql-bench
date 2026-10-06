"""An overloaded open-loop run includes backlog drain in throughput time."""
import json
from pathlib import Path
import socket
import socketserver
import subprocess
import sys
import threading
import time
exe=str(Path(sys.argv[1]).resolve())
root=Path(exe).parent/f"elapsed-{time.monotonic_ns()}";root.mkdir()
with socket.socket() as sock:
    sock.bind(("127.0.0.1",0));server_port=sock.getsockname()[1]
server=subprocess.Popen([exe,"feedkv","--port",str(server_port)],stdout=subprocess.DEVNULL)
def read_value(stream):
    line=stream.readline()
    if not line:raise EOFError()
    if line[:1]==b"*":
        parts=[read_value(stream) for _ in range(int(line[1:-2]))]
        return line+b"".join(part[0] for part in parts),[part[1] for part in parts]
    if line[:1]==b"$":
        size=int(line[1:-2]);data=stream.read(size+2) if size>=0 else b""
        return line+data,data[:-2] if size>=0 else None
    return line,line[1:-2]
class Proxy(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            with socket.create_connection(("127.0.0.1",server_port),timeout=5) as target:
                incoming=self.request.makefile("rb");outgoing=target.makefile("rb")
                while True:
                    encoded,args=read_value(incoming)
                    target.sendall(encoded);response,_=read_value(outgoing)
                    if args[0] in (b"GET",b"ZREVRANGE"):time.sleep(.015)
                    self.request.sendall(response)
        except (EOFError,OSError):pass
class ProxyServer(socketserver.ThreadingTCPServer):daemon_threads=True
proxy=ProxyServer(("127.0.0.1",0),Proxy)
thread=threading.Thread(target=proxy.serve_forever,daemon=True);thread.start()
try:
    deadline=time.monotonic()+10
    while True:
        try:
            with socket.create_connection(("127.0.0.1",server_port),timeout=.2):break
        except OSError:
            if time.monotonic()>deadline:raise
            time.sleep(.02)
    config=root/"backlog.conf"
    config.write_text(f"adapter = feedkv\ndatabase_port = {proxy.server_address[1]}\nmode = open_loop\noffered_rate_ops_sec = 200\nworkers = 1\nwarmup_ms = 0\nduration_ms = 200\nusers = 8\nposts = 16\nfollows = 16\nhashtags = 4\noutput_dir = {root.as_posix()}\n")
    started=time.monotonic()
    subprocess.run([exe,"run","--config",str(config),"--run-id","backlog"],check=True,capture_output=True,timeout=25)
    wall_ms=(time.monotonic()-started)*1000
    summary=json.loads((root/"backlog"/"summary.json").read_text())
    assert summary["valid"] and summary["total_operations"]==40
    assert 400<summary["measured_ms"]<wall_ms,"backlog drain excluded from elapsed measurement"
    for op in summary["operations"]:
        assert abs(op["ops_per_sec"]-op["count"]*1000/summary["measured_ms"])<.01
    print("Backlog drain uses actual elapsed time and retains all 40 scheduled requests")
finally:
    proxy.shutdown();proxy.server_close();server.kill();server.wait(timeout=5)
