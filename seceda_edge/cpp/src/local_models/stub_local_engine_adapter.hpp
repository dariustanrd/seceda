#pragma once

#include "runtime/interfaces.hpp"

#include <mutex>
#include <string>

namespace seceda::edge {

/// Placeholder adapter for engines that are wired in the registry but not implemented yet.
class StubLocalEngineAdapter final : public ILocalModelRuntime {
public:
    explicit StubLocalEngineAdapter(std::string not_implemented_detail);

    bool load(
        const LocalModelConfig & config,
        const std::string & warmup_prompt,
        std::string & error) override;
    bool reload(
        const LocalModelConfig & config,
        const std::string & warmup_prompt,
        std::string & error) override;
    bool is_ready() const override;
    LocalModelInfo info() const override;
    LocalCompletionResult generate(const InferenceRequest & request) override;

private:
    void store_config_identity(const LocalModelConfig & config);

    mutable std::mutex mutex_;
    std::string detail_;
    LocalModelConfig last_config_;
    std::string last_error_;
};

}  // namespace seceda::edge
