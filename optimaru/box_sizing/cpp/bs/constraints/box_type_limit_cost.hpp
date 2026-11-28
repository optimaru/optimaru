#ifndef BS_CONSTRAINTS_BOX_TYPE_LIMIT_COST_HPP
#define BS_CONSTRAINTS_BOX_TYPE_LIMIT_COST_HPP

#include "bs/constraints/common.hpp"

namespace bs {

// Hard, weight 1: boxTypeLimitCost -- at most maxBoxTypes distinct boxes may
// be in use. Raw cost = the boxes in use (those a parcel ships in and the
// required ones) above maxBoxTypes, one global value; 0 = within the limit,
// and always 0 for a problem without one.
//
// A step function of "is this box used at all": moving one parcel cannot take
// a box out of use while others still ship in it, which is why the solver
// ships CloseBoxMove, OpenBoxMove and SwapBoxMove (bs/search/moves/).
struct BoxTypeLimitCost final : detail::BoxTypeCount {
    static constexpr double kDefaultWeight = 1.0;
    static constexpr optimaru::Level kDefaultLevel = optimaru::Level::Hard;

    explicit BoxTypeLimitCost(double weight = kDefaultWeight)
        : BoxTypeCount("boxTypeLimitCost", weight, kDefaultLevel) {}
    double cost() const override {
        return limit_ >= 0 && count_ > limit_ ? static_cast<double>(count_ - limit_) : 0.0;
    }
};

}  // namespace bs

#endif  // BS_CONSTRAINTS_BOX_TYPE_LIMIT_COST_HPP
