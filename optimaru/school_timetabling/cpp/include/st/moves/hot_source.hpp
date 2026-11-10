#ifndef ST_MOVES_HOT_SOURCE_HPP
#define ST_MOVES_HOT_SOURCE_HPP

#include <vector>

#include "st/canonical_constraints.hpp"
#include "st/moves/reassign.hpp"

namespace st::canon
{

// Moved verbatim out of canonical_solver.cpp — one operator per file, the
// domain-side counterpart of the engine's ruin_recreate/ layout.

// Violation guidance for the decomposed plugin path, shaped like the
// monolithic calculator's hot hooks. A hot slot is a unit whose weighted
// hard cost is currently > 0 (the contract behind the engine's
// penalized_units(): the per-slot class/teacher conflicts, disallowed slots
// and room capacity in the standard assembly); a hot lesson is a uniformly
// random member of a random hot slot. Hard globals (unassigned lessons, the
// course/teacher limits, block adjacency) do not feed penalized_units() —
// they steer through the score itself.
class DecomposedHotSource
{
public:
    explicit DecomposedHotSource(
        const optimaru::DecomposedObjective<Timetable> *calculator)
        : calculator_(calculator)
    {
    }

    bool hasHotViolations() const
    {
        return !calculator_->penalized_units().empty();
    }

    int hotLesson(std::mt19937_64 &rng) const
    {
        const std::vector<int> &hot = calculator_->penalized_units();
        if (hot.empty())
            return -1;
        const std::vector<int> &members
            = calculator_->members_per_unit()[hot[rng() % hot.size()]];
        if (members.empty())
            return -1;
        return members[rng() % members.size()];
    }

private:
    const optimaru::DecomposedObjective<Timetable> *calculator_;
};

// Does the assembly treat "unassigned" as a modelling choice?
// unassignedLessonsCost demoted to soft is exactly the CP-SAT reference's
// UnassignedLessonsCost(level='soft') contract: unmet periods become a (heavy)
// penalty instead of an infeasible model. An assembly without it at all
// says the same thing even louder — it puts no price on unassigned lessons.
bool allowsUnassigned(
    const optimaru::DecomposedObjective<Timetable> &calculator)
{
    // Every instance counts: a constraint picked several times may hold a
    // hard and a soft unassignedLessonsCost, and one hard instance is enough
    // to make every unassignment a hard loss.
    for (const auto &global : calculator.global_constraint_list())
    {
        if (dynamic_cast<const UnassignedLessonsCost *>(global.get())
                != nullptr
            && global->level == optimaru::Level::Hard)
        {
            return false;
        }
    }
    return true;
}

// Guided-search tables shared by both solve() overloads; the smart operator

}  // namespace st::canon

#endif  // ST_MOVES_HOT_SOURCE_HPP
