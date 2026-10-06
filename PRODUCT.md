# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

## Stack

C++20 benchmark engine, local control API and FeedKV server; C# Blazor WebAssembly dashboard.

## Users

Developers evaluating database operation paths on a local workstation. They configure synthetic workloads, run independent targets and inspect recorded measurements and conditions.

## Product Purpose

BenchForge is a local cross-database benchmark platform. It loads seeded synthetic feed records, executes database operations, captures latency/throughput and presents run status and comparisons.

The completed experiment contains 60 valid captures across 20 configuration groups with three repetitions each. It documents measured tradeoffs under specific resource and workload conditions.

## Positioning

BenchForge pairs a purpose-built C++ database, FeedKV, with Redis, MongoDB, Cassandra and Neo4j reference targets. It makes operation semantics, storage models, durability, errors and repetition ranges visible alongside measurements.

## Operating Context

The operator starts the loopback C++ API and C# dashboard. Benchmark workers run as separate local processes, and each database is an independent local target. Configurations, synthetic records and result artifacts remain local. Saved-result analysis works without a running database.

## Capabilities and Constraints

- Six operations: timeline reads, profile reads, likes, post creation, hashtag searches and follows.
- Configurable weights, normal/celebrity scenarios, dataset size, seed, warm-up, workers and closed/open-loop load.
- Five database adapters, isolated workers, JSON/CSV evidence and per-operation histograms.
- Saved-capture comparisons, eligibility checks, repetition medians/ranges and explicit exclusions.
- Cooperative cancellation, namespace ownership, failure diagnostics and crash recovery journals.
- Loopback connections and local test-server assumptions; remote/cloud targets, authentication, TLS and distributed orchestration are outside the implemented scope.
- FeedKV is volatile and supports a workload-specific RESP subset. `noop` measures harness overhead only.

## Evidence on Hand

- `ARCHITECTURE.md` documents the implemented scope, components, measurement and recovery behavior.
- `docs/ADAPTERS.md` documents supported servers, schemas, durability and integration checks.
- `docs/BENCHMARK_RESULTS_2026-10-06.md` reports 60 controlled captures, versions, provenance, settings and limitations.
- Native correctness checks passed for all five targets. Nine default CTest checks and dashboard comparison checks passed; the .NET 9 build completed with zero warnings/errors.
- Browser verification covered saved comparisons, repetition counts, responsive table scrolling, API offline/recovery, startup failures and graceful cancellation.

## Product Principles

- Preserve equivalent workload intent and expose native-model differences.
- Report valid measurements with their configuration, resource conditions and limits.
- Keep invalid captures readable for diagnosis; withhold unsupported comparisons.
- Show real run state and evidence, without fabricated progress or rankings.
- Keep local execution and exact namespace ownership explicit.
