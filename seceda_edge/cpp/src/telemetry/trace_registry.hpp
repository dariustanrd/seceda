#pragma once

#include "runtime/contracts.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace seceda::edge {

using TraceSubscriberCallback = std::function<void(const PromptTraceEvent &)>;

class TraceRegistry {
public:
    explicit TraceRegistry(std::size_t event_capacity = 8192);

    void push_event(PromptTraceEvent event);

    PromptTraceBatch get_events(
        std::uint64_t since_id,
        std::size_t limit,
        std::string request_id = {}) const;

    std::uint64_t subscribe(
        std::string request_id,
        TraceSubscriberCallback callback);

    void unsubscribe(std::uint64_t token);

private:
    struct Subscriber {
        std::string request_id;
        TraceSubscriberCallback callback;
    };

    std::string now_utc() const;
    std::uint64_t next_sequence_number_locked(const std::string & request_id);

    std::size_t event_capacity_ = 0;
    std::atomic<std::uint64_t> next_event_id_{1};
    std::atomic<std::uint64_t> next_subscriber_token_{1};

    mutable std::mutex events_mutex_;
    std::deque<PromptTraceEvent> events_;
    std::map<std::string, std::uint64_t> request_sequence_numbers_;

    mutable std::mutex subscribers_mutex_;
    std::map<std::uint64_t, Subscriber> subscribers_;
};

}  // namespace seceda::edge
