#ifndef BS_ACCEPT_REHEATING_LATE_ACCEPTANCE_HPP
#define BS_ACCEPT_REHEATING_LATE_ACCEPTANCE_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <random>
#include <vector>

#include "bs/model/domain.hpp"
#include "optimaru/accept/acceptor.hpp"

namespace bs {

// ---------------------------------------------------------------------------
// Late acceptance that starts over from just below its best
// ---------------------------------------------------------------------------
// Late acceptance keeps a candidate that is not worse than the current score
// or than the score `history` steps ago. It explores for as long as the
// history holds scores worse than the current one, which it does on the way
// down from a poor start. Box sizing starts from the greedy construction, a
// good solution: the history is that score from the first step, nothing worse
// is ever taken, and the search is a plain descent that ends in the first set
// of sizes no single close, open or swap improves.
//
// So when the search has gone two histories without an improving step, this
// acceptor refills its history with a score a little below the best one seen:
// the search may then walk through sets that are worse by up to that gap, and
// descends again as the history fills with what it finds. The gap is a
// fraction of the best soft score and goes round kGaps, so narrow and wide
// walks alternate. The hard level of the bar is the best one seen: a walk
// never trades feasibility for volume.
//
// Everything is counted in steps, so a seeded solve with a step limit is
// reproducible. The engine keeps the best solution, whatever the walks do.
class ReheatingLateAcceptance final : public optimaru::Acceptor {
public:
    static constexpr double kGaps[] = {0.005, 0.01, 0.02, 0.04, 0.08};

    ReheatingLateAcceptance(std::size_t historyLength, const Score& initial)
        : history_(std::max<std::size_t>(1, historyLength), initial),
          best_(initial),
          previous_(initial) {}

    bool accepts(const Score& candidate, const Score& current, const Score& /*best*/,
                 const int* /*entities*/, int /*entityCount*/, double /*progress*/,
                 std::mt19937_64& /*rng*/) override {
        return candidate >= current || candidate >= history_[position_];
    }

    void on_step_end(const Score& current) override {
        if (current > best_) best_ = current;
        idle_ = current > previous_ ? 0 : idle_ + 1;
        previous_ = current;
        history_[position_] = current;
        position_ = (position_ + 1) % history_.size();
        if (idle_ >= 2 * history_.size()) reheat();
    }

    // How often the history was refilled (for the tests).
    std::int64_t reheats() const { return reheats_; }

private:
    void reheat() {
        const double gap = kGaps[static_cast<std::size_t>(reheats_) % std::size(kGaps)];
        const Score bar{best_.hard, best_.soft - gap * std::fabs(best_.soft)};
        std::fill(history_.begin(), history_.end(), bar);
        idle_ = 0;
        ++reheats_;
    }

    std::vector<Score> history_;
    std::size_t position_ = 0;
    Score best_;
    Score previous_;
    std::size_t idle_ = 0;
    std::int64_t reheats_ = 0;
};

}  // namespace bs

#endif  // BS_ACCEPT_REHEATING_LATE_ACCEPTANCE_HPP
