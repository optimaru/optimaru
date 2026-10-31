#ifndef OPTIMARU_API_SCORE_LEVEL_HPP
#define OPTIMARU_API_SCORE_LEVEL_HPP

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Score levels
// ---------------------------------------------------------------------------
// Every constraint scores on exactly one level. Hard dominates medium, medium
// dominates soft. The engine itself compares HardSoftScore (two levels), so a
// domain with a medium level declares ONCE how its medium accumulator folds
// into those two levels (see MediumFold below); the decomposed calculator
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
inline const char* levelName(Level level) {
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
// MediumFold: how a domain packs its medium level into HardSoftScore
// ---------------------------------------------------------------------------
//   none()                no medium level (the default). A Level::Medium
//                         constraint is a configuration error and the
//                         calculator throws at reset.
//   intoSoft(unit)        engine.soft = medium * unit + soft. Medium stays a
//                         double like soft. (bed, vehicle-routing,
//                         pickup-delivery, project-job: 1e9; food-packaging:
//                         1e10; meeting, tournament: 1e6.)
//   belowHard(hardUnit)   engine.hard = hard * hardUnit + medium, medium
//                         rounded per unit like hard. (task-assigning: 1e9.)
//
// A domain declares its fold once, next to its Score adapter
// (toEngineScore/fromEngineScore in domain.hpp), and registers it for its
// solution type by specializing MediumFoldOf:
//
//   constexpr std::int64_t kMediumUnit = 1000000000LL;
//   constexpr optimaru::MediumFold kMediumFold =
//       optimaru::MediumFold::intoSoft(kMediumUnit);
//   inline optimaru::HardSoftScore toEngineScore(const Score& s) {
//       return kMediumFold.toEngine(s.hard, s.medium, s.soft);
//   }
//   ...
//   template <> struct optimaru::MediumFoldOf<vrp::VrpSolution> {
//       static constexpr optimaru::MediumFold value = vrp::kMediumFold;
//   };
//
// The monolithic calculator (through toEngineScore) and the decomposed
// calculator (through MediumFoldOf) then fold with the same constant.
struct MediumFold {
    enum class Kind : std::uint8_t { None, IntoSoft, BelowHard };
    Kind kind = Kind::None;
    // IntoSoft: soft units per medium unit. BelowHard: medium units per hard
    // unit (an integer, so the folded hard level stays integral).
    double unit = 0.0;

    static constexpr MediumFold none() { return MediumFold{Kind::None, 0.0}; }
    static constexpr MediumFold intoSoft(double softPerMedium) {
        return softPerMedium > 0.0
                       ? MediumFold{Kind::IntoSoft, softPerMedium}
                       : throw std::invalid_argument("MediumFold::intoSoft needs a positive unit");
    }
    static constexpr MediumFold belowHard(std::int64_t mediumPerHard) {
        return mediumPerHard > 0
                       ? MediumFold{Kind::BelowHard, static_cast<double>(mediumPerHard)}
                       : throw std::invalid_argument("MediumFold::belowHard needs a positive unit");
    }

    constexpr bool hasMedium() const { return kind != Kind::None; }
    // BelowHard lands medium in the integral hard level, so each unit's
    // medium contribution is rounded the way a hard one is.
    constexpr bool roundsMedium() const { return kind == Kind::BelowHard; }

    // Domain-side Score adapter: three score levels (penalties negative) to
    // the engine's two. The decomposed calculator folds its accumulators with
    // the same arithmetic, so both calculators agree by construction.
    HardSoftScore toEngine(std::int64_t hard, double medium, double soft) const {
        switch (kind) {
            case Kind::IntoSoft:
                return {hard, soft + medium * unit};
            case Kind::BelowHard:
                return {hard * static_cast<std::int64_t>(unit) + std::llround(medium), soft};
            case Kind::None:
                break;
        }
        if (medium != 0.0) {
            throw std::logic_error("a medium score on a domain without a medium fold");
        }
        return {hard, soft};
    }
};

// The medium fold of a solution type. The primary template says "no medium
// level"; a domain with one specializes it (see above).
template <typename Solution>
struct MediumFoldOf {
    static constexpr MediumFold value = MediumFold::none();
};

}  // namespace optimaru

#endif  // OPTIMARU_API_SCORE_LEVEL_HPP
