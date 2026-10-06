#include "result_archive.hpp"
#include <fstream>
#include <iostream>
#include <chrono>
using namespace benchforge::detail;
using Json=nlohmann::json;
void Check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main() {
    // All test writes and cleanup stay in this unique, resolved build fixture directory.
    const auto root=std::filesystem::absolute("archive_fixture_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        auto save=[&](const std::filesystem::path& path,std::string id) {
            std::filesystem::create_directories(path);
            std::ofstream(path / "summary.json") << Json{{"schema_version",2},{"run_id",id},{"adapter","neo4j"},{"scenario","normal"},
                {"started_at_utc","2026-10-06T12:00:00Z"},{"valid",true},{"config",Json::object()},{"operations",Json::array()}}.dump();
        };
        Check(ListArchivedResults(root).at("results").empty(),"missing root is not empty");
        save(root / "run_one","run_one"); save(root / "api" / "run_one","run_one");
        save(root / "root" / "run_two","run_two");
        save(root / "verification" / "mismatch","other_id");
        save(root / "api" / "broken","broken"); std::ofstream(root/"api"/"broken"/"summary.json")<<"{";
        save(root / "api" / "oversized","oversized"); std::ofstream(root/"api"/"oversized"/"summary.json")<<std::string(1024*1024+1,'x');
        const auto archive=ListArchivedResults(root);
        Check(archive.at("results").size()==3 && archive.at("skipped")==3,"archive did not skip malformed/mismatched/oversized summaries");
        for(const auto& result:archive.at("results"))
            Check(ReadArchivedResult(root,result.at("key").get<std::string>()).at("run_id")==result.at("runId"),"archive key lost bucket identity");
        for(const std::string key:{"../api~run_one","api~../run_one","api~run_one/summary.json","api~broken","api~oversized","api~mismatch","invalid"}) {
            bool rejected=false; try { ReadArchivedResult(root,key); } catch(const std::exception&) { rejected=true; }
            Check(rejected,"unsafe/unreadable archive key accepted");
        }
        std::filesystem::remove_all(root); std::cout<<"Result archive checks passed\n"; return 0;
    } catch(const std::exception& error) { std::filesystem::remove_all(root); std::cerr<<error.what()<<'\n'; return 1; }
}
