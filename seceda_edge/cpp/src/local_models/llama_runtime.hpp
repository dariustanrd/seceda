#pragma once

#include "runtime/interfaces.hpp"

#include <memory>
#include <mutex>

namespace seceda::edge {

class LlamaRuntime : public ILocalModelRuntime {
public:
    LlamaRuntime();
    ~LlamaRuntime() override;

    LlamaRuntime(const LlamaRuntime &) = delete;
    LlamaRuntime & operator=(const LlamaRuntime &) = delete;

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
    LocalCompletionResult generate_stream(
        const InferenceRequest & request,
        const StreamDeltaCallback & on_delta) override;

private:
    struct Bundle;

    static bool backend_ready();
    static std::unique_ptr<Bundle> load_bundle(
        const LocalModelConfig & config,
        std::string & error);
    static LocalCompletionResult generate_with_bundle(
        Bundle & bundle,
        const InferenceRequest & request,
        bool warmup_mode,
        const StreamDeltaCallback * on_delta = nullptr);

    mutable std::mutex mutex_;
    std::unique_ptr<Bundle> bundle_;
    std::string last_error_;
};

}  // namespace seceda::edge
