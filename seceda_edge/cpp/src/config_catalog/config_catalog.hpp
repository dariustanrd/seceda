#pragma once

#include <string>
#include <vector>

namespace seceda::edge {

enum class ConfigValueKind {
    kString,
    kInteger,
    kFloat,
    kBoolean,
    kCsv,
    kStructuredSpec,
};

struct ConfigSourceMapping {
    std::string cli_flag;
    std::string env_var;
    std::string config_key;
    std::string tui_field_id;
};

struct ConfigCatalogEntry {
    std::string key;
    std::string group;
    std::string label;
    std::string description;
    ConfigValueKind value_kind = ConfigValueKind::kString;
    std::string cli_value_name;
    std::vector<std::string> enum_values;
    std::string default_value;
    std::string units;
    std::string min_value;
    std::string max_value;
    ConfigSourceMapping sources;
    bool sensitive = false;
    bool hot_reloadable = false;
    bool requires_restart = true;
    bool advanced = false;
    bool experimental = false;
    bool deprecated = false;
};

bool load_runtime_config_catalog(
    const char * program_name,
    std::vector<ConfigCatalogEntry> & out,
    std::string & error);

bool resolve_runtime_config_path(const char * program_name, std::string & out_path);

bool resolve_runtime_config_catalog_path(const char * program_name, std::string & out_path);

std::string render_runtime_config_catalog_help(const char * program_name);

}  // namespace seceda::edge
