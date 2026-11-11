// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_ACCEPT_HISTORY_STAGES_HPP
#define OPTIMARU_ACCEPT_HISTORY_STAGES_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "optimaru/accept/acceptor.hpp"
#include "optimaru/accept/late_acceptance.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/settings.hpp"

namespace optimaru {

// Late acceptance in stages of growing history: see
// AcceptanceSettings::history_growth. The step loop tells it when the search
// has converged and when a new best is found, and asks it for the acceptors.
// Times are fractions of the solve (StopCondition::progress()).
class HistoryStages {
public:
    // A history holds one Score per step it remembers, so its length is
    // memory: a long budget on a small instance would otherwise double it
    // without end. At the cap a stage restarts with the same length, an
    // independent run of the search.
    static constexpr std::size_t kMaxHistory = std::size_t{1} << 20;

    HistoryStages(const Settings& config, bool custom_acceptor, const Score& start_score,
                  double now)
        : growth_(config.search.acceptance.history_growth),
          enabled_(growth_ > 1.0 && !custom_acceptor &&
                   config.search.acceptance.rule == AcceptanceRule::LateAcceptance),
          history_(
                  std::clamp<std::size_t>(config.search.acceptance.history_length, 1, kMaxHistory)),
          start_score_(start_score),
          stage_start_(now),
          last_best_(now) {}

    bool enabled() const { return enabled_; }
    std::size_t history() const { return history_; }
    // A stage has converged only when two histories have gone by without an
    // improving move. Until the search has found something better than its
    // start there is no stage to wait out: it then stalls when it would
    // without the stages.
    std::int64_t min_patience() const {
        return found_best_ ? 2 * static_cast<std::int64_t>(history_) : 0;
    }

    // Tells the stages that the search has converged, and returns whether
    // the next stage starts now. It records how long the stage that just
    // ended took (the first call: a stage of the configured length).
    //
    // A search that stalls before it has found anything better than its
    // start began at a local optimum: a warm start from a solved solution, or
    // a construction that no move improves. A stage would start over from the
    // start's score, which is where the search stands, and loosen nothing,
    // while it takes the place of the kick that is due. The stages end there
    // for the rest of the solve, and the search goes on as it does without
    // them.
    bool on_converged(double now, bool no_kicks) {
        if (!found_best_) {
            enabled_ = false;
            return false;
        }
        if (stage_cost_ < 0.0 || in_stage_) stage_cost_ = now - stage_start_;
        in_stage_ = false;
        const double next_cost =
                stage_cost_ * static_cast<double>(next_history()) / static_cast<double>(history_);
        const bool stalled = no_kicks || now - last_best_ >= 0.5 * next_cost;
        return stalled && now + next_cost <= 1.0;
    }

    std::unique_ptr<Acceptor> start_next(double now) {
        history_ = next_history();
        stage_start_ = now;
        in_stage_ = true;
        restarted_ = true;
        return acceptor(start_score_);
    }

    std::unique_ptr<Acceptor> acceptor(const Score& initial) const {
        return std::make_unique<LateAcceptance>(history_, initial);
    }

    void on_new_best(double now) {
        last_best_ = now;
        found_best_ = true;
    }

    // Whether a stage has started since the last call (true once per stage):
    // the first new best after a restart is that far from the one before
    // because of the restart. The step loop asks at a new best, so the flag
    // stays set until the next one, however many stages or kicks come in
    // between. A kick is not a restart: kicks are part of the rhythm of
    // stalls and new bests that the average is there to follow.
    bool take_restart() {
        const bool restarted = restarted_;
        restarted_ = false;
        return restarted;
    }

private:
    // Always longer than the current one below the cap, also for a factor
    // close to 1 on a short history.
    std::size_t next_history() const {
        const double grown = std::ceil(static_cast<double>(history_) * growth_);
        const std::size_t next = grown >= static_cast<double>(kMaxHistory)
                                         ? kMaxHistory
                                         : static_cast<std::size_t>(grown);
        return std::min(kMaxHistory, std::max(history_ + 1, next));
    }

    double growth_;
    bool enabled_;
    std::size_t history_;
    Score start_score_;
    double stage_start_;
    double stage_cost_ = -1.0;
    double last_best_;
    bool in_stage_ = false;
    bool restarted_ = false;
    bool found_best_ = false;  // a solution better than the start's
};

}  // namespace optimaru

#endif  // OPTIMARU_ACCEPT_HISTORY_STAGES_HPP
