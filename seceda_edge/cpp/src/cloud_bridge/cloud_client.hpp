#pragma once

#include "runtime/interfaces.hpp"

namespace seceda::edge {

class CloudClient : public ICloudClient {
public:
    explicit CloudClient(CloudConfig config, std::vector<CloudConfig> named_backends = {});

    bool is_configured() const override;
    CloudClientInfo info() const override;
    CloudCompletionResult complete(const InferenceRequest & request) override;
    CloudCompletionResult complete_stream(
        const InferenceRequest & request,
        const StreamDeltaCallback & on_delta) override;

private:
    const CloudConfig * default_backend() const;
    const CloudConfig * select_backend(const InferenceRequest & request) const;
    CloudCompletionResult complete_impl(
        const InferenceRequest & request,
        const StreamDeltaCallback * on_delta);

    CloudConfig config_;
    std::vector<CloudConfig> named_backends_;
};

}  // namespace seceda::edge
