#include "bs/score/incremental_calculator.hpp"

#include <cstddef>
#include <cstdint>

#include "bs/constraints/box_cost.hpp"
#include "bs/constraints/box_type_limit_cost.hpp"
#include "bs/constraints/box_weight_cost.hpp"
#include "bs/constraints/parcel_fit_cost.hpp"
#include "bs/constraints/void_volume_cost.hpp"
#include "bs/score/score_calculator.hpp"

namespace bs {

namespace {

// The same constants the components and ScoreCalculator read (integral; see
// score_calculator.cpp).
constexpr auto kFitWeight = static_cast<std::int64_t>(ParcelFitCost::kDefaultWeight);
constexpr auto kWeightWeight = static_cast<std::int64_t>(BoxWeightCost::kDefaultWeight);
constexpr auto kLimitWeight = static_cast<std::int64_t>(BoxTypeLimitCost::kDefaultWeight);

}  // namespace

void BoxSizingIncrementalScoreCalculator::rebuild(BoxSizingProblem& solution) {
    ScoreCalculator::prepare(solution);
    problem_ = &solution;

    const std::size_t boxCount = solution.boxes.size();
    voidOfBox_.assign(boxCount, 0);
    quantityOfBox_.assign(boxCount, 0);
    parcelsOfBox_.assign(boxCount, 0);
    fitExcess_ = 0;
    weightExcess_ = 0;
    void_ = 0;
    boxTypes_ = 0;
    anyCost_ = false;
    for (const Box& box : solution.boxes) {
        if (box.required) ++boxTypes_;
        if (box.cost != 0.0) anyCost_ = true;
    }

    assign_.assign(solution.parcels.size(), -1);
    for (int p = 0; p < solution.parcelCount(); ++p) {
        const int box = solution.parcels[p].box;
        assign_[p] = box;
        if (box >= 0) insert(p, box);
    }
}

// Adds parcel `parcelIndex` to box `boxIndex`.
void BoxSizingIncrementalScoreCalculator::insert(int parcelIndex, int boxIndex) {
    fitExcess_ += problem_->fitExcess(parcelIndex, boxIndex);        // hard 1
    weightExcess_ += problem_->weightExcess(parcelIndex, boxIndex);  // hard 2
    // hard 3: the first parcel switches a box that is not required on.
    if (parcelsOfBox_[boxIndex]++ == 0 && !problem_->boxes[boxIndex].required) ++boxTypes_;
    const std::int64_t air = problem_->voidOf(parcelIndex, boxIndex);  // soft 4
    voidOfBox_[boxIndex] += air;
    void_ += air;
    quantityOfBox_[boxIndex] += problem_->parcels[parcelIndex].quantity;  // soft 5
}

// Exact mirror of insert.
void BoxSizingIncrementalScoreCalculator::retract(int parcelIndex, int boxIndex) {
    fitExcess_ -= problem_->fitExcess(parcelIndex, boxIndex);
    weightExcess_ -= problem_->weightExcess(parcelIndex, boxIndex);
    if (--parcelsOfBox_[boxIndex] == 0 && !problem_->boxes[boxIndex].required) --boxTypes_;
    const std::int64_t air = problem_->voidOf(parcelIndex, boxIndex);
    voidOfBox_[boxIndex] -= air;
    void_ -= air;
    quantityOfBox_[boxIndex] -= problem_->parcels[parcelIndex].quantity;
}

void BoxSizingIncrementalScoreCalculator::before_assign(int parcelIndex) {
    const int old = assign_[parcelIndex];
    if (old >= 0) retract(parcelIndex, old);  // unassigned: nothing to retract
    assign_[parcelIndex] = -1;
}

void BoxSizingIncrementalScoreCalculator::after_assign(int parcelIndex) {
    const int now = problem_->parcels[parcelIndex].box;  // the new value
    assign_[parcelIndex] = now;
    if (now >= 0) insert(parcelIndex, now);  // now unassigned: nothing to insert
}

Score BoxSizingIncrementalScoreCalculator::score() {
    const std::int64_t overLimit = problem_->maxBoxTypes && boxTypes_ > *problem_->maxBoxTypes
                                           ? boxTypes_ - *problem_->maxBoxTypes
                                           : 0;
    double soft = 0.0;
    if (anyCost_) {
        // The order of ScoreCalculator::score, so the doubles are the same.
        for (std::size_t b = 0; b < voidOfBox_.size(); ++b) {
            soft += VoidVolumeCost::kDefaultWeight * static_cast<double>(voidOfBox_[b]);
            soft += BoxCost::kDefaultWeight *
                    (static_cast<double>(quantityOfBox_[b]) * problem_->boxes[b].cost);
        }
    } else {
        // Whole numbers below 2^53 (prepare checks): any order gives this sum.
        soft = VoidVolumeCost::kDefaultWeight * static_cast<double>(void_);
    }
    Score score;
    score.hard =
            -(kFitWeight * fitExcess_ + kWeightWeight * weightExcess_ + kLimitWeight * overLimit);
    score.soft = soft == 0.0 ? 0.0 : -soft;
    return score;
}

}  // namespace bs
