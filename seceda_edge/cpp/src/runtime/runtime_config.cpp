#include "runtime/runtime_config.hpp"

#include "runtime/dotenv_load.hpp"
#include "config_catalog/config_catalog.hpp"
#include "config_catalog/simple_toml_read.hpp"
#include "config_catalog/simple_toml.hpp"
#include "file_utils/read_text_file.hpp"
#include "local_models/local_execution_modes.hpp"
#include "text_utils/normalize.hpp"

#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

namespace seceda::edge {
namespace {

using simple_toml::Value;
namespace fs_utils = seceda::edge::file_utils;
namespace text = seceda::edge::text_utils;
namespace toml_read = seceda::edge::simple_toml_read;

struct ConfigFileSelection {
    std::string path;
    bool explicit_path = false;
    bool disable = false;
};

std::vector<std::string> split_on(const std::string & value, char delimiter) {
    std::vector<std::string> out;
    std::stringstream stream(value);
    std::string item;

    while (std::getline(stream, item, delimiter)) {
        item = text::trim_copy(item);
        if (!item.empty()) {
            out.push_back(item);
        }
    }

    return out;
}

std::vector<std::string> split_csv(const std::string & value) {
    return split_on(value, ',');
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

bool parse_bool_flag(
    const std::string & raw,
    const std::string & flag,
    bool & out,
    std::string & error) {
    if (parse_bool(raw, out)) {
        return true;
    }

    error = "Invalid boolean value for " + flag + ": " + raw;
    return false;
}

bool load_toml_file(const std::string & path, Value & root, std::string & error) {
    std::string content;
    if (!fs_utils::read_text_file(path, content, error)) {
        return false;
    }

    if (!simple_toml::parse_document(content, root, error)) {
        error = "Failed to parse TOML file '" + path + "': " + error;
        return false;
    }

    return true;
}

bool validate_allowed_keys(
    const Value & table,
    const std::set<std::string> & allowed_keys,
    const std::string & context,
    std::string & error) {
    for (const auto & key : simple_toml::table_keys(table)) {
        if (allowed_keys.count(key) == 0) {
            error = context + " contains unknown key: " + key;
            return false;
        }
    }
    return true;
}

bool parse_key_value_spec(
    const std::string & raw,
    std::vector<std::pair<std::string, std::string>> & items,
    std::string & error) {
    items.clear();
    for (const auto & token : split_csv(raw)) {
        const auto equal_pos = token.find('=');
        if (equal_pos == std::string::npos) {
            error = "Invalid catalog spec token '" + token + "'; expected key=value";
            return false;
        }

        const std::string key = text::trim_copy(token.substr(0, equal_pos));
        const std::string value = text::trim_copy(token.substr(equal_pos + 1));
        if (key.empty() || value.empty()) {
            error = "Invalid catalog spec token '" + token + "'";
            return false;
        }
        items.emplace_back(key, value);
    }

    if (items.empty()) {
        error = "Catalog spec must not be empty";
        return false;
    }

    return true;
}

bool parse_local_engine_spec(
    const std::string & raw,
    std::vector<LocalModelConfig> & out,
    std::string & error) {
    std::vector<std::pair<std::string, std::string>> items;
    if (!parse_key_value_spec(raw, items, error)) {
        return false;
    }

    LocalModelConfig entry;
    for (const auto & [key, value] : items) {
        if (key == "engine_id") {
            entry.engine_id = value;
        } else if (key == "backend_id") {
            entry.backend_id = value;
        } else if (key == "model_id") {
            entry.model_id = value;
        } else if (key == "model_alias") {
            entry.model_alias = value;
        } else if (key == "display_name") {
            entry.display_name = value;
        } else if (key == "execution_mode") {
            entry.execution_mode = value;
        } else if (key == "model_path") {
            entry.model_path = value;
        } else if (key == "sidecar_base_url") {
            entry.sidecar_base_url = value;
        } else if (key == "sidecar_timeout_seconds") {
            if (!parse_number(value, entry.sidecar_timeout_seconds, error)) {
                return false;
            }
        } else if (key == "sidecar_connect_timeout_seconds") {
            if (!parse_number(value, entry.sidecar_connect_timeout_seconds, error)) {
                return false;
            }
        } else if (key == "sidecar_verify_tls") {
            if (!parse_bool_flag(value, "--local-engine", entry.sidecar_verify_tls, error)) {
                return false;
            }
        } else if (key == "capabilities") {
            entry.capabilities = split_on(value, '|');
        } else if (key == "context_size") {
            if (!parse_number(value, entry.context_size, error)) {
                return false;
            }
        } else if (key == "batch_size") {
            if (!parse_number(value, entry.batch_size, error)) {
                return false;
            }
        } else {
            error = "Unknown local engine spec key: " + key;
            return false;
        }
    }

    if (entry.display_name.empty()) {
        entry.display_name = entry.model_alias.empty() ? entry.engine_id : entry.model_alias;
    }
    if (is_sidecar_execution_mode(entry.execution_mode) && entry.sidecar_base_url.empty()) {
        error = "Named sidecar engines require sidecar_base_url";
        return false;
    }

    out.push_back(std::move(entry));
    return true;
}

bool parse_remote_backend_spec(
    const std::string & raw,
    std::vector<CloudConfig> & out,
    std::string & error) {
    std::vector<std::pair<std::string, std::string>> items;
    if (!parse_key_value_spec(raw, items, error)) {
        return false;
    }

    CloudConfig entry;
    for (const auto & [key, value] : items) {
        if (key == "backend_id") {
            entry.backend_id = value;
        } else if (key == "model") {
            entry.model = value;
        } else if (key == "model_alias") {
            entry.model_alias = value;
        } else if (key == "display_name") {
            entry.display_name = value;
        } else if (key == "execution_mode") {
            entry.execution_mode = value;
        } else if (key == "base_url") {
            entry.base_url = value;
        } else if (key == "api_key") {
            entry.api_key = value;
        } else if (key == "timeout_seconds") {
            if (!parse_number(value, entry.timeout_seconds, error)) {
                return false;
            }
        } else if (key == "connect_timeout_seconds") {
            if (!parse_number(value, entry.connect_timeout_seconds, error)) {
                return false;
            }
        } else if (key == "retry_attempts") {
            if (!parse_number(value, entry.retry_attempts, error)) {
                return false;
            }
        } else if (key == "retry_backoff_ms") {
            if (!parse_number(value, entry.retry_backoff_ms, error)) {
                return false;
            }
        } else if (key == "send_modal_session_id") {
            if (!parse_bool_flag(value, "--remote-backend", entry.send_modal_session_id, error)) {
                return false;
            }
        } else if (key == "verify_tls") {
            if (!parse_bool_flag(value, "--remote-backend", entry.verify_tls, error)) {
                return false;
            }
        } else if (key == "capabilities") {
            entry.capabilities = split_on(value, '|');
        } else {
            error = "Unknown remote backend spec key: " + key;
            return false;
        }
    }

    if (entry.base_url.empty()) {
        error = "Named remote backends require base_url";
        return false;
    }
    if (entry.display_name.empty()) {
        entry.display_name = entry.model_alias.empty() ? entry.backend_id : entry.model_alias;
    }

    out.push_back(std::move(entry));
    return true;
}

bool parse_exposed_model_spec(
    const std::string & raw,
    std::vector<ModelCatalogEntry> & out,
    std::string & error) {
    std::vector<std::pair<std::string, std::string>> items;
    if (!parse_key_value_spec(raw, items, error)) {
        return false;
    }

    ModelCatalogEntry entry;
    for (const auto & [key, value] : items) {
        if (key == "id") {
            entry.id = value;
        } else if (key == "display_name") {
            entry.display_name = value;
        } else if (key == "owned_by") {
            entry.owned_by = value;
        } else if (key == "route") {
            if (!parse_route_target(value, entry.route_target)) {
                error = "Invalid route value in exposed model spec: " + value;
                return false;
            }
        } else if (key == "engine_id") {
            entry.engine_id = value;
        } else if (key == "backend_id") {
            entry.backend_id = value;
        } else if (key == "model_id") {
            entry.model_id = value;
        } else if (key == "model_alias") {
            entry.model_alias = value;
        } else if (key == "execution_mode") {
            entry.execution_mode = value;
        } else if (key == "capabilities") {
            entry.capabilities = split_on(value, '|');
        } else {
            error = "Unknown exposed model spec key: " + key;
            return false;
        }
    }

    if (entry.id.empty()) {
        error = "Exposed model specs require id";
        return false;
    }
    if (entry.display_name.empty()) {
        entry.display_name = entry.id;
    }
    if (entry.model_alias.empty()) {
        entry.model_alias = entry.id;
    }

    out.push_back(std::move(entry));
    return true;
}

bool parse_local_engine_table(
    const std::string & engine_name,
    const Value & table,
    LocalModelConfig & out,
    std::string & error) {
    if (!validate_allowed_keys(
            table,
            {
                "engine_id",
                "backend_id",
                "model_id",
                "model_alias",
                "display_name",
                "execution_mode",
                "capabilities",
                "model_path",
                "context_size",
                "batch_size",
                "n_gpu_layers",
                "n_threads",
                "n_threads_batch",
                "sidecar_base_url",
                "sidecar_timeout_seconds",
                "sidecar_connect_timeout_seconds",
                "sidecar_verify_tls",
            },
            "local engine '" + engine_name + "'",
            error)) {
        return false;
    }

    LocalModelConfig config;
    if (!toml_read::read_optional_string(table, "engine_id", config.engine_id, error) ||
        !toml_read::read_optional_string(table, "backend_id", config.backend_id, error) ||
        !toml_read::read_optional_string(table, "model_id", config.model_id, error) ||
        !toml_read::read_optional_string(table, "model_alias", config.model_alias, error) ||
        !toml_read::read_optional_string(table, "display_name", config.display_name, error) ||
        !toml_read::read_optional_string(table, "execution_mode", config.execution_mode, error) ||
        !toml_read::read_optional_string(table, "model_path", config.model_path, error) ||
        !toml_read::read_optional_string(table, "sidecar_base_url", config.sidecar_base_url, error) ||
        !toml_read::read_optional_integer(table, "context_size", config.context_size, error) ||
        !toml_read::read_optional_integer(table, "batch_size", config.batch_size, error) ||
        !toml_read::read_optional_integer(table, "n_gpu_layers", config.n_gpu_layers, error) ||
        !toml_read::read_optional_integer(table, "n_threads", config.n_threads, error) ||
        !toml_read::read_optional_integer(table, "n_threads_batch", config.n_threads_batch, error) ||
        !toml_read::read_optional_integer(
            table,
            "sidecar_timeout_seconds",
            config.sidecar_timeout_seconds,
            error) ||
        !toml_read::read_optional_integer(
            table,
            "sidecar_connect_timeout_seconds",
            config.sidecar_connect_timeout_seconds,
            error) ||
        !toml_read::read_optional_bool(table, "sidecar_verify_tls", config.sidecar_verify_tls, error) ||
        !toml_read::read_optional_string_array(table, "capabilities", config.capabilities, error)) {
        return false;
    }

    if (config.display_name.empty()) {
        config.display_name = config.model_alias.empty() ? engine_name : config.model_alias;
    }

    out = std::move(config);
    return true;
}

bool parse_remote_backend_table(
    const std::string & backend_name,
    const Value & table,
    CloudConfig & out,
    std::string & error) {
    if (!validate_allowed_keys(
            table,
            {
                "backend_id",
                "model",
                "model_alias",
                "display_name",
                "execution_mode",
                "capabilities",
                "base_url",
                "api_key",
                "timeout_seconds",
                "connect_timeout_seconds",
                "retry_attempts",
                "retry_backoff_ms",
                "send_modal_session_id",
                "verify_tls",
            },
            "remote backend '" + backend_name + "'",
            error)) {
        return false;
    }

    CloudConfig config;
    if (!toml_read::read_optional_string(table, "backend_id", config.backend_id, error) ||
        !toml_read::read_optional_string(table, "model", config.model, error) ||
        !toml_read::read_optional_string(table, "model_alias", config.model_alias, error) ||
        !toml_read::read_optional_string(table, "display_name", config.display_name, error) ||
        !toml_read::read_optional_string(table, "execution_mode", config.execution_mode, error) ||
        !toml_read::read_optional_string(table, "base_url", config.base_url, error) ||
        !toml_read::read_optional_string(table, "api_key", config.api_key, error) ||
        !toml_read::read_optional_integer(table, "timeout_seconds", config.timeout_seconds, error) ||
        !toml_read::read_optional_integer(
            table,
            "connect_timeout_seconds",
            config.connect_timeout_seconds,
            error) ||
        !toml_read::read_optional_integer(table, "retry_attempts", config.retry_attempts, error) ||
        !toml_read::read_optional_integer(table, "retry_backoff_ms", config.retry_backoff_ms, error) ||
        !toml_read::read_optional_bool(
            table,
            "send_modal_session_id",
            config.send_modal_session_id,
            error) ||
        !toml_read::read_optional_bool(table, "verify_tls", config.verify_tls, error) ||
        !toml_read::read_optional_string_array(table, "capabilities", config.capabilities, error)) {
        return false;
    }

    if (config.display_name.empty()) {
        config.display_name = config.model_alias.empty() ? backend_name : config.model_alias;
    }

    out = std::move(config);
    return true;
}

bool parse_exposed_model_table(
    const Value & table,
    ModelCatalogEntry & out,
    std::string & error) {
    if (!validate_allowed_keys(
            table,
            {
                "id",
                "display_name",
                "owned_by",
                "route_target",
                "engine_id",
                "backend_id",
                "model_id",
                "model_alias",
                "execution_mode",
                "capabilities",
            },
            "exposed_models entry",
            error)) {
        return false;
    }

    ModelCatalogEntry entry;
    std::string route_target;
    if (!toml_read::read_required_string(table, "id", entry.id, error) ||
        !toml_read::read_optional_string(table, "display_name", entry.display_name, error) ||
        !toml_read::read_optional_string(table, "owned_by", entry.owned_by, error) ||
        !toml_read::read_optional_string(table, "engine_id", entry.engine_id, error) ||
        !toml_read::read_optional_string(table, "backend_id", entry.backend_id, error) ||
        !toml_read::read_optional_string(table, "model_id", entry.model_id, error) ||
        !toml_read::read_optional_string(table, "model_alias", entry.model_alias, error) ||
        !toml_read::read_optional_string(table, "execution_mode", entry.execution_mode, error) ||
        !toml_read::read_optional_string(table, "route_target", route_target, error) ||
        !toml_read::read_optional_string_array(table, "capabilities", entry.capabilities, error)) {
        return false;
    }

    if (!route_target.empty() && !parse_route_target(route_target, entry.route_target)) {
        error = "Invalid exposed model route_target: " + route_target;
        return false;
    }
    if (entry.display_name.empty()) {
        entry.display_name = entry.id;
    }
    if (entry.model_alias.empty()) {
        entry.model_alias = entry.id;
    }

    out = std::move(entry);
    return true;
}

void add_or_update_exposed_model(
    std::vector<ModelCatalogEntry> & exposed_models,
    ModelCatalogEntry entry) {
    for (auto & existing : exposed_models) {
        if (existing.id == entry.id) {
            if (!entry.display_name.empty()) {
                existing.display_name = std::move(entry.display_name);
            }
            if (!entry.owned_by.empty()) {
                existing.owned_by = std::move(entry.owned_by);
            }
            if (entry.route_target != RouteTarget::kAuto) {
                existing.route_target = entry.route_target;
            }
            if (!entry.engine_id.empty()) {
                existing.engine_id = std::move(entry.engine_id);
            }
            if (!entry.backend_id.empty()) {
                existing.backend_id = std::move(entry.backend_id);
            }
            if (!entry.model_id.empty()) {
                existing.model_id = std::move(entry.model_id);
            }
            if (!entry.model_alias.empty()) {
                existing.model_alias = std::move(entry.model_alias);
            }
            if (!entry.execution_mode.empty()) {
                existing.execution_mode = std::move(entry.execution_mode);
            }
            if (!entry.capabilities.empty()) {
                existing.capabilities = std::move(entry.capabilities);
            }
            return;
        }
    }

    exposed_models.push_back(std::move(entry));
}

void ensure_default_public_model(DaemonConfig & config) {
    ModelCatalogEntry default_entry;
    default_entry.id = config.public_model_alias;
    default_entry.display_name = "Seceda default route";
    default_entry.owned_by = "seceda";
    add_or_update_exposed_model(config.exposed_models, std::move(default_entry));
}

bool validate_cloud_config(const CloudConfig & config, const std::string & label, std::string & error) {
    if (config.timeout_seconds <= 0) {
        error = label + " timeout seconds must be positive";
        return false;
    }
    if (config.connect_timeout_seconds <= 0) {
        error = label + " connect timeout seconds must be positive";
        return false;
    }
    if (config.retry_attempts < 0) {
        error = label + " retry attempts must be zero or greater";
        return false;
    }
    if (config.retry_backoff_ms < 0) {
        error = label + " retry backoff must be zero or greater";
        return false;
    }
    return true;
}

bool validate_local_config(const LocalModelConfig & config, const std::string & label, std::string & error) {
    if (config.context_size <= 0) {
        error = label + " context size must be positive";
        return false;
    }
    if (config.batch_size <= 0) {
        error = label + " batch size must be positive";
        return false;
    }
    if (config.sidecar_timeout_seconds <= 0) {
        error = label + " sidecar timeout seconds must be positive";
        return false;
    }
    if (config.sidecar_connect_timeout_seconds <= 0) {
        error = label + " sidecar connect timeout seconds must be positive";
        return false;
    }
    if (is_sidecar_execution_mode(config.execution_mode) && config.sidecar_base_url.empty()) {
        error = label + " sidecar engines require sidecar_base_url";
        return false;
    }
    return true;
}

bool apply_daemon_table(const Value & table, DaemonConfig & config, std::string & error) {
    if (!validate_allowed_keys(
            table,
            {"host", "port", "public_model_alias", "default_system_prompt", "warmup_prompt"},
            "daemon",
            error)) {
        return false;
    }

    return toml_read::read_optional_string(table, "host", config.host, error) &&
        toml_read::read_optional_integer(table, "port", config.port, error) &&
        toml_read::read_optional_string(table, "public_model_alias", config.public_model_alias, error) &&
        toml_read::read_optional_string(
            table,
            "default_system_prompt",
            config.default_system_prompt,
            error) &&
        toml_read::read_optional_string(table, "warmup_prompt", config.warmup_prompt, error);
}

bool apply_generation_table(const Value & table, DaemonConfig & config, std::string & error) {
    if (!validate_allowed_keys(
            table,
            {
                "max_completion_tokens",
                "temperature",
                "top_p",
                "top_k",
                "min_p",
                "seed",
            },
            "generation",
            error)) {
        return false;
    }

    return toml_read::read_optional_integer(
               table,
               "max_completion_tokens",
               config.default_generation.max_completion_tokens,
               error) &&
        toml_read::read_optional_float(
               table,
               "temperature",
               config.default_generation.temperature,
               error) &&
        toml_read::read_optional_float(table, "top_p", config.default_generation.top_p, error) &&
        toml_read::read_optional_integer(table, "top_k", config.default_generation.top_k, error) &&
        toml_read::read_optional_float(table, "min_p", config.default_generation.min_p, error) &&
        toml_read::read_optional_integer(table, "seed", config.default_generation.seed, error);
}

bool apply_router_table(const Value & table, DaemonConfig & config, std::string & error) {
    if (!validate_allowed_keys(
            table,
            {
                "max_prompt_chars",
                "max_estimated_tokens",
                "structured_keywords",
                "cloud_keywords",
                "freshness_keywords",
            },
            "router",
            error)) {
        return false;
    }

    return toml_read::read_optional_integer(
               table,
               "max_prompt_chars",
               config.router.max_prompt_chars,
               error) &&
        toml_read::read_optional_integer(
               table,
               "max_estimated_tokens",
               config.router.max_estimated_tokens,
               error) &&
        toml_read::read_optional_string_array(
               table,
               "structured_keywords",
               config.router.structured_keywords,
               error) &&
        toml_read::read_optional_string_array(
               table,
               "cloud_keywords",
               config.router.cloud_keywords,
               error) &&
        toml_read::read_optional_string_array(
               table,
               "freshness_keywords",
               config.router.freshness_keywords,
               error);
}

bool apply_observability_table(const Value & table, DaemonConfig & config, std::string & error) {
    if (!validate_allowed_keys(table, {"event_log_capacity"}, "observability", error)) {
        return false;
    }

    return toml_read::read_optional_integer(table, "event_log_capacity", config.event_log_capacity, error);
}

bool apply_local_table(const Value & table, DaemonConfig & config, std::string & error) {
    if (!validate_allowed_keys(table, {"active_engine", "engines"}, "local", error)) {
        return false;
    }

    std::string active_engine;
    if (!toml_read::read_optional_string(table, "active_engine", active_engine, error)) {
        return false;
    }

    const Value * const engines = simple_toml::table_get(table, "engines");
    if (engines == nullptr) {
        if (!active_engine.empty()) {
            error = "local.active_engine requires [local.engines.<name>] tables";
            return false;
        }
        return true;
    }
    if (!engines->is_table()) {
        error = "local.engines must be a table";
        return false;
    }
    if (active_engine.empty()) {
        error = "local.active_engine is required when local.engines is configured";
        return false;
    }

    std::vector<std::pair<std::string, LocalModelConfig>> parsed_engines;
    for (const auto & engine_name : simple_toml::table_keys(*engines)) {
        const Value * const engine_table = simple_toml::table_get(*engines, engine_name);
        if (engine_table == nullptr || !engine_table->is_table()) {
            error = "local.engines." + engine_name + " must be a table";
            return false;
        }

        LocalModelConfig parsed;
        if (!parse_local_engine_table(engine_name, *engine_table, parsed, error)) {
            return false;
        }
        parsed_engines.emplace_back(engine_name, std::move(parsed));
    }

    bool found_active = false;
    config.local_engines.clear();
    for (auto & [engine_name, engine_config] : parsed_engines) {
        if (engine_name == active_engine) {
            config.local = engine_config;
            found_active = true;
        } else {
            config.local_engines.push_back(std::move(engine_config));
        }
    }

    if (!found_active) {
        error = "local.active_engine '" + active_engine + "' does not match any configured engine";
        return false;
    }

    return true;
}

bool apply_remote_table(const Value & table, DaemonConfig & config, std::string & error) {
    if (!validate_allowed_keys(table, {"default_backend", "backends"}, "remote", error)) {
        return false;
    }

    std::string default_backend;
    if (!toml_read::read_optional_string(table, "default_backend", default_backend, error)) {
        return false;
    }

    const Value * const backends = simple_toml::table_get(table, "backends");
    if (backends == nullptr) {
        if (!default_backend.empty()) {
            error = "remote.default_backend requires [remote.backends.<name>] tables";
            return false;
        }
        return true;
    }
    if (!backends->is_table()) {
        error = "remote.backends must be a table";
        return false;
    }
    if (default_backend.empty()) {
        error = "remote.default_backend is required when remote.backends is configured";
        return false;
    }

    std::vector<std::pair<std::string, CloudConfig>> parsed_backends;
    for (const auto & backend_name : simple_toml::table_keys(*backends)) {
        const Value * const backend_table = simple_toml::table_get(*backends, backend_name);
        if (backend_table == nullptr || !backend_table->is_table()) {
            error = "remote.backends." + backend_name + " must be a table";
            return false;
        }

        CloudConfig parsed;
        if (!parse_remote_backend_table(backend_name, *backend_table, parsed, error)) {
            return false;
        }
        parsed_backends.emplace_back(backend_name, std::move(parsed));
    }

    bool found_default = false;
    config.remote_backends.clear();
    for (auto & [backend_name, backend_config] : parsed_backends) {
        if (backend_name == default_backend) {
            config.cloud = backend_config;
            found_default = true;
        } else {
            config.remote_backends.push_back(std::move(backend_config));
        }
    }

    if (!found_default) {
        error =
            "remote.default_backend '" + default_backend +
            "' does not match any configured backend";
        return false;
    }

    return true;
}

bool apply_exposed_models(const Value & array, DaemonConfig & config, std::string & error) {
    if (!array.is_array()) {
        error = "exposed_models must be an array of tables";
        return false;
    }

    config.exposed_models.clear();
    for (const auto & item : array.array_items) {
        if (!item.is_table()) {
            error = "exposed_models entries must be tables";
            return false;
        }

        ModelCatalogEntry parsed;
        if (!parse_exposed_model_table(item, parsed, error)) {
            return false;
        }
        config.exposed_models.push_back(std::move(parsed));
    }

    return true;
}

bool apply_config_file(const std::string & path, DaemonConfig & config, std::string & error) {
    Value root;
    if (!load_toml_file(path, root, error)) {
        return false;
    }

    if (!validate_allowed_keys(
            root,
            {
                "schema_version",
                "daemon",
                "generation",
                "router",
                "local",
                "remote",
                "observability",
                "exposed_models",
            },
            "config root",
            error)) {
        return false;
    }

    std::int64_t schema_version = 1;
    if (!toml_read::read_optional_integer(root, "schema_version", schema_version, error)) {
        return false;
    }
    if (schema_version != 1) {
        error = "Unsupported schema_version in config file: " + std::to_string(schema_version);
        return false;
    }

    if (const Value * const daemon = simple_toml::table_get(root, "daemon")) {
        if (!daemon->is_table()) {
            error = "daemon must be a table";
            return false;
        }
        if (!apply_daemon_table(*daemon, config, error)) {
            return false;
        }
    }
    if (const Value * const generation = simple_toml::table_get(root, "generation")) {
        if (!generation->is_table()) {
            error = "generation must be a table";
            return false;
        }
        if (!apply_generation_table(*generation, config, error)) {
            return false;
        }
    }
    if (const Value * const router = simple_toml::table_get(root, "router")) {
        if (!router->is_table()) {
            error = "router must be a table";
            return false;
        }
        if (!apply_router_table(*router, config, error)) {
            return false;
        }
    }
    if (const Value * const local = simple_toml::table_get(root, "local")) {
        if (!local->is_table()) {
            error = "local must be a table";
            return false;
        }
        if (!apply_local_table(*local, config, error)) {
            return false;
        }
    }
    if (const Value * const remote = simple_toml::table_get(root, "remote")) {
        if (!remote->is_table()) {
            error = "remote must be a table";
            return false;
        }
        if (!apply_remote_table(*remote, config, error)) {
            return false;
        }
    }
    if (const Value * const observability = simple_toml::table_get(root, "observability")) {
        if (!observability->is_table()) {
            error = "observability must be a table";
            return false;
        }
        if (!apply_observability_table(*observability, config, error)) {
            return false;
        }
    }
    if (const Value * const exposed_models = simple_toml::table_get(root, "exposed_models")) {
        if (!apply_exposed_models(*exposed_models, config, error)) {
            return false;
        }
    }

    return true;
}

bool scan_config_file_selection(
    int argc,
    char ** argv,
    ConfigFileSelection & selection,
    std::string & error,
    bool & show_help) {
    selection = ConfigFileSelection{};
    error.clear();
    show_help = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            show_help = true;
            return true;
        }

        if (arg.rfind("--config", 0) == 0) {
            if (selection.disable) {
                error = "Cannot combine --config with --no-config";
                return false;
            }
            if (!next_value(argc, argv, i, arg, selection.path, error)) {
                return false;
            }
            selection.explicit_path = true;
        } else if (arg == "--no-config") {
            if (selection.explicit_path) {
                error = "Cannot combine --config with --no-config";
                return false;
            }
            selection.disable = true;
        }
    }

    if (selection.disable) {
        return true;
    }

    if (!selection.explicit_path) {
        if (const char * env_config = std::getenv("SECEDA_CONFIG")) {
            selection.path = env_config;
            selection.explicit_path = true;
        } else {
            resolve_runtime_config_path(argv[0], selection.path);
        }
    }

    if (selection.explicit_path && !selection.path.empty() &&
        !std::filesystem::is_regular_file(selection.path)) {
        error = "Config file not found: " + selection.path;
        return false;
    }

    if (!selection.path.empty()) {
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(selection.path, ec);
        if (!ec) {
            selection.path = absolute.string();
        }
    }

    return true;
}

}  // namespace

bool RuntimeConfigParser::parse(
    int argc,
    char ** argv,
    DaemonConfig & config,
    std::string & error,
    bool & show_help) {
    error.clear();

    ConfigFileSelection config_file;
    if (!scan_config_file_selection(argc, argv, config_file, error, show_help)) {
        return false;
    }
    if (show_help) {
        return true;
    }

    {
        std::string dotenv_error;
        if (!load_dotenv_for_config_context(config_file.path, config_file.disable, dotenv_error)) {
            error = std::move(dotenv_error);
            return false;
        }
    }

    if (!config_file.disable && !config_file.path.empty()) {
        if (!apply_config_file(config_file.path, config, error)) {
            return false;
        }
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            show_help = true;
            return true;
        }

        if (arg.rfind("--config", 0) == 0) {
            std::string ignored_value;
            if (!next_value(argc, argv, i, arg, ignored_value, error)) {
                return false;
            }
            continue;
        }
        if (arg == "--no-config") {
            continue;
        }

        if (arg.rfind("--host", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.host = value;
        } else if (arg.rfind("--port", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.port, error)) {
                return false;
            }
        } else if (arg.rfind("--model-path", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.local.model_path = value;
        } else if (arg.rfind("--local-engine-id", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.local.engine_id = value;
        } else if (arg.rfind("--local-backend-id", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.local.backend_id = value;
        } else if (arg.rfind("--local-model-id", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.local.model_id = value;
        } else if (arg.rfind("--local-model-alias", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.local.model_alias = value;
        } else if (arg.rfind("--local-display-name", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.local.display_name = value;
        } else if (arg.rfind("--local-execution-mode", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            const std::string trimmed = text::trim_copy(value);
            config.local.execution_mode = trimmed.empty() ? std::string{"in_process"} : trimmed;
        } else if (arg.rfind("--local-sidecar-base-url", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.local.sidecar_base_url = value;
        } else if (arg.rfind("--local-sidecar-timeout-seconds", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.local.sidecar_timeout_seconds, error)) {
                return false;
            }
        } else if (arg.rfind("--local-sidecar-connect-timeout-seconds", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.local.sidecar_connect_timeout_seconds, error)) {
                return false;
            }
        } else if (arg.rfind("--local-sidecar-verify-tls", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_bool_flag(value, "--local-sidecar-verify-tls", config.local.sidecar_verify_tls, error)) {
                return false;
            }
        } else if (arg.rfind("--system-prompt", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.default_system_prompt = value;
        } else if (arg.rfind("--warmup-prompt", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.warmup_prompt = value;
        } else if (arg.rfind("--n-ctx", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.local.context_size, error)) {
                return false;
            }
        } else if (arg.rfind("--n-batch", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.local.batch_size, error)) {
                return false;
            }
        } else if (arg.rfind("--n-gpu-layers", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.local.n_gpu_layers, error)) {
                return false;
            }
        } else if (arg.rfind("--n-threads-batch", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.local.n_threads_batch, error)) {
                return false;
            }
        } else if (arg.rfind("--n-threads", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.local.n_threads, error)) {
                return false;
            }
        } else if (arg.rfind("--default-max-tokens", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.default_generation.max_completion_tokens, error)) {
                return false;
            }
        } else if (arg.rfind("--public-model-alias", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.public_model_alias = value;
        } else if (arg.rfind("--default-temperature", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.default_generation.temperature, error)) {
                return false;
            }
        } else if (arg.rfind("--default-top-p", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.default_generation.top_p, error)) {
                return false;
            }
        } else if (arg.rfind("--default-top-k", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.default_generation.top_k, error)) {
                return false;
            }
        } else if (arg.rfind("--default-min-p", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.default_generation.min_p, error)) {
                return false;
            }
        } else if (arg.rfind("--default-seed", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.default_generation.seed, error)) {
                return false;
            }
        } else if (arg.rfind("--local-engine", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_local_engine_spec(value, config.local_engines, error)) {
                return false;
            }
        } else if (arg.rfind("--cloud-base-url", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.cloud.base_url = value;
        } else if (arg.rfind("--cloud-backend-id", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.cloud.backend_id = value;
        } else if (arg.rfind("--cloud-model", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.cloud.model = value;
        } else if (arg.rfind("--cloud-model-alias", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.cloud.model_alias = value;
        } else if (arg.rfind("--cloud-display-name", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.cloud.display_name = value;
        } else if (arg.rfind("--cloud-api-key", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.cloud.api_key = value;
        } else if (arg.rfind("--cloud-timeout-seconds", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.cloud.timeout_seconds, error)) {
                return false;
            }
        } else if (arg.rfind("--cloud-connect-timeout-seconds", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.cloud.connect_timeout_seconds, error)) {
                return false;
            }
        } else if (arg.rfind("--cloud-retry-attempts", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.cloud.retry_attempts, error)) {
                return false;
            }
        } else if (arg.rfind("--cloud-retry-backoff-ms", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.cloud.retry_backoff_ms, error)) {
                return false;
            }
        } else if (arg.rfind("--cloud-send-modal-session-id", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_bool_flag(
                    value,
                    "--cloud-send-modal-session-id",
                    config.cloud.send_modal_session_id,
                    error)) {
                return false;
            }
        } else if (arg.rfind("--cloud-verify-tls", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_bool_flag(value, "--cloud-verify-tls", config.cloud.verify_tls, error)) {
                return false;
            }
        } else if (arg.rfind("--remote-backend", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_remote_backend_spec(value, config.remote_backends, error)) {
                return false;
            }
        } else if (arg.rfind("--exposed-model", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_exposed_model_spec(value, config.exposed_models, error)) {
                return false;
            }
        } else if (arg.rfind("--router-max-chars", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.router.max_prompt_chars, error)) {
                return false;
            }
        } else if (arg.rfind("--router-max-estimated-tokens", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error) ||
                !parse_number(value, config.router.max_estimated_tokens, error)) {
                return false;
            }
        } else if (arg.rfind("--router-structured-keywords", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.router.structured_keywords = split_csv(value);
        } else if (arg.rfind("--router-cloud-keywords", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.router.cloud_keywords = split_csv(value);
        } else if (arg.rfind("--router-freshness-keywords", 0) == 0) {
            std::string value;
            if (!next_value(argc, argv, i, arg, value, error)) {
                return false;
            }
            config.router.freshness_keywords = split_csv(value);
        } else if (arg.rfind("--event-log-capacity", 0) == 0) {
            std::string value;
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
    if (config.default_generation.max_completion_tokens <= 0) {
        error = "Default max tokens must be positive";
        return false;
    }
    if (config.router.max_estimated_tokens <= 0) {
        error = "Router max estimated tokens must be positive";
        return false;
    }
    if (!validate_local_config(config.local, "Local runtime", error)) {
        return false;
    }
    if (!validate_cloud_config(config.cloud, "Cloud backend", error)) {
        return false;
    }
    for (std::size_t i = 0; i < config.local_engines.size(); ++i) {
        if (!validate_local_config(config.local_engines[i], "Named local engine", error)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < config.remote_backends.size(); ++i) {
        if (!validate_cloud_config(config.remote_backends[i], "Named remote backend", error)) {
            return false;
        }
    }

    if (const char * env_api_key = std::getenv("SECEDA_CLOUD_API_KEY")) {
        config.cloud.api_key = env_api_key;
    }

    ensure_default_public_model(config);
    return true;
}

std::string RuntimeConfigParser::help_text(const char * program_name) {
    return render_runtime_config_catalog_help(program_name);
}

}  // namespace seceda::edge
