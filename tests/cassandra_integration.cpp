#include "database_adapter.hpp"
#include "cql_client.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace benchforge;
using namespace benchforge::detail;
namespace {
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::int64_t Count(CqlClient& client, const std::string& table) {
    const auto result = client.Query("SELECT count(*) FROM " + table);
    Check(result.rows.size() == 1 && result.rows[0].size() == 1, "count result missing");
    return CqlReadBigint(result.rows[0][0]);
}
void ScenarioCheck(Scenario scenario, const std::string& run) {
    RunConfig config;
    config.adapter = "cassandra"; config.scenario = scenario;
    config.users = 300; config.posts = 40; config.follows = 600; config.hashtags = 4;
    const auto adapter = CreateCassandraAdapter(config, "benchforge:" + run + ":");
    try {
        adapter->LoadDataset(config);
        const auto endpoint = adapter->Endpoint();
        const auto ks = endpoint.substr(endpoint.find('/') + 1U);
        Check(ks.rfind("benchforge_run_", 0) == 0 && ks.size() == 31, "unsafe keyspace name");
        CqlClient probe(config.database_host, 9042);
        std::mt19937_64 random(config.seed);
        for (std::uint64_t id = 0; id < config.posts; ++id) {
            const auto author = random() % config.users;
            const auto tag = random() % config.hashtags;
            const auto row = probe.Query("SELECT author,created_at,tag FROM " + ks + ".posts WHERE id=" + std::to_string(id));
            Check(row.rows.size() == 1 && row.rows[0].size() == 3, "seeded post absent");
            Check(CqlReadBigint(row.rows[0][0]) == static_cast<std::int64_t>(author) &&
                  CqlReadBigint(row.rows[0][1]) == static_cast<std::int64_t>(id + 1U) &&
                  CqlReadBigint(row.rows[0][2]) == static_cast<std::int64_t>(tag), "seed mismatch");
        }
        const auto collision = CreateCassandraAdapter(config, "benchforge:" + run + ":");
        bool rejected = false;
        try { collision->LoadDataset(config); } catch (const std::exception&) { rejected = true; }
        Check(rejected && collision->Cleanup() == "no run keyspace owned", "namespace collision adopted existing data");
        Check(Count(probe, ks + ".posts") == 40, "collision damaged original run");

        const auto worker = CreateCassandraAdapter(config, "benchforge:" + run + ":");
        worker->PrepareWorker();
        Check(worker->Cleanup() == "no run keyspace owned", "worker owns loader keyspace");
        Operation op;
        op.type = OperationType::UserProfileRead; op.target_user_id = 12; worker->Execute(op);
        op.type = OperationType::PostLike; op.post_id = 0; op.user_id = 12;
        worker->Execute(op); worker->Execute(op); op.user_id = 13; worker->Execute(op);
        Check(Count(probe, ks + ".likes_by_post WHERE post=0") == 2, "duplicate likes counted");
        op.type = OperationType::PostCreate; op.post_id = 1000; op.user_id = 299; op.created_at_ms = 1001; op.hashtag_id = 2;
        // Equal timestamps deliberately exercise the descending ID tie-break and top20 cap.
        for (std::uint64_t id = 1000; id <= 1030; ++id) { op.post_id = id; worker->Execute(op); }
        Check(Count(probe, ks + ".posts") == 71 && Count(probe, ks + ".posts_by_author") == 71 &&
              Count(probe, ks + ".posts_by_tag") == 71, "post denormalization incomplete");
        const auto before = Count(probe, ks + ".follows_by_user");
        const auto present = Count(probe, ks + ".follows_by_user WHERE source=0 AND target=299");
        op.type = OperationType::UserFollow; op.user_id = 0; op.target_user_id = 299;
        worker->Execute(op); worker->Execute(op);
        Check(Count(probe, ks + ".follows_by_user") == before + (present == 0 ? 1 : 0), "duplicate follow counted");
        op.type = OperationType::TimelineRead; worker->Execute(op);
        op.type = OperationType::HashtagSearch; worker->Execute(op);
        const auto tag = probe.Query("SELECT id FROM " + ks + ".posts_by_tag WHERE tag=2 LIMIT 20");
        Check(tag.rows.size() == 20 && CqlReadBigint(tag.rows.front()[0]) == 1030 &&
              CqlReadBigint(tag.rows.back()[0]) == 1011, "hashtag top20 ordering incorrect");
        if (scenario == Scenario::Celebrity) {
            const auto statement = probe.Prepare("SELECT target FROM " + ks + ".follows_by_user WHERE source=?");
            const auto first = probe.Execute(statement, {CqlBigint(0)});
            Check(!first.paging_state.empty(), "test did not exercise follow paging");
            const auto second = probe.Execute(statement, {CqlBigint(0)}, first.paging_state);
            Check(first.rows.size() + second.rows.size() == 299 && second.paging_state.empty(), "follow pages lost rows");
        }
        const auto calibration = adapter->Calibrate();
        Check(calibration.samples == 256, "calibration sample count incorrect");
        Check(adapter->Cleanup() == "run keyspace removed", "cleanup failed");
        const auto remaining = probe.Query("SELECT keyspace_name FROM system_schema.keyspaces WHERE keyspace_name='" + ks + "'");
        Check(remaining.rows.empty(), "run keyspace remained after cleanup");
        std::cout << ToString(scenario) << ": seeded data, operations, paging, ownership and cleanup passed\n";
    } catch (...) { std::cerr << adapter->Cleanup() << '\n'; throw; }
}
} // namespace
int main() {
    try {
        const auto token = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        ScenarioCheck(Scenario::Normal, "integration_normal_" + token);
        ScenarioCheck(Scenario::Celebrity, "integration_celebrity_" + token);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
