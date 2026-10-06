"""Exercise graceful cancellation through the actual loopback HTTP API."""
from pathlib import Path
import json
import shutil
import socket
import subprocess
import sys
import time
from urllib.parse import urlencode
from urllib.request import Request,urlopen

original=Path(sys.argv[1]).resolve()
root=original.parent/f"api-lifecycle-{time.monotonic_ns()}"
(root/"build").mkdir(parents=True)
(root/"config").mkdir()
(root/"config"/"default.conf").write_text("adapter = noop\n")
exe=root/"build"/original.name;shutil.copy2(original,exe)
def port():
    with socket.socket() as s: s.bind(("127.0.0.1",0));return s.getsockname()[1]
api_port=port();db_port=port()
server=subprocess.Popen([str(exe),"feedkv","--port",str(db_port)],stdout=subprocess.DEVNULL)
api=subprocess.Popen([str(exe),"serve","--port",str(api_port)],stdout=subprocess.DEVNULL)
def request(path,data=None):
    req=Request(f"http://127.0.0.1:{api_port}{path}",data=data,headers={"Content-Type":"application/x-www-form-urlencoded"})
    with urlopen(req,timeout=5) as response:return json.load(response)
def wait(predicate,timeout=20):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        try:
            if result:=predicate():return result
        except OSError:pass
        time.sleep(.05)
    raise AssertionError("API condition timed out")
id=None
try:
    wait(lambda:request("/health"))
    data=urlencode({"adapter":"feedkv","database_port":db_port,"workers":2,"duration_ms":30000,"warmup_ms":100,"users":32,"posts":128,"follows":64,"hashtags":8}).encode()
    run=request("/api/runs",data);id=run["runId"]
    manifest=root/"runs"/"api"/id/"recovery.json"
    wait(lambda:manifest.exists() and json.loads(manifest.read_text())["namespace_owned"])
    response=request(f"/api/runs/{id}/cancel",b"")
    assert response["status"]=="cancelling"
    run=wait(lambda:(r if (r:=request(f"/api/runs/{id}"))["status"]=="cancelled" else None))
    assert run["exitCode"]==130 and not run["error"]
    summary=request(f"/api/runs/{id}/results")
    assert not summary["valid"] and summary["cleanup_status"]=="run namespace removed"
    assert not json.loads(manifest.read_text())["namespace_owned"]
    archive=request("/api/results")
    assert any(item["runId"]==id and not item["valid"] for item in archive["results"])
    print("HTTP cancel remains responsive, saves invalid diagnostic results and cleans the run namespace")
finally:
    if id is not None:
        directory=root/"runs"/"api"/id
        directory.mkdir(parents=True,exist_ok=True)
        (directory/"cancel.request").write_text("cancel")
    api.kill();api.wait(timeout=5)
    server.kill();server.wait(timeout=5)
