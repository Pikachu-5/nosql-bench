# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

## Stack

C++20 benchmark engine and local control API; C# Blazor WebAssembly dashboard. The dashboard uses C# rather than React/TypeScript, as requested.

## Users

Developers evaluating database designs and implementations. The initial operator is a developer running BenchForge locally on a workstation or laptop to configure benchmark workloads, launch database workers, and review results. The user described the audience broadly as developers; narrower roles are undecided.

## Product Purpose

BenchForge is a local cross-database benchmark platform. It runs a configurable synthetic workload against database adapters and presents run status and measurements. The social-feed domain is only a benchmark workload; BenchForge does not provide a social network or store real posts, follows, profiles, or user activity.

Success means a developer can repeat equivalent workload runs against supported database targets and use per-operation throughput, latency, errors, and recorded configuration to understand where each target performs well or poorly. The completed local experiment provides 60 valid captures across 20 configuration groups, with three repetitions each; measured tradeoffs and limitations are documented.

## Positioning

BenchForge compares independent database implementations under the same seeded, feed-shaped workload. Its differentiator is FeedKV, a purpose-built C++ database under test, evaluated against Redis and native MongoDB, Cassandra and Neo4j adapters. FeedKV is implemented; no performance advantage is established.

## Operating Context

The operator starts the local C++ control API, configures a run in the C# dashboard, and reviews run status and local result artifacts. Benchmark workers run as separate processes. Normal benchmark configuration, generated synthetic data, and results remain local.

An optional small Firestore database may be used as a remote benchmark target with synthetic records and strict operation caps. The benchmark platform, dashboard, and run artifacts are not hosted on GCP. Phase 7 is excluded by the user and no cloud target has been provisioned.

## Capabilities and Constraints

- The benchmark workload covers timeline reads, profile reads, likes, post creation, hashtag searches, and follows. These actions model database operations; they are not end-user social features.
- C++ implements the benchmark engine, control API, FeedKV server, and database adapters.
- C# Blazor WebAssembly implements the dashboard; React and TypeScript are out of scope.
- FeedKV, Redis, MongoDB, Cassandra and Neo4j adapters are implemented. Firestore remains optional. They are independent benchmark targets, not a required database ensemble.
- GCP scope is limited to an optional small synthetic-data Firestore benchmark. The intended profile stays within published free quotas, but free quotas are not an absolute cost guarantee.
- Native correctness checks passed for all five database targets. The final local experiment adds repeated measurements with recorded limits and no universal ranking. The `noop` adapter measures harness overhead only.

## Evidence on Hand

- `ARCHITECTURE.md` records the agreed scope, benchmark methodology, implementation phases, and cloud guardrails.
- The supplied Review 1 and Review 2 PDFs provide a feed-shaped benchmark workload reference.
- The C++ harness, API, FeedKV, and database adapters compile locally. The .NET 9 dashboard builds with zero warnings/errors and its Cassandra launch/results path has been exercised in a browser.
- `docs/VERIFICATION_2026-10-06.md` preserves earlier correctness checks and the open-loop send-lag fix. `docs/BENCHMARK_RESULTS_2026-10-06.md` reports the completed 60-capture local experiment, raw evidence, versions, resource settings and limits. The Analysis repetition view, API offline/recovery and graceful cancellation were exercised in the browser.

## Product Principles

- Keep product operations distinct from the synthetic workload domain.
- Preserve equivalent workload semantics across adapters and report configuration with results.
- Present measured tradeoffs with honest limits; never claim an unmeasured performance win.
- Keep the platform and ordinary run artifacts local; make remote benchmarking explicit and bounded.
