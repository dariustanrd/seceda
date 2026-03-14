#include "router/heuristic_router.hpp"

#include <iostream>

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
    RouterConfig config;
    config.max_prompt_chars = 32;
    config.max_estimated_tokens = 8;
    config.structured_keywords = {"json", "schema"};
    config.cloud_keywords = {"plan", "compare"};
    config.freshness_keywords = {"today"};

    HeuristicRouter router(config);

    InferenceRequest local_request;
    local_request.text = "hello there";
    auto local_decision = router.decide(local_request);
    if (!require(local_decision.target == RouteTarget::kLocal, "short prompt should stay local")) {
        return 1;
    }

    InferenceRequest structured_request;
    structured_request.text = "return json with a schema";
    auto structured_decision = router.decide(structured_request);
    if (!require(
            structured_decision.target == RouteTarget::kCloud,
            "structured prompt should route to cloud")) {
        return 1;
    }

    InferenceRequest freshness_request;
    freshness_request.text = "what is the weather today";
    auto freshness_decision = router.decide(freshness_request);
    if (!require(
            freshness_decision.target == RouteTarget::kCloud,
            "freshness prompt should route to cloud")) {
        return 1;
    }

    InferenceRequest long_request;
    long_request.text = "this prompt is intentionally long enough to exceed the short max chars";
    auto long_decision = router.decide(long_request);
    if (!require(long_decision.target == RouteTarget::kCloud, "long prompt should route to cloud")) {
        return 1;
    }

    return 0;
}
