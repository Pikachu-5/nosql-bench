cmake_minimum_required(VERSION 3.22)

project(BenchForge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(benchforge
    src/main.cpp
    src/config.cpp
    src/environment.cpp
    src/operation.cpp
    src/workload.cpp
    src/noop_adapter.cpp
    src/resp_client.cpp
    src/mongo_client.cpp
    src/mongo_adapter.cpp
    src/cql_client.cpp
    src/cassandra_adapter.cpp
    src/database_adapter.cpp
    src/feedkv_server.cpp
    src/runner.cpp
    src/results.cpp
    src/child_process.cpp
    src/control_api.cpp
)

target_include_directories(benchforge PRIVATE include)
target_link_libraries(benchforge PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(benchforge PRIVATE ws2_32)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(cql_tests tests/cql_tests.cpp src/cql_client.cpp)
    target_include_directories(cql_tests PRIVATE include src)
    add_executable(cassandra_integration tests/cassandra_integration.cpp
        src/cql_client.cpp src/cassandra_adapter.cpp src/config.cpp src/operation.cpp)
    target_include_directories(cassandra_integration PRIVATE include src)
    if(WIN32)
        target_link_libraries(cql_tests PRIVATE ws2_32)
        target_link_libraries(cassandra_integration PRIVATE ws2_32)
    endif()
    target_link_libraries(cql_tests PRIVATE Threads::Threads)
    target_link_libraries(cassandra_integration PRIVATE Threads::Threads)
    add_test(NAME cql_parser COMMAND cql_tests)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME open_loop_send_lag COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/open_loop_regression.py" $<TARGET_FILE:benchforge>)
    endif()
    option(BENCHFORGE_LIVE_DATABASE_TESTS "Run tests against local Cassandra on port 9042" OFF)
    if(BENCHFORGE_LIVE_DATABASE_TESTS)
        add_test(NAME cassandra_live COMMAND cassandra_integration)
        set_tests_properties(cassandra_live PROPERTIES LABELS integration TIMEOUT 180)
    endif()
endif()

if(MSVC)
    target_compile_options(benchforge PRIVATE /W4 /permissive-)
else()
    target_compile_options(benchforge PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()
