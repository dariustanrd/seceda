#pragma once

#include "runtime/interfaces.hpp"

namespace seceda::edge {

class CloudClient : public ICloudClient {
public:
    explicit CloudClient(CloudConfig config);

    bool is_configured() const override;
    CloudClientInfo info() const override;
    CloudCompletionResult complete(const InferenceRequest & request) override;
    CloudCompletionResult complete_stream(
        const InferenceRequest & request,
        const StreamDeltaCallback & on_delta) override;

private:
    CloudCompletionResult complete_impl(
        const InferenceRequest & request,
        const StreamDeltaCallback * on_delta);

    CloudConfig config_;
};

}  // namespace seceda::edge
