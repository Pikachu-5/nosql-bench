# Phase 5 runtime verification — 2026-10-06

The continuation from `BENCHFORGE_HANDOFF_2026-10-06.md` verified MongoDB, added the Cassandra milestone, and fixed an open-loop scheduler defect revealed by a dashboard run. Neo4j remains the next Phase 5 milestone, followed by Phase 6 analysis UI.

## Scope and environment

All records are synthetic. This is bounded correctness verification, not a database performance comparison. The host is Windows x86_64 with 22 logical processors and 16,639,741,952 bytes physical memory. MongoDB 8.2.1 was already running on loopback port 27017 with authentication disabled.

Cassandra 4.1.12 ran in an isolated Docker Desktop Linux container, bound to `127.0.0.1:9042`, with a 2 GiB memory cap, 2 CPU limit, `MAX_HEAP_SIZE=768M`, and `HEAP_NEWSIZE=128M`. The image was `cassandra:4.1`, resolved manifest digest `sha256:57e5dd97a964fa97f96269c821941c910f9fc7ee905ea4cba586b7b63eb18883`, image ID `sha256:d4f138ee00e8fc2664905ef566645a152a2bf87ce36a48b4b76366913126a203`. `nodetool info` reported 755.25 MiB maximum heap and zero exceptions; `nodetool compactionstats` reported zero pending tasks at inspection. These observations describe the test environment, not fixed runtime behavior.

## Checks

- `cmake --build build --parallel 4` — succeeds.
- `dotnet build ui/BenchForge.UI.csproj --no-restore` — succeeds, zero warnings/errors.
- `ctest --test-dir build --output-on-failure` — CQL parser and open-loop send-lag regression pass. The latter runs three real scheduled workloads with three workers at 101 ops/s and rejects lag beyond the process wall time.
- `build/cassandra_integration.exe` — normal and celebrity datasets pass seeded-record checks, all workload operations, repeated likes/follows, three post indexes, descending ID tie-breaking with 31 equal-timestamp posts, the top20 limit, a 299-author follow list spanning multiple pages, 256 transport probes, refusal to adopt an existing namespace, loader/worker ownership, and post-cleanup absence.
- Strict premium UI audit reports zero findings; Impeccable detector reports no findings for the dashboard. Browser checks cover discovery, Cassandra guidance, native selection, keyboard numeric entry, narrow/desktop layout, worker launch, disabled controls while running, cancellation, and completed result rendering.

## Smoke artifacts

Artifacts live under ignored `runs/verification/<run-id>/` directories and contain exact configuration, environment, per-operation counts, approximate histograms, and cleanup status. All completed checks below have zero measured errors/timeouts and successful cleanup.

| Adapter | Run ID | Configuration | Measured operations |
|---|---|---|---:|
| MongoDB | `20261006T092656_700732Z` | normal, closed loop, 4 workers, 250 ms warm-up, 1000 ms measured | 5254 |
| MongoDB | `20261006T093538_503193Z` | celebrity, open loop, 100 ops/s, 4 workers, 250 ms warm-up, 1000 ms measured | 100 |
| Cassandra | `20261006T094458_684624Z` | normal, open loop, 100 ops/s, 2 workers, 100 ms warm-up, 1000 ms measured | 100 |
| Cassandra | `20261006T094501_711176Z` | celebrity, closed loop, 2 workers, 100 ms warm-up, 1000 ms measured | 367 |

Each smoke dataset has seed 42, 32 users, 128 posts, 64 follows, and 8 hashtags, with weights 40/25/15/10/5/5. MongoDB records the unique follows index, compound author/time/ID index, multikey hashtag/time/ID index, `w:1`, and planner summaries. Its exact normal run database `benchforge_run_29c5ec508fc1840d` was confirmed absent after cleanup.

The final dashboard smoke is `runs/api/run_1791280084833_0/`, using the same Cassandra normal/open-loop smoke settings. It completed 100 measured operations with zero errors/timeouts, valid capture checks, and confirmed keyspace removal. Its worker configuration and result artifacts verify the API-to-worker path. A dashboard screenshot is saved at `output/playwright/cassandra-dashboard.jpg`.

After the final builds and live tests passed, the exact labelled verification container and its anonymous test volume were removed, and the temporary API/UI processes were stopped. MongoDB's pre-existing service was left running.

## Defect found and fixed

A Windows timer could wake before an open-loop scheduled deadline. Casting negative send lag directly to `uint64_t` wrapped it to nearly `2^64`. That escaped validity checks and could overflow the dashboard's signed integer result model, incorrectly displaying an offline API. Scheduling now rechecks the steady-clock deadline before dispatch and clamps lag at zero before unsigned conversion. A CTest regression bounds every lag statistic by the actual process wall time. Result deserialization errors are now reported as result-loading errors while health/discovery remains available.

The original diagnostic artifact `runs/api/run_1791279654498_1/summary.json` is preserved, but its lag measurements are invalid despite the old worker reporting `valid=true`. Do not use its timing data. Fresh post-fix artifacts above replace it for verification.

## Limitations and next work

- These short runs establish correctness/connectivity within the tested configurations. There are no repeated comparative measurements or performance rankings.
- Cassandra is a single-node local adapter: no authentication, TLS, cluster discovery, failover, or automatic reprepare after a server restart. It uses protocol v4, prepared statements, RF=1, consistency ONE, durable writes, and explicit size-tiered compaction. Commitlog sync remains server-managed; comparative runs must capture it alongside heap/GC/compaction activity.
- Author/tag/like partitions are unbucketed and intended for bounded laptop-scale workloads. Read-time timeline fan-out, record fetches, and like partition counts are included in operation timing. Logged post/index writes do not provide transactionally isolated multi-partition reads.
- Cancellation force-terminates the worker and may leave a partially loaded namespace. The cancelled dashboard run `run_1791279551805_0` left synthetic keyspace `benchforge_run_703a0f17f81dcfd7`; that exact keyspace was removed during verification. Completed Cassandra cleanup deletes only a loader-owned namespace. Graceful cancellation and recovery manifests remain future reliability work.
- FeedKV and Redis still need live correctness checks and controlled repeated comparisons. Neo4j is the next cross-database adapter, then the analysis UI. Firestore remains optional and requires a separate explicit selection.

Protocol and schema implementation references: [Apache Cassandra native protocol](https://cassandra.apache.org/doc/latest/cassandra/reference/native-protocol.html), [CQL data definition](https://cassandra.apache.org/doc/latest/cassandra/developing/cql/ddl.html), and [CQL data manipulation](https://cassandra.apache.org/doc/latest/cassandra/developing/cql/dml.html).
