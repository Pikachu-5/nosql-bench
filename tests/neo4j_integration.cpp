#include "database_adapter.hpp"
#include "neo4j_client.hpp"
#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
using namespace benchforge;
using namespace benchforge::detail;
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Test(Scenario scenario) {
    RunConfig config; config.adapter="neo4j"; config.scenario=scenario;
    config.users=32; config.posts=40; config.follows=64; config.hashtags=4;
    const std::string run = "benchforge:neo_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ":";
    auto adapter=CreateNeo4jAdapter(config,run);
    Neo4jClient probe("127.0.0.1",7474);
    auto query = [&](const std::string& statement) { return probe.Query(statement,{{"run",run}}).at("data").at("values"); };
    try {
        adapter->LoadDataset(config);
        auto collision=CreateNeo4jAdapter(config,run);
        bool rejected=false; try { collision->LoadDataset(config); } catch(const std::exception&) { rejected=true; }
        Check(rejected && collision->Cleanup()=="no run graph owned", "collision adopted existing graph");
        auto worker=CreateNeo4jAdapter(config,run);
        Check(worker->Cleanup()=="no run graph owned", "worker owns graph");
        std::mt19937_64 random(config.seed);
        const auto posts=query("MATCH (p:BFPost {run:$run}) RETURN p.id,p.author,p.created_at,p.tag ORDER BY p.id");
        Check(posts.size()==40, "seeded count incorrect");
        for (std::uint64_t id=0; id<40; ++id) {
            const auto author=random()%32, tag=random()%4;
            Check(posts.at(id)==nlohmann::json::array({id,author,id+1,tag}), "seed mismatch");
        }
        const auto expected=query("MATCH (u:BFUser {run:$run,id:0})-[:FOLLOWS]->(v) RETURN v.id ORDER BY v.id");
        Check(!expected.empty(), "follow graph missing");
        if (scenario==Scenario::Normal) Check(expected==nlohmann::json::array({nlohmann::json::array({1}),nlohmann::json::array({2})}), "normal follow graph wrong");
        else Check(query("MATCH (:BFUser {run:$run})-[r:FOLLOWS]->(:BFUser {run:$run,id:0}) RETURN count(r)").at(0).at(0)==31, "celebrity graph wrong");
        Operation op; op.type=OperationType::UserProfileRead; op.target_user_id=12; worker->Execute(op);
        op.type=OperationType::PostCreate; op.user_id=31; op.hashtag_id=2; op.created_at_ms=1001;
        for(std::uint64_t id=1000; id<=1030; ++id) { op.post_id=id; worker->Execute(op); }
        op.type=OperationType::UserFollow; op.user_id=0; op.target_user_id=31;
        worker->Execute(op); worker->Execute(op);
        Check(query("MATCH (:BFUser {run:$run,id:0})-[r:FOLLOWS]->(:BFUser {run:$run,id:31}) RETURN count(r)").at(0).at(0)==1,"follow not idempotent");
        op.type=OperationType::TimelineRead; worker->Execute(op);
        op.type=OperationType::HashtagSearch; worker->Execute(op);
        const auto newest=query("MATCH (:BFTag {run:$run,id:2})<-[:TAGGED]-(p) RETURN p.id ORDER BY p.created_at DESC,p.id DESC LIMIT 20");
        Check(newest.size()==20 && newest.at(0).at(0)==1030 && newest.at(19).at(0)==1011,"top20 tie-break wrong");
        // Concurrent duplicate likes must remain a single relationship.
        std::vector<std::thread> threads; std::vector<std::exception_ptr> failures(4);
        for(std::size_t i=0;i<4;++i) threads.emplace_back([&,i] {
            try { auto concurrent=CreateNeo4jAdapter(config,run); Operation like;
                like.type=OperationType::PostLike; like.user_id=12; like.post_id=0;
                for(int j=0;j<5;++j) concurrent->Execute(like);
            } catch(...) { failures[i]=std::current_exception(); }
        });
        for(auto& thread:threads) thread.join(); for(auto& failure:failures) if(failure) std::rethrow_exception(failure);
        Check(query("MATCH (:BFUser {run:$run,id:12})-[r:LIKES]->(:BFPost {run:$run,id:0}) RETURN count(r)").at(0).at(0)==1,"concurrent likes duplicated");
        Check(adapter->Calibrate().samples==256,"calibration wrong");
        Check(adapter->StorageConfiguration().find("NodeUniqueIndexSeek")!=std::string::npos,"indexed starting node plan missing");
        Check(adapter->Cleanup()=="run graph removed; shared indexes retained","cleanup failed");
        Check(query("MATCH (n:BenchForge {run:$run}) RETURN count(n)").at(0).at(0)==0,"graph remained after cleanup");
    } catch(...) { std::cerr << adapter->Cleanup() << '\n'; throw; }
}
int main() { try { Test(Scenario::Normal); Test(Scenario::Celebrity); std::cout<<"Neo4j integration passed\n"; return 0; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; } }
