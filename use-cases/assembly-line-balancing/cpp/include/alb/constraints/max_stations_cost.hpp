#ifndef ALB_CONSTRAINTS_MAX_STATIONS_COST_HPP
#define ALB_CONSTRAINTS_MAX_STATIONS_COST_HPP

#include <algorithm>
#include <vector>

#include "alb/constraints/common.hpp"

namespace alb
{

// Hard, weight 1: maxStationsCost — tasks placed beyond the line's
// max_stations. A task on station s costs max(0, s - max_stations + 1) (how
// many stations past the last allowed one, 0-based: station max_stations
// costs 1); nothing when the problem sets no max_stations. Raw cost per
// station = that term times the number of its tasks; 0 = every task within
// the allowed stations.
//
// Scoped to the stations [max_stations, V): the term is 0 below them, so the
// calculator never visits this component elsewhere.
struct MaxStationsCost final : PerTaskConstraint
{
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit MaxStationsCost(double weight = kDefaultWeight)
        : PerTaskConstraint("maxStationsCost", weight, kDefaultLevel)
    {
    }
    void on_reset(const AssemblyLineBalancingProblem &problem) override
    {
        PerTaskConstraint::on_reset(problem);
        scoped_.clear();
        if (problem.maxStations)
        {
            for (int s = std::max(0, *problem.maxStations); s < problem.stationCount; ++s)
                scoped_.push_back(s);
        }
    }
    double taskCost(const AssemblyLineBalancingProblem &problem,
                    int station,
                    int /*task*/) const override
    {
        if (!problem.maxStations)
            return 0.0;
        const int beyond = station - *problem.maxStations + 1;
        return beyond > 0 ? static_cast<double>(beyond) : 0.0;
    }
    const std::vector<int> *scoped_units() const override { return &scoped_; }

private:
    std::vector<int> scoped_;
};

}  // namespace alb

#endif  // ALB_CONSTRAINTS_MAX_STATIONS_COST_HPP
