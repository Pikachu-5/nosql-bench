#pragma once
#include "benchforge/adapter.hpp"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>

namespace benchforge::detail {
class Neo4jQueryError : public std::runtime_error {
public:
    Neo4jQueryError(std::string code, const std::string& message)
        : std::runtime_error(message), code_(std::move(code)) {}
    const std::string& Code() const { return code_; }
private:
    std::string code_;
};
// One persistent HTTP/1.1 connection per adapter/worker. No automatic write retry.
class Neo4jClient {
public:
    Neo4jClient(const std::string& host, std::uint16_t port);
    ~Neo4jClient();
    Neo4jClient(const Neo4jClient&) = delete;
    Neo4jClient& operator=(const Neo4jClient&) = delete;
    nlohmann::json Query(const std::string& statement,
                         const nlohmann::json& parameters = nlohmann::json::object());
private:
    std::intptr_t socket_{-1};
    std::string host_, input_;
    void Send(const std::string& bytes);
    std::string Line();
    std::string Exact(std::size_t size);
    void Receive();
};
// Checks Neo4j's error array even when HTTP status is 202.
nlohmann::json ParseNeo4jResponse(const std::string& body);
} // namespace benchforge::detail
