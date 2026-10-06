import importlib.util
from pathlib import Path
spec=importlib.util.spec_from_file_location("suite",Path(__file__).resolve().parents[1]/"scripts"/"benchmark_suite.py")
suite=importlib.util.module_from_spec(spec);spec.loader.exec_module(suite)
def fixture(id,rate):
    return {"run_id":id,"adapter":"feedkv","scenario":"normal","mode":"closed_loop","valid":True,"total_errors":0,"total_timeouts":0,"telemetry_dropped":0,"measured_ms":1000,"total_operations":rate,"cleanup_status":"run namespace removed","operations":[{"type":"timeline_read","count":rate, "ops_per_sec":rate,"p50_ns":rate,"p95_ns":rate*2,"p99_ns":rate*3,"p999_ns":rate*4,"send_lag_p95_ns":0}]}
runs=[fixture("a",10),fixture("b",100),fixture("c",30),fixture("bad",999)]
runs[-1]["valid"]=False
row=suite.summarize(runs)[0]
assert row["valid_repetitions"]==3 and row["excluded_run_ids"]==["bad"]
assert row["ops_per_sec"]=={"median":30,"minimum":10,"maximum":100}
assert row["operations"]["timeline_read"]["p95_ns"]["median"]==60
for run in runs: run["valid"]=False
assert suite.summarize(runs)[0]["ops_per_sec"] is None
print("Suite aggregation excludes invalid runs and reports per-run median/minimum/maximum")
