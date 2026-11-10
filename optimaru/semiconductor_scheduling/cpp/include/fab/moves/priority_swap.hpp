#ifndef FAB_MOVES_PRIORITY_SWAP_HPP
#define FAB_MOVES_PRIORITY_SWAP_HPP

#include <random>
#include <vector>

#include "fab/domain.hpp"
#include "optimaru/search/operators.hpp"

namespace fab {

// ---------------------------------------------------------------------------
// PrioritySwapMove — exchange the decode priorities of two operations
// ---------------------------------------------------------------------------
// The search's second neighbour: swap the ranks of two rows and keep
// both machine choices (both slots). It prefers rows of different lots,
// whose ranks compete at the same decode steps; a row of the same lot is
// the fallback. A pair with equal ranks is no move.
//
// The built-in assignment swap is no substitute: it exchanges whole values, so the
// machine slot of one operation would land on another operation's option
// list.
class PrioritySwapMove final : public optimaru::MoveOperator<SemiconductorSchedule> {
public:
    const char* name() const override { return "prioritySwap"; }

    bool apply(optimaru::OperatorContext<SemiconductorSchedule>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        SemiconductorSchedule& s = ctx.editor.solution();
        const int n = s.rowCount();
        if (n < 2) return false;
        const int a = static_cast<int>(rng() % static_cast<std::uint64_t>(n));
        const int valueA = s.rows[static_cast<std::size_t>(a)].value;
        if (valueA < 0) return false;
        const int rankA = s.rankOf(valueA);
        int b = -1;
        for (int attempt = 0; attempt < 16; ++attempt) {
            int candidate = static_cast<int>(rng() % static_cast<std::uint64_t>(n - 1));
            if (candidate >= a) ++candidate;
            const int value = s.rows[static_cast<std::size_t>(candidate)].value;
            if (value < 0 || s.rankOf(value) == rankA) continue;
            b = candidate;
            if (s.rows[static_cast<std::size_t>(candidate)].lot !=
                s.rows[static_cast<std::size_t>(a)].lot)
                break;  // a different lot: the preferred partner
        }
        if (b < 0) return false;
        const int valueB = s.rows[static_cast<std::size_t>(b)].value;
        a_ = a;
        b_ = b;
        oldA_ = valueA;
        oldB_ = valueB;
        ctx.editor.begin_batch();
        ctx.editor.assign(a, s.valueOf(s.rankOf(valueB), s.slotOf(valueA)));
        ctx.editor.assign(b, s.valueOf(rankA, s.slotOf(valueB)));
        ctx.editor.end_batch();
        entities.push_back(a);
        entities.push_back(b);
        return true;
    }

    void undo_last(optimaru::OperatorContext<SemiconductorSchedule>& ctx) override {
        ctx.editor.begin_batch();
        ctx.editor.assign(a_, oldA_);
        ctx.editor.assign(b_, oldB_);
        ctx.editor.end_batch();
    }

private:
    int a_ = -1;
    int b_ = -1;
    int oldA_ = -1;
    int oldB_ = -1;
};

}  // namespace fab

#endif  // FAB_MOVES_PRIORITY_SWAP_HPP
