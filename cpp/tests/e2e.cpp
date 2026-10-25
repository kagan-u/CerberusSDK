// End-to-end: real `cerberus gateway start --transport stdio` + fake upstream
// + this SDK. Mirrors rs/tests/e2e.rs. Exits 77 to skip when fixtures are
// missing (ctest SKIP_RETURN_CODE 77); CERBERUS_REQUIRE_E2E=1 turns a missing
// fixture into a failure (CI does).
//
// Fixture overrides: CERBERUS_BIN, CERBERUS_FAKE_UPSTREAM_BIN.

#include <cerberus/sdk.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
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

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

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

constexpr const char* kHttpToken = "e2e-http-secret";

/// Redirect the child's stdout/stderr to /dev/null (used after fork).
void redirect_devnull() {
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
}

/// Poll the gateway state file until it names a concrete host:port.
std::string wait_for_http_endpoint(const fs::path& state_file) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream in(state_file);
        std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        // Tiny parse: the state file is {"pid":..,"bind":"host:port",..}.
        const std::string needle = "\"bind\": \"";
        const auto pos = text.find(needle);
        if (pos != std::string::npos) {
            const auto start = pos + needle.size();
            const auto end = text.find('"', start);
            if (end != std::string::npos) {
                std::string bind = text.substr(start, end - start);
                if (!bind.empty() && bind.size() > 2 &&
                    bind.substr(bind.size() - 2) != ":0") {
                    return bind;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error("gateway state file never reported a port: " +
                             state_file.string());
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

    // ---- streamable HTTP + bearer auth -------------------------------------
    // Separate temp dir so the stdio and http gateways never share a state
    // file; the registry is identical.
    if (rc == 0) {
        std::string httptmpl =
            (fs::temp_directory_path() / "cerberus-sdk-e2e-http-XXXXXX")
                .string();
        std::vector<char> hbuf(httptmpl.begin(), httptmpl.end());
        hbuf.push_back('\0');
        if (mkdtemp(hbuf.data()) == nullptr) {
            rc = fail("mkdtemp (http)");
        } else {
            const fs::path hconfig = hbuf.data();
            {
                std::ofstream out(hconfig / "cerberus.yaml");
                out << registry_yaml(fs::absolute(upstream));
            }

            const std::string bind = "127.0.0.1:0";
            std::vector<std::string> args = {
                fs::absolute(cerberus).string(), "gateway", "start",
                "--transport", "http", "--bind", bind, "--token",
                kHttpToken};
            std::vector<char*> argv;
            for (auto& a : args) argv.push_back(a.data());
            argv.push_back(nullptr);

            std::string config_str = hconfig.string();
            std::string data_str = (hconfig / "data").string();
            std::vector<std::string> env_storage;
            for (char** e = environ; *e != nullptr; ++e) {
                std::string entry(*e);
                // The stdio section setenv'd these process-wide; drop the
                // inherited copies so the overrides below take effect
                // (duplicate env entries resolve to the first occurrence).
                if (entry.rfind("CERBERUS_CONFIG_DIR=", 0) == 0 ||
                    entry.rfind("CERBERUS_DATA_DIR=", 0) == 0) {
                    continue;
                }
                env_storage.push_back(std::move(entry));
            }
            env_storage.push_back("CERBERUS_CONFIG_DIR=" + config_str);
            env_storage.push_back("CERBERUS_DATA_DIR=" + data_str);
            std::vector<char*> envp;
            for (auto& e : env_storage) envp.push_back(e.data());
            envp.push_back(nullptr);

            pid_t pid = fork();
            if (pid == -1) {
                rc = fail("fork (http gateway)");
            } else if (pid == 0) {
                redirect_devnull();
                execve(argv[0], argv.data(), envp.data());
                _exit(127);
            } else {
                try {
                    const std::string endpoint = wait_for_http_endpoint(
                        hconfig / "data" / "state" / "gateway.json");
                    const std::string url = "http://" + endpoint + "/mcp";

                    // Token *rejection* is covered by the rs/ts/py
                    // harnesses; mcp-cpp retries 401s through its OAuth
                    // dance (~30s per attempt), so only the positive
                    // contract runs here.
                    cerberus::Gateway gw =
                        cerberus::Gateway::connect_http(url, kHttpToken);
                    const auto htools = gw.list_tools();
                    const auto hecho =
                        std::find_if(htools.begin(), htools.end(),
                                     [](const auto& t) {
                                         return t.name ==
                                                "echo-server__echo";
                                     });
                    if (hecho == htools.end()) {
                        throw std::runtime_error(
                            "`echo-server__echo` not listed over http");
                    }
                    if (std::any_of(htools.begin(), htools.end(),
                                    [](const auto& t) {
                                        return t.name.rfind("locked__", 0) ==
                                               0;
                                    })) {
                        throw std::runtime_error(
                            "denied tool must not be listed over http");
                    }
                    const auto hout =
                        gw.call("echo-server", "echo", {{"text", "hi"}});
                    if (hout.text != "echo: hi") {
                        throw std::runtime_error("http roundtrip returned: " +
                                                 hout.text);
                    }
                    bool hdenied = false;
                    try {
                        gw.call("locked", "echo", {{"text", "hi"}});
                    } catch (const cerberus::GatewayError& e) {
                        hdenied = true;
                        if (e.code !=
                            cerberus::GatewayErrorCode::PolicyDenied) {
                            throw std::runtime_error(
                                "http policy_denied code mismatch");
                        }
                    }
                    if (!hdenied) {
                        throw std::runtime_error(
                            "expected GatewayError for denied http call");
                    }
                    gw.disconnect();
                } catch (const std::exception& e) {
                    rc = fail(e.what());
                }
                kill(pid, SIGTERM);
                waitpid(pid, nullptr, 0);
                if (rc == 0) fs::remove_all(hconfig);
            }
        }
    }

    if (rc == 0) {
        fs::remove_all(config);
        std::cout << "e2e ok\n";
    }
    return rc;
}

#endif
