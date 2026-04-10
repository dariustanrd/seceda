#pragma once

#include "runtime/interfaces.hpp"

namespace seceda::edge {

class HeuristicRouter : public IRouter {
public:
    explicit HeuristicRouter(RouterConfig config);

    RouteDecision decide(const InferenceRequest & request) const override;
    RouterConfig config() const override;

private:
    static std::string to_lower_ascii(std::string value);
    static bool contains_any(
        const std::string & haystack,
        const std::vector<std::string> & needles,
        std::vector<std::string> & matched);
    static int estimate_token_count(const std::string & text);

    RouterConfig config_;
};

}  // namespace seceda::edge
