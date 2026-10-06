"""Real-process cancellation and crash recovery; no external database required."""
import json
from pathlib import Path
import socket
import subprocess
import sys
import time

exe = str(Path(sys.argv[1]).resolve())
root = Path(exe).parent / f"lifecycle-{time.monotonic_ns()}"
root.mkdir()
with socket.socket() as reservation:
    reservation.bind(("127.0.0.1", 0))
    port = reservation.getsockname()[1]
server = subprocess.Popen([exe,"feedkv","--port",str(port)],stdout=subprocess.DEVNULL)

def wait_for(predicate, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            value=predicate()
            if value: return value
        except (OSError,json.JSONDecodeError): pass
        time.sleep(.02)
    raise AssertionError("condition timed out")

def command(*args):
    with socket.create_connection(("127.0.0.1",port),timeout=3) as connection:
        encoded=[str(arg).encode() for arg in args]
        connection.sendall(b"*"+str(len(encoded)).encode()+b"\r\n"+b"".join(b"$"+str(len(arg)).encode()+b"\r\n"+arg+b"\r\n" for arg in encoded))
        stream=connection.makefile("rb")
        def read():
            line=stream.readline(); kind=line[:1]; data=line[1:-2]
            if kind==b"$": return None if int(data)==-1 else stream.read(int(data)+2)[:-2]
            if kind==b"*": return [read() for _ in range(int(data))]
            if kind==b":": return int(data)
            if kind==b"-": raise RuntimeError(data.decode())
            return data
        return read()

def start(id, posts=8000, mode="closed_loop"):
    conf=root / f"{id}.conf"
    conf.write_text(f"adapter = feedkv\ndatabase_port = {port}\nusers = 32\nposts = {posts}\nfollows = 64\nhashtags = 8\nworkers = 2\nwarmup_ms = 100\nduration_ms = 30000\nmode = {mode}\noffered_rate_ops_sec = 1\noutput_dir = {root.as_posix()}\n")
    worker=subprocess.Popen([exe,"run","--config",str(conf),"--run-id",id],stdout=subprocess.DEVNULL)
    manifest=root/id/"recovery.json"
    wait_for(lambda: manifest.exists() and json.loads(manifest.read_text())["namespace_owned"])
    return worker,manifest

worker=None
try:
    wait_for(lambda: command("PING")==b"PONG")
    command("SET","benchforge:unrelated_lifecycle","preserve")
    for id, posts, mode, delay in [("cancel_load",8000,"closed_loop",0),("cancel_measure",16,"closed_loop",.6),("cancel_wait",16,"open_loop",.6)]:
        worker,manifest=start(id,posts,mode);time.sleep(delay)
        (manifest.parent/"cancel.request").write_text("cancel")
        assert worker.wait(timeout=15)==130
        summary=json.loads((manifest.parent/"summary.json").read_text())
        assert not summary["valid"] and "run cancelled" in summary["invalid_reasons"]
        assert not json.loads(manifest.read_text())["namespace_owned"]
        assert command("SCAN",0,"MATCH",f"benchforge:{id}:*","COUNT",100000)[1]==[]
    worker,manifest=start("crash_recover")
    assert subprocess.run([exe,"recover","--manifest",str(manifest)],capture_output=True).returncode!=0,"live-worker recovery accepted"
    worker.kill();worker.wait(timeout=5)
    assert json.loads(manifest.read_text())["namespace_owned"]
    subprocess.run([exe,"recover","--manifest",str(manifest)],check=True,timeout=15)
    subprocess.run([exe,"recover","--manifest",str(manifest)],check=True,timeout=15)
    assert command("SCAN",0,"MATCH","benchforge:crash_recover:*","COUNT",100000)[1]==[]
    assert command("GET","benchforge:unrelated_lifecycle")==b"preserve"
    print("Loading, measurement and scheduled-wait cancellation; live-worker refusal; killed-worker recovery and idempotent cleanup passed")
finally:
    if worker is not None and worker.poll() is None: worker.kill();worker.wait()
    server.kill();server.wait(timeout=5)
