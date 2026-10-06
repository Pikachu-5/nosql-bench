#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace benchforge::detail {

using CqlRow = std::vector<std::optional<std::string>>;
struct CqlResult {
    std::vector<std::string> columns;
    std::vector<CqlRow> rows;
    std::string paging_state;
};
struct CqlStatement {
    std::string id;
    std::vector<std::string> values;
};

// One synchronous connection per worker; protocol v4, no auth/TLS/compression.
class CqlClient {
public:
    CqlClient(const std::string& host, std::uint16_t port);
    ~CqlClient();
    CqlClient(const CqlClient&) = delete;
    CqlClient& operator=(const CqlClient&) = delete;

    CqlResult Query(const std::string& query);
    std::string Prepare(const std::string& query);
    CqlResult Execute(const std::string& id, const std::vector<std::string>& values,
                      const std::string& paging_state = {});
    void Batch(const std::vector<CqlStatement>& statements);
    void Probe();

private:
    std::intptr_t socket_{-1};
    std::string Exchange(std::uint8_t opcode, const std::string& body,
                         std::uint8_t expected_opcode);
    void Send(const std::string& bytes);
    std::string ReadExact(std::size_t size);
};

std::string CqlBigint(std::uint64_t value);
std::int64_t CqlReadBigint(const std::optional<std::string>& value);
CqlResult ParseCqlResult(const std::string& body);

} // namespace benchforge::detail
