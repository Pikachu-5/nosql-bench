#include "cql_client.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>

using namespace benchforge::detail;
namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Reject(const std::function<void()>& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("malformed input accepted");
}
void Number(std::string& out, std::uint64_t value, unsigned size) {
    for (unsigned i = size; i != 0; --i) out.push_back(static_cast<char>(value >> ((i - 1U) * 8U)));
}
void String(std::string& out, const std::string& value) { Number(out, value.size(), 2); out += value; }
void Bytes(std::string& out, const std::string& value) { Number(out, value.size(), 4); out += value; }
std::string RowFrame() {
    std::string out;
    Number(out, 2, 4);
    Number(out, 3, 4); // global table spec + paging
    Number(out, 2, 4);
    Bytes(out, "next-page");
    String(out, "ks"); String(out, "table");
    String(out, "id"); Number(out, 2, 2);
    String(out, "name"); Number(out, 0x0d, 2);
    Number(out, 2, 4);
    Bytes(out, CqlBigint(42)); Bytes(out, "user|42");
    Bytes(out, CqlBigint(0)); Number(out, 0xffffffffU, 4);
    return out;
}
} // namespace
int main() {
    try {
        const auto frame = RowFrame();
        const auto result = ParseCqlResult(frame);
        Check(result.columns == std::vector<std::string>{"id", "name"}, "metadata incorrect");
        Check(result.paging_state == "next-page", "paging state lost");
        Check(result.rows.size() == 2 && CqlReadBigint(result.rows[0][0]) == 42, "bigint incorrect");
        Check(result.rows[0][1] == "user|42" && !result.rows[1][1], "text/null incorrect");
        for (std::size_t end = 0; end < frame.size(); ++end) {
            Reject([&] { (void)ParseCqlResult(frame.substr(0, end)); });
        }
        Check(CqlReadBigint(CqlBigint(0x7fffffffffffffffULL)) == 0x7fffffffffffffffLL, "signed bigint boundary");
        Reject([] { (void)CqlBigint(0x8000000000000000ULL); });
        Reject([] { (void)CqlReadBigint(std::nullopt); });
        Reject([] { (void)CqlReadBigint(std::string(7, 'x')); });
        std::string invalid;
        Number(invalid, 2, 4); Number(invalid, 0, 4); Number(invalid, 0x7fffffff, 4);
        Reject([&] { (void)ParseCqlResult(invalid); });
        invalid.clear(); Number(invalid, 2, 4); Number(invalid, 4, 4); Number(invalid, 1, 4);
        Reject([&] { (void)ParseCqlResult(invalid); });
        std::string collection;
        Number(collection, 2, 4); Number(collection, 1, 4); Number(collection, 1, 4);
        String(collection, "ks"); String(collection, "t"); String(collection, "settings");
        Number(collection, 0x21, 2); Number(collection, 0x0d, 2); Number(collection, 0x22, 2); Number(collection, 2, 2);
        Number(collection, 0, 4);
        Check(ParseCqlResult(collection).columns[0] == "settings", "collection metadata incorrect");
        std::cout << "CQL parsing, truncation, paging, collections, and bigint checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
