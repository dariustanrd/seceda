#pragma once

#include "runtime/contracts.hpp"

#include <cpp-httplib/httplib.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace seceda::edge {
class EdgeDaemon;
}

namespace seceda::edge::openai_compat {

using json = nlohmann::json;

std::vector<ModelCatalogEntry> configured_model_catalog(const DaemonConfig & config);

json model_catalog_entry_json(const ModelCatalogEntry & entry);

json models_list_payload(const DaemonConfig & config);

bool parse_chat_completion_request(
    const std::string & body,
    const DaemonConfig & config,
    InferenceRequest & request,
    std::string & error);

/// Shared by OpenAI chat requests and the deprecated Seceda `/inference` bridge `options` object.
bool read_completion_token_limit(
    const json & payload,
    int & out,
    std::string & error);

json assistant_message_json(const AssistantMessage & message);

json chat_completion_response(
    const InferenceRequest & request,
    const InferenceResponse & response,
    const std::string & completion_id,
    std::int64_t created_at);

std::string chat_completion_sse(
    const InferenceRequest & request,
    const InferenceResponse & response,
    const std::string & completion_id,
    std::int64_t created_at);

std::string chat_completion_sse_delta(
    const InferenceRequest & request,
    const std::string & completion_id,
    std::int64_t created_at,
    const json & delta);

std::string chat_completion_sse_finish(
    const InferenceRequest & request,
    const std::string & completion_id,
    std::int64_t created_at,
    const std::string & finish_reason);

std::string chat_completion_sse_usage(
    const InferenceRequest & request,
    const InferenceResponse & response,
    const std::string & completion_id,
    std::int64_t created_at);

std::string chat_completion_sse_error(
    const std::string & message,
    const std::string & type,
    const std::string & param = {},
    const std::string & code = {});

std::string chat_completion_sse_done();

void set_streaming_chat_completion_response(
    httplib::Response & response,
    EdgeDaemon & daemon,
    InferenceRequest request,
    std::function<bool()> is_connection_closed = {});

std::string make_chat_completion_id();

std::int64_t unix_timestamp_now();

int http_status_for_inference(const InferenceResponse & response);

std::string openai_error_type(const InferenceResponse & response);

json openai_error_payload(
    const std::string & message,
    const std::string & type,
    const std::string & param = {},
    const std::string & code = {});

void set_openai_error(
    httplib::Response & response,
    int status,
    const std::string & message,
    const std::string & type,
    const std::string & param = {},
    const std::string & code = {});

}  // namespace seceda::edge::openai_compat
