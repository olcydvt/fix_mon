#pragma once
//
// Minimal Prometheus registry - counters and gauges, text exposition format.
//
// Deliberate constraint: labels are limited to session, direction, msg_type and
// reason. No ClOrdID, no symbol, no account. Per-order data belongs in the
// event store, not in Prometheus. A hard series cap is enforced so a
// misconfigured label can never take the scrape target down.
//
// Second constraint, added once sessions started being imported from the
// engine's own config: a label whose name looks like a credential is refused
// outright. The config reader already drops those values at parse time, so this
// is the backstop for code that has not been written yet. A scrape endpoint is
// usually unauthenticated and its contents end up in a time series database and
// on dashboards, which is the last place a password should land.
//
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace fixmon {

using Labels = std::vector<std::pair<std::string, std::string>>;

class Counter {
public:
    void inc(uint64_t n = 1) { value_.fetch_add(n, std::memory_order_relaxed); }

    // Mirrors a monotonic counter owned elsewhere (an adapter's line count, the
    // store's error count). Using inc() for those would double count, since the
    // source already accumulates. Only ever called with non-decreasing values.
    void set(uint64_t v) { value_.store(v, std::memory_order_relaxed); }

    uint64_t value() const { return value_.load(std::memory_order_relaxed); }
private:
    std::atomic<uint64_t> value_{0};
};

class Gauge {
public:
    void set(double v);
    double value() const;
private:
    std::atomic<uint64_t> bits_{0};  // double bit-punned, relaxed is fine here
};

class MetricRegistry {
public:
    explicit MetricRegistry(size_t max_series = 20000);

    void declare_counter(const std::string& name, const std::string& help);
    void declare_gauge(const std::string& name, const std::string& help);

    // Returns nullptr if the series cap is hit or a label name looks like a
    // credential; callers must tolerate that.
    Counter* counter(const std::string& name, const Labels& labels);
    Gauge*   gauge(const std::string& name, const Labels& labels);

    std::string expose() const;

    uint64_t series_count()   const;
    uint64_t rejected_series() const { return rejected_.load(); }

    // Series refused because a label named a secret. Non-zero means some code
    // path tried to publish one, which is a bug worth alerting on rather than a
    // condition to tolerate quietly.
    uint64_t redacted_series() const { return redacted_.load(); }

private:
    struct Family {
        std::string help;
        bool        is_gauge = false;
        std::map<std::string, std::unique_ptr<Counter>> counters;
        std::map<std::string, std::unique_ptr<Gauge>>   gauges;
    };

    static std::string encode_labels(const Labels&);

    // True when any label name is sensitive. Counts the refusal as a side
    // effect, so it shows up instead of disappearing.
    bool refuse_sensitive(const Labels&) const;

    mutable std::shared_mutex      mutex_;
    std::map<std::string, Family>  families_;
    size_t                         max_series_;
    std::atomic<uint64_t>          rejected_{0};
    mutable std::atomic<uint64_t>  redacted_{0};
};

// Declares every family this collector exports.
void declare_all_metrics(MetricRegistry&);

}  // namespace fixmon
