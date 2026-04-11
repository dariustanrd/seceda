#include "telemetry/trace_registry.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

namespace seceda::edge {

TraceRegistry::TraceRegistry(std::size_t event_capacity)
    : event_capacity_(event_capacity == 0 ? 1 : event_capacity) {}

void TraceRegistry::push_event(PromptTraceEvent event) {
    if (event.request_id.empty()) {
        return;
    }

    event.id = next_event_id_.fetch_add(1);

    std::vector<TraceSubscriberCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        event.sequence_number = next_sequence_number_locked(event.request_id);
        if (event.timestamp_utc.empty()) {
            event.timestamp_utc = now_utc();
        }

        events_.push_back(event);
        while (events_.size() > event_capacity_) {
            events_.pop_front();
        }
    }

    {
        std::lock_guard<std::mutex> lock(subscribers_mutex_);
        for (const auto & [token, subscriber] : subscribers_) {
            (void)token;
            if (!subscriber.request_id.empty() && subscriber.request_id != event.request_id) {
                continue;
            }
            callbacks.push_back(subscriber.callback);
        }
    }

    for (const auto & callback : callbacks) {
        callback(event);
    }
}

PromptTraceBatch TraceRegistry::get_events(
    std::uint64_t since_id,
    std::size_t limit,
    std::string request_id) const {
    PromptTraceBatch batch;
    batch.since_id = since_id;
    limit = limit == 0 ? 1000 : limit;

    std::lock_guard<std::mutex> lock(events_mutex_);
    if (!events_.empty()) {
        batch.latest_id = events_.back().id;
    }

    for (const auto & event : events_) {
        if (event.id <= since_id) {
            continue;
        }
        if (!request_id.empty() && event.request_id != request_id) {
            continue;
        }

        batch.events.push_back(event);
        if (batch.events.size() >= limit) {
            break;
        }
    }

    return batch;
}

std::uint64_t TraceRegistry::subscribe(
    std::string request_id,
    TraceSubscriberCallback callback) {
    const std::uint64_t token = next_subscriber_token_.fetch_add(1);

    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_[token] = Subscriber{
        std::move(request_id),
        std::move(callback),
    };
    return token;
}

void TraceRegistry::unsubscribe(std::uint64_t token) {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    subscribers_.erase(token);
}

std::string TraceRegistry::now_utc() const {
    const std::time_t now = std::time(nullptr);
    std::tm utc_tm {};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &now);
#else
    gmtime_r(&now, &utc_tm);
#endif

    std::ostringstream out;
    out << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::uint64_t TraceRegistry::next_sequence_number_locked(const std::string & request_id) {
    auto & current = request_sequence_numbers_[request_id];
    current += 1;
    return current;
}

}  // namespace seceda::edge
