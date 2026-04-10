#pragma once

#include "runtime/contracts.hpp"

#include <functional>

namespace seceda::edge {

struct LocalCompletionResult {
    bool ok = false;
    std::string text;
    AssistantMessage message;
    std::string finish_reason = "stop";
    std::string error;
    TimingInfo timing;
    ExecutionTargetIdentity identity;
    std::string active_model_path;
};

struct CloudCompletionResult {
    bool ok = false;
    std::string text;
    AssistantMessage message;
    std::string finish_reason = "stop";
    std::string error;
    TimingInfo timing;
    ExecutionTargetIdentity identity;
};

struct StreamedChatDelta {
    std::string content;
    std::string tool_calls_json;
};

using StreamDeltaCallback = std::function<bool(const StreamedChatDelta &)>;

class ILocalModelRuntime {
public:
    virtual ~ILocalModelRuntime() = default;

    virtual bool load(
        const LocalModelConfig & config,
        const std::string & warmup_prompt,
        std::string & error) = 0;
    virtual bool reload(
        const LocalModelConfig & config,
        const std::string & warmup_prompt,
        std::string & error) = 0;
    virtual bool is_ready() const = 0;
    virtual LocalModelInfo info() const = 0;
    virtual LocalCompletionResult generate(const InferenceRequest & request) = 0;
    virtual LocalCompletionResult generate_stream(
        const InferenceRequest & request,
        const StreamDeltaCallback & on_delta) {
        (void)on_delta;
        return generate(request);
    }
};

class ICloudClient {
public:
    virtual ~ICloudClient() = default;

    virtual bool is_configured() const = 0;
    virtual CloudClientInfo info() const = 0;
    virtual CloudCompletionResult complete(const InferenceRequest & request) = 0;
    virtual CloudCompletionResult complete_stream(
        const InferenceRequest & request,
        const StreamDeltaCallback & on_delta) {
        (void)on_delta;
        return complete(request);
    }
};

class IRouter {
public:
    virtual ~IRouter() = default;

    virtual RouteDecision decide(const InferenceRequest & request) const = 0;
    virtual RouterConfig config() const = 0;
};

}  // namespace seceda::edge
