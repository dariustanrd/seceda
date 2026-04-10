#pragma once

#include "runtime/contracts.hpp"

#include <string>

namespace seceda::edge {

/// Which concrete local adapter the registry should delegate to for a given `LocalModelConfig`.
enum class ResolvedLocalEngine {
    kLlamaInProcess,
    kCactusSdkBridge,
    kRunAnywhereSdkBridge,
    kSidecarServer,
    kUnknown,
};

/// Selects an engine implementation. On failure, returns `kUnknown` and sets `error`.
ResolvedLocalEngine resolve_local_engine(const LocalModelConfig & config, std::string & error);

}  // namespace seceda::edge
