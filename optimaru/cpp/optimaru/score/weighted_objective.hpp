// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SCORE_WEIGHTED_OBJECTIVE_HPP
#define OPTIMARU_SCORE_WEIGHTED_OBJECTIVE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/score/breakdown.hpp"
#include "optimaru/score/objective.hpp"
#include "optimaru/score/score.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// WeightedObjective
// ---------------------------------------------------------------------------
// Sums independent objectives (CapacityCost, TimeWindowCost, ...) into one
// weighted score. Each component is a small Objective that maintains ONE
// concern incrementally, so adding a cost means writing a component rather
// than editing a monolithic objective.
//
//   auto composite = optimaru::WeightedObjective<MySolution>();
//   composite.add(std::make_unique<CapacityCost>(), "capacity");          // weight 1/1
//   composite.add(std::make_unique<OvertimeCost>(), "overtime", 2, 1.0);  // hard x2
//   solver.solve(solution, composite);
//
// Every hook is forwarded to every component; score() is the
// weighted sum. Component scores are weighted per level:
//   total.hard += hard_weight * component.hard
//   total.soft += soft_weight * component.soft
// Components are owned by the composite.
template <typename Solution>
class WeightedObjective final : public Objective<Solution> {
public:
    void add(std::unique_ptr<Objective<Solution>> component, std::string name = "",
             std::int64_t hard_weight = 1, double soft_weight = 1.0) {
        if (name.empty()) {
            name = "constraint " + std::to_string(components_.size());
        }
        components_.push_back({std::move(component), std::move(name), hard_weight, soft_weight});
    }

    void rebuild(Solution& solution) override {
        for (auto& c : components_) c.objective->rebuild(solution);
    }
    void before_assign(int entity_index) override {
        for (auto& c : components_) c.objective->before_assign(entity_index);
    }
    void after_assign(int entity_index) override {
        for (auto& c : components_) c.objective->after_assign(entity_index);
    }
    void before_list_edit(int list_index) override {
        for (auto& c : components_) c.objective->before_list_edit(list_index);
    }
    void after_list_edit(int list_index) override {
        for (auto& c : components_) c.objective->after_list_edit(list_index);
    }
    Score score() override {
        Score total;
        for (auto& c : components_) {
            Score s = c.objective->score();
            total.hard += c.hard_weight * s.hard;
            total.soft += c.soft_weight * s.soft;
        }
        return total;
    }

    // The current score split per component (weighted); see explain_score().
    ScoreBreakdown breakdown() {
        ScoreBreakdown analysis;
        for (auto& c : components_) {
            Score s = c.objective->score();
            Score weighted{c.hard_weight * s.hard, c.soft_weight * s.soft};
            analysis.constraints.push_back({c.name, weighted});
            analysis.score.hard += weighted.hard;
            analysis.score.soft += weighted.soft;
        }
        return analysis;
    }

private:
    struct Component {
        std::unique_ptr<Objective<Solution>> objective;
        std::string name;
        std::int64_t hard_weight;
        double soft_weight;
    };
    std::vector<Component> components_;
};

}  // namespace optimaru

#endif  // OPTIMARU_SCORE_WEIGHTED_OBJECTIVE_HPP
