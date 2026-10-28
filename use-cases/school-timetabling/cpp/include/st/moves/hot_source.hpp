#ifndef ST_MOVES_HOT_SOURCE_HPP
#define ST_MOVES_HOT_SOURCE_HPP

#include <vector>

#include "st/canonical_constraints.hpp"
#include "st/moves/reassign.hpp"

namespace st::canon
{

// Moved verbatim out of canonical_solver.cpp — one operator per file, the
// domain-side counterpart of the engine's generic/ruin layout.

// Violation guidance for the decomposed plugin path, shaped like the
// monolithic calculator's hot hooks. A hot slot is a unit whose weighted
// hard cost is currently > 0 (the contract behind the engine's
// penalizedUnits(): the per-slot conflicts H3/H4/H5/H6 in the standard
// assembly); a hot lesson is a uniformly random member of a random hot
// slot. Hard globals (H1, H7~H10) do not feed penalizedUnits() — they
// steer through the score itself.
class DecomposedHotSource
{
public:
    explicit DecomposedHotSource(
        const optimaru::DecomposedScoreCalculator<Timetable> *calculator)
        : calculator_(calculator)
    {
    }

    bool hasHotViolations() const
    {
        return !calculator_->penalizedUnits().empty();
    }

    int hotLesson(std::mt19937_64 &rng) const
    {
        const std::vector<int> &hot = calculator_->penalizedUnits();
        if (hot.empty())
            return -1;
        const std::vector<int> &members
            = calculator_->membersPerUnit()[hot[rng() % hot.size()]];
        if (members.empty())
            return -1;
        return members[rng() % members.size()];
    }

private:
    const optimaru::DecomposedScoreCalculator<Timetable> *calculator_;
};

// Does the assembly treat "unassigned" as a modelling choice? H1 demoted to
// soft is exactly the CP-SAT reference's WeeklyPeriodCounts(hard=false)
// contract: unmet periods become a (heavy) penalty instead of an infeasible
// model. An assembly without H1 at all says the same thing even louder — it
// puts no price on unassigned lessons.
bool allowsUnassigned(
    const optimaru::DecomposedScoreCalculator<Timetable> &calculator)
{
    for (const auto &global : calculator.globalConstraintList())
    {
        if (dynamic_cast<const UnassignedLesson *>(global.get()) != nullptr)
            return !global->isHard;
    }
    return true;
}

// Guided-search tables shared by both solve() overloads; the smart operator

}  // namespace st::canon

#endif  // ST_MOVES_HOT_SOURCE_HPP
