#include "openai_compat/openai_compat.hpp"

namespace seceda::edge::openai_compat {

int http_status_for_inference(const InferenceResponse & response) {
    if (response.ok) {
        return 200;
    }

    switch (response.error_kind) {
        case InferenceErrorKind::kInvalidRequest:
        case InferenceErrorKind::kUnsupportedFeature:
            return 400;
        case InferenceErrorKind::kLocalUnavailable:
        case InferenceErrorKind::kCloudUnavailable:
            return 503;
        case InferenceErrorKind::kLocalFailure:
        case InferenceErrorKind::kCloudFailure:
            return 502;
        case InferenceErrorKind::kNone:
            return 500;
    }

    return 500;
}

std::string openai_error_type(const InferenceResponse & response) {
    switch (response.error_kind) {
        case InferenceErrorKind::kInvalidRequest:
        case InferenceErrorKind::kUnsupportedFeature:
            return "invalid_request_error";
        case InferenceErrorKind::kLocalUnavailable:
        case InferenceErrorKind::kCloudUnavailable:
        case InferenceErrorKind::kLocalFailure:
        case InferenceErrorKind::kCloudFailure:
        case InferenceErrorKind::kNone:
            return "server_error";
    }

    return "server_error";
}

json openai_error_payload(
    const std::string & message,
    const std::string & type,
    const std::string & param,
    const std::string & code) {
    json error = {
        {"message", message},
        {"type", type},
        {"param", param.empty() ? json(nullptr) : json(param)},
        {"code", code.empty() ? json(nullptr) : json(code)},
    };
    return {{"error", std::move(error)}};
}

void set_openai_error(
    httplib::Response & response,
    int status,
    const std::string & message,
    const std::string & type,
    const std::string & param,
    const std::string & code) {
    response.status = status;
    response.set_content(openai_error_payload(message, type, param, code).dump(), "application/json");
}

}  // namespace seceda::edge::openai_compat
