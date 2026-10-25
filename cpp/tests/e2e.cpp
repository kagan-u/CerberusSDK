// End-to-end: real `cerberus gateway start --transport stdio` + fake upstream
// + this SDK. Mirrors rs/tests/e2e.rs. Exits 77 to skip when fixtures are
// missing (ctest SKIP_RETURN_CODE 77); CERBERUS_REQUIRE_E2E=1 turns a missing
// fixture into a failure (CI does).
//
// Fixture overrides: CERBERUS_BIN, CERBERUS_FAKE_UPSTREAM_BIN.

#include <cerberus/sdk.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
int main() {
    const char* require = std::getenv("CERBERUS_REQUIRE_E2E");
    if (require != nullptr && std::string(require) == "1") {
        std::cerr << "e2e FAILED: stdio e2e is POSIX-only (mcp-cpp limitation)\n";
        return 1;
    }
    std::cout << "e2e skipped: POSIX-only (mcp-cpp stdio)\n";
    return 77;
}
#else

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

int fail(const std::string& what) {
    std::cerr << "e2e FAILED: " << what << "\n";
    return 1;
}

bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && std::string(v) == "1";
}

fs::path resolve_env_or(const char* var, const fs::path& fallback) {
    if (const char* v = std::getenv(var); v != nullptr && *v != '\0') {
        return fs::path(v);
    }
    return fallback;
}

std::string registry_yaml(const fs::path& upstream) {
    const std::string up = upstream.string();
    return "schema_version: 1\n"
           "\n"
           "settings:\n"
           "  default_mode: direct\n"
           "\n"
           "servers:\n"
           "  echo-server:\n"
           "    description: e2e echo upstream\n"
           "    transport:\n"
           "      type: stdio\n"
           "      command: '" +
           up +
           "'\n"
           "      args: []\n"
           "      env:\n"
           "        CERBERUS_FAKE_UPSTREAM:\n"
           "          source: literal\n"
           "          value: \"1\"\n"
           "    timeout_ms: 15000\n"
           "    policy:\n"
           "      deny_tools: []\n"
           "\n"
           "  locked:\n"
           "    description: e2e upstream whose echo tool is denied\n"
           "    transport:\n"
           "      type: stdio\n"
           "      command: '" +
           up +
           "'\n"
           "      args: []\n"
           "    timeout_ms: 15000\n"
           "    policy:\n"
           "      deny_tools: [\"echo\"]\n";
}

}  // namespace

int main() {
    // CERBERUS_E2E_ROOT is the cpp/ directory; the repo root is its parent.
    const fs::path repo_root = fs::path(CERBERUS_E2E_ROOT).parent_path();
    const fs::path cerberus = resolve_env_or(
        "CERBERUS_BIN",
        repo_root / ".." / "CerberusMCP" / "target" / "debug" / "cerberus");
    const fs::path upstream = resolve_env_or(
        "CERBERUS_FAKE_UPSTREAM_BIN",
        repo_root / "rs" / "target" / "debug" / "examples" / "fake_upstream");

    if (!fs::exists(cerberus) || !fs::exists(upstream)) {
        const std::string detail =
            cerberus.string() + ", " + upstream.string();
        if (env_flag("CERBERUS_REQUIRE_E2E")) {
            return fail("fixtures missing: " + detail);
        }
        std::cout << "e2e skipped: fixtures missing (" << detail << ")\n";
        return 77;
    }

    std::string tmpl =
        (fs::temp_directory_path() / "cerberus-sdk-e2e-XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (mkdtemp(buf.data()) == nullptr) {
        return fail("mkdtemp");
    }
    const fs::path config = buf.data();

    {
        std::ofstream out(config / "cerberus.yaml");
        if (!out) {
            return fail("cannot write cerberus.yaml");
        }
        out << registry_yaml(fs::absolute(upstream));
    }
    setenv("CERBERUS_CONFIG_DIR", config.c_str(), 1);
    setenv("CERBERUS_DATA_DIR", (config / "data").string().c_str(), 1);

    int rc = 0;
    try {
        cerberus::Gateway gw = cerberus::Gateway::connect_stdio(
            cerberus::Gateway::StdioOptions{fs::absolute(cerberus).string()});

        const auto tools = gw.list_tools();
        const auto echo =
            std::find_if(tools.begin(), tools.end(), [](const auto& t) {
                return t.name == "echo-server__echo";
            });
        if (echo == tools.end()) {
            throw std::runtime_error("`echo-server__echo` not listed");
        }
        if (echo->server != "echo-server" || echo->tool != "echo") {
            throw std::runtime_error("route resolution mismatch: " +
                                     echo->server + "::" + echo->tool);
        }
        if (std::any_of(tools.begin(), tools.end(), [](const auto& t) {
                return t.name.rfind("locked__", 0) == 0;
            })) {
            throw std::runtime_error("denied tool must not be listed");
        }

        const auto out = gw.call("echo-server", "echo", {{"text", "hi"}});
        if (out.text != "echo: hi") {
            throw std::runtime_error("roundtrip returned: " + out.text);
        }

        bool denied = false;
        try {
            gw.call("locked", "echo", {{"text", "hi"}});
        } catch (const cerberus::GatewayError& e) {
            denied = true;
            if (e.code != cerberus::GatewayErrorCode::PolicyDenied) {
                throw std::runtime_error("code was " +
                                         std::string(cerberus::to_string(e.code)));
            }
            if (e.retryable) {
                throw std::runtime_error("retryable must be false");
            }
            if (e.exit_code != 6) {
                throw std::runtime_error("exit_code must be 6, got " +
                                         std::to_string(e.exit_code));
            }
            if (!e.server || *e.server != "locked") {
                throw std::runtime_error("server must be `locked`");
            }
            if (!e.tool || *e.tool != "echo") {
                throw std::runtime_error("tool must be `echo`");
            }
        }
        if (!denied) {
            throw std::runtime_error("expected GatewayError for denied call");
        }

        gw.disconnect();
    } catch (const std::exception& e) {
        rc = fail(e.what());
    }

    if (rc == 0) {
        fs::remove_all(config);
        std::cout << "e2e ok\n";
    }
    return rc;
}

#endif
