#include "local_models/local_execution_modes.hpp"
#include "text_utils/normalize.hpp"

namespace seceda::edge {

std::optional<LocalExecutionModeKind> parse_local_execution_mode_kind(const std::string & value) {
    const std::string trimmed = text_utils::trim_copy(value);
    if (trimmed.empty()) {
        return LocalExecutionModeKind::kInProcess;
    }

    const std::string lowered = text_utils::to_lower_ascii_copy(trimmed);
    if (lowered == "in_process" || lowered == "in-process") {
        return LocalExecutionModeKind::kInProcess;
    }
    if (lowered == "sdk_bridge" || lowered == "sdk-bridge") {
        return LocalExecutionModeKind::kSdkBridge;
    }
    if (is_sidecar_execution_mode(lowered)) {
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
    return text_utils::trim_copy(raw);
}

}  // namespace seceda::edge
