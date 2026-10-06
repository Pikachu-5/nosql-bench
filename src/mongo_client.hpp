#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace benchforge::detail {

using BsonDocument = std::string;

struct MongoValue {
    enum class Kind { Null, Boolean, Number, String, Document, Array } kind{Kind::Null};
    bool boolean{false};
    double number{0};
    std::int64_t integer{0};
    bool integer_value{false};
    std::string text;
    std::unordered_map<std::string, MongoValue> fields;
    std::vector<MongoValue> elements;

    const MongoValue* Find(const std::string& name) const noexcept;
    std::int64_t Integer(const std::string& context) const;
    const std::string& String(const std::string& context) const;
    const std::vector<MongoValue>& Array(const std::string& context) const;
};

class BsonBuilder {
public:
    BsonBuilder& Int32(const std::string& name, std::int32_t value);
    BsonBuilder& Int64(const std::string& name, std::int64_t value);
    BsonBuilder& Boolean(const std::string& name, bool value);
    BsonBuilder& String(const std::string& name, const std::string& value);
    BsonBuilder& Document(const std::string& name, const BsonDocument& value);
    BsonBuilder& Array(const std::string& name, const BsonDocument& value);
    BsonBuilder& RawFields(const BsonDocument& value);
    BsonDocument Finish() const;

private:
    std::string elements_;
    void Element(std::uint8_t type, const std::string& name,
                 const std::string& value);
};

BsonDocument ArrayDocuments(const std::vector<BsonDocument>& values);
BsonDocument ArrayInt64(const std::vector<std::int64_t>& values);
BsonDocument ArrayStrings(const std::vector<std::string>& values);
MongoValue ParseBsonDocument(const std::string& bytes, std::size_t offset = 0,
                             std::size_t* next_offset = nullptr);

class MongoClient {
public:
    MongoClient(const std::string& host, std::uint16_t port);
    ~MongoClient();

    MongoClient(const MongoClient&) = delete;
    MongoClient& operator=(const MongoClient&) = delete;
    MongoClient(MongoClient&& other) noexcept;
    MongoClient& operator=(MongoClient&& other) noexcept;

    MongoValue Command(const std::string& database,
                       const BsonDocument& command);

private:
    std::intptr_t socket_{-1};
    std::int32_t request_id_{0};
    std::string Exchange(const BsonDocument& command);
    void Send(const std::string& bytes);
    std::string ReadExact(std::size_t length);
};

void RequireMongoOk(const MongoValue& reply, const std::string& context);

} // namespace benchforge::detail
