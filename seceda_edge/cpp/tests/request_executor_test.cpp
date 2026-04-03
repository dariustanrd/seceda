#include "runtime/request_executor.hpp"

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

class FakeLocalRuntime : public ILocalModelRuntime {
public:
    bool load(const LocalModelConfig &, const std::string &, std::string &) override { return ready_; }
    bool reload(const LocalModelConfig &, const std::string &, std::string &) override { return ready_; }
    bool is_ready() const override { return ready_; }
    LocalModelInfo info() const override {
        LocalModelInfo info;
        info.ready = ready_;
        info.model_path = model_path_;
        return info;
    }
    LocalCompletionResult generate(const InferenceRequest &) override { return result_; }

    bool ready_ = true;
    std::string model_path_ = "fake.gguf";
    LocalCompletionResult result_;
};

class FakeCloudClient : public ICloudClient {
public:
    bool is_configured() const override { return configured_; }
    CloudClientInfo info() const override {
        CloudClientInfo info;
        info.configured = configured_;
        info.base_url = "https://example.test/v1";
        info.model = "fake-cloud";
        return info;
    }
    CloudCompletionResult complete(const InferenceRequest &) override { return result_; }

    bool configured_ = true;
    CloudCompletionResult result_;
};

class FakeRouter : public IRouter {
public:
    RouteDecision decide(const InferenceRequest &) const override { return decision_; }
    RouterConfig config() const override { return {}; }

    RouteDecision decision_;
};

}  // namespace

int main() {
    FakeLocalRuntime local;
    FakeCloudClient cloud;
    FakeRouter router;
    MetricsRegistry metrics(16);
    RequestExecutor executor(local, cloud, router, metrics);

    router.decision_.target = RouteTarget::kLocal;
    router.decision_.reason = "local_default";
    local.result_.ok = true;
    local.result_.text = "local answer";

    InferenceRequest request;
    request.messages.push_back({"user", "hello", {}, {}, {}});
    refresh_request_views(request);

    auto local_response = executor.execute(request);
    if (!require(local_response.ok, "local request should succeed")) {
        return 1;
    }
    if (!require(local_response.final_target == RouteTarget::kLocal, "local path should stay local")) {
        return 1;
    }

    local.result_.ok = false;
    local.result_.error = "local boom";
    cloud.result_.ok = true;
    cloud.result_.text = "cloud answer";

    auto fallback_response = executor.execute(request);
    if (!require(fallback_response.ok, "cloud fallback should recover local failure")) {
        return 1;
    }
    if (!require(
            fallback_response.final_target == RouteTarget::kCloud,
            "local failure should end on cloud")) {
        return 1;
    }
    if (!require(fallback_response.fallback_used, "fallback flag should be set")) {
        return 1;
    }

    router.decision_.target = RouteTarget::kCloud;
    router.decision_.reason = "complexity_hint";
    cloud.configured_ = false;
    local.result_.ok = true;
    local.result_.text = "best effort local";

    auto best_effort_response = executor.execute(request);
    if (!require(best_effort_response.ok, "best effort local should be used when cloud is absent")) {
        return 1;
    }
    if (!require(
            best_effort_response.initial_target == RouteTarget::kCloud &&
                best_effort_response.final_target == RouteTarget::kLocal,
            "best effort response should show cloud-to-local fallback")) {
        return 1;
    }

    request.seceda.route_override = RouteTarget::kLocal;
    local.ready_ = false;
    auto forced_local_response = executor.execute(request);
    if (!require(!forced_local_response.ok, "forced local should fail when local runtime is unavailable")) {
        return 1;
    }
    if (!require(
            forced_local_response.error_kind == InferenceErrorKind::kLocalUnavailable,
            "forced local should report local_unavailable")) {
        return 1;
    }

    return 0;
}
