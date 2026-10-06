#include "neo4j_client.hpp"
#include <iostream>
using namespace benchforge::detail;
int main(int argc, char** argv) {
    try {
        for (const std::string body : {"[]", "{}", "{\"data\":{\"values\":{} }}",
             "{\"errors\":[{\"code\":\"Neo.ClientError.Statement.SyntaxError\"}],\"data\":{\"values\":[]}}"}) {
            bool rejected = false;
            try { ParseNeo4jResponse(body); } catch (const std::exception&) { rejected = true; }
            if (!rejected) throw std::runtime_error("malformed/error response accepted");
        }
        if (argc == 2) {
            Neo4jClient client("127.0.0.1",static_cast<std::uint16_t>(std::stoi(argv[1])));
            for (int i = 0; i < 2; ++i)
                if (client.Query("RETURN $value", {{"value",i}}).at("data").at("values").at(0).at(0) != i)
                    throw std::runtime_error("persistent HTTP framing lost response");
            bool rejected = false;
            try { client.Query("invalid"); } catch (const std::exception&) { rejected = true; }
            if (!rejected) throw std::runtime_error("HTTP202 query error accepted");
        }
        std::cout << "Neo4j response checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
