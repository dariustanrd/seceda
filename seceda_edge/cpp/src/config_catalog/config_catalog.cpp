#include "config_catalog/config_catalog.hpp"

#include "config_catalog/simple_toml_read.hpp"
#include "config_catalog/simple_toml.hpp"
#include "file_utils/read_text_file.hpp"

#include <cstdlib>
#include <filesystem>
#include <set>

namespace seceda::edge {
namespace {

using simple_toml::Value;
namespace fs_utils = seceda::edge::file_utils;
namespace toml_read = seceda::edge::simple_toml_read;

constexpr const char * kDefaultRuntimeConfigRelativePath = "seceda_edge/config/seceda.toml";
constexpr const char * kDefaultRuntimeConfigCatalogRelativePath =
    "seceda_edge/config/config_catalog.toml";
constexpr const char * kFallbackRuntimeConfigRelativePath = "config/seceda.toml";
constexpr const char * kFallbackRuntimeConfigCatalogRelativePath = "config/config_catalog.toml";

std::string value_kind_label(ConfigValueKind kind) {
    switch (kind) {
        case ConfigValueKind::kString:
            return "string";
        case ConfigValueKind::kInteger:
            return "integer";
        case ConfigValueKind::kFloat:
            return "float";
        case ConfigValueKind::kBoolean:
            return "boolean";
        case ConfigValueKind::kCsv:
            return "csv";
        case ConfigValueKind::kStructuredSpec:
            return "spec";
    }

    return "string";
}

std::vector<std::filesystem::path> search_roots(const char * program_name) {
    std::vector<std::filesystem::path> roots;
    roots.push_back(std::filesystem::current_path());

    if (program_name != nullptr && program_name[0] != '\0') {
        std::filesystem::path program_path(program_name);
        if (program_path.is_relative()) {
            program_path = std::filesystem::current_path() / program_path;
        }
        roots.push_back(program_path.parent_path());
    }

    std::vector<std::filesystem::path> deduped;
    std::set<std::string> seen;
    for (const auto & root : roots) {
        std::error_code ec;
        const auto absolute_root = std::filesystem::absolute(root, ec);
        const std::string normalized = ec ? root.string() : absolute_root.string();
        if (seen.insert(normalized).second) {
            deduped.push_back(ec ? root : absolute_root);
        }
    }
    return deduped;
}

bool search_upward_for_file(
    const std::vector<std::filesystem::path> & roots,
    const std::vector<std::filesystem::path> & relative_candidates,
    std::string & out_path) {
    for (const auto & root : roots) {
        std::filesystem::path current = root;
        while (!current.empty()) {
            for (const auto & candidate_relative : relative_candidates) {
                const auto candidate = current / candidate_relative;
                if (std::filesystem::is_regular_file(candidate)) {
                    out_path = std::filesystem::weakly_canonical(candidate).string();
                    return true;
                }
            }

            const auto parent = current.parent_path();
            if (parent == current) {
                break;
            }
            current = parent;
        }
    }

    return false;
}

bool parse_value_kind(const std::string & raw, ConfigValueKind & out) {
    if (raw == "string") {
        out = ConfigValueKind::kString;
        return true;
    }
    if (raw == "integer") {
        out = ConfigValueKind::kInteger;
        return true;
    }
    if (raw == "float") {
        out = ConfigValueKind::kFloat;
        return true;
    }
    if (raw == "boolean") {
        out = ConfigValueKind::kBoolean;
        return true;
    }
    if (raw == "csv") {
        out = ConfigValueKind::kCsv;
        return true;
    }
    if (raw == "spec" || raw == "structured_spec") {
        out = ConfigValueKind::kStructuredSpec;
        return true;
    }
    return false;
}

}  // namespace

bool resolve_runtime_config_path(const char * program_name, std::string & out_path) {
    const std::vector<std::filesystem::path> candidates = {
        kDefaultRuntimeConfigRelativePath,
        kFallbackRuntimeConfigRelativePath,
    };
    return search_upward_for_file(search_roots(program_name), candidates, out_path);
}

bool resolve_runtime_config_catalog_path(const char * program_name, std::string & out_path) {
    if (const char * explicit_catalog = std::getenv("SECEDA_CONFIG_CATALOG")) {
        if (std::filesystem::is_regular_file(explicit_catalog)) {
            out_path = std::filesystem::weakly_canonical(explicit_catalog).string();
            return true;
        }
    }

    const std::vector<std::filesystem::path> candidates = {
        kDefaultRuntimeConfigCatalogRelativePath,
        kFallbackRuntimeConfigCatalogRelativePath,
    };
    return search_upward_for_file(search_roots(program_name), candidates, out_path);
}

bool load_runtime_config_catalog(
    const char * program_name,
    std::vector<ConfigCatalogEntry> & out,
    std::string & error) {
    out.clear();

    std::string catalog_path;
    if (!resolve_runtime_config_catalog_path(program_name, catalog_path)) {
        error =
            "Unable to locate config catalog file. Expected " +
            std::string{kDefaultRuntimeConfigCatalogRelativePath} + " in or above the current "
            "working directory.";
        return false;
    }

    std::string content;
    if (!fs_utils::read_text_file(catalog_path, content, error)) {
        return false;
    }

    Value root;
    if (!simple_toml::parse_document(content, root, error)) {
        error = "Failed to parse config catalog TOML: " + error;
        return false;
    }

    const Value * const fields = simple_toml::table_get(root, "fields");
    if (fields == nullptr || !fields->is_array()) {
        error = "Config catalog must contain [[fields]] entries";
        return false;
    }

    for (const auto & item : fields->array_items) {
        if (!item.is_table()) {
            error = "Each [[fields]] entry must be a table";
            return false;
        }

        ConfigCatalogEntry entry;
        std::string raw_kind;
        if (!toml_read::read_required_string(item, "key", entry.key, error) ||
            !toml_read::read_required_string(item, "group", entry.group, error) ||
            !toml_read::read_required_string(item, "label", entry.label, error) ||
            !toml_read::read_required_string(item, "description", entry.description, error) ||
            !toml_read::read_required_string(item, "type", raw_kind, error)) {
            return false;
        }
        if (!parse_value_kind(raw_kind, entry.value_kind)) {
            error = "Unknown config value kind: " + raw_kind;
            return false;
        }

        if (!toml_read::read_optional_string(item, "cli_flag", entry.sources.cli_flag, error) ||
            !toml_read::read_optional_string(item, "cli_value_name", entry.cli_value_name, error) ||
            !toml_read::read_optional_string(item, "env_var", entry.sources.env_var, error) ||
            !toml_read::read_optional_string(item, "config_path", entry.sources.config_key, error) ||
            !toml_read::read_optional_string(item, "tui_field_id", entry.sources.tui_field_id, error) ||
            !toml_read::read_optional_string(item, "default", entry.default_value, error) ||
            !toml_read::read_optional_string(item, "units", entry.units, error) ||
            !toml_read::read_optional_string(item, "min", entry.min_value, error) ||
            !toml_read::read_optional_string(item, "max", entry.max_value, error) ||
            !toml_read::read_optional_string_array(item, "enum_values", entry.enum_values, error) ||
            !toml_read::read_optional_bool(item, "sensitive", entry.sensitive, error) ||
            !toml_read::read_optional_bool(item, "hot_reloadable", entry.hot_reloadable, error) ||
            !toml_read::read_optional_bool(item, "requires_restart", entry.requires_restart, error) ||
            !toml_read::read_optional_bool(item, "advanced", entry.advanced, error) ||
            !toml_read::read_optional_bool(item, "experimental", entry.experimental, error) ||
            !toml_read::read_optional_bool(item, "deprecated", entry.deprecated, error)) {
            return false;
        }

        out.push_back(std::move(entry));
    }

    return true;
}

std::string render_runtime_config_catalog_help(const char * program_name) {
    std::vector<ConfigCatalogEntry> catalog;
    std::string error;
    const bool loaded = load_runtime_config_catalog(program_name, catalog, error);

    std::ostringstream out;
    out << "Usage: " << (program_name == nullptr ? "seceda_edge_daemon" : program_name)
        << " [options]\n\n";

    std::string config_path;
    if (resolve_runtime_config_path(program_name, config_path)) {
        out << "Config precedence: built-in defaults, TOML file, environment, CLI overrides.\n";
        out << "Default config file: " << config_path << "\n";
    } else {
        out << "Config precedence: built-in defaults, TOML file, environment, CLI overrides.\n";
        out << "Default config file hint: " << kDefaultRuntimeConfigRelativePath << "\n";
    }
    out << "Use `--config PATH` to load a different file or `--no-config` to skip file loading.\n";
    out << "If a `.env` file exists in the config directory or a parent directory (or in the "
           "current working directory as a fallback), variables are loaded into the process "
           "environment before the TOML file is read; existing environment variables are not "
           "overwritten.\n\n";

    if (!loaded) {
        out << "Catalog load error: " << error << "\n";
        return out.str();
    }

    std::string current_group;
    for (const auto & item : catalog) {
        if (item.group != current_group) {
            if (!current_group.empty()) {
                out << "\n";
            }
            current_group = item.group;
            out << current_group << ":\n";
        }

        out << "  " << item.sources.cli_flag;
        if (!item.cli_value_name.empty()) {
            out << " " << item.cli_value_name;
        }
        out << "\n";
        out << "      " << item.description;
        out << " Type: " << value_kind_label(item.value_kind) << ".";
        if (!item.enum_values.empty()) {
            out << " Values: ";
            for (std::size_t i = 0; i < item.enum_values.size(); ++i) {
                if (i > 0) {
                    out << ", ";
                }
                out << item.enum_values[i];
            }
            out << ".";
        }
        if (!item.default_value.empty()) {
            out << " Default: " << item.default_value << ".";
        }
        if (!item.units.empty()) {
            out << " Units: " << item.units << ".";
        }
        if (!item.sources.env_var.empty()) {
            out << " Env: " << item.sources.env_var << ".";
        }
        if (item.advanced) {
            out << " Advanced.";
        }
        if (item.experimental) {
            out << " Experimental.";
        }
        if (item.deprecated) {
            out << " Deprecated.";
        }
        out << "\n";
    }

    return out.str();
}

}  // namespace seceda::edge
