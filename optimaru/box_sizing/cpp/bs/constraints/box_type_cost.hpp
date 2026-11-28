#ifndef BS_CONSTRAINTS_BOX_TYPE_COST_HPP
#define BS_CONSTRAINTS_BOX_TYPE_COST_HPP

#include "bs/constraints/common.hpp"

namespace bs {

// Soft, weight 0 (opt-in): boxTypeCost -- the number of distinct boxes in use
// (those a parcel ships in and the required ones), one global value. It
// scores nothing until a caller picks it with a weight, and that weight is the
// fixed cost of stocking one more size, in the units of the soft level: the
// solve then stops adding a size once it saves less than that.
//
// A GlobalConstraint so that its exponent shapes the COUNT (count ** e), and
// because a required box counts without a member.
struct BoxTypeCost final : detail::BoxTypeCount {
    static constexpr double kDefaultWeight = 0.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Soft;

    explicit BoxTypeCost(double weight = kDefaultWeight)
        : BoxTypeCount("boxTypeCost", weight, kDefaultLevel) {}
    double cost() const override { return static_cast<double>(count_); }
};

}  // namespace bs

#endif  // BS_CONSTRAINTS_BOX_TYPE_COST_HPP
