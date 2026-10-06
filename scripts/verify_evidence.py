"""Verify completed benchmark evidence without trusting its aggregate or flags."""
import csv
import hashlib
import json
from pathlib import Path
import sys
import re
from benchmark_suite import summarize,write_json

def verify(root):
    manifest=json.loads((root/"experiment.json").read_text(encoding="utf-8"))
    assert manifest["state"]=="completed","experiment is unfinished"
    assert manifest["repetitions"]>=3
    records=manifest["runs"]
    assert len(records)==len(manifest["adapters"])*4*manifest["repetitions"]
    assert len({record["run_id"] for record in records})==len(records)
    runs=[]
    for record in records:
        assert re.fullmatch(r"[A-Za-z0-9_-]{1,64}",record["run_id"]),"unsafe capture identity"
        directory=root/record["run_id"]
        for field,file in [("summary_sha256","summary.json"),("resources_sha256","resources.json")]:
            assert hashlib.sha256((directory/file).read_bytes()).hexdigest()==record[field],f"hash mismatch: {directory/file}"
        run=json.loads((directory/"summary.json").read_text(encoding="utf-8"))
        resources=json.loads((directory/"resources.json").read_text(encoding="utf-8"))
        assert run["run_id"]==record["run_id"] and run["experiment_id"]==manifest["experiment_id"]
        assert run["valid"] and run["experiment_status"]=="completed" and not run["invalid_reasons"]
        assert run["total_errors"]==run["total_timeouts"]==run["telemetry_dropped"]==0
        assert run["total_operations"]>0 and run["measured_ms"]>=run["config"]["duration_ms"]
        assert run["measurement_method"]=="elapsed_wall_time_readonly_warmup_v2"
        assert "failed" not in run["cleanup_status"] and resources["exit_code"]==0 and resources["samples"]
        assert len(run["operations"])==6 and len({op["type"] for op in run["operations"]})==6
        assert sum(op["count"] for op in run["operations"])==run["total_operations"]
        with (directory/"operations.csv").open(encoding="utf-8",newline="") as stream:
            rows=list(csv.DictReader(stream))
        assert len(rows)==6
        for op,row in zip(run["operations"],rows):
            assert op["type"]==row["operation"]
            assert op["count"]==int(row["count"]) and op["errors"]==op["timeouts"]==0
            assert int(row["errors"])==int(row["timeouts"])==0
            assert abs(op["ops_per_sec"]-op["count"]*1000/run["measured_ms"])<.0011
            assert 0<=op["min_ns"]<=op["p50_ns"]<=op["p95_ns"]<=op["p99_ns"]<=op["p999_ns"]
            assert op["max_ns"]>=op["min_ns"]
            assert 0<=op["max_send_lag_ns"]<(run["warmup_ms"]+run["measured_ms"])*1000000
        runs.append(run)
    for adapter,settings in manifest["adapters"].items():
        limits=settings["host_config"]
        assert limits["NanoCpus"]==2000000000 and limits["Memory"]==limits["MemorySwap"]==2147483648
        assert (root/f"{adapter}-verification.txt").is_file()
    for scenario in ("normal","celebrity"):
        open_runs=[run for run in runs if run["scenario"]==scenario and run["mode"]=="open_loop"]
        vectors=[[(op["type"],op["count"]) for op in run["operations"]] for run in open_runs]
        assert all(vector==vectors[0] for vector in vectors),"offered request streams differ across repetitions/adapters"
        assert all(run["total_operations"]==manifest["offered_rate_ops_sec"]*manifest["duration_ms"]//1000 for run in open_runs)
    aggregate=summarize(runs)
    assert all(group["valid_repetitions"]==manifest["repetitions"] for group in aggregate)
    assert aggregate==json.loads((root/"aggregate.json").read_text(encoding="utf-8")),"aggregate arithmetic mismatch"
    result={"experiment_id":manifest["experiment_id"],"valid_runs":len(runs),"groups":len(aggregate),"checks":"raw hashes, CSV/JSON counts, throughput arithmetic, latency/lag bounds, cleanup, resource caps, equal offered streams and regenerated aggregates"}
    write_json(root/"verification.json",result)
    return result
if __name__=="__main__":print(json.dumps(verify(Path(sys.argv[1]).resolve()),indent=2))
