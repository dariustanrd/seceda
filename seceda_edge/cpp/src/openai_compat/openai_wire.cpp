#include "openai_compat/openai_compat.hpp"
#include "runtime/edge_daemon.hpp"

#include <atomic>
#include <chrono>
#include <memory>

namespace seceda::edge::openai_compat {

namespace {

json tool_call_to_json(const ToolCall & tool_call) {
    return {
        {"id", tool_call.id},
        {"type", tool_call.type},
        {"function",
         {
             {"name", tool_call.function.name},
             {"arguments", tool_call.function.arguments_json},
         }},
    };
}

TimingInfo usage_timing_for_response(const InferenceResponse & response) {
    if (response.final_target == RouteTarget::kLocal && response.local_timing.has_value()) {
        return *response.local_timing;
    }
    if (response.final_target == RouteTarget::kCloud && response.cloud_timing.has_value()) {
        return *response.cloud_timing;
    }
    if (response.local_timing.has_value()) {
        return *response.local_timing;
    }
    if (response.cloud_timing.has_value()) {
        return *response.cloud_timing;
    }
    return response.total_timing;
}

json usage_to_json(const TimingInfo & timing) {
    return {
        {"prompt_tokens", timing.prompt_tokens},
        {"completion_tokens", timing.generated_tokens},
        {"total_tokens", total_token_count(timing)},
    };
}

std::string sse_event(const json & payload) {
    return "data: " + payload.dump() + "\n\n";
}

json chat_completion_chunk_payload(
    const InferenceRequest & request,
    const std::string & completion_id,
    std::int64_t created_at,
    const json & delta,
    const json & finish_reason) {
    json choices = json::array();
    choices.push_back(
        {
            {"index", 0},
            {"delta", delta},
            {"finish_reason", finish_reason},
        });

    return json{
        {"id", completion_id},
        {"object", "chat.completion.chunk"},
        {"created", created_at},
        {"model", request.model},
        {"choices", std::move(choices)},
    };
}

json tool_calls_delta_json(const AssistantMessage & message) {
    json tool_calls = json::array();
    for (const auto & tool_call : message.tool_calls) {
        tool_calls.push_back(tool_call_to_json(tool_call));
    }
    return tool_calls;
}

struct StreamingResponseState {
    EdgeDaemon * daemon = nullptr;
    InferenceRequest request;
    std::string completion_id;
    std::int64_t created_at = 0;
    std::function<bool()> is_connection_closed;
    bool started = false;
    bool emitted_role = false;
    bool client_cancelled = false;
    std::string stream_error;
};

bool stream_sink_open(const StreamingResponseState & state, httplib::DataSink & sink) {
    if (state.is_connection_closed && state.is_connection_closed()) {
        return false;
    }
    if (sink.is_writable && !sink.is_writable()) {
        return false;
    }
    return true;
}

bool write_stream_payload(
    StreamingResponseState & state,
    httplib::DataSink & sink,
    const std::string & payload) {
    if (!stream_sink_open(state, sink)) {
        state.client_cancelled = true;
        return false;
    }
    if (!sink.write(payload.data(), payload.size())) {
        state.client_cancelled = true;
        return false;
    }
    return true;
}

bool write_role_chunk_if_needed(StreamingResponseState & state, httplib::DataSink & sink) {
    if (state.emitted_role) {
        return true;
    }

    if (!write_stream_payload(
            state,
            sink,
            chat_completion_sse_delta(
                state.request,
                state.completion_id,
                state.created_at,
                json{{"role", "assistant"}}))) {
        return false;
    }

    state.emitted_role = true;
    return true;
}

}  // namespace

json assistant_message_json(const AssistantMessage & message) {
    json payload = {
        {"role", message.role.empty() ? "assistant" : message.role},
        {"content", message.content.empty() && !message.tool_calls.empty() ? json(nullptr)
                                                                        : json(message.content)},
    };

    if (!message.refusal.empty()) {
        payload["refusal"] = message.refusal;
    }

    if (!message.tool_calls.empty()) {
        payload["tool_calls"] = json::array();
        for (const auto & tool_call : message.tool_calls) {
            payload["tool_calls"].push_back(tool_call_to_json(tool_call));
        }
    }

    return payload;
}

std::string ensure_chat_completion_id(InferenceRequest & request) {
    if (request.seceda.request_id.empty()) {
        request.seceda.request_id = make_chat_completion_id();
    }
    return request.seceda.request_id;
}

json chat_completion_response(
    const InferenceRequest & request,
    const InferenceResponse & response,
    const std::string & completion_id,
    std::int64_t created_at) {
    json choices = json::array();
    choices.push_back(
        {
            {"index", 0},
            {"message", assistant_message_json(response.message)},
            {"finish_reason", response.finish_reason.empty() ? "stop" : response.finish_reason},
        });

    return {
        {"id", completion_id},
        {"object", "chat.completion"},
        {"created", created_at},
        {"model", request.model},
        {"choices", std::move(choices)},
        {"usage", usage_to_json(usage_timing_for_response(response))},
    };
}

std::string chat_completion_sse(
    const InferenceRequest & request,
    const InferenceResponse & response,
    const std::string & completion_id,
    std::int64_t created_at) {
    std::string stream;
    stream += chat_completion_sse_delta(request, completion_id, created_at, json{{"role", "assistant"}});

    if (!response.message.content.empty()) {
        stream += chat_completion_sse_delta(
            request,
            completion_id,
            created_at,
            json{{"content", response.message.content}});
    }

    if (!response.message.tool_calls.empty()) {
        stream += chat_completion_sse_delta(
            request,
            completion_id,
            created_at,
            json{{"tool_calls", tool_calls_delta_json(response.message)}});
    }

    stream += chat_completion_sse_finish(
        request,
        completion_id,
        created_at,
        response.finish_reason.empty() ? "stop" : response.finish_reason);

    if (request.options.include_usage_in_stream) {
        stream += chat_completion_sse_usage(request, response, completion_id, created_at);
    }

    stream += chat_completion_sse_done();
    return stream;
}

std::string chat_completion_sse_delta(
    const InferenceRequest & request,
    const std::string & completion_id,
    std::int64_t created_at,
    const json & delta) {
    return sse_event(chat_completion_chunk_payload(request, completion_id, created_at, delta, nullptr));
}

std::string chat_completion_sse_finish(
    const InferenceRequest & request,
    const std::string & completion_id,
    std::int64_t created_at,
    const std::string & finish_reason) {
    return sse_event(
        chat_completion_chunk_payload(
            request,
            completion_id,
            created_at,
            json::object(),
            finish_reason.empty() ? json("stop") : json(finish_reason)));
}

std::string chat_completion_sse_usage(
    const InferenceRequest & request,
    const InferenceResponse & response,
    const std::string & completion_id,
    std::int64_t created_at) {
    return sse_event(
        json{
            {"id", completion_id},
            {"object", "chat.completion.chunk"},
            {"created", created_at},
            {"model", request.model},
            {"choices", json::array()},
            {"usage", usage_to_json(usage_timing_for_response(response))},
        });
}

std::string chat_completion_sse_error(
    const std::string & message,
    const std::string & type,
    const std::string & param,
    const std::string & code) {
    return sse_event(openai_error_payload(message, type, param, code));
}

std::string chat_completion_sse_done() {
    return "data: [DONE]\n\n";
}

void set_streaming_chat_completion_response(
    httplib::Response & response,
    EdgeDaemon & daemon,
    InferenceRequest request,
    std::function<bool()> is_connection_closed) {
    response.status = 200;
    response.set_header("Cache-Control", "no-cache");

    auto state = std::make_shared<StreamingResponseState>();
    state->daemon = &daemon;
    state->request = std::move(request);
    state->completion_id = ensure_chat_completion_id(state->request);
    state->created_at = unix_timestamp_now();
    state->is_connection_closed =
        is_connection_closed ? std::move(is_connection_closed) : []() { return false; };

    response.set_chunked_content_provider(
        "text/event-stream",
        [state](size_t offset, httplib::DataSink & sink) {
            if (state->started || offset > 0) {
                sink.done();
                return true;
            }
            state->started = true;

            const auto stream_response = state->daemon->handle_inference_stream(
                state->request,
                [&](const StreamedChatDelta & delta) {
                    if (!delta.content.empty() || !delta.tool_calls_json.empty()) {
                        if (!write_role_chunk_if_needed(*state, sink)) {
                            return false;
                        }
                    }

                    if (!delta.content.empty() &&
                        !write_stream_payload(
                            *state,
                            sink,
                            chat_completion_sse_delta(
                                state->request,
                                state->completion_id,
                                state->created_at,
                                json{{"content", delta.content}}))) {
                        return false;
                    }

                    if (!delta.tool_calls_json.empty()) {
                        try {
                            if (!write_stream_payload(
                                    *state,
                                    sink,
                                    chat_completion_sse_delta(
                                        state->request,
                                        state->completion_id,
                                        state->created_at,
                                        json{{"tool_calls", json::parse(delta.tool_calls_json)}}))) {
                                return false;
                            }
                        } catch (const std::exception & exception) {
                            state->stream_error =
                                std::string("Failed to serialize streamed tool call delta: ") +
                                exception.what();
                            return false;
                        }
                    }

                    return true;
                });

            if (state->client_cancelled) {
                return false;
            }

            if (!state->stream_error.empty()) {
                if (!write_stream_payload(
                        *state,
                        sink,
                        chat_completion_sse_error(state->stream_error, "server_error")) ||
                    !write_stream_payload(*state, sink, chat_completion_sse_done())) {
                    return false;
                }
                sink.done();
                return true;
            }

            if (!stream_response.ok) {
                if (!write_stream_payload(
                        *state,
                        sink,
                        chat_completion_sse_error(
                            stream_response.error.empty() ? "Inference request failed"
                                                          : stream_response.error,
                            openai_error_type(stream_response))) ||
                    !write_stream_payload(*state, sink, chat_completion_sse_done())) {
                    return false;
                }
                sink.done();
                return true;
            }

            if (!state->emitted_role) {
                if (!write_role_chunk_if_needed(*state, sink)) {
                    return false;
                }

                if (!stream_response.message.content.empty() &&
                    !write_stream_payload(
                        *state,
                        sink,
                        chat_completion_sse_delta(
                            state->request,
                            state->completion_id,
                            state->created_at,
                            json{{"content", stream_response.message.content}}))) {
                    return false;
                }

                if (!stream_response.message.tool_calls.empty() &&
                    !write_stream_payload(
                        *state,
                        sink,
                        chat_completion_sse_delta(
                            state->request,
                            state->completion_id,
                            state->created_at,
                            json{{"tool_calls", tool_calls_delta_json(stream_response.message)}}))) {
                    return false;
                }
            }

            if (!write_stream_payload(
                    *state,
                    sink,
                    chat_completion_sse_finish(
                        state->request,
                        state->completion_id,
                        state->created_at,
                        stream_response.finish_reason.empty() ? "stop"
                                                             : stream_response.finish_reason))) {
                return false;
            }

            if (state->request.options.include_usage_in_stream &&
                !write_stream_payload(
                    *state,
                    sink,
                    chat_completion_sse_usage(
                        state->request,
                        stream_response,
                        state->completion_id,
                        state->created_at))) {
                return false;
            }

            if (!write_stream_payload(*state, sink, chat_completion_sse_done())) {
                return false;
            }

            sink.done();
            return true;
        },
        [state](bool) {});
}

std::int64_t unix_timestamp_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string make_chat_completion_id() {
    static std::atomic<std::uint64_t> next_id{1};
    return "chatcmpl-seceda-" + std::to_string(unix_timestamp_now()) + "-" +
        std::to_string(next_id.fetch_add(1));
}

}  // namespace seceda::edge::openai_compat
