#pragma once

#include <string>

namespace seceda::edge {

// Loads environment variables from the first readable ".env" file found:
//   1) Walk upward from the config file's directory (when a config path is in use).
//   2) Then ".env" in the current working directory.
// With --no-config, only (2) is tried.
//
// Does not override variables already set in the process environment (setenv overwrite=0).
// Returns false if a candidate file exists but cannot be read or contains invalid entries.
bool load_dotenv_for_config_context(
    const std::string & resolved_config_path,
    bool no_config_file,
    std::string & error);

}  // namespace seceda::edge
