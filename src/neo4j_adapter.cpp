#include "database_adapter.hpp"
#include "neo4j_client.hpp"
#include "benchforge/metrics.hpp"
#include <algorithm>
#include <chrono>
#include <random>
#include <set>
#include <thread>

namespace benchforge::detail {
namespace {
using Json = nlohmann::json;
const std::string kTimeline =
    "MATCH (u:BFUser {run:$run,id:$user})-[:FOLLOWS]->(a:BFUser)-[:AUTHORED]->(p:BFPost) "
    "USING INDEX u:BFUser(run,id) "
    "RETURN p.id,p.author,p.created_at,p.tag ORDER BY p.created_at DESC,p.id DESC LIMIT 20";
const std::string kHashtag =
    "MATCH (t:BFTag {run:$run,id:$tag})<-[:TAGGED]-(p:BFPost) "
    "RETURN p.id,p.author,p.created_at,p.tag ORDER BY p.created_at DESC,p.id DESC LIMIT 20";
const std::string kCreate =
    "UNWIND $rows AS row MATCH (u:BFUser {run:$run,id:row.author}),(t:BFTag {run:$run,id:row.tag}) "
    "CREATE (p:BenchForge:BFPost {run:$run,id:row.id,author:row.author,created_at:row.ts,tag:row.tag}) "
    "CREATE (u)-[:AUTHORED]->(p),(p)-[:TAGGED]->(t) RETURN count(p)";
void Operators(const Json& plan, std::set<std::string>& result) {
    if (plan.contains("operatorType")) result.insert(plan.at("operatorType").get<std::string>());
    if (plan.contains("children")) for (const auto& child : plan.at("children")) Operators(child, result);
}
class Neo4jAdapter final : public DatabaseAdapter {
public:
    Neo4jAdapter(const RunConfig& config, std::string prefix)
        : host_(config.database_host), port_(config.database_port == 0 ? DefaultDatabasePort("neo4j") : config.database_port),
          run_(std::move(prefix)), client_(host_, port_) {}
    std::string Name() const override { return "neo4j"; }
    std::string Version() const override { return version_; }
    std::string Endpoint() const override { return host_ + ":" + std::to_string(port_) + "/db/neo4j/query/v2"; }
    std::string StorageConfiguration() const override {
        return "Neo4j database=neo4j; namespace=" + run_ +
            "; HTTP Query API v2; auth=none; persistent connection per worker; loader=seeded-v1; "
            "composite unique indexes=(run,id) on BFUser/BFPost/BFTag; native FOLLOWS/AUTHORED/TAGGED/LIKES; "
            "timeline=read-time traversal and top20 sort; follow/like=serialized idempotent MERGE; "
            "deadlock=up to 4 retries after explicit rollback, latency includes backoff; no socket/write retry; "
            "durability=server-managed transaction log; GC=not measured; " + settings_ + "; EXPLAIN operators=" + plans_;
    }
    void LoadDataset(const RunConfig& config) override {
        CheckCancellation();
        client_.Query("CREATE CONSTRAINT bf_run_unique IF NOT EXISTS FOR (n:BFRun) REQUIRE n.run IS UNIQUE");
        // CREATE + unique constraint rejects collisions. Ownership begins only after success.
        Query("CREATE (:BenchForge:BFRun {run:$run})"); owns_ = true; NamespaceAcquired();
        for (const auto& label : {"BFUser", "BFPost", "BFTag"})
            client_.Query("CREATE CONSTRAINT bf_" + std::string(label) +
                "_unique IF NOT EXISTS FOR (n:" + label + ") REQUIRE (n.run,n.id) IS UNIQUE");
        client_.Query("CALL db.awaitIndexes(60)");
        Json rows = Json::array();
        auto flush = [&](const std::string& statement) { if (!rows.empty()) { Query(statement, {{"rows", rows}}); rows.clear(); } };
        for (std::uint64_t id = 0; id < config.users; ++id) {
            rows.push_back(id);
            if (rows.size() == 256) flush("UNWIND $rows AS id CREATE (:BenchForge:BFUser {run:$run,id:id,display_name:'user|' + toString(id)})");
        }
        flush("UNWIND $rows AS id CREATE (:BenchForge:BFUser {run:$run,id:id,display_name:'user|' + toString(id)})");
        for (std::uint64_t id = 0; id < config.hashtags; ++id) {
            rows.push_back(id);
            if (rows.size() == 256) flush("UNWIND $rows AS id CREATE (:BenchForge:BFTag {run:$run,id:id})");
        }
        flush("UNWIND $rows AS id CREATE (:BenchForge:BFTag {run:$run,id:id})");
        const std::string follow = "UNWIND $rows AS row MATCH (u:BFUser {run:$run,id:row.source}),(v:BFUser {run:$run,id:row.target}) MERGE (u)-[:FOLLOWS]->(v)";
        auto edge = [&](std::uint64_t source, std::uint64_t target) {
            rows.push_back({{"source",source},{"target",target}}); if (rows.size() == 256) flush(follow);
        };
        if (config.scenario == Scenario::Celebrity) {
            const auto concentrated = std::min(config.follows / 2U, config.users - 1U);
            for (std::uint64_t source = 1; source <= concentrated; ++source) edge(source, 0);
            auto remaining = config.follows - concentrated;
            for (std::uint64_t source = 0; source < config.users && remaining; ++source)
                for (std::uint64_t target = 0; target < config.users && remaining; ++target)
                    if (source != target && target != 0) { edge(source,target); --remaining; }
            if (remaining) throw std::runtime_error("celebrity follow count cannot be represented");
        } else for (std::uint64_t i = 0; i < config.follows; ++i)
            edge(i % config.users, (i % config.users + 1U + (i / config.users) % (config.users - 1U)) % config.users);
        flush(follow);
        std::mt19937_64 random(config.seed);
        for (std::uint64_t id = 0; id < config.posts; ++id) {
            const auto author = random() % config.users; const auto tag = random() % config.hashtags;
            rows.push_back({{"id",id},{"author",author},{"ts",id+1U},{"tag",tag}});
            if (rows.size() == 256) flush(kCreate);
        }
        flush(kCreate);
        CheckCount("MATCH (n:BFUser {run:$run}) RETURN count(n)", config.users);
        CheckCount("MATCH (n:BFPost {run:$run}) RETURN count(n)", config.posts);
        CheckCount("MATCH (n:BFTag {run:$run}) RETURN count(n)", config.hashtags);
        CheckCount("MATCH (:BFUser {run:$run})-[r:FOLLOWS]->() RETURN count(r)", config.follows);
        CheckCount("MATCH (:BFUser {run:$run})-[r:AUTHORED]->() RETURN count(r)", config.posts);
        CheckCount("MATCH (:BFPost {run:$run})-[r:TAGGED]->() RETURN count(r)", config.posts);
        const auto components = client_.Query("CALL dbms.components() YIELD versions,edition RETURN versions[0],edition").at("data").at("values");
        version_ = "Neo4j " + components.at(0).at(0).get<std::string>() + " " + components.at(0).at(1).get<std::string>();
        const auto settings = client_.Query("SHOW SETTINGS YIELD name,value WHERE name IN ['server.memory.heap.initial_size','server.memory.heap.max_size','server.memory.pagecache.size','db.tx_log.flush_enabled'] RETURN name,value").at("data").at("values");
        for (const auto& row : settings) settings_ += row.at(0).get<std::string>() + "=" + row.at(1).get<std::string>() + "; ";
        for (const auto& query : {kTimeline, kHashtag}) {
            const auto plan = Query("EXPLAIN " + query, {{"user",0},{"tag",0}});
            std::set<std::string> names; Operators(plan.at("queryPlan"), names);
            for (const auto& name : names) plans_ += name + ",";
            plans_ += " / ";
        }
    }
    TransportCalibration Calibrate() override {
        LatencyHistogram histogram; std::uint64_t total = 0;
        constexpr std::uint64_t samples = 256;
        for (std::uint64_t i = 0; i < samples; ++i) {
            const auto start = std::chrono::steady_clock::now(); client_.Query("RETURN 1");
            const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());
            histogram.Record(ns); total += ns;
        }
        return {"HTTP Query API RETURN 1 round-trip (includes Cypher execution)",samples,histogram.Percentile(0.5),histogram.Percentile(0.95),total/samples};
    }
    void Execute(const Operation& op) override {
        const Json parameters{{"user",op.user_id},{"target",op.target_user_id},{"post",op.post_id},{"tag",op.hashtag_id}};
        switch (op.type) {
        case OperationType::TimelineRead: CheckPosts(Query(kTimeline, parameters)); return;
        case OperationType::HashtagSearch: CheckPosts(Query(kHashtag, parameters)); return;
        case OperationType::UserProfileRead: {
            const auto rows = Query("MATCH (u:BFUser {run:$run,id:$target}) RETURN u.display_name", parameters).at("data").at("values");
            if (rows.size() != 1 || !rows.at(0).at(0).is_string()) throw std::runtime_error("Neo4j profile missing");
            return;
        }
        case OperationType::PostCreate:
            CheckCount(kCreate, 1, {{"rows",Json::array({Json{{"id",op.post_id},{"author",op.user_id},{"ts",op.created_at_ms},{"tag",op.hashtag_id}}})}}); return;
        case OperationType::PostLike:
            CheckCount("MATCH (u:BFUser {run:$run,id:$user}),(p:BFPost {run:$run,id:$post}) "
                "SET p.lock=coalesce(p.lock,0)+1 MERGE (u)-[r:LIKES]->(p) RETURN count(r)", 1, parameters); return;
        case OperationType::UserFollow:
            CheckCount("MATCH (u:BFUser {run:$run,id:$user}),(v:BFUser {run:$run,id:$target}) "
                "SET u.lock=coalesce(u.lock,0)+1 MERGE (u)-[r:FOLLOWS]->(v) RETURN count(r)", 1, parameters); return;
        case OperationType::Count: throw std::invalid_argument("invalid operation type");
        }
    }
    std::string RecoverNamespace() noexcept override { owns_ = true; return Cleanup(); }
    std::string Cleanup() noexcept override {
        if (!owns_) return "no run graph owned";
        try {
            // A fresh connection can clean up after a broken measurement socket.
            Neo4jClient cleanup(host_,port_);
            while (true) {
                const auto rows = cleanup.Query("MATCH (n:BenchForge {run:$run}) WHERE NOT n:BFRun WITH n LIMIT 256 DETACH DELETE n RETURN count(*)", {{"run",run_}}).at("data").at("values");
                if (rows.at(0).at(0).get<std::uint64_t>() == 0) break;
            }
            cleanup.Query("MATCH (n:BenchForge:BFRun {run:$run}) DELETE n", {{"run",run_}});
            owns_ = false; return "run graph removed; shared indexes retained";
        } catch (const std::exception& error) { return "cleanup failed: " + std::string(error.what()); }
    }
private:
    Json Query(const std::string& statement, Json parameters = Json::object()) {
        CheckCancellation();
        parameters["run"] = run_;
        for (unsigned attempt=0;;++attempt) {
            try { return client_.Query(statement, parameters); }
            catch (const Neo4jQueryError& error) {
                // Query API rolls the implicit transaction back on this explicit error.
                // Unknown completion/socket failures must never replay a write.
                if (error.Code() != "Neo.TransientError.Transaction.DeadlockDetected" || attempt==4) throw;
                std::this_thread::sleep_for(std::chrono::milliseconds(10U*(attempt+1U)));
            }
        }
    }
    void CheckCount(const std::string& statement, std::uint64_t expected, Json parameters = Json::object()) {
        const auto rows = Query(statement,std::move(parameters)).at("data").at("values");
        if (rows.size() != 1 || rows.at(0).size() != 1 || rows.at(0).at(0).get<std::uint64_t>() != expected)
            throw std::runtime_error("Neo4j result count mismatch");
    }
    static void CheckPosts(const Json& result) {
        const auto& rows = result.at("data").at("values");
        if (rows.size() > 20) throw std::runtime_error("Neo4j result exceeds top20");
        std::uint64_t previous_ts = UINT64_MAX, previous_id = UINT64_MAX;
        for (const auto& row : rows) {
            if (row.size() != 4) throw std::runtime_error("Neo4j post shape mismatch");
            for (const auto& value : row) if (!value.is_number_unsigned() && !(value.is_number_integer() && value.get<std::int64_t>() >= 0))
                throw std::runtime_error("Neo4j post value invalid");
            const auto id = row.at(0).get<std::uint64_t>(), ts = row.at(2).get<std::uint64_t>();
            if (ts > previous_ts || (ts == previous_ts && id >= previous_id)) throw std::runtime_error("Neo4j post order mismatch");
            previous_ts = ts; previous_id = id;
        }
    }
    std::string host_; std::uint16_t port_; std::string run_; Neo4jClient client_;
    bool owns_{false}; std::string version_{"Neo4j (not loaded)"}, settings_, plans_;
};
}
std::unique_ptr<DatabaseAdapter> CreateNeo4jAdapter(const RunConfig& config, const std::string& prefix) {
    return std::make_unique<Neo4jAdapter>(config,prefix);
}
} // namespace benchforge::detail
