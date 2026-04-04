#pragma once

#include "local_models/llama_runtime.hpp"
#include "local_models/stub_local_engine_adapter.hpp"
#include "runtime/interfaces.hpp"

#include <mutex>
#include <string>

namespace seceda::edge {

/// Selects and delegates to a local engine adapter (`LlamaRuntime` first; stubs for future engines).
class LocalEngineRegistry final : public ILocalModelRuntime {
public:
    LocalEngineRegistry();
    ~LocalEngineRegistry() override;

    LocalEngineRegistry(const LocalEngineRegistry &) = delete;
    LocalEngineRegistry & operator=(const LocalEngineRegistry &) = delete;

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
    ILocalModelRuntime * resolve_delegate(const LocalModelConfig & config, std::string & error);

    LlamaRuntime llama_;
    StubLocalEngineAdapter cactus_stub_;
    StubLocalEngineAdapter runanywhere_stub_;
    StubLocalEngineAdapter sidecar_stub_;

    mutable std::mutex mutex_;
    ILocalModelRuntime * delegate_ = nullptr;
    LocalModelConfig last_config_;
    std::string last_error_;
};

}  // namespace seceda::edge
