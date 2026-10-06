#include "database_adapter.hpp"
#include "resp_client.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <unordered_map>

using namespace benchforge;
using namespace benchforge::detail;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string Member(std::uint64_t id) { auto value=std::to_string(id); return std::string(20-value.size(),'0')+value; }
void Verify(const std::string& name, std::uint16_t port, Scenario scenario, const std::string& token) {
    RunConfig config; config.adapter=name; config.database_port=port; config.scenario=scenario;
    config.users=32; config.posts=128; config.follows=64; config.hashtags=8;
    const auto prefix="benchforge:resp_"+token+"_"+ToString(scenario)+":";
    auto loader=CreateNetworkAdapter(name,config,prefix);
    RespClient probe("127.0.0.1",port);
    const auto sentinel="benchforge:unrelated_"+token;
    probe.Command({"SET",sentinel,"preserve"});
    try {
        loader->LoadDataset(config);
        std::mt19937_64 random(config.seed);
        std::unordered_map<std::uint64_t,std::vector<std::uint64_t>> authors;
        for (std::uint64_t id=0;id<config.posts;++id) {
            auto author=random()%config.users, tag=random()%config.hashtags; authors[author].push_back(id);
            Check(RequireText(probe.Command({"GET",prefix+"post:"+std::to_string(id)}),"seed")==
                std::to_string(author)+"|"+std::to_string(id+1)+"|"+std::to_string(tag),"deterministic seed mismatch");
        }
        for (std::uint64_t reader=0;reader<config.users;++reader) {
            auto following=probe.Command({"SMEMBERS",prefix+"following:"+std::to_string(reader)});
            std::vector<std::uint64_t> expected;
            for (const auto& author:following.elements) {
                auto id=std::stoull(RequireText(author,"author"));
                expected.insert(expected.end(),authors[id].begin(),authors[id].end());
                Check(RequireInteger(probe.Command({"SISMEMBER",prefix+"followers:"+std::to_string(id),std::to_string(reader)}),"reverse")==1,"reverse follow absent");
            }
            std::sort(expected.begin(),expected.end(),std::greater<>()); if(expected.size()>20) expected.resize(20);
            auto actual=probe.Command({"ZREVRANGE",prefix+"feed:"+std::to_string(reader),"0","19"});
            Check(actual.elements.size()==expected.size(),"seeded timeline size mismatch");
            for(std::size_t i=0;i<expected.size();++i) Check(RequireText(actual.elements[i],"feed")==Member(expected[i]),"seeded timeline ordering mismatch");
        }
        auto collision=CreateNetworkAdapter(name,config,prefix); bool rejected=false;
        try { collision->LoadDataset(config); } catch(const std::exception&) { rejected=true; }
        Check(rejected && collision->Cleanup()=="no run namespace owned","collision adopted data");
        auto worker=CreateNetworkAdapter(name,config,prefix);
        Check(worker->Cleanup()=="no run namespace owned","worker adopted namespace");
        Operation op; op.type=OperationType::PostLike; op.user_id=7; op.post_id=0;
        worker->Execute(op); worker->Execute(op); op.user_id=8; worker->Execute(op);
        Check(RequireInteger(probe.Command({"SCARD",prefix+"likes:0"}),"likes")==2,"duplicate like counted");
        op.type=OperationType::PostCreate; op.user_id=31; op.created_at_ms=1001; op.hashtag_id=2;
        // Cross-width IDs with tied timestamps distinguish numeric from lexical ordering.
        for(std::uint64_t id=980;id<=1010;++id) { op.post_id=id; worker->Execute(op); }
        op.type=OperationType::UserFollow; op.user_id=0; op.target_user_id=31;
        worker->Execute(op); worker->Execute(op);
        auto feed=probe.Command({"ZREVRANGE",prefix+"feed:0","0","-1"});
        Check(feed.elements.size()==20 && feed.elements.front().text==Member(1010) && feed.elements.back().text==Member(991),"follow backfill top20 incorrect");
        op.type=OperationType::TimelineRead; worker->Execute(op);
        op.type=OperationType::HashtagSearch; worker->Execute(op);
        op.type=OperationType::UserProfileRead; worker->Execute(op);
        op.type=OperationType::PostCreate; op.user_id=31; op.post_id=2000; op.created_at_ms=2001; worker->Execute(op);
        feed=probe.Command({"ZREVRANGE",prefix+"feed:0","0","-1"});
        Check(feed.elements.size()==20 && feed.elements.front().text==Member(2000),"post fanout top20 incorrect");
        Check(loader->Calibrate().samples==256,"calibration incomplete");
        Check(loader->Cleanup()=="run namespace removed","cleanup failed");
        auto scan=probe.Command({"SCAN","0","MATCH",prefix+"*","COUNT","10000"});
        Check(scan.elements[1].elements.empty(),"namespace leaked");
        Check(probe.Command({"GET",sentinel}).text=="preserve","unrelated namespace damaged");
        probe.Command({"DEL",sentinel});
        std::cout<<name<<" "<<ToString(scenario)<<": seeds, six operations, numeric ties, ownership and cleanup passed\n";
    } catch(...) { std::cerr<<loader->Cleanup()<<'\n'; probe.Command({"DEL",sentinel}); throw; }
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("usage: resp_integration feedkv|redis PORT");
        const auto port=std::stoul(argv[2]); if(port==0 || port>65535) throw std::runtime_error("invalid port");
        const auto token=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        Verify(argv[1],static_cast<std::uint16_t>(port),Scenario::Normal,token);
        Verify(argv[1],static_cast<std::uint16_t>(port),Scenario::Celebrity,token);
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
