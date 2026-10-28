#ifndef OPTIMARU_IMPL_LOCALSEARCH_TERMINATION_THROTTLE_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_TERMINATION_THROTTLE_HPP

#include <cstdint>
#include <functional>

namespace optimaru {

// ---------------------------------------------------------------------------
// TerminationThrottle
// ---------------------------------------------------------------------------
// The step loop reads the termination between moves. A strategy that runs
// thousands of evaluations inside one move (the descent's route-pair scan,
// the iterated local search's descent, the ejection chain's repair) asks it
// through this instead: once every kInterval evaluations, so the wall-clock
// read stays off the per-evaluation path, and sticky once it answered true.
// `stop` is OperatorContext::stopRequested, which may be null (operators
// driven by hand); then the throttle never stops.
struct TerminationThrottle {
    // ~1 ms of O(1) previews, a few ms of O(route) ones.
    static constexpr std::int64_t kInterval = 1024;

    void reset(const std::function<bool()>* stopRequested) {
        stop_ = stopRequested;
        next_ = kInterval;
        stopped_ = false;
    }

    // `evaluations` is the caller's running count since reset().
    bool reached(std::int64_t evaluations) {
        if (!stopped_ && stop_ != nullptr && evaluations >= next_) {
            next_ = evaluations + kInterval;
            stopped_ = (*stop_)();
        }
        return stopped_;
    }
    bool stopped() const { return stopped_; }

private:
    const std::function<bool()>* stop_ = nullptr;
    std::int64_t next_ = kInterval;
    bool stopped_ = false;
};

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_TERMINATION_THROTTLE_HPP
