#ifndef OPTIMARU_API_SCORE_CALCULATOR_COMPOSITE_INCREMENTAL_SCORE_CALCULATOR_HPP
#define OPTIMARU_API_SCORE_CALCULATOR_COMPOSITE_INCREMENTAL_SCORE_CALCULATOR_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/api/score/analysis/score_analysis.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// CompositeIncrementalScoreCalculator
// ---------------------------------------------------------------------------
// Composes independent constraint classes (CapacityConstraint,
// TimeWindowConstraint, ...) into one weighted score. Each
// component is a small IncrementalScoreCalculator that maintains ONE concern
// incrementally, and adding a new cost is writing a new component — not
// editing a monolithic calculator.
//
//   auto composite = optimaru::CompositeIncrementalScoreCalculator<MySolution>();
//   composite.add(std::make_unique<CapacityCost>(), "capacity");          // weight 1/1
//   composite.add(std::make_unique<OvertimeCost>(), "overtime", 2, 1.0);  // hard x2
//   solver.solve(solution, composite);
//
// Every hook is forwarded to every component; calculateScore() is the
// weighted sum. Component scores are weighted per level:
//   total.hard += hardWeight * component.hard
//   total.soft += softWeight * component.soft
// Components are owned by the composite.
template <typename Solution>
class CompositeIncrementalScoreCalculator final : public IncrementalScoreCalculator<Solution> {
public:
    void add(std::unique_ptr<IncrementalScoreCalculator<Solution>> component,
             std::string constraintName = "", std::int64_t hardWeight = 1,
             double softWeight = 1.0) {
        if (constraintName.empty()) {
            constraintName = "constraint " + std::to_string(components_.size());
        }
        components_.push_back(
                {std::move(component), std::move(constraintName), hardWeight, softWeight});
    }

    void resetWorkingSolution(Solution& workingSolution) override {
        for (auto& c : components_) c.calculator->resetWorkingSolution(workingSolution);
    }
    void beforeVariableChanged(int entityIndex) override {
        for (auto& c : components_) c.calculator->beforeVariableChanged(entityIndex);
    }
    void afterVariableChanged(int entityIndex) override {
        for (auto& c : components_) c.calculator->afterVariableChanged(entityIndex);
    }
    void beforeListChanged(int listIndex) override {
        for (auto& c : components_) c.calculator->beforeListChanged(listIndex);
    }
    void afterListChanged(int listIndex) override {
        for (auto& c : components_) c.calculator->afterListChanged(listIndex);
    }
    HardSoftScore calculateScore() override {
        HardSoftScore total;
        for (auto& c : components_) {
            HardSoftScore s = c.calculator->calculateScore();
            total.hard += c.hardWeight * s.hard;
            total.soft += c.softWeight * s.soft;
        }
        return total;
    }

    // Per-constraint breakdown of the current score (weighted), for
    // SolutionManager::analyze().
    ScoreAnalysis analyze() {
        ScoreAnalysis analysis;
        for (auto& c : components_) {
            HardSoftScore s = c.calculator->calculateScore();
            HardSoftScore weighted{c.hardWeight * s.hard, c.softWeight * s.soft};
            analysis.constraints.push_back({c.constraintName, weighted});
            analysis.score.hard += weighted.hard;
            analysis.score.soft += weighted.soft;
        }
        return analysis;
    }

private:
    struct Component {
        std::unique_ptr<IncrementalScoreCalculator<Solution>> calculator;
        std::string constraintName;
        std::int64_t hardWeight;
        double softWeight;
    };
    std::vector<Component> components_;
};

}  // namespace optimaru

#endif  // OPTIMARU_API_SCORE_CALCULATOR_COMPOSITE_INCREMENTAL_SCORE_CALCULATOR_HPP
