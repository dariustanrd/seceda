#pragma once

#include "runtime/interfaces.hpp"
#include "telemetry/trace_registry.hpp"

namespace seceda::edge {

class RequestTraceRecorder {
public:
    explicit RequestTraceRecorder(TraceRegistry & traces);

    void record_route_selected(
        const InferenceRequest & request,
        const RouteDecision & decision);
    void record_stream_started(const InferenceRequest & request);
    void record_stream_delta(
        const InferenceRequest & request,
        const StreamedChatDelta & delta);
    void record_response(
        const InferenceRequest & request,
        const InferenceResponse & response);

private:
    TraceRegistry & traces_;
};

}  // namespace seceda::edge
