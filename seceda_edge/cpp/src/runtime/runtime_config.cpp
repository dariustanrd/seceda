#include "runtime/runtime_config.hpp"

#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace seceda::edge {
namespace {

std::string trim(const std::string & value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }

    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::vector<std::string> split_csv(const std::string & value) {
    std::vector<std::string> out;
    std::stringstream stream(value);
    std::string item;

    while (std::getline(stream, item, ',')) {
        item = trim(item);
        if (!item.empty()) {
            out.push_back(item);
        }
    }

    return out;
}

bool parse_bool(const std::string & value, bool & out) {
    if (value == "1" || value == "true" || value == "TRUE" || value == "on") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "FALSE" || value == "off") {
        out = false;
        return true;
    }

    return false;
}

bool next_value(
    int argc,
    char ** argv,
    int & index,
    const std::string & current,
    std::string & value,
    std::string & error) {
    const auto equal_pos = current.find('=');
    if (equal_pos != std::string::npos) {
        value = current.substr(equal_pos + 1);
        return true;
    }

    if (index + 1 >= argc) {
        error = "Missing value for argument: " + current;
        return false;
    }

    value = argv[++index];
    return true;
}

template <typename T>
bool parse_number(const std::string & raw, T & out, std::string & error) {
    try {
        std::size_t consumed = 0;
        if constexpr (std::is_same<T, int>::value) {
            out = std::stoi(raw, &consumed);
        } else if constexpr (std::is_same<T, std::size_t>::value) {
            out = static_cast<std::size_t>(std::stoull(raw, &consumed));
        } else if constexpr (std::is_same<T, std::uint32_t>::value) {
            out = static_cast<std::uint32_t>(std::stoul(raw, &consumed));
        } else if constexpr (std::is_same<T, float>::value) {
            out = std::stof(raw, &consumed);
        } else {
            static_assert(sizeof(T) == 0, "Unsupported numeric parse type");
        }

        if (consumed != raw.size()) {
            error = "Invalid numeric value: " + raw;
            return false;
        }
        return true;
    } catch (const std::exception &) {
        error = "Invalid numeric value: " + raw;
        return false;
    }
}

}  // namespace

bool RuntimeConfigParser::parse(
    int argc,
    char ** argv,
    DaemonConfig & config,
    std::string & error,
    bool & show_help) {
    error.clear();
    show_help = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            show_help = true;
            return true;
        }

        if (arg.rfind("--", 0) != 0) {
            error = "Unexpected positional argument: " + arg;
            return false;
        }

        std::string value;
        if (arg.rfind("--host", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.host = value;
        } else if (arg.rfind("--port", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.port, error)) {
                return false;
            }
        } else if (arg.rfind("--model-path", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.local.model_path = value;
        } else if (arg.rfind("--system-prompt", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.default_system_prompt = value;
        } else if (arg.rfind("--warmup-prompt", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.warmup_prompt = value;
        } else if (arg.rfind("--n-ctx", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.local.context_size, error)) {
                return false;
            }
        } else if (arg.rfind("--n-batch", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.local.batch_size, error)) {
                return false;
            }
        } else if (arg.rfind("--n-gpu-layers", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.local.n_gpu_layers, error)) {
                return false;
            }
        } else if (arg.rfind("--n-threads-batch", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.local.n_threads_batch, error)) {
                return false;
            }
        } else if (arg.rfind("--n-threads", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.local.n_threads, error)) {
                return false;
            }
        } else if (arg.rfind("--default-max-tokens", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.default_generation.max_tokens, error)) {
                return false;
            }
        } else if (arg.rfind("--default-temperature", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.default_generation.temperature, error)) {
                return false;
            }
        } else if (arg.rfind("--default-top-p", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.default_generation.top_p, error)) {
                return false;
            }
        } else if (arg.rfind("--default-top-k", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.default_generation.top_k, error)) {
                return false;
            }
        } else if (arg.rfind("--default-min-p", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.default_generation.min_p, error)) {
                return false;
            }
        } else if (arg.rfind("--default-seed", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.default_generation.seed, error)) {
                return false;
            }
        } else if (arg.rfind("--cloud-base-url", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.cloud.base_url = value;
        } else if (arg.rfind("--cloud-model", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.cloud.model = value;
        } else if (arg.rfind("--cloud-api-key", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.cloud.api_key = value;
        } else if (arg.rfind("--cloud-timeout-seconds", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.cloud.timeout_seconds, error)) {
                return false;
            }
        } else if (arg.rfind("--cloud-connect-timeout-seconds", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.cloud.connect_timeout_seconds, error)) {
                return false;
            }
        } else if (arg.rfind("--cloud-retry-attempts", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.cloud.retry_attempts, error)) {
                return false;
            }
        } else if (arg.rfind("--cloud-retry-backoff-ms", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.cloud.retry_backoff_ms, error)) {
                return false;
            }
        } else if (arg.rfind("--cloud-send-modal-session-id", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_bool(value, config.cloud.send_modal_session_id)) {
                error = "Invalid boolean value for --cloud-send-modal-session-id: " + value;
                return false;
            }
        } else if (arg.rfind("--cloud-verify-tls", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_bool(value, config.cloud.verify_tls)) {
                error = "Invalid boolean value for --cloud-verify-tls: " + value;
                return false;
            }
        } else if (arg.rfind("--router-max-chars", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.router.max_prompt_chars, error)) {
                return false;
            }
        } else if (arg.rfind("--router-max-estimated-tokens", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.router.max_estimated_tokens, error)) {
                return false;
            }
        } else if (arg.rfind("--router-structured-keywords", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.router.structured_keywords = split_csv(value);
        } else if (arg.rfind("--router-cloud-keywords", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.router.cloud_keywords = split_csv(value);
        } else if (arg.rfind("--router-freshness-keywords", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.router.freshness_keywords = split_csv(value);
        } else if (arg.rfind("--event-log-capacity", 0) == 0) {
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.event_log_capacity, error)) {
                return false;
            }
        } else {
            error = "Unknown argument: " + arg;
            return false;
        }
    }

    if (config.port <= 0 || config.port > 65535) {
        error = "Port must be between 1 and 65535";
        return false;
    }
    if (config.local.context_size <= 0) {
        error = "Context size must be positive";
        return false;
    }
    if (config.local.batch_size <= 0) {
        error = "Batch size must be positive";
        return false;
    }
    if (config.default_generation.max_tokens <= 0) {
        error = "Default max tokens must be positive";
        return false;
    }
    if (config.router.max_estimated_tokens <= 0) {
        error = "Router max estimated tokens must be positive";
        return false;
    }
    if (config.cloud.timeout_seconds <= 0) {
        error = "Cloud timeout seconds must be positive";
        return false;
    }
    if (config.cloud.connect_timeout_seconds <= 0) {
        error = "Cloud connect timeout seconds must be positive";
        return false;
    }
    if (config.cloud.retry_attempts < 0) {
        error = "Cloud retry attempts must be zero or greater";
        return false;
    }
    if (config.cloud.retry_backoff_ms < 0) {
        error = "Cloud retry backoff must be zero or greater";
        return false;
    }

    if (config.cloud.api_key.empty()) {
        if (const char * env_api_key = std::getenv("SECEDA_CLOUD_API_KEY")) {
            config.cloud.api_key = env_api_key;
        }
    }

    return true;
}

std::string RuntimeConfigParser::help_text(const char * program_name) {
    std::ostringstream out;
    out
        << "Usage: " << program_name << " [options]\n\n"
        << "Core options:\n"
        << "  --host HOST\n"
        << "  --port PORT\n"
        << "  --model-path PATH\n"
        << "  --system-prompt TEXT\n"
        << "  --warmup-prompt TEXT\n\n"
        << "Local llama.cpp options:\n"
        << "  --n-ctx N\n"
        << "  --n-batch N\n"
        << "  --n-gpu-layers N\n"
        << "  --n-threads N\n"
        << "  --n-threads-batch N\n\n"
        << "Generation defaults:\n"
        << "  --default-max-tokens N\n"
        << "  --default-temperature FLOAT\n"
        << "  --default-top-p FLOAT\n"
        << "  --default-top-k N\n"
        << "  --default-min-p FLOAT\n"
        << "  --default-seed N\n\n"
        << "Router options:\n"
        << "  --router-max-chars N\n"
        << "  --router-max-estimated-tokens N\n"
        << "  --router-structured-keywords a,b,c\n"
        << "  --router-cloud-keywords a,b,c\n"
        << "  --router-freshness-keywords a,b,c\n\n"
        << "Cloud fallback options:\n"
        << "  --cloud-base-url URL\n"
        << "  --cloud-model NAME\n"
        << "  --cloud-api-key KEY\n"
        << "  --cloud-timeout-seconds N\n"
        << "  --cloud-connect-timeout-seconds N\n"
        << "  --cloud-retry-attempts N\n"
        << "  --cloud-retry-backoff-ms N\n"
        << "  --cloud-send-modal-session-id true|false\n"
        << "  --cloud-verify-tls true|false\n\n"
        << "Observability:\n"
        << "  --event-log-capacity N\n";
    return out.str();
}

}  // namespace seceda::edge
