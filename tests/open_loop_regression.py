"""Exercise real worker scheduling and reject impossible unsigned send-lag values."""
import json
from pathlib import Path
import subprocess
import sys
import time

def main():
    # Retain diagnostic artifacts in the ignored build directory.
    test_parent = Path(sys.argv[1]).resolve().parent
    root = test_parent / ("scheduler-regression-" + str(time.monotonic_ns()))
    root.mkdir()
    assert root.resolve().is_relative_to(test_parent), "test directory escaped build directory"
    config = root / "scheduled.conf"
    config.write_text("\n".join([
        "adapter = noop", "mode = open_loop", "offered_rate_ops_sec = 101",
        "workers = 3", "duration_ms = 200", "warmup_ms = 10",
        "users = 8", "posts = 16", "follows = 16", "hashtags = 4",
        f"output_dir = {root.as_posix()}",
    ]), encoding="utf-8")
    for index in range(3):
        run_id = f"scheduler_regression_{index}"
        start = time.monotonic_ns()
        subprocess.run([sys.argv[1], "run", "--config", str(config), "--run-id", run_id],
                       check=True, capture_output=True, text=True, timeout=15)
        wall_ns = time.monotonic_ns() - start
        result = json.loads((root / run_id / "summary.json").read_text(encoding="utf-8"))
        assert result["valid"] and result["total_operations"] > 0, result
        for operation in result["operations"]:
            for field in ("max_send_lag_ns", "send_lag_p50_ns", "send_lag_p95_ns", "send_lag_p99_ns"):
                assert 0 <= operation[field] <= wall_ns, (field, operation, wall_ns)
    print("Open-loop lag remained within measured wall time across three worker runs")


if __name__ == "__main__":
    main()
