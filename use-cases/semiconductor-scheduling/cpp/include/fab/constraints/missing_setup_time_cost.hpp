#ifndef FAB_CONSTRAINTS_MISSING_SETUP_TIME_COST_HPP
#define FAB_CONSTRAINTS_MISSING_SETUP_TIME_COST_HPP

#include "fab/constraints/common.hpp"

namespace fab
{

// Hard, weight 1: missingSetupTimeCost — sequence-dependent setup time that
// is missing between consecutive runs on a machine. Raw cost per machine:
// for each consecutive pair (p, c) in machine order, skipping same-family
// pairs on a batch tool, with s = the setup time of (machine group, p's
// family, c's family) (the last setup_times entry wins, none = 0) and s > 0:
// max(0, p.end + s - c.start). 0 = every required setup fits.
struct MissingSetupTimeCost final : ScheduleViewConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;
    static constexpr Term kTerm = Term::MissingSetupTimeCost;

    explicit MissingSetupTimeCost(double weight = kDefaultWeight)
        : ScheduleViewConstraint("missingSetupTimeCost", weight, kDefaultLevel, kTerm)
    {
    }

    static std::int64_t machineCost(const SemiconductorSchedule &schedule, int machine,
                                    const PlacementList &rows,
                                    std::vector<std::int64_t> *items)
    {
        const Machine &m = schedule.machines[static_cast<std::size_t>(machine)];
        std::int64_t total = 0;
        for (std::size_t i = 1; i < rows.size(); ++i)
        {
            const ScorePlacement *previous = rows[i - 1];
            const ScorePlacement *current = rows[i];
            const int from = terms::familyOf(schedule, previous);
            const int to = terms::familyOf(schedule, current);
            if (m.isBatch() && from == to)
                continue;
            const Time required = schedule.setupTime(m.groupIndex, from, to);
            if (required != 0)
            {
                const Time missing = checked::sub(checked::add(previous->end, required), current->start);
                total = checked::add(total, terms::emit(items, std::max<Time>(0, missing)));
            }
        }
        return total;
    }
};

}  // namespace fab

#endif  // FAB_CONSTRAINTS_MISSING_SETUP_TIME_COST_HPP
