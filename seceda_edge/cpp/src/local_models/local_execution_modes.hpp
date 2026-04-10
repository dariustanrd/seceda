#pragma once

#include "text_utils/normalize.hpp"

#include <optional>
#include <string>

namespace seceda::edge {

/// Canonical string values for `LocalModelConfig::execution_mode` (matches approved contract).
inline constexpr const char * kLocalExecutionModeInProcess = "in_process";
inline constexpr const char * kLocalExecutionModeSdkBridge = "sdk_bridge";
inline constexpr const char * kLocalExecutionModeSidecarServer = "sidecar_server";

enum class LocalExecutionModeKind {
    kInProcess,
    kSdkBridge,
    kSidecarServer,
};

/// Parses known execution mode strings; unknown or empty values yield std::nullopt (callers may default).
std::optional<LocalExecutionModeKind> parse_local_execution_mode_kind(const std::string & value);

const char * local_execution_mode_kind_to_contract_string(LocalExecutionModeKind kind);

inline bool is_sidecar_execution_mode(const std::string & raw) {
    const std::string lowered = text_utils::to_lower_ascii_copy(text_utils::trim_copy(raw));
    return lowered == kLocalExecutionModeSidecarServer || lowered == "sidecar-server" ||
        lowered == "sidecar";
}

/// Trims; empty becomes in_process for resolution purposes.
std::string normalized_local_execution_mode_for_resolve(const std::string & raw);

}  // namespace seceda::edge
