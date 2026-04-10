#include "router/heuristic_router.hpp"
#include "text_utils/normalize.hpp"

#include <cmath>
#include <sstream>

namespace seceda::edge {

HeuristicRouter::HeuristicRouter(RouterConfig config) : config_(std::move(config)) {}

RouteDecision HeuristicRouter::decide(const InferenceRequest & request) const {
    RouteDecision decision;
    decision.target = RouteTarget::kLocal;
    decision.preferred_engine_id = request.seceda.preferred_engine_id;
    decision.resolved_backend_id = request.seceda.preferred_backend_id;
    decision.resolved_model_alias = request.seceda.preferred_model_alias;

    const std::string lowered = text_utils::to_lower_ascii_copy(request.normalized.routing_prompt);
    decision.estimated_tokens = estimate_token_count(lowered);

    if (request.capabilities.requires_remote_backend) {
        decision.target = RouteTarget::kCloud;
        decision.reason = "remote_capability_required";
        if (request.capabilities.has_tools) {
            decision.matched_rules.push_back("tools");
        }
        if (request.capabilities.requests_tool_choice) {
            decision.matched_rules.push_back("tool_choice");
        }
        if (request.capabilities.requests_structured_output) {
            decision.matched_rules.push_back("response_format");
        }
        return decision;
    }

    if (request.normalized.routing_prompt.size() > config_.max_prompt_chars) {
        decision.target = RouteTarget::kCloud;
        decision.reason = "prompt_too_long";
        decision.matched_rules.push_back("max_prompt_chars");
        return decision;
    }

    if (decision.estimated_tokens > config_.max_estimated_tokens) {
        decision.target = RouteTarget::kCloud;
        decision.reason = "estimated_tokens_exceeded";
        decision.matched_rules.push_back("max_estimated_tokens");
        return decision;
    }

    if (contains_any(lowered, config_.structured_keywords, decision.matched_rules)) {
        decision.target = RouteTarget::kCloud;
        decision.reason = "structured_output_hint";
        return decision;
    }

    if (contains_any(lowered, config_.freshness_keywords, decision.matched_rules)) {
        decision.target = RouteTarget::kCloud;
        decision.reason = "freshness_hint";
        return decision;
    }

    if (contains_any(lowered, config_.cloud_keywords, decision.matched_rules)) {
        decision.target = RouteTarget::kCloud;
        decision.reason = "complexity_hint";
        return decision;
    }

    decision.reason = "local_default";
    return decision;
}

RouterConfig HeuristicRouter::config() const {
    return config_;
}

bool HeuristicRouter::contains_any(
    const std::string & haystack,
    const std::vector<std::string> & needles,
    std::vector<std::string> & matched) {
    for (const auto & needle : needles) {
        if (needle.empty()) {
            continue;
        }
        if (haystack.find(text_utils::to_lower_ascii_copy(needle)) != std::string::npos) {
            matched.push_back(needle);
        }
    }

    return !matched.empty();
}

int HeuristicRouter::estimate_token_count(const std::string & text) {
    std::istringstream stream(text);
    std::string token;
    int word_count = 0;
    while (stream >> token) {
        ++word_count;
    }

    const int char_estimate = static_cast<int>(std::ceil(static_cast<double>(text.size()) / 4.0));
    return std::max(word_count, char_estimate);
}

}  // namespace seceda::edge
