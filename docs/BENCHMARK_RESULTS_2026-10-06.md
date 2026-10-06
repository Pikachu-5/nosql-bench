# BenchForge local benchmark results — 2026-10-06

Experiment `bf_20261006_v2` completed **60 valid captures: five adapters × two scenarios × two load modes × three repetitions**. Every capture reported zero measured errors, timeouts and dropped telemetry, with successful namespace cleanup. These results describe a small, single-host synthetic workload; they do not establish a general database ranking.

FeedKV and Redis achieved similar throughput. FeedKV's normal closed-loop median was 2,549.6 ops/s versus Redis's 2,473.0; celebrity medians were 2,132.4 versus 2,232.2. Their ranges overlap in both scenarios and the direction changes. Three short repetitions do not establish a statistically reliable FeedKV advantage. All adapters completed the 100 ops/s offered load; small differences in achieved rate reflect the measured drain window rather than rejected requests.

## Method and provenance

- Windows x86_64 host `AYUB`, 22 logical processors, 16,639,741,952 bytes physical memory. The Release C++ worker ran on Windows; each database server ran in Docker Desktop's Linux VM, with its port published only on loopback.
- One database container at a time; each capped at two CPUs and 2 GiB memory with no additional swap. These are quotas, not dedicated or pinned CPU cores. Native service processes and ordinary host activity were not isolated.
- Two workers, seed 42, 64 users, 256 initial posts, 128 follows and eight hashtags. Operation weights: timeline read 40%, profile read 25%, like 15%, create 10%, hashtag search 5%, follow 5%. Celebrity workload biases 10% of post creation toward user 0 and changes the seeded follower distribution.
- One second of alternating, read-only timeline/profile warm-up. The generator restarts at the configured seed before measurement, preserving the initial dataset and measured random stream. Each repetition uses a fresh worker and namespace, while the database process remains warm. Adapter and profile ordering is fixed rather than randomized.
- Five-second requested measurement window. `measured_ms` includes completion of in-flight requests and open-loop backlog. Throughput is completed operations divided by actual elapsed wall time. Method: `elapsed_wall_time_readonly_warmup_v2`.
- Open-loop offers 100 operations/s across both workers. Each of the 30 open-loop captures completed all 500 requests. Operation-type counts were identical across adapters and repetitions within each scenario; concurrent execution order and mutation interleaving can differ. Closed-loop counts and resulting dataset growth vary with throughput.
- Operation latency measures service time; scheduled send lag is reported separately and is not included in that latency. Histogram percentiles are approximate upper bucket bounds (16 subdivisions per power of two), so a percentile can exceed the exact recorded maximum. Calibration is recorded and never subtracted from latency.
- Native source snapshot: [`e07074e49d4f468c8e4d5a4dc0dd3b7e100eb922`](https://github.com/Pikachu-5/nosql-bench/tree/e07074e49d4f468c8e4d5a4dc0dd3b7e100eb922). The manifest retains the capture-time commit ID and records its equivalent in the current history; native source content is unchanged. The working tree was clean at capture start. Windows executable SHA-256: `d3340cca0ab93d589e467d637b48684542d2ee4842b049729351c65725fa9766`. Build flags and Docker image IDs/digests are recorded in [experiment.json](../evidence/benchmarks/bf_20261006_v2/experiment.json). Subsequent commits add this report, evidence import/verification and reproducibility pins; they do not change the measured workload.

## Server settings and semantics

| Adapter/version | Storage, client and durability |
| --- | --- |
| FeedKV 0.2 | Purpose-built C++ RESP2 subset, volatile in-memory, materialized feed and hashtag indexes; one process-wide store. Linux container built with GCC 14, C++20, `-O3 -DNDEBUG -pthread`. |
| Redis 7.4.11 | Same RESP workload and materialized model; RDB save disabled, AOF disabled, 1,536 MiB `maxmemory`, `noeviction`. |
| MongoDB 8.0.32 | Native OP_MSG/BSON, indexed documents and read-time timeline aggregation; native WiredTiger/server defaults. Write acknowledgement and persistence details are in each summary. |
| Cassandra 4.1.12 | Native CQL v4 prepared statements, query-specific partitions, RF=1, consistency ONE, durable writes, three-statement logged batch for post creation and read-time timeline merge. Heap 768 MiB, young generation 128 MiB. |
| Neo4j 5.26.31 community | HTTP Query API v2, native relationships and indexed node lookup; server-managed persistence. Heap initial/max 512 MiB; page cache 256 MiB. |

FeedKV and Redis share the protocol, workload representation and volatile configuration. The other adapters retain their native models, client protocols and durability settings. Their figures compare the implemented workload paths, including adapter/server work; they are not pure engine microbenchmarks. Indexed query-plan summaries and exact storage settings accompany the raw captures.

## Achieved throughput

Each cell reports the median **ops/s (minimum–maximum)** across three independent runs. Open-loop throughput near 100 ops/s reflects the fixed offered load, not maximum capacity.

| Adapter | Normal closed loop | Celebrity closed loop | Normal open loop | Celebrity open loop |
| --- | --- | --- | --- | --- |
| FeedKV | 2,549.6 (2,419.8–2,635.3) | 2,132.4 (1,956.4–2,327.1) | 100.0 (100.0–100.0) | 99.9 (99.8–100.0) |
| Redis | 2,473.0 (2,436.2–2,559.6) | 2,232.2 (1,959.6–2,322.2) | 100.0 (99.9–100.0) | 100.0 (99.9–100.0) |
| MongoDB | 2,085.6 (1,897.6–2,181.6) | 2,005.4 (1,744.8–2,127.8) | 99.9 (99.8–100.0) | 100.0 (99.9–100.0) |
| Cassandra | 207.5 (167.3–208.7) | 321.8 (275.7–321.9) | 99.9 (99.8–100.0) | 99.9 (99.9–99.9) |
| Neo4j | 325.3 (252.6–386.1) | 603.5 (471.7–678.3) | 99.9 (99.9–100.0) | 99.8 (99.8–99.9) |

## Per-operation latency

The table reports **median of per-run p95 latency in milliseconds**, at the normal closed-loop profile. It does not pool samples. Complete p50/p95/p99/p99.9, operation counts, throughput and send-lag medians/ranges for all 20 groups are in [aggregate.json](../evidence/benchmarks/bf_20261006_v2/aggregate.json); raw JSON and CSV remain authoritative.

| Adapter | Timeline | Profile | Like | Create | Hashtag | Follow |
| --- | --- | --- | --- | --- | --- | --- |
| FeedKV | 1.180 | 0.508 | 0.524 | 1.769 | 1.180 | 2.359 |
| Redis | 1.573 | 0.754 | 0.721 | 2.359 | 1.573 | 3.146 |
| MongoDB | 2.032 | 1.114 | 1.245 | 1.114 | 1.376 | 1.311 |
| Cassandra | 31.457 | 2.490 | 4.456 | 3.146 | 41.943 | 2.753 |
| Neo4j | 22.020 | 29.360 | 30.409 | 35.652 | 24.117 | 30.409 |

For example, normal closed-loop Cassandra timeline and hashtag reads perform per-author/partition work, whereas RESP adapters read materialized indexes. Neo4j uses a separate HTTP query path, and its measurements changed substantially as its process warmed. These observations explain why protocol, model and ordering must accompany the numbers; the experiment does not isolate their causal contributions.

## Evidence and verification

The committed [evidence directory](../evidence/benchmarks/bf_20261006_v2/) contains the experiment manifest, 60 raw summary/CSV pairs, per-run CPU/memory observations, exact configurations, native integration-test output for all five adapters, server logs, available GC logs and regenerated aggregates. Per-run observations span the entire worker lifetime, including loading, and are not measurement-only resource consumption. Cassandra's default GC log is retained; Neo4j's default GC log was not readable and that limitation is recorded. GC pauses were not isolated. A reliable per-record memory footprint cannot be inferred from these coarse container samples.

The verifier checked summary/resource hashes, CSV/JSON count agreement, elapsed-throughput arithmetic, latency/lag bounds, zero errors/timeouts/dropped telemetry, successful cleanup, resource caps, equal offered streams and regenerated aggregate equality: **60 valid runs, 20 groups**. [verification.json](../evidence/benchmarks/bf_20261006_v2/verification.json) records that result. Hashes detect changed files; they are not an independent attestation of capture provenance.

Default CTest passed all nine checks: archive boundaries, CQL/Neo4j parsing, HTTP framing, real-process cancellation/recovery, API cancellation, suite aggregation, elapsed-time accounting and scheduled send lag. Live integration executables passed on each of the five actual database servers for normal and celebrity scenarios, verifying deterministic seeds, operations, top20 ordering, idempotence, namespace collision/ownership and cleanup. The Blazor build passed with zero warnings/errors, and comparison checks passed for eligibility, mismatches and repetition medians/ranges.

Browser verification covered three accepted repetitions per adapter, rapid selection changes, exclusion of the unfinished earlier experiment, keyboard/horizontal table scrolling, no page overflow at desktop and 390px viewport widths, API offline/recovery, readable startup connection-failure summaries with zero-sample latency withheld, and graceful run cancellation with a retained invalid summary and successful cleanup. The earlier [bf_20261006 experiment](../evidence/benchmarks/bf_20261006/experiment.json) was aborted after transport stalls exposed a Nagle/delayed-ACK interaction. Those diagnostic captures are preserved and excluded; the final experiment uses TCP_NODELAY at the RESP client and FeedKV server.

## Reproduce and inspect

From a fresh clone, verify the committed evidence and import it into the local Analysis archive:

```powershell
python scripts/verify_evidence.py evidence/benchmarks/bf_20261006_v2
python scripts/import_experiment.py evidence/benchmarks/bf_20261006_v2
.\build\benchforge.exe serve
dotnet run --project ui\BenchForge.UI.csproj --launch-profile http
```

Open `http://localhost:5180/analysis` and select two matching captures from `bf_20261006_v2`. Import is idempotent for identical files and refuses differing existing captures. Build instructions are in [README.md](../README.md).

To collect new measurements with Docker Desktop running and the listed loopback ports free:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
docker build -f Dockerfile.benchmark -t benchforge-feedkv:local .
python scripts/benchmark_suite.py --experiment benchmark_local_001 --repetitions 3 --duration-ms 5000 --rate 100
python scripts/verify_evidence.py evidence/benchmarks/benchmark_local_001
```

Use a fresh experiment ID on every invocation. The driver pins official server images to the digests recorded here and checks native integrations before measurements. FeedKV's local build and its image ID are captured afresh. Compilation and timing are environment-dependent; repeating these commands does not guarantee identical performance or binary/image hashes. The suite removes only its own containers and anonymous volumes, preserves failed evidence, and labels an incomplete experiment accordingly.

## Limits

This experiment uses a small dataset, one laptop, single-node deployments and three five-second repetitions. It does not measure large-dataset scaling, distributed behavior, sustained saturation, durable RESP performance or independent statistical confidence. Fixed ordering, shared host activity, warm server caches/JIT and low tail sample counts limit interpretation. Some p99.9 estimates have too few samples to be useful. No FeedKV speedup percentage should be presented as a general result.

Further scaling and distributed experiments require additional profiles and evidence.
