#pragma once

#include "runtime/interfaces.hpp"
#include "telemetry/metrics_registry.hpp"

namespace seceda::edge {

class RequestExecutor {
public:
    RequestExecutor(
        ILocalModelRuntime & local_runtime,
        ICloudClient & cloud_client,
        IRouter & router,
        MetricsRegistry & metrics);

    InferenceResponse execute(const InferenceRequest & request);

private:
    InferenceResponse execute_local(
        const InferenceRequest & request,
        InferenceResponse response,
        bool allow_cloud_fallback);
    InferenceResponse execute_cloud(
        const InferenceRequest & request,
        InferenceResponse response,
        bool allow_local_best_effort);

    ILocalModelRuntime & local_runtime_;
    ICloudClient & cloud_client_;
    IRouter & router_;
    MetricsRegistry & metrics_;
};

}  // namespace seceda::edge
