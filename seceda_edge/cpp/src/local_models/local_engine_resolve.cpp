#include "local_models/local_engine_resolve.hpp"

#include "local_models/local_execution_modes.hpp"

#include <algorithm>
#include <cctype>
#include <optional>

namespace seceda::edge {
namespace {

std::string to_lower_ascii_copy(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool icontains(const std::string & haystack, const char * needle_lower_ascii) {
    return to_lower_ascii_copy(haystack).find(needle_lower_ascii) != std::string::npos;
}

}  // namespace

ResolvedLocalEngine resolve_local_engine(const LocalModelConfig & config, std::string & error) {
    error.clear();

    const std::string mode_norm = normalized_local_execution_mode_for_resolve(config.execution_mode);
    const std::optional<LocalExecutionModeKind> mode = parse_local_execution_mode_kind(mode_norm);

    if ((mode.has_value() && *mode == LocalExecutionModeKind::kSidecarServer) ||
        mode_norm == kLocalExecutionModeSidecarServer) {
        return ResolvedLocalEngine::kSidecarServer;
    }

    if ((mode.has_value() && *mode == LocalExecutionModeKind::kSdkBridge) ||
        mode_norm == kLocalExecutionModeSdkBridge) {
        if (icontains(config.engine_id, "cactus")) {
            return ResolvedLocalEngine::kCactusSdkBridge;
        }
        if (icontains(config.engine_id, "runanywhere") || icontains(config.engine_id, "run_anywhere")) {
            return ResolvedLocalEngine::kRunAnywhereSdkBridge;
        }
        error =
            "No SDK bridge adapter is registered for engine_id=" + config.engine_id +
            "; expected a Cactus- or RunAnywhere-style engine id.";
        return ResolvedLocalEngine::kUnknown;
    }

    if (!mode.has_value() && !mode_norm.empty()) {
        error = "Unknown local execution_mode: " + config.execution_mode;
        return ResolvedLocalEngine::kUnknown;
    }

    // in_process (default): llama.cpp is the first native adapter.
    if (icontains(config.engine_id, "cactus") || icontains(config.engine_id, "runanywhere") ||
        icontains(config.engine_id, "run_anywhere")) {
        error =
            "Cactus and RunAnywhere are not in-process engines in this build; set execution_mode "
            "to sdk_bridge for those adapters.";
        return ResolvedLocalEngine::kUnknown;
    }

    return ResolvedLocalEngine::kLlamaInProcess;
}

}  // namespace seceda::edge
