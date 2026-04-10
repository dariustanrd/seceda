#pragma once

#include "cloud_bridge/cloud_client.hpp"
#include "runtime/interfaces.hpp"

#include <mutex>

namespace seceda::edge {

class SidecarLocalEngineAdapter final : public ILocalModelRuntime {
public:
    SidecarLocalEngineAdapter() = default;

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
    static CloudConfig sidecar_cloud_config(const LocalModelConfig & config);
    static std::vector<std::string> sidecar_capabilities(const LocalModelConfig & config);
    static LocalCompletionResult to_local_result(
        const LocalModelConfig & config,
        const CloudCompletionResult & cloud_result);
    static LocalModelInfo to_local_info(const LocalModelConfig & config, bool ready, const std::string & last_error);

    mutable std::mutex mutex_;
    LocalModelConfig config_;
    std::string last_error_;
    bool ready_ = false;
    CloudClient client_{CloudConfig{}};
};

}  // namespace seceda::edge
