// SPDX-License-Identifier: MIT
//
// Unit tests for the cerberus-sdk C++ surface. Pure in-memory fixtures —
// no gateway process is required (see the repo README for e2e status).

#include "cerberus/sdk.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                         #cond);                                           \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

using Code = cerberus::GatewayErrorCode;

mcp::Tool fake_tool(const std::string&          name,
                    const nlohmann::json*       route = nullptr) {
    mcp::Tool t;
    t.name         = name;
    t.description  = "desc";
    t.input_schema = nlohmann::json{{"type", "object"},
                                    {"properties", nlohmann::json::object()}};
    if (route) {
        t.meta = nlohmann::json{{std::string(cerberus::kRouteMetaKey), *route}};
    }
    return t;
}

mcp::CallToolResult error_result(const std::string&                  text,
                                 const std::optional<nlohmann::json>& structured) {
    mcp::CallToolResult r;
    r.content.push_back(mcp::TextContent{.text = text});
    r.is_error = true;
    r.structured_content = structured;
    return r;
}

void test_route_metadata_is_authoritative() {
    const nlohmann::json route = {{"server", "remote-filesystem"},
                                  {"tool", "read_file"}};
    const auto routed = cerberus::routed_tool(fake_tool("fs__read_file", &route));
    CHECK(routed.server == "remote-filesystem");
    CHECK(routed.tool == "read_file");
    CHECK(routed.name == "fs__read_file");
    CHECK(cerberus::public_tool_name(routed.server, routed.tool) ==
          "remote-filesystem__read_file");
    CHECK(routed.description.has_value());
    CHECK(routed.input_schema.contains("type"));
}

void test_falls_back_to_first_separator_without_metadata() {
    const auto routed = cerberus::routed_tool(fake_tool("srv__do__it"));
    CHECK(routed.server == "srv");
    CHECK(routed.tool == "do__it");
}

void test_rejects_tools_with_neither_metadata_nor_separator() {
    bool threw = false;
    try {
        (void)cerberus::routed_tool(fake_tool("plain_tool"));
    } catch (const std::invalid_argument& e) {
        threw = true;
        CHECK(std::string(e.what()).find("no route metadata") !=
              std::string::npos);
    }
    CHECK(threw);
}

void test_error_envelope_parses_to_typed_code() {
    const nlohmann::json sc = {
        {"code", "policy_denied"},
        {"message", "policy denied: deny pattern matched"},
        {"retryable", false},
        {"exit_code", 6},
        {"server", "github"},
        {"tool", "search_repos"},
        {"reason", "deny pattern matched"},
    };
    const auto err = cerberus::gateway_error(
        error_result("policy denied: deny pattern matched", sc),
        "github", "search_repos");
    CHECK(err.code == Code::PolicyDenied);
    CHECK(err.retryable == false);
    CHECK(err.exit_code == 6);
    CHECK(err.server.has_value() && *err.server == "github");
    CHECK(err.tool.has_value() && *err.tool == "search_repos");
    CHECK(err.reason.has_value() && *err.reason == "deny pattern matched");
    CHECK(err.message.find("policy denied") != std::string::npos);
    CHECK(std::string(err.what()).find("policy_denied") == 0);
}

void test_error_without_structured_content_is_upstream_error() {
    const auto err =
        cerberus::gateway_error(error_result("boom", std::nullopt), "fs", "read");
    CHECK(err.code == Code::UpstreamError);
    CHECK(err.retryable == false);
    CHECK(err.exit_code == 1);
    CHECK(err.server.has_value() && *err.server == "fs");
    CHECK(err.tool.has_value() && *err.tool == "read");
    CHECK(err.message == "boom");
}

void test_unknown_codes_degrade_but_keep_message() {
    const nlohmann::json sc = {{"code", "brand_new_code"}, {"message", "hm"}};
    const auto err = cerberus::gateway_error(error_result("", sc), "s", "t");
    CHECK(err.code == Code::UpstreamError);
    CHECK(err.message == "hm");
}

void test_contract_table() {
    struct Case {
        Code    code;
        bool    retryable;
        int     exit;
    };
    const std::vector<Case> cases = {
        {Code::UnknownTool, false, 5},
        {Code::ServerNotFound, false, 5},
        {Code::PolicyDenied, false, 6},
        {Code::ConfirmationRequired, false, 6},
        {Code::Timeout, false, 7},
        {Code::ConcurrencySaturated, true, 7},
        {Code::UpstreamUnavailable, true, 1},
        {Code::ShuttingDown, true, 1},
        {Code::UpstreamError, false, 1},
    };
    for (const auto& c : cases) {
        const std::string wire(cerberus::to_string(c.code));
        CHECK(cerberus::error_code_from_string(wire) == c.code);
        CHECK(cerberus::is_retryable(c.code) == c.retryable);
        CHECK(cerberus::exit_code(c.code) == c.exit);
    }
    CHECK(cerberus::error_code_from_string("nope") == Code::UpstreamError);
}

void test_multi_block_text_extraction() {
    mcp::CallToolResult r;
    r.content.push_back(mcp::TextContent{.text = "one"});
    r.content.push_back(mcp::TextContent{.text = "two"});
    const auto out =
        cerberus::gateway_error(error_result("one\ntwo", std::nullopt), "s", "t");
    CHECK(out.message == "one\ntwo");
}

}  // namespace

int main() {
    test_route_metadata_is_authoritative();
    test_falls_back_to_first_separator_without_metadata();
    test_rejects_tools_with_neither_metadata_nor_separator();
    test_error_envelope_parses_to_typed_code();
    test_error_without_structured_content_is_upstream_error();
    test_unknown_codes_degrade_but_keep_message();
    test_contract_table();
    test_multi_block_text_extraction();

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all cerberus-sdk C++ checks passed\n");
    return 0;
}
