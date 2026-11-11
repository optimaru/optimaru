#ifndef SLS_CONSTRAINTS_COMMON_HPP
#define SLS_CONSTRAINTS_COMMON_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "sls/domain.hpp"
#include "sls/score_calculator.hpp"
#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/score/decomposed_objective.hpp"

namespace sls {

// ---------------------------------------------------------------------------
// Plugin-style constraint components (the school-timetabling pattern)
// ---------------------------------------------------------------------------
// The ten constraints of this use case, split into one small component
// each on the engine's DecomposedObjective — an optional assembly path
// NEXT TO the monolithic SportsLeagueSchedulingIncrementalScoreCalculator,
// which stays the tuned default fast path.
//
// Unit choice: with the assignment shape the calculator's unit is the VALUE,
// i.e. one round POSITION; members are the matches played in it. Unassigned
// matches (round == -1) belong to no unit and contribute nothing.
//
// Every constraint of this domain is keyed by the round INDEX rather than the
// position (LeagueSchedule::roundIndexOf), and most of them relate matches on
// ADJACENT indices, which no single unit owns. So only the three rules that
// read one match and its own round are UnitConstraints; the rest are
// GlobalConstraints on the on_member_changed fast path, over one of three
// shared bases below:
//
//   RoundIndexPairConstraint  a sum over match PAIRS keyed by round index
//                             (same index, or index and index + 1). The
//                             subclass writes the marginal of one match
//                             against the current occupants of the indices it
//                             can pair with; join adds it before the match
//                             enters the bucket, leave subtracts it after the
//                             match left, which makes the aggregate exactly
//                             reversible.
//   TeamSideRunConstraint     the consecutive home/away run penalties: a
//                             per-(team, index) presence table whose one
//                             touched team row is re-scored in O(indexCount).
//   (repeat_match_on_next_day_cost.hpp carries its own state: its cost is a
//    clamped per-match indicator, not a pair sum.)
//
// The conventions every use case shares are described once: editing an
// assembly by name in optimaru/score/constraint_assembly.hpp; kDefaultWeight /
// kDefaultLevel, the registry names and the cost sign (positive = penalty, 0 =
// no violation) in docs/extending.md. An assembly of all ten components at
// their default weights scores bit-for-bit like ScoreCalculator::score.
//
// The pattern every component follows (constraints/<subject>_cost.hpp, one
// component per header):
//   - the struct is named <Subject>Cost and its registry name is the struct
//     name with a lower-case first letter ("teamConflictCost"), which the
//     Python bindings see as snake_case ("team_conflict_cost");
//   - its defaults are declared ONCE, as `static constexpr double
//     kDefaultWeight` and `static constexpr optimaru::Level kDefaultLevel`,
//     and the constructor takes the weight first (`explicit
//     X(double weight = kDefaultWeight)`). defaultConstraints().info() —
//     and so the Python constraint_info() and the CP-SAT reference's
//     defaults — read them off the components; nothing restates them;
//   - the level is the `level` member (optimaru::Level::Hard / Soft; this
//     domain has no medium level), `is_hard()` reads it. Weight, level and
//     exponent are assembly-time metadata, not part of the cost: override
//     them per instance through the validated setters or the assembly's
//     named edits. The travel-hop rules cost plain kilometres at weight 1,
//     and workdayClassicMatchesCost carries x 1'000.
//
// Every component requires ScoreCalculator::prepare(schedule) to have run
// (the solve() overload and the monolithic path both call it).

using DecomposedCalculator = optimaru::DecomposedObjective<LeagueSchedule>;
using ConstraintAssembly = optimaru::ConstraintAssembly<LeagueSchedule>;
using SlsUnitConstraint = optimaru::UnitConstraint<LeagueSchedule>;

// A component's kDefaultWeight as an exact integer, for the monolithic
// calculators (Breakdown's weighted totals and the incremental calculator),
// which are the default assembly computed in int64. Fails to compile if a
// default weight stops being integral or if a component leaves the level the
// monolith sums it into.
template <typename Component, optimaru::Level kLevel>
constexpr std::int64_t monolithicWeight() {
    static_assert(Component::kDefaultLevel == kLevel,
                  "the monolithic calculators sum this constraint into "
                  "another level");
    constexpr auto weight = static_cast<std::int64_t>(Component::kDefaultWeight);
    static_assert(weight == Component::kDefaultWeight,
                  "the monolithic calculators assume integral default "
                  "weights");
    return weight;
}
template <typename Component>
constexpr std::int64_t kHardWeight = monolithicWeight<Component, optimaru::Level::Hard>();
template <typename Component>
constexpr std::int64_t kSoftWeight = monolithicWeight<Component, optimaru::Level::Soft>();

namespace detail {

inline void requirePrepared(const LeagueSchedule& schedule) {
    if (!schedule.prepared) {
        throw std::logic_error(
                "sls constraints require "
                "ScoreCalculator::prepare(schedule) first");
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Per-match unit constraints
// ---------------------------------------------------------------------------
// Shared base of the three rules that read one match and the round it is in:
// their cost is a sum of independent per-(match, round) terms, so the
// subclass writes `matchCost` only and both the full re-cost and the exact
// O(1) delta follow from it.
struct PerMatchConstraint : SlsUnitConstraint {
    using SlsUnitConstraint::SlsUnitConstraint;

    // The penalty units match `matchIndex` incurs while played in round
    // position `unit`.
    virtual double matchCost(const LeagueSchedule& schedule, int unit, int matchIndex) const = 0;

    void on_reset(const LeagueSchedule& schedule) override { detail::requirePrepared(schedule); }
    double cost(const LeagueSchedule& schedule, int unit,
                const std::vector<int>& members) const override {
        double total = 0.0;
        for (int matchIndex : members) total += matchCost(schedule, unit, matchIndex);
        return total;
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const LeagueSchedule& schedule, int unit, const std::vector<int>& /*members*/,
                      int entity) const override {
        return matchCost(schedule, unit, entity);
    }
};

// ---------------------------------------------------------------------------
// Round-index pair constraints
// ---------------------------------------------------------------------------
// Shared base of the rules whose total is a sum over match pairs keyed by the
// round index. It keeps the matches bucketed by index and asks the subclass
// for the marginal of one match against the buckets it can pair with.
struct RoundIndexPairConstraint : optimaru::MemberChangeGlobalConstraint<LeagueSchedule> {
    using MemberChangeGlobalConstraint::MemberChangeGlobalConstraint;

    void reset(const LeagueSchedule& schedule,
               const std::vector<std::vector<int>>& members_per_unit) override {
        detail::requirePrepared(schedule);
        indexCount_ = schedule.maxRoundIndex + 1;
        byIndex_.assign(indexCount_ > 0 ? indexCount_ : 0, {});
        total_ = 0;
        for (std::size_t unit = 0; unit < members_per_unit.size(); ++unit) {
            const int index = schedule.roundIndexOf(static_cast<int>(unit));
            for (int matchIndex : members_per_unit[unit]) {
                total_ += marginal(schedule, matchIndex, index);
                byIndex_[index].push_back(matchIndex);
            }
        }
    }

    void on_member_changed(const LeagueSchedule& schedule, int unit,
                           const std::vector<int>& /*members*/, int entity, bool joined) override {
        const int index = schedule.roundIndexOf(unit);
        std::vector<int>& bucket = byIndex_[index];
        if (joined) {
            total_ += marginal(schedule, entity, index);
            bucket.push_back(entity);
            return;
        }
        const auto it = std::find(bucket.begin(), bucket.end(), entity);
        if (it != bucket.end()) {
            *it = bucket.back();
            bucket.pop_back();
        }
        total_ -= marginal(schedule, entity, index);
    }

    double cost() const override { return static_cast<double>(total_); }

protected:
    // The matches currently on round index `index` — NOT including the match
    // whose marginal is being computed.
    const std::vector<int>& at(int index) const {
        if (index < 0 || index >= indexCount_) return empty_;
        return byIndex_[index];
    }

    // The pair terms match `matchIndex` on round index `index` contributes
    // against the current occupants of the adjacent buckets.
    virtual std::int64_t marginal(const LeagueSchedule& schedule, int matchIndex,
                                  int index) const = 0;

private:
    int indexCount_ = 0;
    std::vector<std::vector<int>> byIndex_;
    std::vector<int> empty_;
    std::int64_t total_ = 0;
};

// ---------------------------------------------------------------------------
// Consecutive home/away run constraints
// ---------------------------------------------------------------------------
// Shared base of the two run penalties: a per-(team, round index) presence
// count, with the one touched team's row re-scored through
// ScoreCalculator::runPenalty. O(indexCount) per move, and exact by
// construction rather than by an incremental run-splicing argument.
struct TeamSideRunConstraint : optimaru::MemberChangeGlobalConstraint<LeagueSchedule> {
    using MemberChangeGlobalConstraint::MemberChangeGlobalConstraint;

    // Which team of the match this side tracks (home or away).
    virtual int teamOf(const Match& match) const = 0;

    void reset(const LeagueSchedule& schedule,
               const std::vector<std::vector<int>>& members_per_unit) override {
        detail::requirePrepared(schedule);
        indexCount_ = schedule.maxRoundIndex + 1;
        counts_.assign(static_cast<std::size_t>(schedule.teamCount()) * indexCount_, 0);
        row_.assign(indexCount_, 0);
        penaltyPerTeam_.assign(schedule.teamCount(), 0);
        total_ = 0;
        for (std::size_t unit = 0; unit < members_per_unit.size(); ++unit) {
            const int index = schedule.roundIndexOf(static_cast<int>(unit));
            for (int matchIndex : members_per_unit[unit]) {
                const int team = teamOf(schedule.matches[matchIndex]);
                ++counts_[static_cast<std::size_t>(team) * indexCount_ + index];
            }
        }
        for (int team = 0; team < schedule.teamCount(); ++team) rescore(team);
    }

    void on_member_changed(const LeagueSchedule& schedule, int unit,
                           const std::vector<int>& /*members*/, int entity, bool joined) override {
        const int index = schedule.roundIndexOf(unit);
        const int team = teamOf(schedule.matches[entity]);
        counts_[static_cast<std::size_t>(team) * indexCount_ + index] += joined ? 1 : -1;
        rescore(team);
    }

    double cost() const override { return static_cast<double>(total_); }

private:
    void rescore(int team) {
        const std::size_t base = static_cast<std::size_t>(team) * indexCount_;
        for (int i = 0; i < indexCount_; ++i) row_[i] = counts_[base + i];
        const std::int64_t penalty = ScoreCalculator::runPenalty(row_);
        total_ += penalty - penaltyPerTeam_[team];
        penaltyPerTeam_[team] = penalty;
    }

    int indexCount_ = 0;
    std::vector<int> counts_;  // (team, round index) -> matches on that side
    std::vector<std::int64_t> penaltyPerTeam_;
    mutable std::vector<int> row_;  // scratch row handed to runPenalty
    std::int64_t total_ = 0;
};

}  // namespace sls

#endif  // SLS_CONSTRAINTS_COMMON_HPP
