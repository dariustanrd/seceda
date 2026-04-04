#pragma once

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

/// Trims; empty becomes in_process for resolution purposes.
std::string normalized_local_execution_mode_for_resolve(const std::string & raw);

}  // namespace seceda::edge
