#ifndef ST_CONSTRAINTS_COMMON_HPP
#define ST_CONSTRAINTS_COMMON_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "st/canonical_domain.hpp"
#include "st/canonical_score_calculator.hpp"
#include "optimaru/api/score/calculator/constraint_assembly.hpp"
#include "optimaru/api/score/calculator/decomposed_score_calculator.hpp"

namespace st::canon
{

// ---------------------------------------------------------------------------
// Plugin-style constraint components
// ---------------------------------------------------------------------------
// The canonical H1~H10 / S1~S6 rules, split into one small component each on
// the engine's DecomposedScoreCalculator — an optional assembly path NEXT TO
// the monolithic CanonicalIncrementalScoreCalculator, which stays the tuned
// default fast path.
//
// Unit choice: with the BASIC descriptor the calculator's unit is the VALUE,
// i.e. one timeslot; members are the lessons currently assigned to it. The
// per-slot rules (H3/H4/H5/H6, S5/S6) therefore decompose as UnitConstraints
// with exact deltaCost, while everything whose state spans slots — per
// teacher, class, course or block group — runs as a GlobalConstraint on the
// onMemberChanged fast path, maintaining exactly the aggregates the
// monolithic calculator keeps (occupancy counts folded into per-(owner, day)
// busy masks). Every metric goes through the shared ScoreCalculator building
// blocks (pairs/max0/gapOf/runExcessOf/blockViolation), so an assembly of
// all fifteen components with default metadata reproduces the monolithic
// score bit-for-bit (H2 fixed slots stay enforced by pinning, never scored).
//
// Two structural fast paths close most of the plugin path's dispatch cost
// against the monolithic calculator, without changing any cost definition:
//   - the per-slot family carries a deltaGroupKey, so the calculator
//     compiles every present member into ONE SlotDeltaGroup evaluate per
//     member change (monolithic count maps instead of member re-scans);
//   - the per-(teacher, day) mask family shares ONE TeacherMaskHub
//     projection per assembly instead of five private copies.
// Both are transparent to partial assemblies and metadata overrides.
//
//   DecomposedCalculator calc;
//   addCanonicalConstraints(calc, weights);   // ... or pick components
//   solver.solve(table, calc);
//
// The "Hard:"/"Soft:" level and the weight of each component are DEFAULTS —
// assembly-time metadata, not part of the cost. Override them per instance
// via the mutable `isHard` / `weight` / `exponent` (es pattern). The S1~S6
// default weights are the ObjectiveWeights defaults; unlike the monolithic
// calculators, the components never read Timetable::weights — pass the
// intended weights at assembly time (addCanonicalConstraints does).
//
// Cost sign convention (see decomposed_score_calculator.hpp): positive =
// penalty; the calculator negates into the score.
//
// Every component requires ScoreCalculator::prepare(table) to have run (the
// solve() overload and the monolithic path both call it).

using DecomposedCalculator = optimaru::DecomposedScoreCalculator<Timetable>;
using ConstraintAssembly = optimaru::ConstraintAssembly<Timetable>;
using CanonUnitConstraint = optimaru::UnitConstraint<Timetable>;
using CanonGlobalConstraint = optimaru::GlobalConstraint<Timetable>;

namespace detail
{

inline void requirePrepared(const Timetable &table)
{
    if (!table.prepared)
    {
        throw std::logic_error("st::canon constraints require "
                               "ScoreCalculator::prepare(table) first");
    }
}

// The busy-period bit of a slot (bit = period - 1, the monolithic layout).
inline std::uint16_t periodBit(const Timetable &table, int slot)
{
    return static_cast<std::uint16_t>(1U << (table.periodOfSlot[slot] - 1));
}

// Number of teachers standing in both lessons (H4 counts one conflict pair
// per shared teacher; teacher lists are tiny and duplicate-free).
inline int sharedTeachers(const Lesson &a, const Lesson &b)
{
    int shared = 0;
    for (int x : a.teachers)
    {
        for (int y : b.teachers)
            shared += (x == y) ? 1 : 0;
    }
    return shared;
}

// Busy-mask projection primitive (H9/H10/S1~S4): per-(owner, slot)
// occupancy counts folded into a 16-bit busy-period mask per (owner, day).
// A mask bit flips only on a 0<->1 count transition — the monolithic
// calculator's DISTINCT-period semantics, under which team teaching and
// merged lessons keep a period busy until the last occupant leaves. Owned
// by TeacherMaskHub (one shared instance for the whole teacher family) and
// by ClassGap (its own class-keyed instance); every flip reaches the
// owner's metric fold through the callback.
class OwnerDayMasks
{
public:
    void reset(int ownerCount, int slotCount, int dayCount)
    {
        slotCount_ = slotCount;
        dayCount_ = dayCount;
        occupancy_.assign(static_cast<std::size_t>(ownerCount) * slotCount, 0);
        mask_.assign(static_cast<std::size_t>(ownerCount) * dayCount, 0);
    }

    // Applies one occupancy change of (owner, slot); on a busy-mask flip
    // calls onMaskChange(oldMask, newMask) for the owner's day.
    template <typename F>
    void apply(int owner,
               int slot,
               int day,
               std::uint16_t bit,
               bool joined,
               F &&onMaskChange)
    {
        int &count
            = occupancy_[static_cast<std::size_t>(owner) * slotCount_ + slot];
        if (joined)
        {
            if (++count != 1)
                return;
        }
        else
        {
            if (--count != 0)
                return;
        }
        std::uint16_t &mask
            = mask_[static_cast<std::size_t>(owner) * dayCount_ + day];
        const std::uint16_t oldMask = mask;
        mask = joined ? static_cast<std::uint16_t>(oldMask | bit)
                      : static_cast<std::uint16_t>(
                            oldMask & static_cast<std::uint16_t>(~bit));
        onMaskChange(oldMask, mask);
    }

private:
    std::vector<int> occupancy_;       // (owner, slot) -> assigned lessons
    std::vector<std::uint16_t> mask_;  // (owner, day) -> busy-period mask
    int slotCount_ = 0;
    int dayCount_ = 0;
};

}  // namespace detail

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_COMMON_HPP
