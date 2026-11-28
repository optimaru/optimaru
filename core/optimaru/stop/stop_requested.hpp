// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_STOP_STOP_REQUESTED_HPP
#define OPTIMARU_STOP_STOP_REQUESTED_HPP

#include <atomic>

namespace optimaru {

// Stop once the caller asked (Solver::request_stop) or a run-scoped flag was
// raised (a multi-start run raises it when one start fails or reaches the
// target). Either flag may be null.
class StopRequested {
public:
    StopRequested(const std::atomic<bool>* requested, const std::atomic<bool>* run_stop)
        : requested_(requested), run_stop_(run_stop) {}

    bool reached() const {
        if (requested_ && requested_->load(std::memory_order_relaxed)) return true;
        return run_stop_ && run_stop_->load(std::memory_order_relaxed);
    }

private:
    const std::atomic<bool>* requested_;
    const std::atomic<bool>* run_stop_;
};

}  // namespace optimaru

#endif  // OPTIMARU_STOP_STOP_REQUESTED_HPP
