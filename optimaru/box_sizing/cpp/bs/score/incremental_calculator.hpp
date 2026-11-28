#ifndef BS_INCREMENTAL_CALCULATOR_HPP
#define BS_INCREMENTAL_CALCULATOR_HPP

#include <cstdint>
#include <vector>

#include "bs/model/domain.hpp"
#include "optimaru/score/objective.hpp"

namespace bs {

// The box-sizing implementation of the engine's optimaru::Objective: one
// hand-written incremental calculator for the default constraints.
//
// Every term is a sum over the parcels of a (parcel, box) fact, or a count of
// the boxes in use, so a single variable change is O(1):
//   - hard 1 (parcelFitCost), hard 2 (boxWeightCost), soft 4 (voidVolumeCost):
//     one lookup per change, kept as whole numbers;
//   - hard 3 (boxTypeLimitCost): the count of boxes in use moves when a box's
//     first parcel joins or its last one leaves (a required box always
//     counts);
//   - soft 5 (boxCost): the shipments per box are kept as whole numbers and
//     multiplied by the box's cost when the score is read.
// score() adds the soft terms up box by box, as ScoreCalculator::score does:
// a cost may be fractional, and a running sum of fractions next to the large
// volumes would drift from the full recompute. That read is O(boxes); it is
// O(1) when no box has a cost.
//
// before_assign retracts the parcel's contribution while `box` still holds
// the OLD value; after_assign inserts the NEW value. The full-recompute
// reference is ScoreCalculator::score (checked in the tests and under
// ScoreChecks::Full).
class BoxSizingIncrementalScoreCalculator final : public optimaru::Objective<BoxSizingProblem> {
public:
    void rebuild(BoxSizingProblem& solution) override;
    void before_assign(int parcelIndex) override;
    void after_assign(int parcelIndex) override;
    Score score() override;

private:
    BoxSizingProblem* problem_ = nullptr;
    std::vector<int> assign_;  // box per parcel (-1 == unassigned)

    // Per-box aggregates (same shapes as the full recompute's locals).
    std::vector<std::int64_t> voidOfBox_;
    std::vector<std::int64_t> quantityOfBox_;
    std::vector<int> parcelsOfBox_;  // entities, not quantities

    // Running totals (counted positive; signs and weights applied in score).
    std::int64_t fitExcess_ = 0;     // hard 1
    std::int64_t weightExcess_ = 0;  // hard 2
    std::int64_t boxTypes_ = 0;      // boxes in use, the required ones included
    std::int64_t void_ = 0;          // soft 4
    bool anyCost_ = false;           // some box has a cost: soft 5 is not 0

    void insert(int parcelIndex, int boxIndex);
    void retract(int parcelIndex, int boxIndex);
};

}  // namespace bs

#endif  // BS_INCREMENTAL_CALCULATOR_HPP
