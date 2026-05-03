#pragma once

#include "runtime/interfaces.hpp"
#include "telemetry/metrics_registry.hpp"
#include "telemetry/trace_registry.hpp"

namespace seceda::edge {

class RequestExecutor {
public:
    RequestExecutor(
        ILocalModelRuntime & local_runtime,
        ICloudClient & cloud_client,
        IRouter & router,
        MetricsRegistry & metrics,
        TraceRegistry & traces);

    InferenceResponse execute(const InferenceRequest & request);
    InferenceResponse execute_streaming(
        const InferenceRequest & request,
        const StreamDeltaCallback & on_delta);

private:
    InferenceResponse execute_local(
        const InferenceRequest & request,
        InferenceResponse response,
        bool allow_cloud_fallback);
    InferenceResponse execute_cloud(
        const InferenceRequest & request,
        InferenceResponse response,
        bool allow_local_best_effort);
    InferenceResponse execute_local_streaming(
        const InferenceRequest & request,
        InferenceResponse response,
        bool allow_cloud_fallback,
        const StreamDeltaCallback & on_delta,
        bool & stream_started);
    InferenceResponse execute_cloud_streaming(
        const InferenceRequest & request,
        InferenceResponse response,
        bool allow_local_best_effort,
        const StreamDeltaCallback & on_delta,
        bool & stream_started);

    ILocalModelRuntime & local_runtime_;
    ICloudClient & cloud_client_;
    IRouter & router_;
    MetricsRegistry & metrics_;
    TraceRegistry & traces_;
};

}  // namespace seceda::edge
