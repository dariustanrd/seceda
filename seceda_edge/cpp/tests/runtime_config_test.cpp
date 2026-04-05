#include "runtime/runtime_config.hpp"

#include <filesystem>
#include <fstream>
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
            "--no-config",
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
        if (!require(help.find("--config") != std::string::npos, "help text should include config flag")) {
            return 1;
        }
        if (!require(
                help.find("--local-sidecar-base-url") != std::string::npos,
                "help text should include sidecar base url")) {
            return 1;
        }
        if (!require(
                help.find("--remote-backend") != std::string::npos,
                "help text should include named backend catalog flag")) {
            return 1;
        }
        if (!require(
                help.find("--exposed-model") != std::string::npos,
                "help text should include exposed model flag")) {
            return 1;
        }
    }

    {
        DaemonConfig config;
        std::string error;
        bool show_help = false;
        std::vector<std::string> args = {
            "seceda_edge_daemon",
            "--no-config",
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

    {
        DaemonConfig config;
        std::string error;
        bool show_help = false;
        std::vector<std::string> args = {
            "seceda_edge_daemon",
            "--no-config",
            "--public-model-alias",
            "seceda/public",
            "--local-execution-mode",
            "sidecar_server",
            "--local-sidecar-base-url",
            "http://127.0.0.1:8081",
            "--cloud-backend-id",
            "remote/default",
            "--cloud-model-alias",
            "remote/default",
            "--remote-backend",
            "backend_id=remote/alt,model_alias=remote/alt,model=remote-alt,base_url=http://127.0.0.1:9090",
            "--exposed-model",
            "id=remote/alt,display_name=Remote Alt,route=cloud,backend_id=remote/alt",
        };
        auto argv = to_argv(args);

        if (!require(
                RuntimeConfigParser::parse(
                    static_cast<int>(argv.size()),
                    argv.data(),
                    config,
                    error,
                    show_help),
                "parser should accept sidecar and catalog flags")) {
            return 1;
        }
        if (!require(
                config.local.sidecar_base_url == "http://127.0.0.1:8081",
                "sidecar base url should parse")) {
            return 1;
        }
        if (!require(config.remote_backends.size() == 1, "named remote backend should parse")) {
            return 1;
        }
        if (!require(
                config.remote_backends[0].backend_id == "remote/alt",
                "named remote backend id should parse")) {
            return 1;
        }

        bool saw_public_alias = false;
        bool saw_remote_alias = false;
        for (const auto & model : config.exposed_models) {
            if (model.id == "seceda/public") {
                saw_public_alias = true;
            }
            if (model.id == "remote/alt" && model.backend_id == "remote/alt") {
                saw_remote_alias = true;
            }
        }
        if (!require(saw_public_alias, "public alias should be preserved in exposed model catalog")) {
            return 1;
        }
        if (!require(saw_remote_alias, "named exposed model should parse")) {
            return 1;
        }
    }

    {
        DaemonConfig config;
        std::string error;
        bool show_help = false;
        std::vector<std::string> args = {
            "seceda_edge_daemon",
            "--no-config",
            "--local-execution-mode",
            "sidecar_server",
        };
        auto argv = to_argv(args);

        if (!require(
                !RuntimeConfigParser::parse(
                    static_cast<int>(argv.size()),
                    argv.data(),
                    config,
                    error,
                    show_help),
                "parser should reject sidecar mode without a base url")) {
            return 1;
        }
        if (!require(
                error.find("sidecar") != std::string::npos,
                "sidecar validation error should mention sidecar configuration")) {
            return 1;
        }
    }

    {
        const std::filesystem::path temp_path =
            std::filesystem::temp_directory_path() / "seceda_runtime_config_test.toml";
        {
            std::ofstream out(temp_path);
            out
                << "schema_version = 1\n"
                << "\n"
                << "[daemon]\n"
                << "host = \"127.0.0.1\"\n"
                << "port = 7777\n"
                << "public_model_alias = \"seceda/file\"\n"
                << "\n"
                << "[generation]\n"
                << "max_completion_tokens = 96\n"
                << "\n"
                << "[local]\n"
                << "active_engine = \"sidecar-dev\"\n"
                << "\n"
                << "[local.engines.sidecar-dev]\n"
                << "engine_id = \"local/sidecar\"\n"
                << "backend_id = \"local/sidecar\"\n"
                << "model_id = \"sidecar-model\"\n"
                << "model_alias = \"local/sidecar\"\n"
                << "display_name = \"Sidecar Dev\"\n"
                << "execution_mode = \"sidecar_server\"\n"
                << "capabilities = [\"chat.completions\", \"text\", \"stream\", \"tools\", \"response_format\"]\n"
                << "sidecar_base_url = \"http://127.0.0.1:8081\"\n"
                << "sidecar_timeout_seconds = 33\n"
                << "sidecar_connect_timeout_seconds = 11\n"
                << "\n"
                << "[remote]\n"
                << "default_backend = \"modal-default\"\n"
                << "\n"
                << "[remote.backends.modal-default]\n"
                << "backend_id = \"remote/modal-default\"\n"
                << "model = \"remote-model\"\n"
                << "model_alias = \"remote/default\"\n"
                << "display_name = \"Remote Default\"\n"
                << "execution_mode = \"remote_service\"\n"
                << "capabilities = [\"chat.completions\", \"text\", \"stream\", \"tools\", \"response_format\"]\n"
                << "base_url = \"https://example.test/v1\"\n"
                << "\n"
                << "[router]\n"
                << "max_prompt_chars = 1234\n"
                << "\n"
                << "[[exposed_models]]\n"
                << "id = \"remote/default\"\n"
                << "display_name = \"Remote Default\"\n"
                << "route_target = \"cloud\"\n"
                << "backend_id = \"remote/modal-default\"\n";
        }

        DaemonConfig config;
        std::string error;
        bool show_help = false;
        std::vector<std::string> args = {
            "seceda_edge_daemon",
            "--config",
            temp_path.string(),
            "--port",
            "9090",
        };
        auto argv = to_argv(args);

        if (!require(
                RuntimeConfigParser::parse(
                    static_cast<int>(argv.size()),
                    argv.data(),
                    config,
                    error,
                    show_help),
                "parser should load TOML config file and apply CLI overrides")) {
            std::filesystem::remove(temp_path);
            return 1;
        }
        if (!require(config.host == "127.0.0.1", "host should load from TOML config")) {
            std::filesystem::remove(temp_path);
            return 1;
        }
        if (!require(config.port == 9090, "CLI should override config file values")) {
            std::filesystem::remove(temp_path);
            return 1;
        }
        if (!require(
                config.public_model_alias == "seceda/file",
                "public model alias should load from TOML config")) {
            std::filesystem::remove(temp_path);
            return 1;
        }
        if (!require(
                config.local.execution_mode == "sidecar_server",
                "active local engine should load from TOML config")) {
            std::filesystem::remove(temp_path);
            return 1;
        }
        if (!require(
                config.local.sidecar_base_url == "http://127.0.0.1:8081",
                "sidecar base url should load from TOML config")) {
            std::filesystem::remove(temp_path);
            return 1;
        }
        if (!require(
                config.cloud.base_url == "https://example.test/v1",
                "default remote backend should load from TOML config")) {
            std::filesystem::remove(temp_path);
            return 1;
        }
        if (!require(
                config.router.max_prompt_chars == 1234,
                "router settings should load from TOML config")) {
            std::filesystem::remove(temp_path);
            return 1;
        }

        std::filesystem::remove(temp_path);
    }

    return 0;
}
