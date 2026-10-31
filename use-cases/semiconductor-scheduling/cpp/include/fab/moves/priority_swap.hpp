#ifndef FAB_MOVES_PRIORITY_SWAP_HPP
#define FAB_MOVES_PRIORITY_SWAP_HPP

#include <random>
#include <vector>

#include "fab/domain.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace fab
{

// ---------------------------------------------------------------------------
// PrioritySwapMove — exchange the decode priorities of two operations
// ---------------------------------------------------------------------------
// The 0.1.x search's second neighbour: swap the ranks of two rows and keep
// both machine choices (both slots). It prefers rows of different lots,
// whose ranks compete at the same decode steps; a row of the same lot is
// the fallback. A pair with equal ranks is no move.
//
// The built-in BASIC swap is no substitute: it exchanges whole values, so the
// machine slot of one operation would land on another operation's option
// list.
class PrioritySwapMove final : public optimaru::MoveOperator<SemiconductorSchedule>
{
public:
    const char *name() const override { return "prioritySwap"; }

    bool apply(optimaru::OperatorContext<SemiconductorSchedule> &ctx,
               std::mt19937_64 &rng,
               std::vector<int> &entities) override
    {
        SemiconductorSchedule &s = ctx.director.workingSolution();
        const int n = s.rowCount();
        if (n < 2)
            return false;
        const int a = static_cast<int>(rng() % static_cast<std::uint64_t>(n));
        const int valueA = s.rows[static_cast<std::size_t>(a)].value;
        if (valueA < 0)
            return false;
        const int rankA = s.rankOf(valueA);
        int b = -1;
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            int candidate = static_cast<int>(rng() % static_cast<std::uint64_t>(n - 1));
            if (candidate >= a)
                ++candidate;
            const int value = s.rows[static_cast<std::size_t>(candidate)].value;
            if (value < 0 || s.rankOf(value) == rankA)
                continue;
            b = candidate;
            if (s.rows[static_cast<std::size_t>(candidate)].lot != s.rows[static_cast<std::size_t>(a)].lot)
                break;  // a different lot: the preferred partner
        }
        if (b < 0)
            return false;
        const int valueB = s.rows[static_cast<std::size_t>(b)].value;
        a_ = a;
        b_ = b;
        oldA_ = valueA;
        oldB_ = valueB;
        ctx.director.beginVariableChanges();
        ctx.director.changeVariable(a, s.valueOf(s.rankOf(valueB), s.slotOf(valueA)));
        ctx.director.changeVariable(b, s.valueOf(rankA, s.slotOf(valueB)));
        ctx.director.endVariableChanges();
        entities.push_back(a);
        entities.push_back(b);
        return true;
    }

    void undoLast(optimaru::OperatorContext<SemiconductorSchedule> &ctx) override
    {
        ctx.director.beginVariableChanges();
        ctx.director.changeVariable(a_, oldA_);
        ctx.director.changeVariable(b_, oldB_);
        ctx.director.endVariableChanges();
    }

private:
    int a_ = -1;
    int b_ = -1;
    int oldA_ = -1;
    int oldB_ = -1;
};

}  // namespace fab

#endif  // FAB_MOVES_PRIORITY_SWAP_HPP
