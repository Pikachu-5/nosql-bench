# BenchForge

BenchForge is a standalone C++ cross-database benchmark platform. The social-feed domain is a synthetic workload used to compare database designs; BenchForge is not a social network. The product pairs a local C++ control API and C# dashboard with FeedKV, a purpose-built database under test, and Redis, MongoDB, Cassandra and Neo4j comparison targets. The only planned cloud target is an optional small Firestore benchmark using synthetic data.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the durable scope, decisions, implementation phases, and GCP quota guardrails.

## Current implementation

Phase 1 provides a C++20 command-line harness with configuration validation, seeded operation generation, a pluggable adapter boundary, a no-op calibration adapter, per-operation latency histograms, and JSON/CSV run output. Phase 2 adds a loopback C++ control API that launches isolated worker processes. The no-op adapter measures harness overhead only; its throughput and latency are not database performance results.

The C# Blazor WebAssembly dashboard configures and launches runs, polls run state, supports cancellation, and displays per-operation summaries. Phase 3 adds an in-memory FeedKV database using a documented RESP2 subset and a Redis adapter that uses the same client protocol, workload operations, deterministic data loader, and run-scoped key cleanup. Phase 4 adds closed-loop and scheduled-rate/open-loop execution, latency and send-lag histograms, transport calibration, timeout and invalid-run reporting, captured host environment metadata, and matching dashboard controls. Phase 5 adds MongoDB over its native OP_MSG/BSON protocol, with run-specific databases, deterministic data loading, native indexes, idempotent follow updates, a read-time timeline aggregation, and query-planner summaries. Cassandra adds a native CQL v4 adapter with prepared statements, query-specific partitions, and run-owned keyspace cleanup. Neo4j completes local adapter coverage using HTTP Query API v2 and indexed native relationships. All five adapters passed native integration checks and three repeated captures for each normal/celebrity and closed/open-loop profile.

Local phases **1–6 and 8 are complete**; Phase 7 is excluded. The [results report](docs/BENCHMARK_RESULTS_2026-10-06.md) contains **60 valid captures across 20 configurations**, each with three repetitions, zero measured errors/timeouts and successful cleanup. FeedKV and Redis ranges overlap and their lead changes by scenario; no universal performance advantage is established.

## Build

On Windows with MinGW:

```powershell

cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel

```

The UI targets .NET 9 and requires NuGet package restore:

```powershell

dotnet restore ui\BenchForge.UI.csproj

dotnet run --project ui\BenchForge.UI.csproj --launch-profile http

```

Run `benchforge serve` in another terminal. The UI is served at `http://localhost:5180` and talks to the loopback control API at `http://127.0.0.1:8080`.

## Use the current harness

```powershell

.\build\benchforge.exe list-adapters

.\build\benchforge.exe validate --config config\default.conf

.\build\benchforge.exe run --config config\default.conf

```

The adapter list contains `noop`, `feedkv`, `redis`, `mongo`, `cassandra` and `neo4j`. Runs write `summary.json` and `operations.csv` beneath `runs/<run-id>/`. Start the control API with `.\build\benchforge.exe serve`; it binds to `127.0.0.1:8080` and stores UI-created runs beneath `runs/api/<run-id>/`.

The default `mode = closed_loop` lets each worker start its next operation after the previous one completes. For scheduled-rate load, set `mode = open_loop` and choose `offered_rate_ops_sec`; the rate is shared across all workers. Open-loop reports include send-lag percentiles separately from operation latency. `summary.json` records validity and invalid reasons, timeout counts, transport calibration, and machine environment.

For MongoDB, run a local MongoDB 5.0+ server on `127.0.0.1:27017` with authentication disabled. The current adapter supports local unauthenticated connections only. Each benchmark creates a dedicated `benchforge_run_<hash>` database, records its indexes, write concern, and query-planner summaries in `summary.json`, then drops that database during cleanup. The MongoDB server must allow database creation and deletion.

For Cassandra, use a single local Cassandra 4.1+ node on `127.0.0.1:9042` with authentication disabled. The native CQL v4 adapter prepares statements before measurement, creates a dedicated `benchforge_run_<hash>` keyspace, and refuses an existing keyspace with the same name. Only its owning loader removes it after a completed run. The user must be allowed to create/drop keyspaces and read `system.local`. TLS, authentication, cluster discovery/failover, and automatic retries are outside this milestone.

The schema uses profiles/posts by ID, follows by source, likes by post/user, and posts by author/tag with descending timestamp/ID clustering. Timeline reads merge each followed author's newest 20 posts, then fetch the newest 20 records. Follow and like primary keys provide idempotence; a like count is read from its partition. Post creation uses a logged batch across the post and two indexes. These multi-partition reads do not promise transaction isolation. Replication is `SimpleStrategy` RF=1, read/write consistency is `ONE`, durable writes are enabled, and tables use size-tiered compaction. Unbucketed author/tag partitions are intended for laptop-scale datasets. Heap, GC, commitlog sync, and compaction activity should be recorded separately for comparative runs; CQL cannot report all of them.

With Docker Desktop running, a bounded local smoke setup is:

```powershell

docker run -d --name benchforge-cassandra --memory 2g --cpus 2 -p 127.0.0.1:9042:9042 -e MAX_HEAP_SIZE=768M -e HEAP_NEWSIZE=128M cassandra:4.1

docker exec benchforge-cassandra cqlsh -e "SELECT release_version FROM system.local;"

.\build\benchforge.exe run --config config\cassandra-smoke.conf

```

Wait until the CQL readiness query succeeds before running the harness. Use the supplied smoke config before increasing dataset sizes. `config/cassandra-celebrity-smoke.conf` exercises the celebrity workload in closed-loop mode. The tag can change; record the resolved image digest and database version when measuring.

Cancellation is cooperative: the worker finishes its current operation, cleans the loader-owned namespace and writes an invalid diagnostic summary. Loading batches and scheduled waits also observe cancellation. The API allows 30 seconds before forced termination. Every worker writes `recovery.json` before loading, records ownership after acquiring its unique namespace, and clears ownership only after cleanup succeeds. After a crash, run `benchforge recover --manifest runs/api/<run-id>/recovery.json`. Recovery refuses a living recorded worker, validates the local endpoint and run identity, and cleans only that namespace; repeated successful recovery is harmless. If a process dies between namespace acquisition and journal commit, ownership may require manual inspection. Never use wildcard database/keyspace deletion.

## Verification

```powershell

ctest --test-dir build --output-on-failure

.\build\cassandra_integration.exe

```

CTest runs CQL parser checks and, when Python is available, an open-loop scheduling regression. The explicit integration executable requires local Cassandra on port 9042 and tests seeded records, all operations, descending top20 ordering, idempotence, paging, ownership, namespace collisions, and cleanup. It creates unique synthetic keyspaces. To include it in CTest, configure with `-DBENCHFORGE_LIVE_DATABASE_TESTS=ON`; default builds do not require a running database. Regression artifacts stay under the ignored build directory.

The original MongoDB 8.2.1, Cassandra 4.1.12 and Neo4j 5.26.31 smoke checks are recorded in [verification history](docs/VERIFICATION_2026-10-06.md). The final controlled experiment used FeedKV 0.2, Redis 7.4.11, MongoDB 8.0.32, Cassandra 4.1.12 and Neo4j 5.26.31, with all five native integration suites passing.

## Neo4j

Neo4j completes the local Phase 5 adapter coverage. Phase 6 adds the saved-run analysis route described below.

Use a local unauthenticated Neo4j 5.26+ single node with the default `neo4j` database and HTTP Query API v2 on port 7474. The adapter uses parameterized Cypher over one persistent HTTP connection per worker, composite `(run,id)` unique indexes on users/posts/tags, and native follow/author/tag/like relationships. Loader batches contain at most 256 rows. Run markers reject namespace reuse; cleanup removes only the loader-owned run graph, retaining shared schema constraints. Timeline reads use an indexed source user, follow/author traversals and timestamp/ID top20 sorting. Likes/follows are idempotent `MERGE` updates with a node lock. Explicit Neo4j deadlock rollback errors allow up to four retries with 10/20/30/40 ms backoff, included in operation latency. Unknown write completion and socket failures are never retried. Authentication, TLS, Bolt, cluster routing and remote targets are outside this adapter.

```powershell

docker run -d --name benchforge-neo4j --memory 2g --cpus 2 -p 127.0.0.1:7474:7474 -e NEO4J_AUTH=none -e NEO4J_server_memory_heap_initial__size=512m -e NEO4J_server_memory_heap_max__size=768m -e NEO4J_server_memory_pagecache_size=256m neo4j:5.26-community

# Wait for the Query API readiness request to succeed before running.

Invoke-RestMethod http://127.0.0.1:7474/db/neo4j/query/v2 -Method Post -ContentType application/json -Body '{"statement":"RETURN 1"}'

.\build\benchforge.exe run --config config\neo4j-smoke.conf

.\build\benchforge.exe run --config config\neo4j-celebrity-smoke.conf

.\build\neo4j_integration.exe

```

Record the resolved Docker image digest and database version; the tag can change. Heap/page-cache settings and EXPLAIN operator summaries are captured. GC and database/container resource usage need separate collection. The calibration query (`RETURN 1`) includes Cypher/server execution and is not a pure network probe. The Query API may return HTTP 202 with query errors; the client checks the error array explicitly. The live integration executable verifies both scenarios, deterministic posts/follows, all six operations, top20 tie-breaking, concurrent duplicate likes, collision refusal, worker ownership, indexed plans and cleanup. Enable `BENCHFORGE_LIVE_NEO4J_TESTS` to include it in CTest; it is off by default.

The native client and saved archive use vendored [nlohmann/json 3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0), under its bundled [MIT license](include/nlohmann/LICENSE.MIT). No download is required to build.

## Saved-run analysis

With the control API and dashboard running, open **Analysis** in the header (`http://localhost:5180/analysis`). Select baseline and candidate captures; the URL preserves selections across reloads. The read-only archive survives API restarts and includes schema-v2 summaries under `runs/<run-id>` and `runs/<bucket>/<run-id>`, including CLI verification runs. Live session status history remains separate.

`GET /api/results` returns a bounded catalog; `GET /api/results/<bucket>~<run-id>` reads a capture (empty bucket for direct children). The archive rejects unsafe paths, symbolic links, mismatched IDs, unsupported/malformed JSON and summaries over 1 MiB. Scans stop at 5,000 entries or 32 MiB of source data and return at most 500 captures; skipped/truncated counts are visible. Smaller archives are sorted newest first. Keep important results within these bounds.

Compare achieved throughput, per-operation samples/ops/s, p50/p95/p99/p99.9, errors/timeouts and scheduled send lag. Full configuration, host, adapter/version, storage/durability/query model, calibration and cleanup are shown alongside caveats. Percentage changes require valid database captures with matching workload and host settings; `noop`, inconsistent/mismatched captures and verification-folder smoke checks withhold changes. Zero-sample channels show a dash. Two captures do not establish a ranking. Experiment captures display accepted repetition counts and per-run medians with minimum–maximum ranges. Groups require at least three valid captures with matching experiment, workload, host, resource and adapter settings; invalid, duplicate and mismatched captures are excluded with reasons. Percentiles are never pooled. Each group is bounded to 30 captures.

```powershell

ctest --test-dir build --output-on-failure

dotnet run --project tests\ComparisonChecks

```

Default CTest checks CQL/Neo4j parsing, HTTP framing, archive boundaries, scheduling and actual elapsed throughput, real-process cancellation/crash recovery, HTTP cancellation and suite aggregation. It starts its own bounded FeedKV fixtures and needs no external database. Comparison checks cover eligibility, configuration mismatches, invalid/no-op evidence, approximate histogram bounds and zero denominators.

To benchmark FeedKV, start its volatile in-memory server in another terminal:

```powershell

.\build\benchforge.exe feedkv

```

Then set `adapter = feedkv` in a config file (or choose FeedKV in the dashboard). The default local endpoints are Redis `127.0.0.1:6379` and FeedKV `127.0.0.1:6380`. A config may override them with `database_host` (`localhost` or `127.0.0.1`) and `database_port` (zero selects the adapter default). FeedKV supports only the RESP2 commands needed by BenchForge; it is not a general Redis replacement and does not persist data. The loader uses the configured seed and synthetic dataset sizes. Run keys use an isolated `benchforge:<run-id>:` prefix and are removed after each run; `summary.json` records cleanup status. Redis persistence stays under Redis server control; summary metadata records Redis version, `INFO` persistence fields, and `CONFIG GET save` / `appendonly` values when permitted. The Redis user must be allowed to use `SETNX`, `SCAN`, `DEL`, and the workload's standard key/set/sorted-set commands; cleanup only deletes keys under the current run prefix.

## Controlled local experiments

Build the Release harness and the matching Linux FeedKV image, then run the standard-library Python suite with Docker Desktop running:

```powershell

docker build -f Dockerfile.benchmark -t benchforge-feedkv:local .

python scripts/benchmark_suite.py --experiment benchmark_local_001 --repetitions 3 --duration-ms 5000 --rate 100

```

The suite runs one database container at a time, each limited to two CPUs and 2 GiB without swap, with ports published only on host loopback. It checks readiness and native integration tests, then captures normal/celebrity workloads in closed/open-loop modes. Each repetition creates a new worker and namespace; the database process stays warm. FeedKV and Redis share Linux container topology and volatile persistence. MongoDB, Cassandra and Neo4j retain their reported native durability. Default ports are 16380, 16379, 27018, 9042 and 7474; occupied ports are refused. Containers created by the suite and their anonymous volumes are removed on exit. Existing services remain separate.

Seed 42, two workers, 64 users, 256 posts, 128 follows, eight hashtags, weights 40/25/15/10/5/5, one-second read-only warm-up and five-second requested measurement windows form the default profile. Measurement restarts the seeded generator after warm-up. `measured_ms` includes in-flight/backlog drain; requested duration remains in `config.duration_ms`. This timing method is recorded and older captures cannot silently compare against it. Open-loop latency excludes scheduling lag, which is reported separately.

Raw JSON/CSV, per-run CPU/memory observations, image IDs/digests, resource settings, build/source provenance, server/available GC logs and median/range aggregates are saved under `evidence/benchmarks/<experiment>/`. Local copies under `runs/experiments/` appear in Analysis. Resource observations cover the whole worker lifetime, including loading. GC pauses and competing host activity are not fully controlled. Use a fresh experiment ID for each suite; incomplete evidence is preserved and never presented as a successful comparison. Phase 7 remains excluded.


## Inspect the committed measurements

```powershell
python scripts/verify_evidence.py evidence/benchmarks/bf_20261006_v2
python scripts/import_experiment.py evidence/benchmarks/bf_20261006_v2
```

Import the verified captures, start the API and UI, then select matching `bf_20261006_v2` captures in Analysis. The [report](docs/BENCHMARK_RESULTS_2026-10-06.md) documents measured results, build/image provenance, durability differences, limitations.
