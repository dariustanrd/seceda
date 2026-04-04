#pragma once

#include "runtime/interfaces.hpp"
#include "runtime/request_executor.hpp"

#include <memory>
#include <mutex>

namespace seceda::edge {

class EdgeDaemon {
public:
    EdgeDaemon(
        DaemonConfig config,
        std::shared_ptr<ILocalModelRuntime> local_runtime,
        std::shared_ptr<ICloudClient> cloud_client,
        std::shared_ptr<IRouter> router);

    bool initialize();
    InferenceResponse handle_inference(InferenceRequest request);
    InferenceResponse handle_inference_stream(
        InferenceRequest request,
        const StreamDeltaCallback & on_delta);
    ModelReloadResult reload_model(const std::string & model_path, const std::string & warmup_prompt);

    HealthSnapshot health() const;
    InfoSnapshot info() const;
    std::string metrics_text() const;
    EventBatch events(std::uint64_t since_id, std::size_t limit) const;

private:
    void update_state_locked(bool local_ready);

    mutable std::mutex mutex_;
    DaemonConfig config_;
    std::shared_ptr<ILocalModelRuntime> local_runtime_;
    std::shared_ptr<ICloudClient> cloud_client_;
    std::shared_ptr<IRouter> router_;
    MetricsRegistry metrics_;
    RequestExecutor executor_;
    RuntimeState state_ = RuntimeState::kStarting;
    std::string last_error_;
};

}  // namespace seceda::edge
