#include "local_models/local_execution_modes.hpp"

#include <algorithm>
#include <cctype>

namespace seceda::edge {
namespace {

std::string trim_copy(const std::string & value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string to_lower_ascii_copy(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

}  // namespace

std::optional<LocalExecutionModeKind> parse_local_execution_mode_kind(const std::string & value) {
    const std::string trimmed = trim_copy(value);
    if (trimmed.empty()) {
        return LocalExecutionModeKind::kInProcess;
    }

    const std::string lowered = to_lower_ascii_copy(trimmed);
    if (lowered == "in_process" || lowered == "in-process") {
        return LocalExecutionModeKind::kInProcess;
    }
    if (lowered == "sdk_bridge" || lowered == "sdk-bridge") {
        return LocalExecutionModeKind::kSdkBridge;
    }
    if (lowered == "sidecar_server" || lowered == "sidecar-server" || lowered == "sidecar") {
        return LocalExecutionModeKind::kSidecarServer;
    }

    return std::nullopt;
}

const char * local_execution_mode_kind_to_contract_string(LocalExecutionModeKind kind) {
    switch (kind) {
        case LocalExecutionModeKind::kInProcess:
            return kLocalExecutionModeInProcess;
        case LocalExecutionModeKind::kSdkBridge:
            return kLocalExecutionModeSdkBridge;
        case LocalExecutionModeKind::kSidecarServer:
            return kLocalExecutionModeSidecarServer;
    }
    return kLocalExecutionModeInProcess;
}

std::string normalized_local_execution_mode_for_resolve(const std::string & raw) {
    const auto parsed = parse_local_execution_mode_kind(raw);
    if (parsed.has_value()) {
        return local_execution_mode_kind_to_contract_string(*parsed);
    }
    return trim_copy(raw);
}

}  // namespace seceda::edge
