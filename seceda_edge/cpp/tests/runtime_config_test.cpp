#include "runtime/runtime_config.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace seceda::edge;

bool require(bool condition, const char * message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        return false;
    }
    return true;
}

std::vector<char *> to_argv(std::vector<std::string> & args) {
    std::vector<char *> argv;
    argv.reserve(args.size());
    for (auto & arg : args) {
        argv.push_back(arg.data());
    }
    return argv;
}

}  // namespace

int main() {
    {
        DaemonConfig config;
        std::string error;
        bool show_help = false;
        std::vector<std::string> args = {
            "seceda_edge_daemon",
            "--cloud-connect-timeout-seconds",
            "15",
            "--cloud-retry-attempts",
            "4",
            "--cloud-retry-backoff-ms",
            "250",
            "--cloud-send-modal-session-id",
            "true",
        };
        auto argv = to_argv(args);

        if (!require(
                RuntimeConfigParser::parse(
                    static_cast<int>(argv.size()),
                    argv.data(),
                    config,
                    error,
                    show_help),
                "parser should accept Modal cloud flags")) {
            return 1;
        }
        if (!require(!show_help, "help flag should remain false")) {
            return 1;
        }
        if (!require(config.cloud.connect_timeout_seconds == 15, "connect timeout should parse")) {
            return 1;
        }
        if (!require(config.cloud.retry_attempts == 4, "retry attempts should parse")) {
            return 1;
        }
        if (!require(config.cloud.retry_backoff_ms == 250, "retry backoff should parse")) {
            return 1;
        }
        if (!require(config.cloud.send_modal_session_id, "Modal session flag should parse")) {
            return 1;
        }
    }

    {
        const std::string help = RuntimeConfigParser::help_text("seceda_edge_daemon");
        if (!require(
                help.find("--cloud-connect-timeout-seconds") != std::string::npos,
                "help text should include connect timeout")) {
            return 1;
        }
        if (!require(
                help.find("--cloud-retry-attempts") != std::string::npos,
                "help text should include retry attempts")) {
            return 1;
        }
        if (!require(
                help.find("--cloud-retry-backoff-ms") != std::string::npos,
                "help text should include retry backoff")) {
            return 1;
        }
        if (!require(
                help.find("--cloud-send-modal-session-id") != std::string::npos,
                "help text should include Modal session flag")) {
            return 1;
        }
    }

    {
        DaemonConfig config;
        std::string error;
        bool show_help = false;
        std::vector<std::string> args = {
            "seceda_edge_daemon",
            "--cloud-send-modal-session-id",
            "maybe",
        };
        auto argv = to_argv(args);

        if (!require(
                !RuntimeConfigParser::parse(
                    static_cast<int>(argv.size()),
                    argv.data(),
                    config,
                    error,
                    show_help),
                "parser should reject invalid Modal session flag values")) {
            return 1;
        }
        if (!require(
                error.find("--cloud-send-modal-session-id") != std::string::npos,
                "parse error should mention Modal session flag")) {
            return 1;
        }
    }

    return 0;
}
