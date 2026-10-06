"""Load verified committed evidence into the local read-only Analysis archive."""
import hashlib
import json
from pathlib import Path
import shutil
import sys
from verify_evidence import verify

source=Path(sys.argv[1]).resolve()
verify(source)
manifest=json.loads((source/"experiment.json").read_text(encoding="utf-8"))
archive=Path(__file__).resolve().parents[1]/"runs"/"experiments"
archive.mkdir(parents=True,exist_ok=True)
for record in manifest["runs"]:
    target=archive/record["run_id"]
    assert target.resolve().parent==archive.resolve(),"archive destination escaped its root"
    if target.exists():
        for name in ("summary.json","operations.csv"):
            assert (target/name).is_file() and hashlib.sha256((target/name).read_bytes()).digest()==hashlib.sha256((source/record["run_id"]/name).read_bytes()).digest(),f"existing capture differs: {target}"
        continue
    target.mkdir()
    for name in ("summary.json","operations.csv"):
        shutil.copy2(source/record["run_id"]/name,target/name)
print(f"Imported {len(manifest['runs'])} verified captures into {archive}")
