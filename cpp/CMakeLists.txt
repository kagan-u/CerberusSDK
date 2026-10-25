cmake_minimum_required(VERSION 3.20)
project(cerberus-sdk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CERBERUS_TOP_LEVEL ON)
else()
    set(CERBERUS_TOP_LEVEL OFF)
endif()

option(CERBERUS_BUILD_TESTS "Build the cerberus-sdk test suite" ${CERBERUS_TOP_LEVEL})
option(CERBERUS_ENABLE_HTTP "Enable the streamable-HTTP transport (cpp-httplib, plaintext)" ON)

include(FetchContent)
set(MCP_BUILD_TESTS OFF CACHE INTERNAL "")
set(MCP_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(MCP_ENABLE_HTTP ${CERBERUS_ENABLE_HTTP} CACHE INTERNAL "")
if(CERBERUS_ENABLE_HTTP)
    # Gateway binds are plaintext loopback by default; drop OpenSSL entirely.
    set(MCP_HTTP_NO_TLS ON CACHE INTERNAL "")
endif()
FetchContent_Declare(mcp
    GIT_REPOSITORY https://github.com/Neumann-Labs/mcp-cpp.git
    GIT_TAG        v0.1.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(mcp)

add_library(cerberus_sdk src/sdk.cpp)
add_library(cerberus::sdk ALIAS cerberus_sdk)
target_include_directories(cerberus_sdk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(cerberus_sdk PUBLIC mcp::mcp)
if(CERBERUS_ENABLE_HTTP)
    target_compile_definitions(cerberus_sdk PUBLIC CERBERUS_HAS_HTTP=1)
endif()

if(CERBERUS_BUILD_TESTS)
    enable_testing()
    add_executable(cerberus_sdk_tests tests/test_sdk.cpp)
    target_link_libraries(cerberus_sdk_tests PRIVATE cerberus::sdk)
    add_test(NAME cerberus_sdk_unit COMMAND cerberus_sdk_tests)

    add_executable(cerberus_e2e tests/e2e.cpp)
    target_link_libraries(cerberus_e2e PRIVATE cerberus::sdk)
    target_compile_definitions(cerberus_e2e PRIVATE
        CERBERUS_E2E_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME cerberus_e2e COMMAND cerberus_e2e)
    set_tests_properties(cerberus_e2e PROPERTIES SKIP_RETURN_CODE 77)
endif()
