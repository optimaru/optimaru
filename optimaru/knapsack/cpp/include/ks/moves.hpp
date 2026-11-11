#ifndef KS_MOVES_HPP
#define KS_MOVES_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

#include "ks/domain.hpp"
#include "optimaru/portable_random.hpp"
#include "optimaru/search/operators.hpp"

namespace ks {

// The domain's macro move, registered with the engine by Solver (see
// solver.cpp). Header-only so the tests can drive it through an Editor
// directly.

// ---------------------------------------------------------------------------
// CoreRepackMove — re-decide a window around the break item exactly
// ---------------------------------------------------------------------------
// Sorted by value per weight, a good selection takes the items above some
// break point and leaves those below it; what is left to decide sits in a
// narrow band around that point (the "core" of the knapsack literature). The
// built-in moves cannot finish the job there: at the capacity boundary an
// improving exchange is two items for one, one for two, or larger, and every
// single flip of it is worse or over capacity alone, while a ruin of random
// items almost never holds the right ones on a large instance.
//
// This operator takes a window of the density order around the break item
// (8 to 64 ranks to each side, a little off centre, both redrawn per move),
// frees the capacity its selected items hold, and fills that capacity again
// with the most valuable subset of the window's items: a small 0/1 knapsack,
// solved exactly by dynamic programming over the load. It is a move only when
// that subset is worth more than the one in place; the acceptor then judges
// the whole trade.
//
// The capacity is all it looks at. Resources, groups, dependencies, mandatory
// and forbidden items are judged by the score like everything else, so on an
// instance they shape the move is often refused and the built-in moves do the
// work as before. It declines when the table would be large (heavy items: a
// window is narrowed first), and when the selection is over capacity.
//
// One move costs a pass over the items plus the table, far more than a flip,
// so Solver gates how often a step may draw it (custom_move_probability).
class CoreRepackMove final : public optimaru::MoveOperator<KnapsackProblem> {
public:
    explicit CoreRepackMove(const KnapsackProblem& problem) {
        const int n = problem.itemCount();
        // The order of the greedy construction: descending value / weight,
        // ties in item order.
        std::vector<double> density(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            density[i] = static_cast<double>(problem.items[i].value) /
                         static_cast<double>(problem.items[i].weight);
        }
        order_.resize(static_cast<std::size_t>(n));
        std::iota(order_.begin(), order_.end(), 0);
        std::stable_sort(order_.begin(), order_.end(),
                         [&density](int a, int b) { return density[a] > density[b]; });
        // The first rank the capacity leaves out (the last rank when all fit).
        std::int64_t load = 0;
        breakRank_ = std::max(0, n - 1);
        for (int rank = 0; rank < n; ++rank) {
            load += problem.items[order_[rank]].weight;
            if (load > problem.capacity) {
                breakRank_ = rank;
                break;
            }
        }
    }

    const char* name() const override { return "coreRepack"; }

    bool apply(optimaru::OperatorContext<KnapsackProblem>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        const KnapsackProblem& problem = ctx.editor.solution();
        const int n = static_cast<int>(order_.size());
        if (n < 2) return false;

        std::int64_t load = 0;
        for (const Item& item : problem.items) {
            if (item.isSelected()) load += item.weight;
        }
        if (load > problem.capacity) return false;  // the built-in moves repair that first

        int reach = 8 << optimaru::random_index(rng, 4);  // 8, 16, 32 or 64
        const int centre = breakRank_ - reach / 2 + optimaru::random_index(rng, reach + 1);
        for (; reach >= 2; reach /= 2) {
            const int low = std::clamp(centre - reach, 0, n - 1);
            const int high = std::clamp(centre + reach, 0, n - 1);
            window_.assign(order_.begin() + low, order_.begin() + high + 1);
            // What the window may weigh: the slack plus what its selected
            // items hold now.
            std::int64_t room = problem.capacity - load;
            std::int64_t valueInPlace = 0;
            for (int item : window_) {
                if (problem.items[item].isSelected()) {
                    room += problem.items[item].weight;
                    valueInPlace += problem.items[item].value;
                }
            }
            if (static_cast<std::int64_t>(window_.size()) * (room + 1) > kMaxCells) continue;
            if (!repack(problem, room, valueInPlace)) return false;
            ctx.editor.begin_batch();
            for (int item : flipped_) ctx.editor.assign(item, flippedValue(problem, item));
            ctx.editor.end_batch();
            entities = flipped_;
            return true;
        }
        return false;
    }

    // A flip is its own inverse.
    void undo_last(optimaru::OperatorContext<KnapsackProblem>& ctx) override {
        const KnapsackProblem& problem = ctx.editor.solution();
        ctx.editor.begin_batch();
        for (int item : flipped_) ctx.editor.assign(item, flippedValue(problem, item));
        ctx.editor.end_batch();
    }

private:
    // The table of one move: window items x (room + 1) loads.
    static constexpr std::int64_t kMaxCells = 1'000'000;

    static int flippedValue(const KnapsackProblem& problem, int item) {
        return problem.items[item].isSelected() ? kNotSelected : kSelected;
    }

    // The most valuable subset of the window within `room`. Fills flipped_
    // with the items whose selection it changes; false when it is worth no
    // more than the subset in place.
    bool repack(const KnapsackProblem& problem, std::int64_t room, std::int64_t valueInPlace) {
        const std::size_t loads = static_cast<std::size_t>(room) + 1;
        best_.assign(loads, 0);
        taken_.assign(window_.size() * loads, 0);
        for (std::size_t k = 0; k < window_.size(); ++k) {
            const Item& item = problem.items[window_[k]];
            if (item.weight > room) continue;
            const std::size_t weight = static_cast<std::size_t>(item.weight);
            for (std::size_t at = loads - 1; at >= weight; --at) {
                const std::int64_t with = best_[at - weight] + item.value;
                if (with > best_[at]) {
                    best_[at] = with;
                    taken_[k * loads + at] = 1;
                }
                if (at == 0) break;
            }
        }
        if (best_[loads - 1] <= valueInPlace) return false;

        flipped_.clear();
        std::size_t at = loads - 1;
        for (std::size_t k = window_.size(); k-- > 0;) {
            const Item& item = problem.items[window_[k]];
            const bool take = taken_[k * loads + at] != 0;
            if (take) at -= static_cast<std::size_t>(item.weight);
            if (take != item.isSelected()) flipped_.push_back(window_[k]);
        }
        return !flipped_.empty();
    }

    std::vector<int> order_;  // item indices by descending density
    int breakRank_ = 0;

    // Scratch, reused across moves (no per-move allocation once grown).
    std::vector<int> window_;
    std::vector<std::int64_t> best_;
    std::vector<char> taken_;
    std::vector<int> flipped_;  // the last move's items
};

}  // namespace ks

#endif  // KS_MOVES_HPP
