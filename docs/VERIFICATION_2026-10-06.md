# Phase 5 runtime verification — 2026-10-06

The continuation from `BENCHFORGE_HANDOFF_2026-10-06.md` verified MongoDB, added Cassandra, and fixed an open-loop scheduler defect. The next continuation completed Neo4j coverage and Phase 6 saved-run analysis. The final continuation completed native correctness checks, graceful cancellation/recovery, and 60 controlled repeated captures. See [the results report](BENCHMARK_RESULTS_2026-10-06.md). This document preserves the earlier test environment and limitations; later reliability work supersedes the historical force-termination behavior described below.

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
- FeedKV and Redis still need live correctness checks and controlled repeated comparisons. Repetition grouping/median-range aggregation remain future analysis work. Firestore remains optional and requires a separate explicit selection.

## Neo4j and Phase 6 continuation

Neo4j **5.26.31 community** ran in a uniquely labelled local test container, bound only to `127.0.0.1:7474`, with authentication disabled, 2 GiB memory and 2 CPU limits. Configured heap was 512 MiB initial / 768 MiB maximum, with 256 MiB page cache. The image tag `neo4j:5.26-community` resolved to digest `sha256:c7d25c0eeebfe125718d58b72ed5d663c85d0479047733845eb2237c67ce8069`. GC was not measured. Native requests use the Query API v2; HTTP 202 query error bodies are rejected explicitly.

- `build/neo4j_integration.exe` passes normal/celebrity deterministic posts and follows, all six operations, 31 equal-timestamp created posts with descending ID top20 tie-breaking, duplicate follows, concurrent duplicate likes from four worker connections, collision refusal, worker/loader ownership, 256 calibration probes, indexed query plan evidence and scoped cleanup.
- The first concurrent-like check exposed `Neo.TransientError.Transaction.DeadlockDetected`. The adapter retries only that explicit rolled-back transaction error, at most four times with 10/20/30/40 ms backoff included in operation timing. It never replays ambiguous socket/write failures. The repeated integration check passed after that change.
- Normal open-loop smoke `20261006T101847_035538Z`: 100 operations. Celebrity closed-loop smoke `20261006T101849_775064Z`: 301 operations. Both use seed 42, 32 users / 128 posts / 64 follows / 8 tags, two workers, 100 ms warm-up, 1,000 ms measurement and 40/25/15/10/5/5 weights. Both have zero measured errors/timeouts, valid captures and successful graph cleanup.
- Dashboard/API run `run_1791282887088_0` uses those normal/open-loop settings and completes 100 operations with zero errors/timeouts and successful graph cleanup. Its final EXPLAIN summaries show `NodeUniqueIndexSeek` for both the timeline starting user and hashtag starting node. Earlier CLI smoke plans predate the explicit timeline index hint; their metadata is preserved.
- Final C++ build succeeds. CTest passes five checks: result archive, Neo4j response parser, CQL parser, fragmented/chunked persistent HTTP with HTTP202 errors, and open-loop send lag. The archive test rejects malformed, mismatched-ID, oversized and unsafe-path inputs and verifies duplicate IDs in different buckets remain distinct. Live Neo4j checks are opt-in separately from Cassandra.
- Blazor builds with zero warnings/errors. `dotnet run --project tests/ComparisonChecks` passes matching metadata, dataset/weight/mode/host differences, invalid/no-op/missing/null evidence, large inconsistent sample counts, approximate histogram bounds and zero denominators. An additional check deserializes the real matching Cassandra/Neo4j CLI smoke metadata successfully; this checks eligibility logic only, not a comparative performance conclusion.
- A freshly started control session lists zero live session runs while its persisted archive finds previous summaries. An isolated empty archive served by the real API displays the empty state and Configure link. Browser checks cover loading, offline retry with stale numbers and withheld changes, real capture loading, workload mismatch feedback, legacy out-of-range JSON error without an offline health state, keyboard selection with focus retained, URL/reload selection restoration, 390px layout and horizontal table scrolling. The IAB capture does not show the platform-native popup itself; native keyboard selection and dismissal work. A desktop screenshot is saved at `output/playwright/analysis-dashboard.jpg`.
- Strict premium audit reports zero findings using `premium-ui.json` and `UX-CONTRACT.md`; Impeccable detection reports no findings for Home and Analysis. These static checks do not establish formal accessibility certification.

The archive scans at most 5,000 entries / 32 MiB of source data and lists at most 500 results. Each schema-v2 file is capped at 1 MiB, with safe bucket/run identifiers, complete catalog metadata, ID matching and symbolic-link refusal. Selection and comparison are read-only. Unreadable files are skipped; failed measurement captures that produce valid summaries remain visible for diagnosis. Verification-folder captures never show comparative changes. The older wrapped-lag diagnostic file remains unchanged and is rejected by the UI's signed measurement deserialization.

The new vendored nlohmann/json 3.12.0 header is MIT licensed; SHA-256 is `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`. Its license is included in `include/nlohmann/LICENSE.MIT`.

Sources: [Neo4j Query API](https://neo4j.com/docs/query-api/current/query/), [Query API EXPLAIN/PROFILE](https://neo4j.com/docs/query-api/current/profile-query/), and [nlohmann/json 3.12.0 release](https://github.com/nlohmann/json/releases/tag/v3.12.0).

After final verification, no `BenchForge` nodes remain in the test database. The exact labelled Neo4j test container and its two anonymous test volumes were removed; temporary API/UI processes were stopped. Existing MongoDB and unrelated Docker resources were left in place. The dashboard development server must be restarted after a rebuild so its fingerprinted WASM/debug asset manifest matches the rebuilt files.

Protocol and schema implementation references: [Apache Cassandra native protocol](https://cassandra.apache.org/doc/latest/cassandra/reference/native-protocol.html), [CQL data definition](https://cassandra.apache.org/doc/latest/cassandra/developing/cql/ddl.html), and [CQL data manipulation](https://cassandra.apache.org/doc/latest/cassandra/developing/cql/dml.html).
