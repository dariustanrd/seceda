#pragma once

#include "runtime/interfaces.hpp"

namespace seceda::edge {

class CloudClient : public ICloudClient {
public:
    explicit CloudClient(CloudConfig config);

    bool is_configured() const override;
    CloudClientInfo info() const override;
    CloudCompletionResult complete(const InferenceRequest & request) override;

private:
    CloudConfig config_;
};

}  // namespace seceda::edge
