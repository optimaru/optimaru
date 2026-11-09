#ifndef BP_DOMAIN_HPP
#define BP_DOMAIN_HPP

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"

namespace bp
{

// ---------------------------------------------------------------------------
// Score
// ---------------------------------------------------------------------------
// Score, exactly the engine's score type, so the mapping is the
// identity and there is no medium level (no optimaru::MediumFoldOf
// specialization). At the default weights:
//   hard = -(overload above capacity + extra same-colour items + conflict
//            pairs sharing a bin)
//   soft = -(bins in use)
// The weights are the components' kDefaultWeight (bp/constraints/*.hpp); the
// problem carries none (removed in 0.2.0).
using Score = optimaru::Score;

// ---------------------------------------------------------------------------
// Item  (@PlanningEntity)
// ---------------------------------------------------------------------------
// `bin` is the single BASIC planning variable: a 0-based bin index into the
// value range 0..itemCount()-1 (n identical bins always suffice), -1 ==
// unassigned. Every solve returns every item assigned; -1 appears only in an
// unsolved input and inside the engine's ruin-and-recreate.
struct Item
{
    std::string id;
    std::int64_t weight = 0;  // > 0 (one-dimensional size)
    int bin = -1;             // planning variable; -1 == unassigned
    // nullopt = no colour. Two items of the same colour should not share a
    // bin (colorConflictCost).
    std::optional<std::string> color;

    bool isAssigned() const { return bin >= 0; }
};

// ---------------------------------------------------------------------------
// BinPackingProblem  (@PlanningSolution)
// ---------------------------------------------------------------------------
struct BinPackingProblem
{
    // --- problem facts (the canonical model) ---
    std::int64_t binCapacity = 0;  // the capacity of every (identical) bin
    std::vector<Item> items;
    // Item-id pairs that must not share a bin. Listed as given: a duplicated
    // or reversed pair counts once per listing (as in 0.1.5).
    std::vector<std::pair<std::string, std::string>> conflicts;

    // No constraint weights here (removed in 0.2.0): the defaults are the
    // components' kDefaultWeight (every one is 1), and a caller re-weights
    // through a constraint assembly instead.

    // true = start the search from items[*].bin (a seed, completed by the
    // construction where an item is still -1); false = a plain solve
    // ignores the input bins. Every solve clears it: a seed is used once.
    bool warmStart = false;

    Score score;

    // --- derived by ScoreCalculator::prepare(problem) ---
    // Set once the facts below are built; ScoreCalculator::prepare skips the
    // rebuild while it holds. Clear it after editing `items` or `conflicts`
    // (the Python bindings' setters do).
    bool prepared = false;
    // Per item: the index of its colour among the colours carried by at
    // least two items, or -1 (no colour, or a colour no other item has: such
    // an item can never violate colorConflictCost).
    std::vector<int> colorClass;
    int colorClassCount = 0;
    // The conflicts resolved to item indices, in listing order.
    std::vector<std::pair<int, int>> conflictPairs;
    // Per item: the partner indices of its conflict pairs, with multiplicity
    // (a pair listed twice appears twice).
    std::vector<std::vector<int>> conflictPartners;

    int itemCount() const { return static_cast<int>(items.size()); }

    // The value range of Item::bin: one bin per item.
    int binCount() const { return itemCount(); }

    // Distinct bins >= 0 holding at least one item.
    int binsUsed() const
    {
        std::vector<int> bins;
        bins.reserve(items.size());
        for (const Item &item : items)
        {
            if (item.bin >= 0)
                bins.push_back(item.bin);
        }
        std::sort(bins.begin(), bins.end());
        return static_cast<int>(std::unique(bins.begin(), bins.end())
                                - bins.begin());
    }

    // [] when no item is assigned, else the load of every bin 0..max bin
    // (an empty bin in between shows as 0), as in 0.1.5.
    std::vector<std::int64_t> binLoads() const
    {
        int top = -1;
        for (const Item &item : items)
            top = std::max(top, item.bin);
        std::vector<std::int64_t> loads(static_cast<std::size_t>(top + 1), 0);
        for (const Item &item : items)
        {
            if (item.bin >= 0)
                loads[item.bin] += item.weight;
        }
        return loads;
    }

    // The current bin per item (the solution).
    std::vector<int> assignment() const
    {
        std::vector<int> out;
        out.reserve(items.size());
        for (const Item &item : items)
            out.push_back(item.bin);
        return out;
    }

    // Bulk write of the planning variable. Range-checked at the next
    // ScoreCalculator::prepare (every scoring / solving entry point).
    void setAssignment(const std::vector<int> &bins)
    {
        if (bins.size() != items.size())
        {
            throw std::invalid_argument(
                "assignment length must equal the item count");
        }
        for (std::size_t i = 0; i < bins.size(); ++i)
            items[i].bin = bins[i];
    }
};

}  // namespace bp

// ---------------------------------------------------------------------------
// SolutionTraits specialization: entities are items, the planning
// variable is Item::bin, the value range is the n bins 0..n-1.
// ---------------------------------------------------------------------------
template <> struct optimaru::SolutionTraits<bp::BinPackingProblem>
{
    static constexpr optimaru::Shape shape
        = optimaru::Shape::Assignment;
    static int entity_count(const bp::BinPackingProblem &problem)
    {
        return problem.itemCount();
    }
    static int value_count(const bp::BinPackingProblem &problem)
    {
        return problem.binCount();
    }
    static int value_of(const bp::BinPackingProblem &problem, int item)
    {
        return problem.items[item].bin;
    }
    static void assign(bp::BinPackingProblem &problem, int item, int bin)
    {
        problem.items[item].bin = bin;
    }
};

#endif  // BP_DOMAIN_HPP
