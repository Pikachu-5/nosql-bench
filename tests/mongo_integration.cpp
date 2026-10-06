#include "database_adapter.hpp"
#include "mongo_client.hpp"
#include <chrono>
#include <iostream>
#include <random>
using namespace benchforge;
using namespace benchforge::detail;
void Check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void Test(std::uint16_t port,Scenario scenario) {
    RunConfig config; config.adapter="mongo";config.database_port=port;config.scenario=scenario;
    config.users=32;config.posts=40;config.follows=64;config.hashtags=4;
    const auto prefix="benchforge:mongo_test_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+":";
    auto adapter=CreateMongoAdapter(config,prefix);
    MongoClient probe("127.0.0.1",port);
    const auto endpoint=adapter->Endpoint();const auto database=endpoint.substr(endpoint.find('/')+1);
    auto count=[&](const std::string& collection,BsonDocument filter=BsonBuilder().Finish()) {
        BsonBuilder command; command.String("count",collection).Document("query",filter);
        const auto reply=probe.Command(database,command.Finish());const auto* n=reply.Find("n");
        Check(n!=nullptr,"count missing");return n->Integer("count");
    };
    auto find=[&](const std::string& collection,BsonDocument filter) {
        BsonBuilder command;command.String("find",collection).Document("filter",filter).Int32("limit",20).Boolean("singleBatch",true);
        const auto reply=probe.Command(database,command.Finish());const auto* cursor=reply.Find("cursor");
        Check(cursor && cursor->Find("firstBatch"),"cursor missing");return cursor->Find("firstBatch")->Array("batch");
    };
    try {
        adapter->LoadDataset(config);
        Check(count("users")==32 && count("posts")==40 && count("follows")==64,"seed counts incorrect");
        std::mt19937_64 random(config.seed);
        for(std::uint64_t id=0;id<40;++id) {
            const auto author=random()%32, tag=random()%4;BsonBuilder filter;filter.Int64("_id",static_cast<std::int64_t>(id));
            const auto rows=find("posts",filter.Finish());Check(rows.size()==1,"seeded post absent");
            Check(rows[0].Find("author")->Integer("author")==static_cast<std::int64_t>(author) &&
                rows[0].Find("createdAt")->Integer("timestamp")==static_cast<std::int64_t>(id+1) &&
                rows[0].Find("hashtags")->Array("tags")[0].Integer("tag")==static_cast<std::int64_t>(tag),"seed differs");
        }
        auto collision=CreateMongoAdapter(config,prefix);bool rejected=false;
        try { collision->LoadDataset(config); } catch(const std::exception&) { rejected=true; }
        Check(rejected && collision->Cleanup()=="no run database owned" && count("posts")==40,"collision damaged database");
        auto worker=CreateMongoAdapter(config,prefix);Check(worker->Cleanup()=="no run database owned","worker owns namespace");
        Operation op;op.type=OperationType::PostLike;op.post_id=0;op.user_id=7;
        worker->Execute(op);worker->Execute(op);op.user_id=8;worker->Execute(op);
        BsonBuilder zero;zero.Int64("_id",0);const auto liked=find("posts",zero.Finish());
        Check(liked[0].Find("likeCount")->Integer("likes")==2 && liked[0].Find("likes")->Array("users").size()==2,"duplicate like counted");
        op.type=OperationType::PostCreate;op.user_id=31;op.created_at_ms=1001;op.hashtag_id=2;
        for(std::uint64_t id=980;id<=1010;++id) { op.post_id=id;worker->Execute(op); }
        Check(count("posts")==71,"created posts missing");
        BsonBuilder edge;edge.Int64("source",0).Int64("target",31);auto before=count("follows");auto present=count("follows",edge.Finish());
        op.type=OperationType::UserFollow;op.user_id=0;op.target_user_id=31;worker->Execute(op);worker->Execute(op);
        Check(count("follows")==before+(present==0?1:0),"duplicate follow counted");
        op.type=OperationType::TimelineRead;worker->Execute(op);op.type=OperationType::HashtagSearch;worker->Execute(op);
        op.type=OperationType::UserProfileRead;worker->Execute(op);
        Check(adapter->Calibrate().samples==256,"calibration incomplete");
        Check(adapter->Cleanup()=="run database removed" && count("posts")==0,"cleanup leaked data");
        std::cout<<"mongo "<<ToString(scenario)<<": seeds, six operations, likes, collision ownership and cleanup passed\n";
    } catch(...) { std::cerr<<adapter->Cleanup()<<'\n';throw; }
}
int main(int argc,char** argv) {
    try { if(argc!=3 || std::string(argv[1])!="mongo") throw std::runtime_error("usage: mongo_integration mongo PORT");
        auto port=std::stoul(argv[2]);if(port==0 || port>65535) throw std::runtime_error("invalid port");
        Test(static_cast<std::uint16_t>(port),Scenario::Normal);Test(static_cast<std::uint16_t>(port),Scenario::Celebrity);return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
