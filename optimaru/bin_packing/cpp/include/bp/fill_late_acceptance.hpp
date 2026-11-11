#ifndef BP_FILL_LATE_ACCEPTANCE_HPP
#define BP_FILL_LATE_ACCEPTANCE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "bp/domain.hpp"
#include "optimaru/accept/acceptor.hpp"

namespace bp {

// ---------------------------------------------------------------------------
// FillLateAcceptance — late acceptance that tells equal scores apart
// ---------------------------------------------------------------------------
// usedBinsCost counts bins, so most steps of a search keep the score, and
// plain late acceptance takes every one of them: the packing drifts, while a
// bin closes only once the other bins have taken up its whole load.
//
// This is late acceptance with one more rule, for a candidate whose score
// equals the current one. The fill of a packing is the sum over its bins of
// the squared load (a load counts up to the capacity): of two packings with
// the same score, the one with the higher fill has its weight in fuller bins
// and the rest in lighter ones. A step that keeps the score is taken when it
// does not lower the fill, and otherwise one time in kLosingTieOdds, which is
// what lets the search take a full bin apart again. A step that changes the
// score is judged by late acceptance alone, so the fill never trades against
// the score. It is not part of the score either: the engine's best score, its
// limits and on_new_best see the score alone.
//
// The odds, measured with TradeMove on Falkenauer's triplets (three instances
// of 60, 120, 249 and 501 items, two seeds, 10 s; benchmarks/bin-packing,
// 2026-11-12), as runs that end one bin above the capacity bound, of 24:
//   never   12 (all six of 60 items, five of 120, one of 501)
//   1 in 2   5 (all of 501 items: the full bins do not hold)
//   1 in 4   0
//   1 in 8   0
//   1 in 16  1, 1 in 32  2, 1 in 128  8 (60 and 120 items)
//
// The acceptor keeps its own copy of the assignment and the loads. accepts()
// is called with the move in place and the items it moved: it brings the
// copy to the candidate, and takes that back when it refuses. Every n steps
// the copy is rebuilt from the solution, so a move that reports fewer items
// than it moved costs some misjudged ties and nothing else. The engine asks
// for a new acceptor after every kick, which starts from the solution as it
// then is.
//
// The fill is exact while n * capacity^2 < 2^62. Above that the loads are
// counted in units of 2^shift, the smallest shift that keeps the sum in an
// int64.
class FillLateAcceptance final : public optimaru::Acceptor {
public:
    static constexpr std::uint64_t kLosingTieOdds = 8;

    // `live` is the solution the engine searches in place; it outlives the
    // acceptor.
    FillLateAcceptance(std::size_t historyLength, const Score& initialScore,
                       const BinPackingProblem& live)
        : live_(live), history_(std::max<std::size_t>(historyLength, 1), initialScore) {
        const double n = static_cast<double>(std::max<std::size_t>(live.items.size(), 1));
        auto overflows = [&] {
            const double unit = static_cast<double>(live_.binCapacity >> shift_);
            return unit * unit * n >= 0x1p62;
        };
        while (overflows()) ++shift_;
        rebuild();
    }

    bool accepts(const Score& candidate, const Score& current, const Score& /*best*/,
                 const int* moveEntities, int moveEntityCount, double /*progress*/,
                 std::mt19937_64& rng) override {
        const std::int64_t before = fill_;
        moved_.clear();
        for (int m = 0; m < moveEntityCount; ++m) {
            const int item = moveEntities[m];
            const int now = live_.items[item].bin;
            if (now == bin_[item]) continue;
            moved_.push_back({item, bin_[item]});
            place(item, now);
        }
        const bool accepted = candidate == current
                                      ? fill_ >= before || rng() % kLosingTieOdds == 0
                                      : candidate > current || candidate >= history_[pos_];
        if (!accepted) {
            for (auto move = moved_.rbegin(); move != moved_.rend(); ++move)
                place(move->first, move->second);
        }
        return accepted;
    }

    void on_step_end(const Score& current) override {
        if (++stepsSinceRebuild_ >= bin_.size()) rebuild();
        history_[pos_] = current;
        pos_ = (pos_ + 1) % history_.size();
    }

    // The fill of the acceptor's copy of the packing.
    std::int64_t fill() const { return fill_; }

private:
    std::int64_t squared(std::int64_t load) const {
        const std::int64_t counted = std::min(load, live_.binCapacity) >> shift_;
        return counted * counted;
    }

    void add(int bin, std::int64_t weight) {
        fill_ -= squared(load_[bin]);
        load_[bin] += weight;
        fill_ += squared(load_[bin]);
    }

    // Moves the item in the copy (-1 = no bin).
    void place(int item, int bin) {
        const std::int64_t weight = live_.items[item].weight;
        if (bin_[item] >= 0) add(bin_[item], -weight);
        if (bin >= 0) add(bin, weight);
        bin_[item] = bin;
    }

    void rebuild() {
        const std::size_t n = live_.items.size();
        bin_.assign(n, -1);
        load_.assign(n, 0);
        fill_ = 0;
        for (std::size_t i = 0; i < n; ++i) place(static_cast<int>(i), live_.items[i].bin);
        stepsSinceRebuild_ = 0;
    }

    const BinPackingProblem& live_;
    std::vector<Score> history_;
    std::size_t pos_ = 0;
    int shift_ = 0;
    std::vector<int> bin_;            // per item, as of the last step
    std::vector<std::int64_t> load_;  // per bin
    std::int64_t fill_ = 0;
    std::size_t stepsSinceRebuild_ = 0;
    std::vector<std::pair<int, int>> moved_;  // (item, bin before) of the candidate
};

}  // namespace bp

#endif  // BP_FILL_LATE_ACCEPTANCE_HPP
