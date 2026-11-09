// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_LEVELS_HPP
#define OPTIMARU_LEVELS_HPP

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "optimaru/score.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Score levels
// ---------------------------------------------------------------------------
// Every constraint scores on exactly one level. Hard dominates medium, medium
// dominates soft. The engine itself compares Score (two levels), so a
// domain with a medium level declares ONCE how its medium accumulator folds
// into those two levels (see MediumFold below); the decomposed objective
// keeps three separate accumulators and applies that fold when it reports a
// score.
//
// Weights are in units of their level: a medium constraint at weight 1 costs
// one medium unit per raw unit, whatever the fold factor is. Flipping it to
// hard gives one hard unit per raw unit; flipping it to soft gives one soft
// unit. The fold factor never appears in a weight.
enum class Level : std::uint8_t { Hard, Medium, Soft };

// The spelling clients see ("hard" / "medium" / "soft"): constraint_info()'s
// default_level and constraint_costs()'s "level".
inline const char* level_name(Level level) {
    switch (level) {
        case Level::Hard:
            return "hard";
        case Level::Medium:
            return "medium";
        case Level::Soft:
            return "soft";
    }
    return "soft";
}

// ---------------------------------------------------------------------------
// The range of a score
// ---------------------------------------------------------------------------
// The engine's hard level is an int64 and its soft level a double. Two rules
// keep every reported score exact or, where it cannot be, on the right side
// (the Python score_costs mirror in _constraint.py applies the same two):
//
//   1. One contribution (one unit of one constraint after its weight and
//      exponent, or a global constraint's single shaped total) must be
//      finite, and a hard one, or a medium one that a below_hard fold lands
//      in the hard level, must stay below kMaxHardContribution = 2^62
//      (about 4.6e18) in magnitude. Otherwise the objective throws
//      ContributionOverflowError (a std::overflow_error) naming the
//      constraint: a weight or exponent that large cannot be scored. Python
//      sees OverflowError with the mirror's exact text, the constraint named
//      by its Python name (python/constraint_binding.hpp).
//   2. The hard TOTAL, and a below_hard fold hard * unit + medium, is summed
//      exactly (HardTotal) and SATURATES at +-kHardScoreLimit (INT64_MAX)
//      instead of wrapping: a penalty beyond int64 reports -INT64_MAX hard,
//      never a positive, feasible-looking score, and a larger penalty never
//      looks better than a smaller one. Saturated scores tie.
inline constexpr double kMaxHardContribution = 4611686018427387904.0;  // 2^62
inline constexpr std::int64_t kHardScoreLimit = std::numeric_limits<std::int64_t>::max();

namespace detail {

// Portable checked int64 arithmetic (the GCC/Clang builtins, a plain check
// on MSVC). `out` receives the wrapped two's-complement result either way.
inline bool add_overflows(std::int64_t a, std::int64_t b, std::int64_t& out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, &out);
#else
    out = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
    return (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
           (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b);
#endif
}

inline bool mul_overflows(std::int64_t a, std::int64_t b, std::int64_t& out) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, &out);
#else
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    out = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
    if (a == 0 || b == 0) return false;
    if (a > 0) return b > 0 ? a > kMax / b : b < kMin / a;
    return b > 0 ? a < kMin / b : b < kMax / a;
#endif
}

inline std::int64_t clamp_to_limit(std::int64_t value) {
    return value < -kHardScoreLimit ? -kHardScoreLimit : value;
}

}  // namespace detail

// Rule 1's exception. constraint() is the registry name, value() the
// offending contribution; beyond_hard_limit() is true for a finite hard (or
// below_hard medium) contribution of 2^62 or more, false for a non-finite one
// on any level. what() names the registry id; the Python bindings raise
// OverflowError with message(python_name(constraint()), repr(value()), ...),
// which is word for word the text of the Python mirror (_constraint.py
// _contribution / _round_hard).
class ContributionOverflowError : public std::overflow_error {
public:
    ContributionOverflowError(std::string constraint, double value, bool beyond_hard_limit)
        : std::overflow_error(message(constraint, shown(value), beyond_hard_limit)),
          constraint_(std::move(constraint)),
          value_(value),
          beyond_hard_limit_(beyond_hard_limit) {}

    const std::string& constraint() const noexcept { return constraint_; }
    double value() const noexcept { return value_; }
    bool beyond_hard_limit() const noexcept { return beyond_hard_limit_; }

    // The text for a constraint and a contribution as the caller's layer
    // spells them.
    static std::string message(const std::string& constraint, const std::string& value,
                               bool beyond_hard_limit) {
        if (beyond_hard_limit) {
            return "constraint '" + constraint + "': a hard contribution of " + value +
                   " exceeds the engine's limit of 2**62 (about 4.6e18) per unit; lower its "
                   "weight or exponent";
        }
        return "constraint '" + constraint +
               "': its contribution weight * cost^exponent is not finite (" + value +
               "); lower its weight or exponent";
    }

private:
    static std::string shown(double value) {
        char text[48];
        std::snprintf(text, sizeof text, "%.17g", value);
        return text;
    }

    std::string constraint_;
    double value_;
    bool beyond_hard_limit_;
};

// Rule 1, as the exception the objective throws. Cold path. A non-finite
// value is reported as not finite on every level (the Python mirror checks
// finiteness before the hard limit too).
[[noreturn]] inline void throw_contribution_overflow(const std::string& constraint, double value,
                                                   bool hard_level) {
    throw ContributionOverflowError(constraint, value, hard_level && std::isfinite(value));
}

// A hard (or below_hard medium) contribution, rounded half away from zero,
// after rule 1's range check.
inline long long round_hard_contribution(double value, const std::string& constraint) {
    if (!(std::fabs(value) < kMaxHardContribution)) {
        throw_contribution_overflow(constraint, value, /*hard_level=*/true);
    }
    return std::llround(value);
}

// A soft (or into_soft medium) contribution, after rule 1's finiteness check.
inline double checked_soft_contribution(double value, const std::string& constraint) {
    if (!std::isfinite(value)) throw_contribution_overflow(constraint, value, /*hard_level=*/false);
    return value;
}

// `value`, with a negative zero turned into +0.0: a penalty-free score
// should print "0soft", not "-0soft". Negating a zero penalty total gives
// -0.0, and GCC at -O2 also compiles `0.0 - static_cast<double>(n)` for an
// integer n into a plain negation, so domain Score adapters pass their final
// soft (and medium) level through this.
inline double without_negative_zero(double value) { return value == 0.0 ? 0.0 : value; }

// One level of a domain's integral Score adapter (bed-allocation,
// food-packaging, pickup-delivery, project-job, task-assigning,
// vehicle-routing report int64 medium/soft levels) from a double total:
// rounded half away from zero, or std::overflow_error when it does not fit
// an int64 (llround's result would be unspecified there).
inline std::int64_t round_score_level(double value, const char* level) {
    if (!(std::fabs(value) < 0x1p63)) {
        char shown[48];
        std::snprintf(shown, sizeof shown, "%.17g", value);
        throw std::overflow_error(std::string("the ") + level + " score " + shown +
                                  " does not fit this domain's int64 Score; lower the weights "
                                  "or exponents");
    }
    return std::llround(value);
}

// An exact hard-level sum that cannot wrap (rule 2): the true value is
// lo + wraps * 2^64, so additions and the subtractions an incremental update
// makes stay exact and reversible however large the total grows. Reads
// saturate: saturated() is the value clamped to +-kHardScoreLimit.
class HardTotal {
public:
    constexpr HardTotal() = default;
    constexpr explicit HardTotal(std::int64_t value) : lo_(value) {}

    // x must not be INT64_MIN (every caller adds a value bounded by rule 1,
    // or a difference of two such values).
    void add(std::int64_t x) {
        std::int64_t sum;
        if (detail::add_overflows(lo_, x, sum)) wraps_ += x > 0 ? 1 : -1;
        lo_ = sum;
    }
    void add(const HardTotal& other) {
        add(other.lo_);
        wraps_ += other.wraps_;
    }
    void subtract(std::int64_t x) { add(-x); }

    bool is_zero() const { return lo_ == 0 && wraps_ == 0; }
    bool positive() const { return wraps_ > 0 || (wraps_ == 0 && lo_ > 0); }
    // The exact value fits an int64 (and is that int64).
    bool fits() const { return wraps_ == 0; }
    std::int64_t low() const { return lo_; }
    double approx() const { return static_cast<double>(lo_) + static_cast<double>(wraps_) * 0x1p64; }
    std::int64_t saturated() const {
        if (wraps_ > 0) return kHardScoreLimit;
        if (wraps_ < 0) return -kHardScoreLimit;
        return detail::clamp_to_limit(lo_);
    }

    bool operator==(const HardTotal& o) const { return lo_ == o.lo_ && wraps_ == o.wraps_; }
    bool operator!=(const HardTotal& o) const { return !(*this == o); }

private:
    std::int64_t lo_ = 0;
    std::int64_t wraps_ = 0;
};

// hard * unit + round(medium), exact when it fits, else saturated to
// +-kHardScoreLimit by the sign of the true value (rule 2). Sign-agnostic, so
// it folds penalty sums and score levels (negative) alike.
inline std::int64_t fold_below_hard_saturating(const HardTotal& hard, double medium,
                                            std::int64_t unit) {
    std::int64_t scaled = 0;
    std::int64_t total = 0;
    if (hard.fits() && !detail::mul_overflows(hard.low(), unit, scaled) &&
        std::fabs(medium) < 0x1p63 &&
        !detail::add_overflows(scaled, static_cast<std::int64_t>(std::llround(medium)), total)) {
        return detail::clamp_to_limit(total);
    }
    const double approx = hard.approx() * static_cast<double>(unit) + medium;
    return approx > 0.0 ? kHardScoreLimit : (approx < 0.0 ? -kHardScoreLimit : 0);
}

// ---------------------------------------------------------------------------
// MediumFold: how a domain packs its medium level into Score
// ---------------------------------------------------------------------------
//   none()                no medium level (the default). A Level::Medium
//                         constraint is a configuration error and the
//                         objective throws at reset.
//   into_soft(unit)        engine.soft = medium * unit + soft. Medium stays a
//                         double like soft. (bed, vehicle-routing,
//                         pickup-delivery, project-job: 1e9; food-packaging:
//                         1e10; meeting, tournament: 1e6.)
//   below_hard(hardUnit)   engine.hard = hard * hardUnit + medium, medium
//                         rounded per unit like hard. (task-assigning: 1e9.)
//                         The fold saturates at +-INT64_MAX, and it leaves
//                         the hard level 1/hardUnit of its range: hard
//                         penalties above about 9.2e18 / hardUnit (9.2e9
//                         hard units for task-assigning) all report
//                         -INT64_MAX. A medium weight of hardUnit or more
//                         per raw unit reaches into the hard level's digits.
//
// A domain declares its fold once, next to its Score adapter
// (toEngineScore/fromEngineScore in domain.hpp), and registers it for its
// solution type by specializing MediumFoldOf:
//
//   constexpr std::int64_t kMediumUnit = 1000000000LL;
//   constexpr optimaru::MediumFold kMediumFold =
//       optimaru::MediumFold::into_soft(kMediumUnit);
//   inline optimaru::Score toEngineScore(const Score& s) {
//       return kMediumFold.to_engine(s.hard, s.medium, s.soft);
//   }
//   ...
//   template <> struct optimaru::MediumFoldOf<vrp::VrpSolution> {
//       static constexpr optimaru::MediumFold value = vrp::kMediumFold;
//   };
//
// The monolithic objective (through toEngineScore) and the decomposed
// objective (through MediumFoldOf) then fold with the same constant.
struct MediumFold {
    enum class Kind : std::uint8_t { None, IntoSoft, BelowHard };
    Kind kind = Kind::None;
    // IntoSoft: soft units per medium unit. BelowHard: medium units per hard
    // unit (an integer, so the folded hard level stays integral).
    double unit = 0.0;

    static constexpr MediumFold none() { return MediumFold{Kind::None, 0.0}; }
    static constexpr MediumFold into_soft(double soft_per_medium) {
        return soft_per_medium > 0.0
                       ? MediumFold{Kind::IntoSoft, soft_per_medium}
                       : throw std::invalid_argument("MediumFold::intoSoft needs a positive unit");
    }
    static constexpr MediumFold below_hard(std::int64_t medium_per_hard) {
        return medium_per_hard > 0
                       ? MediumFold{Kind::BelowHard, static_cast<double>(medium_per_hard)}
                       : throw std::invalid_argument("MediumFold::belowHard needs a positive unit");
    }

    constexpr bool has_medium() const { return kind != Kind::None; }
    // BelowHard lands medium in the integral hard level, so each unit's
    // medium contribution is rounded the way a hard one is.
    constexpr bool rounds_medium() const { return kind == Kind::BelowHard; }
    // Whether a contribution on `level` counts toward the engine's hard
    // score: hard always, medium where it folds below hard (BelowHard).
    // Such a level keeps "hard 0 = no violation", which is why it refuses
    // rewards and the literal offset formula (DecomposedObjective::
    // validate_level, python::check_literal_offset).
    constexpr bool counts_toward_hard(Level level) const {
        return level == Level::Hard || (level == Level::Medium && rounds_medium());
    }

    // Domain-side Score adapter: three score levels (penalties negative) to
    // the engine's two. The decomposed objective folds its accumulators with
    // the same arithmetic, so both objectives agree by construction.
    Score to_engine(std::int64_t hard, double medium, double soft) const {
        switch (kind) {
            case Kind::IntoSoft:
                return {hard, without_negative_zero(soft + medium * unit)};
            case Kind::BelowHard:
                // Saturating (see "The range of a score" above).
                return {fold_below_hard_saturating(HardTotal(hard), medium,
                                                static_cast<std::int64_t>(unit)),
                        without_negative_zero(soft)};
            case Kind::None:
                break;
        }
        if (medium != 0.0) {
            throw std::logic_error("a medium score on a domain without a medium fold");
        }
        return {hard, without_negative_zero(soft)};
    }
};

// The medium fold of a solution type. The primary template says "no medium
// level"; a domain with one specializes it (see above).
template <typename Solution>
struct MediumFoldOf {
    static constexpr MediumFold value = MediumFold::none();
};

}  // namespace optimaru

#endif  // OPTIMARU_LEVELS_HPP
