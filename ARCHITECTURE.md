# BenchForge — System Architecture

**Working title:** BenchForge  
**Status:** Phases 1–6 implemented; MongoDB, Cassandra and Neo4j smoke-verified; controlled comparative measurements remain pending
**Last reviewed:** 2026-10-06  
**Purpose:** Durable project brief and source of truth for future implementation decisions

### Implementation status (2026-10-06)

The C++20 harness and local control API build with CMake and MinGW. The harness includes strict key/value configuration parsing and validation, deterministic per-worker synthetic operation generation, a database-adapter interface, a no-op calibration adapter, concurrent worker loops, approximate latency histograms, and JSON/CSV summaries. The no-op adapter does not access a database, so its results describe harness overhead only.

The loopback-only control API exposes health, adapter discovery, run creation/list/detail, cancellation, and completed summaries. It validates bounded form fields, launches only the BenchForge worker executable as a child process, allows one active run per control session, and stores run configuration and output under `runs/api/<run-id>/`. The C# Blazor UI configures runs, polls state, supports cancellation, and displays per-operation summaries. Phase 3 includes an in-memory FeedKV server with a limited RESP2 command set plus a Redis adapter over the same RESP client. Phase 4 adds open-loop scheduling, transport calibration, send-lag reporting, and environment metadata. Phase 5 adds a MongoDB adapter using OP_MSG/BSON, run-specific databases, deterministic collection loading, native compound and multikey indexes, read-time timeline aggregation, idempotent follow updates, and captured query-planner summaries. MongoDB requires a local unauthenticated connection. Cassandra adds a native CQL v4 client, prepared worker statements, query-specific partitions, read-time timeline merging, idempotent follows/likes, and loader-owned keyspace cleanup. Both projects compile; bounded MongoDB and Cassandra runtime checks pass. Open-loop scheduling rechecks early timer wakeups and clamps lag before unsigned conversion. Cancellation is cooperative, with loader checks, invalid diagnostic results, namespace cleanup, a 30-second API grace period and run-journal crash recovery. Hard termination can still require explicit recovery. FeedKV/Redis runtime verification and comparative runs remain pending. See `docs/VERIFICATION_2026-10-06.md` for evidence and limits.

## 1. Project summary

Neo4j completes the local Phase 5 coverage with native indexed graph operations over HTTP Query API v2, run-owned graph cleanup and captured EXPLAIN/heap/page-cache settings. Phase 6 adds a persisted read-only summary archive and two-capture analysis route with configuration differences, validity gating, complete metadata and asynchronous recovery. The comparison checks do not establish a performance ranking; controlled repetitions remain required.

BenchForge is a standalone C++ database-benchmark platform with a small C# UI. Its product is the benchmark/control system, not a social network.

The social-feed domain comes from the supplied NoSQL Review 1 and Review 2 reports. It is a repeatable synthetic workload used to compare database designs: post creation, follows, timeline reads, likes, profile reads, and hashtag searches. The reports describe a CLI benchmark prototype and explicitly distinguish it from a user-facing social application. This redesign adds a local control API and UI around benchmark runs; it does not build the social application itself.

The central systems project is FeedKV: a purpose-built C++ database under test, optimized for the benchmark’s feed-shaped operation mix. Compare it with Redis under controlled local workloads. The reports’ MongoDB, Cassandra, and Neo4j adapters are reference targets; add them in stages instead of treating every course-report detail as a mandatory requirement.

GCP is only an optional remote benchmark target: one small Firestore database containing synthetic benchmark data. The benchmark platform, UI, run metadata, and all normal data stay local.

## 2. Scope and boundaries

### In scope

- A C++ benchmark engine that loads seeded data and runs configurable workloads against pluggable database adapters.
- A C++ local control API that accepts run configurations, starts and monitors benchmark workers, and serves results.
- A C# Blazor WebAssembly UI for configuring runs and viewing status, charts, and comparisons.
- FeedKV as a standalone C++ database target, purpose-built for the selected benchmark workload.
- A fair local FeedKV-versus-Redis benchmark.
- Additional local MongoDB, Cassandra, and Neo4j adapters in staged milestones.
- An optional Firestore adapter for bounded synthetic remote-database runs, managed with Google Cloud CLI.

### Not the product

- No user registration, real posts, public timeline, or social-network API.
- No MongoDB/Neo4j/Redis ensemble acting as the storage layer of a deployed social application.
- No GCP-hosted API or UI.
- No cloud storage of ordinary user data or benchmark results.

## 3. Reference workload

The reports provide a useful starting workload, not binding implementation instructions. Keep the operation semantics consistent across adapters while allowing each database to use a model suited to its strengths.

| Operation | Starting mix from Review 2 | Intent |
|---|---:|---|
| Timeline read | 40% | Return the newest 20 posts from followed authors |
| User profile read | 25% | Retrieve a profile by user ID |
| Post like | 15% | Record a like and represent/update the count |
| Post create | 10% | Create a post, index its tags, and make it visible to followers |
| Hashtag search | 5% | Return the newest posts for a tag |
| User follow | 5% | Create an idempotent directed relationship |

Make operation weights, data sizes, follower skew, duration, concurrency, and offered rate configurable. Start with a laptop-scale deterministic profile and a skewed/high-follower profile. The report’s larger dataset and strict engine-overhead targets are reference points to evaluate, not promises the new project must copy.

This distinction matters: the workload may simulate a social feed, but the benchmark tool itself stores synthetic test records, submits database operations, and reports measurements. It is not a Twitter clone.

## 4. Proposed architecture

    Browser
      └── Blazor WebAssembly UI
             └── HTTP ──> Local C++ Control API
                              ├── validates run configuration
                              ├── starts/stops isolated benchmark worker
                              ├── reports run status
                              └── reads local run artifacts

    Isolated C++ benchmark worker
      ├── deterministic dataset/workload generator
      ├── worker threads and measured request loop
      ├── database adapter interface
      │     ├── FeedKV
      │     ├── Redis
      │     ├── MongoDB
      │     ├── Cassandra
      │     ├── Neo4j (native graph over HTTP Query API v2)
      │     └── Firestore (optional GCP target)
      └── telemetry and local JSON/CSV results

The control API, UI rendering, chart calculations, and result-file writing must remain outside the timed database hot path. Run each benchmark workload in a separate worker process so the local web server does not contaminate the measurements. The UI can poll the control API for state and read completed artifacts after a run.

Run metadata and output live locally in a run directory, for example:

    runs/<run-id>/manifest.json
    runs/<run-id>/summary.json
    runs/<run-id>/operations.csv
    runs/<run-id>/histograms/

Do not add another database merely to store benchmark results in the first version.

## 5. Main components

### C++ control API

The API is a local control plane for the benchmark product, not an application backend for posts and feeds. It should:

- Validate adapter names, workload settings, data sizes, and run limits.
- Start a worker from a fixed set of known profiles and arguments.
- Expose run status, cancellation, health, adapter availability, and result metadata.
- Keep generated result files and logs outside the measurement process.

Initial routes can be small: create a run, list runs, fetch a run, cancel a run, list adapters, and check health. Keep it local-only until there is a separate security and deployment decision.

The current API listens on IPv4 loopback at port 8080 by default. It supports `GET /health`, `GET /api/adapters`, `GET /api/runs`, `POST /api/runs`, `GET /api/runs/{id}`, `POST /api/runs/{id}/cancel`, and `GET /api/runs/{id}/results`. Run creation accepts `application/x-www-form-urlencoded` fields and rejects unknown keys. Requests are bounded, CORS is limited to the local UI origin, the Host header must be local, and the server never passes request values through a shell. Only one benchmark can run at a time so concurrent runs do not distort one another.

### Benchmark engine

The engine is responsible for measurement quality:

- Generate deterministic data and operation streams from a seed.
- Precompute request payloads where that avoids unrelated allocator work.
- Support closed-loop and scheduled-rate/open-loop modes; the open-loop rate is aggregate across workers and scheduler lag is recorded separately from operation latency.
- Measure each operation and classify it by operation type and success/error.
- Record p50, p95, p99, and p99.9 latency, throughput, errors, timeouts, and send lag.
- Use a no-op adapter and transport baseline to estimate engine and connection overhead.
- Mark a run invalid when measured operations fail or time out, telemetry is dropped, cleanup fails, no measured operations occur, or measurement invariants fail.
- Capture host name, operating system, architecture, logical processor count, and total physical memory in each result manifest.

The reports suggest thread-per-core workers, preallocation, SPSC telemetry, HDR histograms, and affinity experiments. Reuse these as engineering options, but benchmark the instrumentation itself and keep only complexity that improves measurement credibility. Do not promise sub-100-nanosecond overhead or 500,000 database operations per second before measuring it on the actual machine.

### Database adapter interface

Adapters implement the same workload intent using native queries and data models. Results should include adapter/database version, schema/index setup, durability settings, endpoint, resource limits, and data-loader version. Run one database target at a time on constrained hardware.

Initial practical order:

1. Redis and FeedKV, because they establish the custom database comparison.
2. MongoDB, to preserve the document-store comparison from the project reports.
3. Cassandra and Neo4j after engine/API and result correctness are stable.
4. Firestore as an optional remote target with its own explicitly network-inclusive report.

The order is a build plan, not a judgement that an adapter is unimportant.

## 6. FeedKV: custom database under test

FeedKV is not a cache inside a social application. It is a standalone C++ database target that receives benchmark operations through a client adapter.

The first implementation uses RESP2 arrays of bulk strings and a small command subset. This lets FeedKV and the Redis adapter share an identical client protocol while keeping the server focused on BenchForge; FeedKV does not claim general Redis compatibility. Implemented commands are `PING`, `INFO server|persistence`, `SET`, `GET`, `SADD`, `SISMEMBER`, `SCARD`, `SMEMBERS`, `ZADD`, `ZSCORE`, `ZREVRANGE`, `ZREMRANGEBYRANK`, `SCAN` with prefix `MATCH` and `COUNT`, and `DEL`. Sorted-set scores are integer timestamps, and reads return the newest 20 indexed posts. The server binds to IPv4 loopback and keeps all data in memory.

The custom engine implements the selected workload through profiles, follows in both useful directions, posts and author indexes, 20-item ordered timelines, idempotent likes represented by sets/cardinality, and 20-item hashtag searches. A follow also backfills the newest posts from the newly followed author. The deterministic loader creates the same synthetic graph and posts for each adapter. Normal runs use a regular directed-follow layout; celebrity runs concentrate followers on user 0. All keys are prefixed with `benchforge:<run-id>:` and only that namespace is removed after the run. Candidate optimization areas include compact record layouts, bounded timeline pages, batched fan-out updates, reduced allocation, sharded maps, and efficient ordered reads. Choose designs from profiling and correctness checks, not from the expectation that custom code must win.

Redis remains an independent reference target. FeedKV and Redis should receive equivalent generated data and equivalent operation semantics. Report the storage configuration and durability differences; do not compare an in-memory, persistence-disabled custom engine with a durable Redis configuration without calling that out.

## 7. Database-model decisions and bottlenecks

The benchmark exists to reveal workload-dependent tradeoffs. A lower result for MongoDB or Neo4j in one report is not a universal ranking and should not be “fixed” by hiding the operation or changing its semantics.

- **Redis:** use native key/value structures and materialized ordered timelines for the baseline.
- **MongoDB:** use appropriate compound/multikey indexes and report the query plan and write concern. Timeline reads that query followed authors and sort posts are a deliberate read-time model.
- **Cassandra:** use query-first partitions and report replication/consistency settings, JVM heap, and compaction behavior.
- **Neo4j:** use indexed starting nodes and native relationships; report traversal fan-out, sort work, heap/page-cache settings, and GC behavior.
- **FeedKV:** profile the same operation mix and report which workload dimensions it optimizes and where it degrades.

Use controlled data volumes that fit available hardware. The reports’ own laptop caveat notes that very large graphs and write-time fan-out can exceed a 16 GB machine’s working set. Scale until the machine or database boundary becomes visible, but do not label disk thrashing or out-of-memory behavior as a clean database comparison.

## 8. C# UI

Use Blazor WebAssembly for a small local dashboard; no React or TypeScript is needed. The UI configures and observes benchmark runs, not social content.

The first Phase 2 screen is a single measurement workstation for configuring and launching a run, reviewing recent run status, cancelling an active worker, and reading per-operation percentiles. Its design target is `ui/design/benchforge-dashboard-comp.png`, with design tokens and guidance in `DESIGN.md`. Adapter discovery exposes `noop`, FeedKV, Redis, MongoDB, Cassandra and Neo4j. The dashboard identifies their default local ports and notes that FeedKV must be started separately.

Initial screens:

- **New run:** adapter, workload profile, seed, dataset size, mode, concurrency/rate, duration, and warm-up.
- **Run monitor:** state, current operation counts, throughput estimate, errors, and cancellation.
- **Results:** overall and per-operation latency percentiles, throughput, errors, and run-to-run comparison.
- **Environment:** database versions, resource limits, machine details, adapter health, and calibration status.

Make benchmark caveats visible in the results view: local versus remote, open versus closed mode, persistence settings, warm-up, and run validity.

## 9. Benchmark methodology

### FeedKV versus Redis

This is the primary engine-level comparison. Run both locally on the same host, with matching data, client path, resource limits, operation stream, warm-up, and duration. Compare only supported equivalent operations.

Report:

- Throughput and p50/p95/p99/p99.9 latency overall and by operation.
- Error, timeout, dropped-telemetry, and send-lag counts.
- Memory use and per-record footprint where measurable.
- Normal and skewed/high-follower results.
- At least three repetitions for important configurations, with median and range.

### Other local database targets

Native schemas can differ because each adapter should use the database’s strengths. Document those differences and preserve the same high-level operation semantics. Report all settings that could change the result. Distinguish workload-level performance from a pure engine microbenchmark.

### Firestore

Firestore is a separate optional managed-database experiment. The local worker sends a small synthetic document workload to the remote Firestore API. Report end-to-end latency, throughput, reads/writes, response sizes, and the runner’s location/network. Those timings include internet and managed-service overhead; they are not directly comparable to local Redis or FeedKV engine latency.

## 10. GCP: one optional free-tier benchmark target

GCP is not where the benchmark platform runs. It provides only one small Firestore database that the local worker can target when explicitly selected. No application records or benchmark result files are stored there.

Firestore is the proposed target because it is a managed document database with a published free quota and Google Cloud CLI support. Current published limits include one free database per project, 1 GiB stored data, 50,000 document reads/day, 20,000 writes/day, 20,000 deletes/day, and 10 GiB/month outbound data transfer. Verify the current quota and selected Firestore region before creating anything.

Use only synthetic records under a dedicated run namespace, cap operations before starting a run, and provide explicit cleanup. Do not use TTL deletes, backups, point-in-time recovery, restore, or clone features for this experiment.

The Google-maintained Firestore server client library list does not include C++. Keep the Firestore REST adapter inside the benchmark worker and authenticate with local application-default credentials. This integration is optional and must not become a dependency of the control API or local benchmark runs.

GCP Free Tier use requires an active billing account. Usage above free quotas can be billed on a paid billing account, and a budget alert does not stop usage. Therefore the design is quota-conscious, not an absolute guarantee of zero charges. If any possible charge is unacceptable, skip the cloud adapter; all local functionality and primary benchmarks remain available.

Google Cloud CLI manages and inspects the optional database/project. The benchmark worker runs locally. A future command flow can use:

    gcloud init
    gcloud config set project PROJECT_ID
    gcloud auth application-default login
    gcloud firestore databases list
    benchforge run --adapter firestore --profile small --max-operations 5000

Do not add Cloud Run, Compute Engine, Pub/Sub, Cloud Scheduler, Memorystore, Atlas, Aura, or another database to the cloud profile without a new decision from the user.

## 11. Reliability and security

- Keep the control API local-only initially; bind to loopback by default.
- Whitelist adapter/container profiles and arguments. Do not let API input become an arbitrary shell command.
- Bound dataset sizes, worker counts, request rates, durations, and output volume.
- Keep GCP credentials outside the repository; never commit service-account keys.
- Use application-default credentials only for the explicitly selected Firestore adapter.
- Use synthetic data for every benchmark target.
- Write run manifests before execution so partial runs can be diagnosed.
- Preserve invalid-run reasons and raw outputs; do not silently discard failed runs.

## 12. Implementation sequence

1. **Scope and harness foundation:** C++20/CMake layout, workload schema, config validation, result-file format, no-op adapter.
2. **Control plane:** local C++ API, isolated worker process, Blazor run form and status view.
3. **First comparable targets:** FeedKV RESP2 server, Redis adapter, deterministic loader, and run namespace cleanup are implemented. Both targets passed deterministic native integration checks and three repeated measurements in each scenario/load mode. RESP transport stalls were fixed with TCP_NODELAY before the final experiment.
4. **Measurement quality:** closed-loop and scheduled-rate/open-loop execution, bounded logarithmic latency and send-lag histograms, RESP PING transport calibration, invalid-run reasons, timeout counts, and host/OS/architecture/processor/memory capture are implemented. Actual elapsed wall time includes in-flight/backlog drain; read-only warm-up preserves the seeded measurement stream. Cancellation and atomic recovery journals preserve invalid diagnostics and remove owned namespaces. Real-process, API and delayed-backlog regressions passed.
5. **Cross-database coverage:** MongoDB, Cassandra and Neo4j adapters implemented and smoke-verified. Neo4j uses HTTP Query API v2, composite unique indexes, native relationships, deterministic batch loading, top20 traversal reads, idempotent relationships and loader-owned run graph cleanup. Explicit deadlock rollbacks have bounded retries included in latency; socket/write completion failures are never replayed. All five native integration suites and 60 controlled captures passed. See `docs/BENCHMARK_RESULTS_2026-10-06.md` for versions, measured results and limits; `docs/VERIFICATION_2026-10-06.md` preserves earlier verification history.
6. **Analysis UI:** implemented read-only persisted result archive and `/analysis` route. Two captures expose throughput, operation percentiles through p99.9, send lag, environment, configuration, calibration, durability, validity and limitations. Mismatched/invalid/no-op and verification-folder captures withhold percentage changes. Selections survive reload through URL parameters; stale requests are cancelled. Experiment repetition groups expose per-run medians and minimum–maximum ranges after at least three matching valid captures. Duplicate, invalid and mismatched evidence is excluded; percentile samples are never pooled.
7. **Optional GCP benchmark (excluded by user):** Firestore REST adapter in the worker, operation caps, synthetic-data namespace, CLI workflow, and cleanup.
8. **Benchmark evidence (complete):** committed 60 valid captures across 20 configuration groups, each with three repetitions; JSON/CSV, resource caps and observations, image/build provenance, native correctness logs, hashes, median/range aggregates, verifier/import commands. FeedKV and Redis ranges overlap and their lead changes by scenario; no general speedup is claimed. See `docs/BENCHMARK_RESULTS_2026-10-06.md`.

Local phases 1–6 and 8 are complete. The optional Firestore phase remains excluded by the user.

## 14. Source documents and change policy

The supplied Review 1 and Review 2 reports establish the existing project’s context: a cross-database benchmark suite using a social-feed workload, with Redis, MongoDB, Cassandra, and Neo4j. They are reference material, not instructions to reproduce every prior implementation constraint. The user’s current direction controls this redesign: standalone C++, a backend/control API, a small C# UI, a custom database target, and one optional GCP benchmark database.

Free-tier facts were checked on 2026-10-03; recheck before creating a cloud resource.

- [Google Cloud Free Tier products and limits](https://docs.cloud.google.com/free/docs/free-cloud-features)
- [Firestore pricing and free quota](https://cloud.google.com/firestore/pricing)
- [Firestore server client libraries](https://docs.cloud.google.com/firestore/native/docs/reference/libraries)
- [Firestore REST API](https://docs.cloud.google.com/firestore/docs/reference/rest)
- [Firestore locations](https://docs.cloud.google.com/firestore/docs/locations)
- [Budget alerts do not cap spend](https://docs.cloud.google.com/billing/docs/how-to/budgets)

Update this document when a deliberate decision changes, when actual measurements disprove a planned optimization, or when GCP free-tier terms change. Keep the workload-versus-product distinction explicit so a context reset does not turn the benchmark into a social-network app.
