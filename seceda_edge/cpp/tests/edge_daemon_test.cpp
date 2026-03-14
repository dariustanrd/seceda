#include "runtime/edge_daemon.hpp"

#include <iostream>
#include <memory>

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
    bool load(const LocalModelConfig & config, const std::string &, std::string & error) override {
        if (!next_load_ok_) {
            error = "load failed";
            ready_ = false;
            return false;
        }
        ready_ = true;
        model_path_ = config.model_path;
        return true;
    }

    bool reload(const LocalModelConfig & config, const std::string &, std::string & error) override {
        if (!next_reload_ok_) {
            error = "reload failed";
            return false;
        }
        ready_ = true;
        model_path_ = config.model_path;
        return true;
    }

    bool is_ready() const override { return ready_; }

    LocalModelInfo info() const override {
        LocalModelInfo info;
        info.ready = ready_;
        info.model_path = model_path_;
        return info;
    }

    LocalCompletionResult generate(const InferenceRequest &) override {
        LocalCompletionResult result;
        result.ok = true;
        result.text = "local";
        result.active_model_path = model_path_;
        return result;
    }

    bool next_load_ok_ = true;
    bool next_reload_ok_ = true;
    bool ready_ = false;
    std::string model_path_;
};

class FakeCloudClient : public ICloudClient {
public:
    bool is_configured() const override { return configured_; }
    CloudClientInfo info() const override {
        CloudClientInfo info;
        info.configured = configured_;
        return info;
    }
    CloudCompletionResult complete(const InferenceRequest &) override {
        CloudCompletionResult result;
        result.ok = configured_;
        result.text = "cloud";
        return result;
    }

    bool configured_ = false;
};

class FakeRouter : public IRouter {
public:
    RouteDecision decide(const InferenceRequest &) const override {
        RouteDecision decision;
        decision.target = RouteTarget::kLocal;
        decision.reason = "local_default";
        return decision;
    }

    RouterConfig config() const override { return {}; }
};

}  // namespace

int main() {
    auto local = std::make_shared<FakeLocalRuntime>();
    auto cloud = std::make_shared<FakeCloudClient>();
    auto router = std::make_shared<FakeRouter>();

    DaemonConfig config;
    config.local.model_path = "initial.gguf";
    EdgeDaemon ready_daemon(config, local, cloud, router);
    if (!require(ready_daemon.initialize(), "daemon should initialize with a valid local model")) {
        return 1;
    }

    auto ready_health = ready_daemon.health();
    if (!require(ready_health.state == RuntimeState::kReady, "daemon should be ready after local load")) {
        return 1;
    }

    auto reload = ready_daemon.reload_model("next.gguf", "warmup");
    if (!require(reload.ok, "reload should succeed")) {
        return 1;
    }
    if (!require(reload.active_model_path == "next.gguf", "reload should update active model path")) {
        return 1;
    }

    auto degraded_local = std::make_shared<FakeLocalRuntime>();
    auto degraded_cloud = std::make_shared<FakeCloudClient>();
    degraded_cloud->configured_ = true;
    EdgeDaemon degraded_daemon(DaemonConfig{}, degraded_local, degraded_cloud, router);
    if (!require(degraded_daemon.initialize(), "daemon should initialize in degraded cloud-only mode")) {
        return 1;
    }
    if (!require(
            degraded_daemon.health().state == RuntimeState::kDegraded,
            "daemon should report degraded when no local model is configured")) {
        return 1;
    }

    auto failed_local = std::make_shared<FakeLocalRuntime>();
    failed_local->next_load_ok_ = false;
    DaemonConfig failed_config;
    failed_config.local.model_path = "broken.gguf";
    EdgeDaemon failed_daemon(failed_config, failed_local, std::make_shared<FakeCloudClient>(), router);
    if (!require(!failed_daemon.initialize(), "daemon should fail without local or cloud capability")) {
        return 1;
    }
    if (!require(
            failed_daemon.health().state == RuntimeState::kFailed,
            "daemon should report failed when startup cannot load local and no cloud is configured")) {
        return 1;
    }

    return 0;
}
