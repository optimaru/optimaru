// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SCORE_HARD_MEDIUM_SOFT_SCORE_HPP
#define OPTIMARU_SCORE_HARD_MEDIUM_SOFT_SCORE_HPP

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include "optimaru/score/levels.hpp"
#include "optimaru/score/score.hpp"

namespace optimaru {

// A three-level score with whole-number levels: the Score a use case with a
// medium level reports. Higher is better and the levels compare
// lexicographically: hard dominates medium, medium dominates soft. A solution
// is feasible when hard >= 0.
//
// The engine itself searches on the two-level Score. A use case declares once
// how its medium level folds into that (a MediumFold, see levels.hpp) and
// converts with the three functions below:
//
//   using Score = optimaru::HardMediumSoftScore;
//   constexpr optimaru::MediumFold kMediumFold = optimaru::MediumFold::into_soft(1e9);
//
//   optimaru::Score engine = optimaru::to_engine_score(kMediumFold, score);
//   Score reported = optimaru::from_engine_score(kMediumFold, engine);
struct HardMediumSoftScore {
    std::int64_t hard = 0;
    std::int64_t medium = 0;
    std::int64_t soft = 0;

    bool feasible() const { return hard >= 0; }

    // Above every score: as a target, it is never reached.
    static HardMediumSoftScore unreachable() {
        constexpr std::int64_t max = std::numeric_limits<std::int64_t>::max();
        return {max, max, max};
    }

    bool operator<(const HardMediumSoftScore& o) const {
        if (hard != o.hard) return hard < o.hard;
        if (medium != o.medium) return medium < o.medium;
        return soft < o.soft;
    }
    bool operator>(const HardMediumSoftScore& o) const { return o < *this; }
    bool operator<=(const HardMediumSoftScore& o) const { return !(o < *this); }
    bool operator>=(const HardMediumSoftScore& o) const { return !(*this < o); }
    bool operator==(const HardMediumSoftScore& o) const {
        return hard == o.hard && medium == o.medium && soft == o.soft;
    }

    // "<hard>hard/<medium>medium/<soft>soft".
    std::string str() const {
        return std::to_string(hard) + "hard/" + std::to_string(medium) + "medium/" +
               std::to_string(soft) + "soft";
    }
};

// The engine score of a three-level one under an into_soft fold:
// soft + medium * unit. Exact while the levels are whole numbers and the sum
// stays below 2^53 in magnitude.
inline Score to_engine_score(const MediumFold& fold, const HardMediumSoftScore& score) {
    return fold.to_engine(score.hard, static_cast<double>(score.medium),
                          static_cast<double>(score.soft));
}

// A target score (Solver::Config::target_score) in engine terms. kNoLevelFloor
// (levels.hpp) on the medium or the soft level makes the engine's soft limit
// -inf, so only the hard level is compared and no soft score, however low,
// misses the stop: {0, kNoLevelFloor, kNoLevelFloor} stops at the first
// feasible solution. Folded like a score, the sentinel would become a finite
// soft that a low enough score never reaches. Any other limit folds like a
// score.
inline Score to_engine_score_limit(const MediumFold& fold, const HardMediumSoftScore& limit) {
    if (limit.medium == kNoLevelFloor || limit.soft == kNoLevelFloor) {
        return {limit.hard, -std::numeric_limits<double>::infinity()};
    }
    return to_engine_score(fold, limit);
}

// The inverse of to_engine_score, for an into_soft fold with a whole-number
// unit (any other fold is a std::logic_error: the soft level would not hold
// the medium one, or not exactly). Penalties are whole numbers at or below
// zero, so the truncating division, which rounds toward zero, decodes both
// levels exactly.
inline HardMediumSoftScore from_engine_score(const MediumFold& fold, const Score& engine) {
    if (fold.kind != MediumFold::Kind::IntoSoft || fold.unit != std::floor(fold.unit)) {
        throw std::logic_error(
                "from_engine_score needs an into_soft fold with a whole-number unit");
    }
    const auto unit = static_cast<std::int64_t>(fold.unit);
    const std::int64_t combined = round_score_level(engine.soft, "soft");
    return {engine.hard, combined / unit, combined % unit};
}

}  // namespace optimaru

#endif  // OPTIMARU_SCORE_HARD_MEDIUM_SOFT_SCORE_HPP
