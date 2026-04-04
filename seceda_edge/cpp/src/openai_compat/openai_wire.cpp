#include "openai_compat/openai_compat.hpp"

#include <atomic>
#include <chrono>

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
    const auto chunk_base = [&](const json & delta, const json & finish_reason) {
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
    };

    std::string stream;
    stream += sse_event(chunk_base(json{{"role", "assistant"}}, nullptr));

    if (!response.message.content.empty()) {
        stream += sse_event(chunk_base(json{{"content", response.message.content}}, nullptr));
    }

    if (!response.message.tool_calls.empty()) {
        json tool_calls = json::array();
        for (const auto & tool_call : response.message.tool_calls) {
            tool_calls.push_back(tool_call_to_json(tool_call));
        }
        stream += sse_event(chunk_base(json{{"tool_calls", std::move(tool_calls)}}, nullptr));
    }

    stream += sse_event(
        chunk_base(
            json::object(),
            response.finish_reason.empty() ? json("stop") : json(response.finish_reason)));

    if (request.options.include_usage_in_stream) {
        stream += sse_event(
            json{
                {"id", completion_id},
                {"object", "chat.completion.chunk"},
                {"created", created_at},
                {"model", request.model},
                {"choices", json::array()},
                {"usage", usage_to_json(usage_timing_for_response(response))},
            });
    }

    stream += "data: [DONE]\n\n";
    return stream;
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
