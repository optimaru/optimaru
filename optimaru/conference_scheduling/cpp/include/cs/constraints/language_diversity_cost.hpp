#ifndef CS_CONSTRAINTS_LANGUAGE_DIVERSITY_COST_HPP
#define CS_CONSTRAINTS_LANGUAGE_DIVERSITY_COST_HPP

#include "cs/constraints/common.hpp"

namespace cs {

// Soft, weight 10, REWARD (raw cost <= 0; never hard, not flippable):
// languageDiversityCost — one timeslot offering talks in different languages.
// Raw cost (one global total) = the sum of -(the timeslot's duration) for each
// pair of talks in the same timeslot with different languages; 0 = no reward
// earned (no timeslot mixes languages).
struct LanguageDiversityCost final : BreakdownConstraint {
    static constexpr double kDefaultWeight = 10.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit LanguageDiversityCost(double weight = kDefaultWeight)
        : BreakdownConstraint("languageDiversityCost", weight, kDefaultLevel) {
        reward = true;  // raw cost <= 0; level='hard' is refused
        level_flippable = false;
    }
    std::int64_t select(const Breakdown& breakdown) const override {
        return -breakdown.languageDiversityCost;
    }
};

}  // namespace cs

#endif  // CS_CONSTRAINTS_LANGUAGE_DIVERSITY_COST_HPP
