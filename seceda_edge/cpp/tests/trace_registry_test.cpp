#include "telemetry/trace_registry.hpp"

#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace seceda::edge;

bool require(bool condition, const char * message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        return false;
    }
    return true;
}

}  // namespace

int main() {
    TraceRegistry traces(8);

    std::vector<PromptTraceEvent> subscribed;
    std::mutex subscribed_mutex;
    const std::uint64_t token = traces.subscribe(
        "chatcmpl-seceda-test-1",
        [&](const PromptTraceEvent & event) {
            std::lock_guard<std::mutex> lock(subscribed_mutex);
            subscribed.push_back(event);
        });

    PromptTraceEvent request_event;
    request_event.request_id = "chatcmpl-seceda-test-1";
    request_event.transport = "chat_completions";
    request_event.phase = "request_normalized";
    request_event.item_type = "request";
    traces.push_event(request_event);

    PromptTraceEvent delta_event;
    delta_event.request_id = "chatcmpl-seceda-test-1";
    delta_event.transport = "chat_completions";
    delta_event.phase = "stream_delta";
    delta_event.item_type = "output_text";
    delta_event.delta_text = "hello";
    traces.push_event(delta_event);

    PromptTraceEvent other_event;
    other_event.request_id = "chatcmpl-seceda-test-2";
    other_event.transport = "chat_completions";
    other_event.phase = "request_complete";
    other_event.item_type = "request";
    traces.push_event(other_event);

    traces.unsubscribe(token);

    const auto batch = traces.get_events(0, 8);
    if (!require(batch.events.size() == 3, "three trace events should be recorded")) {
        return 1;
    }
    if (!require(batch.events[0].sequence_number == 1, "first sequence number should start at one")) {
        return 1;
    }
    if (!require(batch.events[1].sequence_number == 2, "sequence number should advance per request")) {
        return 1;
    }
    if (!require(!batch.events[0].timestamp_utc.empty(), "trace timestamp should be assigned")) {
        return 1;
    }
    if (!require(batch.events[1].delta_text == "hello", "delta text should be recorded")) {
        return 1;
    }

    const auto filtered = traces.get_events(0, 8, "chatcmpl-seceda-test-1");
    if (!require(filtered.events.size() == 2, "request filter should keep matching trace events")) {
        return 1;
    }

    std::lock_guard<std::mutex> lock(subscribed_mutex);
    if (!require(subscribed.size() == 2, "subscriber should receive only matching request events")) {
        return 1;
    }
    if (!require(
            subscribed.front().phase == "request_normalized" &&
                subscribed.back().phase == "stream_delta",
            "subscriber should receive events in order")) {
        return 1;
    }

    return 0;
}
