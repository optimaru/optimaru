#ifndef BP_MOVES_HPP
#define BP_MOVES_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "bp/domain.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace bp
{

// The domain's macro moves, registered with the engine by Solver (see
// solver.cpp, runEngine). Header-only so the tests can drive them through a
// ScoreDirector directly. Both read the derived facts of
// ScoreCalculator::prepare (every solve and every calculator reset runs it).

// ---------------------------------------------------------------------------
// EmptyBinMove — the domain's missing macro move (0.1.5 `_try_empty_bin`)
// ---------------------------------------------------------------------------
// usedBinsCost is a step function of "does this bin hold anything": a bin
// only stops costing once its LAST item leaves, so every single-item change
// on the way there is no better and usually breaks the capacity of the bin it
// lands in. This operator does the whole step in one move: take one of the
// `candidates` lightest non-empty bins (uniformly; random among equal loads)
// and move its items, heaviest first (random among equal weights), each to
// the best-fitting OTHER non-empty bin that still has room. It gives up (no
// move) when some item fits nowhere. Like 0.1.5 it checks the capacity only:
// colours and conflicts are judged by the score, through the acceptor, like
// everything else.
//
// Cost is O(n + used bins * moved items) per move (a census plus a best-fit
// scan per moved item); the adaptive roulette wheel decides how often it is
// worth that.
class EmptyBinMove final : public optimaru::MoveOperator<BinPackingProblem>
{
public:
    explicit EmptyBinMove(int candidates) : candidates_(std::max(1, candidates)) {}

    const char *name() const override { return "emptyBin"; }

    bool apply(optimaru::OperatorContext<BinPackingProblem> &ctx,
               std::mt19937_64 &rng,
               std::vector<int> &entities) override
    {
        BinPackingProblem &problem = ctx.director.workingSolution();
        const int n = problem.itemCount();
        const std::int64_t capacity = problem.binCapacity;

        load_.assign(static_cast<std::size_t>(n), 0);
        count_.assign(static_cast<std::size_t>(n), 0);
        for (const Item &item : problem.items)
        {
            if (item.bin >= 0)
            {
                load_[item.bin] += item.weight;
                ++count_[item.bin];
            }
        }
        used_.clear();
        for (int b = 0; b < n; ++b)
        {
            if (count_[b] > 0)
                used_.push_back({load_[b], rng(), b});
        }
        if (used_.size() < 2)
            return false;  // nowhere to move the items to

        // The k lightest bins, random among equal loads.
        const std::size_t k
            = std::min<std::size_t>(static_cast<std::size_t>(candidates_), used_.size());
        std::partial_sort(used_.begin(),
                          used_.begin() + static_cast<std::ptrdiff_t>(k),
                          used_.end());
        const int source = used_[rng() % k].bin;

        members_.clear();
        for (int i = 0; i < n; ++i)
        {
            if (problem.items[i].bin == source)
                members_.push_back(i);
        }
        std::shuffle(members_.begin(), members_.end(), rng);
        std::stable_sort(members_.begin(),
                         members_.end(),
                         [&problem](int a, int b)
                         { return problem.items[a].weight > problem.items[b].weight; });

        // Plan every target first, so a failure leaves the solution as it is.
        targets_.clear();
        for (int item : members_)
        {
            const std::int64_t weight = problem.items[item].weight;
            int target = -1;
            std::int64_t bestRoom = 0;
            for (const Candidate &candidate : used_)
            {
                const int bin = candidate.bin;
                if (bin == source || load_[bin] + weight > capacity)
                    continue;
                const std::int64_t room = capacity - load_[bin] - weight;
                if (target < 0 || room < bestRoom || (room == bestRoom && bin < target))
                {
                    target = bin;
                    bestRoom = room;
                }
            }
            if (target < 0)
                return false;
            load_[target] += weight;
            targets_.push_back(target);
        }

        lastSource_ = source;
        ctx.director.beginVariableChanges();
        for (std::size_t m = 0; m < members_.size(); ++m)
        {
            ctx.director.changeVariable(members_[m], targets_[m]);
            entities.push_back(members_[m]);
        }
        ctx.director.endVariableChanges();
        return true;
    }

    void undoLast(optimaru::OperatorContext<BinPackingProblem> &ctx) override
    {
        ctx.director.beginVariableChanges();
        for (int item : members_)
            ctx.director.changeVariable(item, lastSource_);
        ctx.director.endVariableChanges();
    }

private:
    struct Candidate
    {
        std::int64_t load;
        std::uint64_t tieBreak;
        int bin;
        bool operator<(const Candidate &other) const
        {
            if (load != other.load)
                return load < other.load;
            return tieBreak < other.tieBreak;
        }
    };

    int candidates_;

    // Scratch, reused across moves (no per-move allocation).
    std::vector<std::int64_t> load_;
    std::vector<int> count_;
    std::vector<Candidate> used_;
    std::vector<int> members_;
    std::vector<int> targets_;
    int lastSource_ = -1;
};

// ---------------------------------------------------------------------------
// RepackMove — a small bin-level ruin and recreate that keeps the hard
// constraints
// ---------------------------------------------------------------------------
// EmptyBinMove checks the capacity only, so on an instance with colours or
// conflicts emptying a bin into the others nearly always breaks one of them,
// and late acceptance rejects the move: the 0.2.0 search reached feasibility
// by opening bins and never closed them again. This move empties a light bin
// TOGETHER with a few other light bins, so the items have somewhere new to go:
//   1. the source is one of the `candidates` lightest non-empty bins
//      (uniformly; random among equal loads), like EmptyBinMove's;
//   2. p partners, p uniform in 1..maxPartners, are drawn uniformly from the
//      2p + 2 lightest non-empty bins other than the source;
//   3. every item of the source and the partners comes out and goes back,
//      heaviest first (random among equal weights), into the fullest OPEN bin
//      it still fits: room for its weight, no other item of its colour, no
//      conflict partner. Open = every non-empty bin but the source; an
//      emptied partner is open (empty, so it is the last resort among them).
//   4. An item that fits no open bin goes back to the source when it fits
//      there, else into a new (empty) bin, which then counts as open; with no
//      empty bin left it goes to the source regardless.
// The move saves a bin whenever every item found an open bin, and more than
// one when a partner stays empty too. Otherwise it reshuffles the light bins
// at the same score, which late acceptance takes and which gives the next
// move another packing to start from. A move that would move nothing is not
// made.
//
// "Fits" is the default constraint set's notion of a clash (bp/constraints),
// read off the prepared facts directly; under a caller's constraint selection
// the acceptor still judges every move by that selection's score, so this is
// only a proposal heuristic, like everything a move does.
//
// Cost per move: an O(n) census, a partial sort of the lightest bins, and a
// best-fit scan over the open bins per moved item, each candidate checked
// against that bin's items and the item's conflict partners. No score is
// evaluated until the engine scores the finished move.
class RepackMove final : public optimaru::MoveOperator<BinPackingProblem>
{
public:
    RepackMove(int candidates, int maxPartners)
        : candidates_(std::max(1, candidates)), maxPartners_(std::max(0, maxPartners))
    {
    }

    const char *name() const override { return "repack"; }

    bool apply(optimaru::OperatorContext<BinPackingProblem> &ctx,
               std::mt19937_64 &rng,
               std::vector<int> &entities) override
    {
        BinPackingProblem &problem = ctx.director.workingSolution();
        const int n = problem.itemCount();
        const std::int64_t capacity = problem.binCapacity;

        // Census: load and items per bin, and the plan (the bin every item
        // will be in), which starts as the current assignment.
        load_.assign(static_cast<std::size_t>(n), 0);
        if (members_.size() < static_cast<std::size_t>(n))
            members_.resize(static_cast<std::size_t>(n));
        for (int b = 0; b < n; ++b)
            members_[b].clear();
        plan_.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            const int bin = problem.items[i].bin;
            plan_[i] = bin;
            if (bin >= 0)
            {
                load_[bin] += problem.items[i].weight;
                members_[bin].push_back(i);
            }
        }
        open_.assign(static_cast<std::size_t>(n), 0);
        used_.clear();
        for (int b = 0; b < n; ++b)
        {
            if (!members_[b].empty())
            {
                used_.push_back({load_[b], rng(), b});
                open_[b] = 1;
            }
        }
        if (used_.size() < 2)
            return false;  // nowhere to move the items to

        // The lightest bins, enough for the source and the partner pool.
        const std::size_t k
            = std::min<std::size_t>(static_cast<std::size_t>(candidates_), used_.size());
        const std::size_t sorted = std::min<std::size_t>(
            used_.size(), k + 2 * static_cast<std::size_t>(maxPartners_) + 2);
        std::partial_sort(used_.begin(),
                          used_.begin() + static_cast<std::ptrdiff_t>(sorted),
                          used_.end());
        // Source to the front; used_[1 .. sorted) stay the lightest others,
        // in order.
        const std::size_t pick = rng() % k;
        std::rotate(used_.begin(),
                    used_.begin() + static_cast<std::ptrdiff_t>(pick),
                    used_.begin() + static_cast<std::ptrdiff_t>(pick) + 1);
        const int source = used_[0].bin;
        open_[source] = 0;

        // Partners: p of the 2p + 2 lightest others, into used_[1 .. 1 + p).
        std::size_t partners = 0;
        if (maxPartners_ > 0)
        {
            partners = 1 + static_cast<std::size_t>(rng() % static_cast<std::uint64_t>(maxPartners_));
            partners = std::min(partners, used_.size() - 1);
        }
        const std::size_t pool = std::min(used_.size() - 1, 2 * partners + 2);
        for (std::size_t p = 0; p < partners; ++p)
        {
            const std::size_t j = 1 + p + rng() % (pool - p);
            std::swap(used_[1 + p], used_[j]);
        }

        // Take every item out of the source and the partners.
        removed_.clear();
        for (std::size_t p = 0; p <= partners; ++p)
        {
            const int bin = used_[p].bin;
            for (int item : members_[bin])
            {
                removed_.push_back(item);
                plan_[item] = -1;
            }
            members_[bin].clear();
            load_[bin] = 0;
        }
        std::shuffle(removed_.begin(), removed_.end(), rng);
        std::stable_sort(removed_.begin(),
                         removed_.end(),
                         [&problem](int a, int b)
                         { return problem.items[a].weight > problem.items[b].weight; });

        // Put them back, best fit decreasing over the open bins.
        int freshCursor = 0;
        for (int item : removed_)
        {
            const std::int64_t weight = problem.items[item].weight;
            int target = -1;
            std::int64_t bestRoom = 0;
            for (std::size_t c = 1; c < used_.size(); ++c)
            {
                const int bin = used_[c].bin;
                if (load_[bin] + weight > capacity)
                    continue;
                const std::int64_t room = capacity - load_[bin] - weight;
                if (target >= 0 && (room > bestRoom || (room == bestRoom && bin > target)))
                    continue;
                if (!fits(problem, item, bin))
                    continue;
                target = bin;
                bestRoom = room;
            }
            if (target < 0)
            {
                if (load_[source] + weight <= capacity && fits(problem, item, source))
                {
                    target = source;
                }
                else
                {
                    while (freshCursor < n
                           && (open_[freshCursor] || freshCursor == source
                               || !members_[freshCursor].empty()))
                    {
                        ++freshCursor;
                    }
                    if (freshCursor < n)
                    {
                        target = freshCursor;
                        open_[target] = 1;
                        used_.push_back({0, 0, target});  // open from now on
                    }
                    else
                    {
                        target = source;  // every bin is in use: the acceptor judges
                    }
                }
            }
            plan_[item] = target;
            load_[target] += weight;
            members_[target].push_back(item);
        }

        moved_.clear();
        for (int item : removed_)
        {
            if (plan_[item] != problem.items[item].bin)
                moved_.push_back({item, problem.items[item].bin});
        }
        if (moved_.empty())
            return false;  // every item went back where it was
        ctx.director.beginVariableChanges();
        for (const std::pair<int, int> &move : moved_)
        {
            ctx.director.changeVariable(move.first, plan_[move.first]);
            entities.push_back(move.first);
        }
        ctx.director.endVariableChanges();
        return true;
    }

    void undoLast(optimaru::OperatorContext<BinPackingProblem> &ctx) override
    {
        ctx.director.beginVariableChanges();
        for (const std::pair<int, int> &move : moved_)
            ctx.director.changeVariable(move.first, move.second);
        ctx.director.endVariableChanges();
    }

private:
    struct Candidate
    {
        std::int64_t load;
        std::uint64_t tieBreak;
        int bin;
        bool operator<(const Candidate &other) const
        {
            if (load != other.load)
                return load < other.load;
            return tieBreak < other.tieBreak;
        }
    };

    // No colour or conflict clash with what the plan already has in `bin`.
    // (Capacity is checked by the caller.)
    bool fits(const BinPackingProblem &problem, int item, int bin) const
    {
        const int cls = problem.colorClass[item];
        if (cls >= 0)
        {
            for (int other : members_[bin])
            {
                if (problem.colorClass[other] == cls)
                    return false;
            }
        }
        for (int partner : problem.conflictPartners[item])
        {
            if (plan_[partner] == bin)
                return false;
        }
        return true;
    }

    int candidates_;
    int maxPartners_;

    // Scratch, reused across moves.
    std::vector<std::int64_t> load_;
    std::vector<std::vector<int>> members_;  // per bin, as planned
    std::vector<int> plan_;                  // per item: its bin after the move
    std::vector<char> open_;                 // per bin: may receive an item
    std::vector<Candidate> used_;            // [0] the source, then open bins
    std::vector<int> removed_;
    std::vector<std::pair<int, int>> moved_;  // (item, bin before the move)
};

}  // namespace bp

#endif  // BP_MOVES_HPP
