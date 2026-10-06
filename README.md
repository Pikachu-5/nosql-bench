# BenchForge

BenchForge is a standalone C++ cross-database benchmark platform. The social-feed domain is a synthetic workload used to compare database designs; BenchForge is not a social network. The product pairs a local C++ control API and C# dashboard with FeedKV, a purpose-built database under test, and Redis, MongoDB, and Cassandra comparison targets. The only planned cloud target is an optional small Firestore benchmark using synthetic data.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the durable scope, decisions, implementation phases, and GCP quota guardrails.

## Current implementation

Phase 1 provides a C++20 command-line harness with configuration validation, seeded operation generation, a pluggable adapter boundary, a no-op calibration adapter, per-operation latency histograms, and JSON/CSV run output. Phase 2 adds a loopback C++ control API that launches isolated worker processes. The no-op adapter measures harness overhead only; its throughput and latency are not database performance results.

The C# Blazor WebAssembly dashboard configures and launches runs, polls run state, supports cancellation, and displays per-operation summaries. Phase 3 adds an in-memory FeedKV database using a documented RESP2 subset and a Redis adapter that uses the same client protocol, workload operations, deterministic data loader, and run-scoped key cleanup. Phase 4 adds closed-loop and scheduled-rate/open-loop execution, latency and send-lag histograms, transport calibration, timeout and invalid-run reporting, captured host environment metadata, and matching dashboard controls. Phase 5 adds MongoDB over its native OP_MSG/BSON protocol, with run-specific databases, deterministic data loading, native indexes, idempotent follow updates, a read-time timeline aggregation, and query-planner summaries. Cassandra adds a native CQL v4 adapter with prepared statements, query-specific partitions, and run-owned keyspace cleanup. Both projects compile. MongoDB and Cassandra passed bounded runtime checks; live comparative runs remain pending.

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

The adapter list contains `noop`, `feedkv`, `redis`, `mongo`, and `cassandra`. Runs write `summary.json` and `operations.csv` beneath `runs/<run-id>/`. Start the control API with `.\build\benchforge.exe serve`; it binds to `127.0.0.1:8080` and stores UI-created runs beneath `runs/api/<run-id>/`.

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

The API currently force-terminates cancelled workers. Cancellation, crashes, or process termination can leave a partial database namespace; cleanup is guaranteed only when the worker reaches its cleanup step. Recover only the exact namespace derived from that run ID, or discard an isolated test container. Never use a wildcard keyspace/database deletion.

## Verification

```powershell
ctest --test-dir build --output-on-failure
.\build\cassandra_integration.exe
```

CTest runs CQL parser checks and, when Python is available, an open-loop scheduling regression. The explicit integration executable requires local Cassandra on port 9042 and tests seeded records, all operations, descending top20 ordering, idempotence, paging, ownership, namespace collisions, and cleanup. It creates unique synthetic keyspaces. To include it in CTest, configure with `-DBENCHFORGE_LIVE_DATABASE_TESTS=ON`; default builds do not require a running database. Regression artifacts stay under the ignored build directory.

MongoDB 8.2.1 and Cassandra 4.1.12 passed bounded runtime checks on 2026-10-06. See [verification evidence and limitations](docs/VERIFICATION_2026-10-06.md). These are correctness checks; no comparative performance claim follows from them. FeedKV-versus-Redis measurements remain pending; Neo4j is the next Phase 5 adapter milestone.

To benchmark FeedKV, start its volatile in-memory server in another terminal:

```powershell
.\build\benchforge.exe feedkv
```

Then set `adapter = feedkv` in a config file (or choose FeedKV in the dashboard). The default local endpoints are Redis `127.0.0.1:6379` and FeedKV `127.0.0.1:6380`. A config may override them with `database_host` (`localhost` or `127.0.0.1`) and `database_port` (zero selects the adapter default). FeedKV supports only the RESP2 commands needed by BenchForge; it is not a general Redis replacement and does not persist data. The loader uses the configured seed and synthetic dataset sizes. Run keys use an isolated `benchforge:<run-id>:` prefix and are removed after each run; `summary.json` records cleanup status. Redis persistence stays under Redis server control; summary metadata records Redis version, `INFO` persistence fields, and `CONFIG GET save` / `appendonly` values when permitted. The Redis user must be allowed to use `SCAN`, `DEL`, and the workload's standard key/set/sorted-set commands; cleanup only deletes keys under the current run prefix.

