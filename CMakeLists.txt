cmake_minimum_required(VERSION 3.25)
project(tessera VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler Warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wno-unused-parameter
  )
endif()

# Sanitizers
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(ENABLE_SANITIZERS)
  message(STATUS "Enabling AddressSanitizer and UndefinedBehaviorSanitizer")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Threading & OpenSSL
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# Check for Protobuf and gRPC
find_package(Protobuf QUIET)
find_package(gRPC QUIET CONFIG)

# Include directories
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}
)

# Core Library Sources
set(CORE_SOURCES
  src/common/types.cpp
  src/common/logger.cpp
  src/common/correlation.cpp
  src/backend/mock_backend.cpp
  src/server/call_data.cpp
  src/server/gateway_server.cpp
  src/server/health_service.cpp
  src/cache/sha256.cpp
  src/cache/redis_client.cpp
  src/cache/redis_pool.cpp
  src/cache/exact_cache.cpp
  src/cache/cached_backend.cpp
  src/cache/embedding_engine.cpp
  src/cache/onnx_embedding.cpp
  src/cache/vector_store.cpp
  src/cache/semantic_cache.cpp
  src/cache/two_tier_cache.cpp
  src/ratelimit/token_bucket.cpp
  src/ratelimit/rate_limiter.cpp
  src/ratelimit/rate_limited_backend.cpp
)

add_library(tessera_core STATIC ${CORE_SOURCES})
target_link_libraries(tessera_core PUBLIC Threads::Threads OpenSSL::Crypto)

if(Protobuf_FOUND AND gRPC_FOUND)
  message(STATUS "Found system Protobuf and gRPC. Compiling protos/gateway.proto...")
  target_compile_definitions(tessera_core PUBLIC TESSERA_HAS_SYSTEM_GRPC=1)
  # Protobuf & gRPC code generation logic
  get_target_property(grpc_cpp_plugin_location gRPC::grpc_cpp_plugin LOCATION)
  
  set(PROTO_SRC "${CMAKE_CURRENT_BINARY_DIR}/gateway.pb.cc")
  set(PROTO_HDR "${CMAKE_CURRENT_BINARY_DIR}/gateway.pb.h")
  set(GRPC_SRC "${CMAKE_CURRENT_BINARY_DIR}/gateway.grpc.pb.cc")
  set(GRPC_HDR "${CMAKE_CURRENT_BINARY_DIR}/gateway.grpc.pb.h")
  
  add_custom_command(
    OUTPUT "${PROTO_SRC}" "${PROTO_HDR}" "${GRPC_SRC}" "${GRPC_HDR}"
    COMMAND protobuf::protoc
      --proto_path="${CMAKE_CURRENT_SOURCE_DIR}/protos"
      --cpp_out="${CMAKE_CURRENT_BINARY_DIR}"
      --grpc_out="${CMAKE_CURRENT_BINARY_DIR}"
      --plugin=protoc-gen-grpc="${grpc_cpp_plugin_location}"
      "${CMAKE_CURRENT_SOURCE_DIR}/protos/gateway.proto"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/protos/gateway.proto"
    COMMENT "Compiling gateway.proto"
  )
  target_sources(tessera_core PRIVATE ${PROTO_SRC} ${GRPC_SRC})
  target_link_libraries(tessera_core PUBLIC gRPC::grpc++ protobuf::libprotobuf)
else()
  message(STATUS "System gRPC/Protobuf not found. Using self-contained high-performance native RPC models.")
endif()

# Server Executable
add_executable(gateway_server src/main.cpp)
target_link_libraries(gateway_server PRIVATE tessera_core)

# Client CLI Tool
add_executable(gateway_client tools/gateway_client.cpp)
target_link_libraries(gateway_client PRIVATE tessera_core)

# Benchmark Tools
add_executable(cache_benchmark tools/cache_benchmark.cpp)
target_link_libraries(cache_benchmark PRIVATE tessera_core)

add_executable(semantic_benchmark tools/semantic_benchmark.cpp)
target_link_libraries(semantic_benchmark PRIVATE tessera_core)

# Testing
option(BUILD_TESTS "Build test suite" ON)
if(BUILD_TESTS)
  enable_testing()
  
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    message(STATUS "GTest not found in system. Using gtest_compat compatibility harness.")
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tests/gtest_compat)
  endif()

  # Unit Tests
  add_executable(unit_tests
    tests/test_main.cpp
    tests/unit/test_types.cpp
    tests/unit/test_mock_backend.cpp
    tests/unit/test_health_check.cpp
    tests/unit/test_validation.cpp
    tests/unit/test_sha256.cpp
    tests/unit/test_exact_cache.cpp
    tests/unit/test_rate_limiter.cpp
    tests/unit/test_redis_pool.cpp
    tests/unit/test_cache_bypass.cpp
    tests/unit/test_tokenizer.cpp
    tests/unit/test_embedding.cpp
    tests/unit/test_vector_store.cpp
    tests/unit/test_two_tier_cache.cpp
  )
  target_link_libraries(unit_tests PRIVATE tessera_core)
  if(GTest_FOUND)
    target_link_libraries(unit_tests PRIVATE GTest::gtest GTest::gtest_main)
  endif()
  add_test(NAME unit_tests COMMAND unit_tests)

  # Integration Tests
  add_executable(integration_tests
    tests/test_main.cpp
    tests/integration/test_server_async.cpp
    tests/integration/test_validation_integration.cpp
    tests/integration/test_graceful_shutdown.cpp
    tests/integration/test_redis_integration.cpp
    tests/integration/test_redis_resilience.cpp
    tests/integration/test_semantic_integration.cpp
    tests/integration/test_semantic_resilience.cpp
  )
  target_link_libraries(integration_tests PRIVATE tessera_core)
  if(GTest_FOUND)
    target_link_libraries(integration_tests PRIVATE GTest::gtest GTest::gtest_main)
  endif()
  add_test(NAME integration_tests COMMAND integration_tests)

  # Rate Limit Concurrency Test (200 threads)
  add_executable(ratelimit_concurrency_test
    tests/test_main.cpp
    tests/integration/test_ratelimit_concurrency.cpp
  )
  target_link_libraries(ratelimit_concurrency_test PRIVATE tessera_core)
  if(GTest_FOUND)
    target_link_libraries(ratelimit_concurrency_test PRIVATE GTest::gtest GTest::gtest_main)
  endif()
  add_test(NAME ratelimit_concurrency_test COMMAND ratelimit_concurrency_test)

  # Concurrency Load Test (500 clients)
  add_executable(concurrency_test
    tests/test_main.cpp
    tests/integration/test_concurrency.cpp
  )
  target_link_libraries(concurrency_test PRIVATE tessera_core)
  if(GTest_FOUND)
    target_link_libraries(concurrency_test PRIVATE GTest::gtest GTest::gtest_main)
  endif()
  add_test(NAME concurrency_test COMMAND concurrency_test)
endif()
