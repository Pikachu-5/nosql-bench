# Database adapter guide

BenchForge connects to one independent local database target per run. Connections use `localhost` or `127.0.0.1`; `database_port = 0` selects the adapter's default. All data is synthetic and isolated by run identity. Completed runs clean their owned namespace and record the result in `summary.json`.

The [automated experiment driver](../scripts/benchmark_suite.py) provides the resource-capped, pinned-image setup used for measurements. Manual servers use the operator's configuration, which must be recorded before comparing results.

## FeedKV and Redis

Default ports: FeedKV `6380`, Redis `6379`. Both use the same native RESP2 client and workload representation. Profiles/posts use string values; follows and likes use sets; author/tag indexes and materialized top20 timelines use sorted sets. A new follow backfills recent author posts. Ordering is descending timestamp, then numeric post ID.

Start FeedKV and run the small profile in separate terminals:

```powershell
.\build\benchforge.exe feedkv
```

```powershell
.\build\benchforge.exe run --config config\feedkv-smoke.conf
```

FeedKV is volatile, with a workload-specific RESP command subset and one process-wide store. It implements `PING`, `INFO server|persistence`, `SET`, `SETNX`, `GET`, `SADD`, `SISMEMBER`, `SCARD`, `SMEMBERS`, `ZADD`, `ZSCORE`, `ZREVRANGE`, `ZREMRANGEBYRANK`, `SCAN` with prefix `MATCH`/`COUNT`, and `DEL`. RESP sockets use TCP_NODELAY.

Redis requires the workload commands plus `SETNX`, `SCAN` and `DEL` for ownership/cleanup. The loader acquires an atomic marker under `benchforge:<run-id>:`. Worker adapters do not acquire cleanup ownership; the loader removes only its prefix, retaining the owner marker until cleanup succeeds. Redis version, `INFO` persistence fields and permitted `CONFIG GET save`/`appendonly` values are recorded.

The controlled experiment used FeedKV 0.2 and Redis 7.4.11, with Redis RDB/AOF disabled, `maxmemory=1536mb` and `maxmemory-policy=noeviction`. Both servers ran in Linux Docker containers with the same CPU/memory caps. Other Redis persistence settings describe a different experiment.

Native integration checks require the indicated server to be running:

```powershell
.\build\resp_integration.exe feedkv 6380
.\build\resp_integration.exe redis 6379
```

They verify both scenarios, deterministic seeds, all six operations, top20 numeric ties, duplicate likes/follows, namespace collisions, worker ownership and cleanup with unrelated-key preservation.

## MongoDB

Requires a local unauthenticated MongoDB 5.0+ server; default port `27017`. The adapter implements OP_MSG/BSON directly and creates a dedicated `benchforge_run_<hash>` database with a marker collection. Existing nonempty namespaces are refused.

The model uses indexed profiles/posts, directed follows, embedded like identities and native compound/multikey indexes. Timeline reads aggregate followed authors' posts at read time. Follow updates are idempotent. Summary metadata captures indexes, query-planner output, version and write concern. Cleanup drops only the loader-owned run database through a fresh connection; the server must allow database creation/deletion.

```powershell
.\build\benchforge.exe run --config config\mongo-smoke.conf
.\build\benchforge.exe run --config config\mongo-celebrity-smoke.conf
.\build\mongo_integration.exe mongo 27017
```

The integration executable independently checks BSON records, seeded counts, all operations, duplicate likes/follows, collision refusal, ownership and cleanup. The controlled experiment used MongoDB 8.0.32 on published port `27018`, separate from any existing `27017` service.

## Cassandra

Requires one local unauthenticated Cassandra 4.1+ node; default port `9042`. The native CQL v4 adapter prepares statements before measurement. It creates a dedicated `benchforge_run_<hash>` keyspace, refuses an existing keyspace and gives cleanup ownership only to the loader. The server must permit keyspace creation/deletion and reading `system.local`.

Schema: profiles/posts by ID, follows by source, likes by post/user, and posts by author/tag clustered by descending timestamp/ID. Timeline reads merge each followed author's newest 20 posts, then fetch the top20 records. Like/follow primary keys provide idempotence; like counts are read from a partition. Post creation uses a three-statement logged batch across the post and its indexes. Multi-partition reads do not promise transaction isolation.

Settings: `SimpleStrategy` RF=1, read/write consistency `ONE`, `durable_writes=true`, size-tiered compaction and unbucketed author/tag partitions for small local datasets. The controlled experiment used Cassandra 4.1.12 with 768 MiB heap and 128 MiB young generation. Heap, GC and commitlog/compaction activity require server-side observation; CQL cannot expose all of them. TLS, authentication, cluster discovery/failover and automatic retries are outside this adapter.

```powershell
.\build\benchforge.exe run --config config\cassandra-smoke.conf
.\build\benchforge.exe run --config config\cassandra-celebrity-smoke.conf
.\build\cassandra_integration.exe
```

The live executable checks seeded records, operations, top20 ordering, idempotence, paging, collision/ownership and cleanup. `-DBENCHFORGE_LIVE_DATABASE_TESTS=ON` adds it to CTest; default CTest needs no external server.

## Neo4j

Requires one local unauthenticated Neo4j 5.26+ node, the default `neo4j` database and HTTP Query API v2; default port `7474`. Each worker uses a persistent HTTP connection and parameterized Cypher. The schema uses composite `(run,id)` unique indexes on users/posts/tags and native follow/author/tag/like relationships. Loading batches contain at most 256 rows.

Run markers reject namespace reuse. The loader removes only its owned run graph, preserving shared schema constraints. Timeline reads use indexed source-user lookup, follow/author traversals and timestamp/ID sorting. Like/follow updates use idempotent `MERGE` with a node lock. Explicit deadlock rollback errors allow up to four retries with 10/20/30/40 ms backoff, included in latency. Unknown write completion and socket failures are never replayed.

The client checks Query API error arrays even when HTTP 202 is returned. Calibration (`RETURN 1`) includes server/Cypher work rather than pure network delay. EXPLAIN summaries and available heap/page-cache settings are recorded. Authentication, TLS, Bolt and cluster routing are outside this adapter.

```powershell
.\build\benchforge.exe run --config config\neo4j-smoke.conf
.\build\benchforge.exe run --config config\neo4j-celebrity-smoke.conf
.\build\neo4j_integration.exe
```

The live executable checks both scenarios, seeded posts/follows, top20 ties, concurrent duplicate likes, namespace collisions, worker ownership, indexed plans and cleanup. `-DBENCHFORGE_LIVE_NEO4J_TESTS=ON` adds it to CTest. The controlled experiment used Neo4j 5.26.31 community with initial/max heap 512 MiB and page cache 256 MiB.

## Capture conditions and cleanup

Database version, endpoint, schema, durability, calibration and run cleanup accompany each summary. The suite also records exact image IDs/digests, resource caps and whole-worker-lifetime Docker observations. Only FeedKV and the persistence-disabled Redis profile share both the client protocol and volatile materialized model.

After a crash, use the exact `recovery.json` with `benchforge recover --manifest PATH`. Recovery refuses a live worker and validates ownership/local endpoint/run identity. A crash before the ownership journal is committed can require manual inspection. [ARCHITECTURE.md](../ARCHITECTURE.md) documents lifecycle behavior; [the results report](BENCHMARK_RESULTS_2026-10-06.md) documents evidence and interpretation.
