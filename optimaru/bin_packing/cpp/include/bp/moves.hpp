#ifndef BP_MOVES_HPP
#define BP_MOVES_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "bp/domain.hpp"
#include "optimaru/search/operators.hpp"

namespace bp {

// The domain's macro moves, registered with the engine by Solver (see
// solver.cpp, runEngine). Header-only so the tests can drive them through a
// Editor directly. RepackMove reads the derived facts of
// ScoreCalculator::prepare (every solve and every calculator reset runs it).

// ---------------------------------------------------------------------------
// EmptyBinMove — the domain's bin-emptying macro move
// ---------------------------------------------------------------------------
// usedBinsCost is a step function of "does this bin hold anything": a bin
// only stops costing once its LAST item leaves, so every single-item change
// on the way there is no better and usually breaks the capacity of the bin it
// lands in. This operator does the whole step in one move: take one of the
// `candidates` lightest non-empty bins (uniformly; random among equal loads)
// and move its items, heaviest first (random among equal weights), each to
// the best-fitting OTHER non-empty bin that still has room. It gives up (no
// move) when some item fits nowhere. It checks the capacity only:
// colours and conflicts are judged by the score, through the acceptor, like
// everything else.
//
// Cost is O(n + used bins * moved items) per move (a census plus a best-fit
// scan per moved item); the adaptive roulette wheel decides how often it is
// worth that.
class EmptyBinMove final : public optimaru::MoveOperator<BinPackingProblem> {
public:
    explicit EmptyBinMove(int candidates) : candidates_(std::max(1, candidates)) {}

    const char* name() const override { return "emptyBin"; }

    bool apply(optimaru::OperatorContext<BinPackingProblem>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        BinPackingProblem& problem = ctx.editor.solution();
        const int n = problem.itemCount();
        const std::int64_t capacity = problem.binCapacity;

        load_.assign(static_cast<std::size_t>(n), 0);
        count_.assign(static_cast<std::size_t>(n), 0);
        for (const Item& item : problem.items) {
            if (item.bin >= 0) {
                load_[item.bin] += item.weight;
                ++count_[item.bin];
            }
        }
        used_.clear();
        for (int b = 0; b < n; ++b) {
            if (count_[b] > 0) used_.push_back({load_[b], rng(), b});
        }
        if (used_.size() < 2) return false;  // nowhere to move the items to

        // The k lightest bins, random among equal loads.
        const std::size_t k =
                std::min<std::size_t>(static_cast<std::size_t>(candidates_), used_.size());
        std::partial_sort(used_.begin(), used_.begin() + static_cast<std::ptrdiff_t>(k),
                          used_.end());
        const int source = used_[rng() % k].bin;

        members_.clear();
        for (int i = 0; i < n; ++i) {
            if (problem.items[i].bin == source) members_.push_back(i);
        }
        std::shuffle(members_.begin(), members_.end(), rng);
        std::stable_sort(members_.begin(), members_.end(), [&problem](int a, int b) {
            return problem.items[a].weight > problem.items[b].weight;
        });

        // Plan every target first, so a failure leaves the solution as it is.
        targets_.clear();
        for (int item : members_) {
            const std::int64_t weight = problem.items[item].weight;
            int target = -1;
            std::int64_t bestRoom = 0;
            for (const Candidate& candidate : used_) {
                const int bin = candidate.bin;
                if (bin == source || load_[bin] + weight > capacity) continue;
                const std::int64_t room = capacity - load_[bin] - weight;
                if (target < 0 || room < bestRoom || (room == bestRoom && bin < target)) {
                    target = bin;
                    bestRoom = room;
                }
            }
            if (target < 0) return false;
            load_[target] += weight;
            targets_.push_back(target);
        }

        lastSource_ = source;
        ctx.editor.begin_batch();
        for (std::size_t m = 0; m < members_.size(); ++m) {
            ctx.editor.assign(members_[m], targets_[m]);
            entities.push_back(members_[m]);
        }
        ctx.editor.end_batch();
        return true;
    }

    void undo_last(optimaru::OperatorContext<BinPackingProblem>& ctx) override {
        ctx.editor.begin_batch();
        for (int item : members_) ctx.editor.assign(item, lastSource_);
        ctx.editor.end_batch();
    }

private:
    struct Candidate {
        std::int64_t load;
        std::uint64_t tieBreak;
        int bin;
        bool operator<(const Candidate& other) const {
            if (load != other.load) return load < other.load;
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
// and late acceptance rejects the move: without RepackMove the search reaches
// feasibility by opening bins and never closes them again. This move empties a light bin
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
class RepackMove final : public optimaru::MoveOperator<BinPackingProblem> {
public:
    RepackMove(int candidates, int maxPartners)
        : candidates_(std::max(1, candidates)), maxPartners_(std::max(0, maxPartners)) {}

    const char* name() const override { return "repack"; }

    bool apply(optimaru::OperatorContext<BinPackingProblem>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        BinPackingProblem& problem = ctx.editor.solution();
        const int n = problem.itemCount();
        const std::int64_t capacity = problem.binCapacity;

        // Census: load and items per bin, and the plan (the bin every item
        // will be in), which starts as the current assignment.
        load_.assign(static_cast<std::size_t>(n), 0);
        if (members_.size() < static_cast<std::size_t>(n))
            members_.resize(static_cast<std::size_t>(n));
        for (int b = 0; b < n; ++b) members_[b].clear();
        plan_.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const int bin = problem.items[i].bin;
            plan_[i] = bin;
            if (bin >= 0) {
                load_[bin] += problem.items[i].weight;
                members_[bin].push_back(i);
            }
        }
        open_.assign(static_cast<std::size_t>(n), 0);
        used_.clear();
        for (int b = 0; b < n; ++b) {
            if (!members_[b].empty()) {
                used_.push_back({load_[b], rng(), b});
                open_[b] = 1;
            }
        }
        if (used_.size() < 2) return false;  // nowhere to move the items to

        // The lightest bins, enough for the source and the partner pool.
        const std::size_t k =
                std::min<std::size_t>(static_cast<std::size_t>(candidates_), used_.size());
        const std::size_t sorted = std::min<std::size_t>(
                used_.size(), k + 2 * static_cast<std::size_t>(maxPartners_) + 2);
        std::partial_sort(used_.begin(), used_.begin() + static_cast<std::ptrdiff_t>(sorted),
                          used_.end());
        // Source to the front; used_[1 .. sorted) stay the lightest others,
        // in order.
        const std::size_t pick = rng() % k;
        std::rotate(used_.begin(), used_.begin() + static_cast<std::ptrdiff_t>(pick),
                    used_.begin() + static_cast<std::ptrdiff_t>(pick) + 1);
        const int source = used_[0].bin;
        open_[source] = 0;

        // Partners: p of the 2p + 2 lightest others, into used_[1 .. 1 + p).
        std::size_t partners = 0;
        if (maxPartners_ > 0) {
            partners =
                    1 + static_cast<std::size_t>(rng() % static_cast<std::uint64_t>(maxPartners_));
            partners = std::min(partners, used_.size() - 1);
        }
        const std::size_t pool = std::min(used_.size() - 1, 2 * partners + 2);
        for (std::size_t p = 0; p < partners; ++p) {
            const std::size_t j = 1 + p + rng() % (pool - p);
            std::swap(used_[1 + p], used_[j]);
        }

        // Take every item out of the source and the partners.
        removed_.clear();
        for (std::size_t p = 0; p <= partners; ++p) {
            const int bin = used_[p].bin;
            for (int item : members_[bin]) {
                removed_.push_back(item);
                plan_[item] = -1;
            }
            members_[bin].clear();
            load_[bin] = 0;
        }
        std::shuffle(removed_.begin(), removed_.end(), rng);
        std::stable_sort(removed_.begin(), removed_.end(), [&problem](int a, int b) {
            return problem.items[a].weight > problem.items[b].weight;
        });

        // Put them back, best fit decreasing over the open bins.
        int freshCursor = 0;
        for (int item : removed_) {
            const std::int64_t weight = problem.items[item].weight;
            int target = -1;
            std::int64_t bestRoom = 0;
            for (std::size_t c = 1; c < used_.size(); ++c) {
                const int bin = used_[c].bin;
                if (load_[bin] + weight > capacity) continue;
                const std::int64_t room = capacity - load_[bin] - weight;
                if (target >= 0 && (room > bestRoom || (room == bestRoom && bin > target)))
                    continue;
                if (!fits(problem, item, bin)) continue;
                target = bin;
                bestRoom = room;
            }
            if (target < 0) {
                if (load_[source] + weight <= capacity && fits(problem, item, source)) {
                    target = source;
                } else {
                    while (freshCursor < n && (open_[freshCursor] || freshCursor == source ||
                                               !members_[freshCursor].empty())) {
                        ++freshCursor;
                    }
                    if (freshCursor < n) {
                        target = freshCursor;
                        open_[target] = 1;
                        used_.push_back({0, 0, target});  // open from now on
                    } else {
                        target = source;  // every bin is in use: the acceptor judges
                    }
                }
            }
            plan_[item] = target;
            load_[target] += weight;
            members_[target].push_back(item);
        }

        moved_.clear();
        for (int item : removed_) {
            if (plan_[item] != problem.items[item].bin)
                moved_.push_back({item, problem.items[item].bin});
        }
        if (moved_.empty()) return false;  // every item went back where it was
        ctx.editor.begin_batch();
        for (const std::pair<int, int>& move : moved_) {
            ctx.editor.assign(move.first, plan_[move.first]);
            entities.push_back(move.first);
        }
        ctx.editor.end_batch();
        return true;
    }

    void undo_last(optimaru::OperatorContext<BinPackingProblem>& ctx) override {
        ctx.editor.begin_batch();
        for (const std::pair<int, int>& move : moved_) ctx.editor.assign(move.first, move.second);
        ctx.editor.end_batch();
    }

private:
    struct Candidate {
        std::int64_t load;
        std::uint64_t tieBreak;
        int bin;
        bool operator<(const Candidate& other) const {
            if (load != other.load) return load < other.load;
            return tieBreak < other.tieBreak;
        }
    };

    // No colour or conflict clash with what the plan already has in `bin`.
    // (Capacity is checked by the caller.)
    bool fits(const BinPackingProblem& problem, int item, int bin) const {
        const int cls = problem.colorClass[item];
        if (cls >= 0) {
            for (int other : members_[bin]) {
                if (problem.colorClass[other] == cls) return false;
            }
        }
        for (int partner : problem.conflictPartners[item]) {
            if (plan_[partner] == bin) return false;
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

// ---------------------------------------------------------------------------
// TradeMove — a trade between two bins that leaves one of them fuller
// ---------------------------------------------------------------------------
// usedBinsCost counts bins, so every packing with the same number of bins has
// the same score, and a bin closes only once the other bins have taken up its
// whole load. Nothing in the change and swap moves leads there: on
// Falkenauer's triplets, where the best packing fills every bin exactly, the
// search stayed one bin above it.
//
// This move takes one bin (with probability 1/2 one of the `candidates`
// lightest, as in the other two moves, else any non-empty bin) and up to
// kMaxPartners other non-empty bins. Against each partner it looks at every
// trade of none, one or two items of the first bin for none, one or two of
// the partner that keeps both within the capacity and leaves the fuller of
// the two at least as full as the fuller one was. It makes the trade that
// raises the two bins' sum of squared loads most, random among equal ones.
// A trade that raises nothing still changes which items make up the two bins
// (two full bins stay full, or the two bins swap loads), and is made when no
// trade raises the sum: it is how items get from a full bin to the bins that
// need them. Not counted as a trade: the same weights changing hands, and all
// of one bin for all of the other. No trade with any of the partners = no
// move.
//
// The move cannot hold what it gains on its own: plain late acceptance takes
// every step that keeps the score, and the change and swap moves undo the
// trades as fast as they are made. FillLateAcceptance
// (bp/fill_late_acceptance.hpp) compares equal scores by the same sum of
// squared loads; the two are meant to run together.
//
// It checks the capacity only; colours and conflicts are judged by the
// score, through the acceptor, like everything else. A bin above the
// capacity takes part in no trade (none leaves it both within the capacity
// and no less full); the change and swap moves bring it back.
//
// Cost per move: an O(n) census, and per partner at most 37 x 37 trades
// (kMaxItems items of a bin are looked at; a bin of three items has 7 x 7).
// The census is the larger part from a few thousand items up; the three
// macro moves each take their own, and the selector that draws them counts
// outcomes, not time.
class TradeMove final : public optimaru::MoveOperator<BinPackingProblem> {
public:
    static constexpr std::size_t kMaxItems = 8;
    static constexpr std::size_t kMaxPartners = 16;

    explicit TradeMove(int candidates) : candidates_(std::max(1, candidates)) {}

    const char* name() const override { return "trade"; }

    bool apply(optimaru::OperatorContext<BinPackingProblem>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        BinPackingProblem& problem = ctx.editor.solution();
        const int n = problem.itemCount();
        const std::int64_t capacity = problem.binCapacity;

        // Census: load and items per bin.
        load_.assign(static_cast<std::size_t>(n), 0);
        if (members_.size() < static_cast<std::size_t>(n))
            members_.resize(static_cast<std::size_t>(n));
        for (int b = 0; b < n; ++b) members_[b].clear();
        for (int i = 0; i < n; ++i) {
            const int bin = problem.items[i].bin;
            if (bin >= 0) {
                load_[bin] += problem.items[i].weight;
                members_[bin].push_back(i);
            }
        }
        used_.clear();
        for (int b = 0; b < n; ++b) {
            if (!members_[b].empty()) used_.push_back({load_[b], rng(), b});
        }
        if (used_.size() < 2) return false;  // no second bin to trade with

        // The first bin, to the front: one of the k lightest, or any.
        std::size_t pick = rng() % used_.size();
        if (rng() % 2 == 0) {
            const std::size_t k =
                    std::min<std::size_t>(static_cast<std::size_t>(candidates_), used_.size());
            std::partial_sort(used_.begin(), used_.begin() + static_cast<std::ptrdiff_t>(k),
                              used_.end());
            pick = rng() % k;
        }
        std::swap(used_[0], used_[pick]);
        const int first = used_[0].bin;
        parts(problem, members_[first], rng, give_);

        // The best trade over the partners, drawn without repetition.
        double best = -1.0;
        std::uint64_t ties = 0;
        Part give{}, take{};
        int partner = -1;
        const std::size_t partners = std::min(kMaxPartners, used_.size() - 1);
        for (std::size_t p = 0; p < partners; ++p) {
            std::swap(used_[1 + p], used_[1 + p + rng() % (used_.size() - 1 - p)]);
            const int other = used_[1 + p].bin;
            parts(problem, members_[other], rng, take_);
            const std::int64_t fuller = std::max(load_[first], load_[other]);
            const std::int64_t lighter = std::min(load_[first], load_[other]);
            for (const Part& given : give_) {
                for (const Part& taken : take_) {
                    // Nothing would change: the same weights change hands, or
                    // the two bins change contents.
                    if (given.weight == taken.weight && sameWeights(problem, given, taken))
                        continue;
                    if (count(given) == members_[first].size() &&
                        count(taken) == members_[other].size())
                        continue;
                    const std::int64_t change = given.weight - taken.weight;
                    const std::int64_t top = std::max(load_[first] - change, load_[other] + change);
                    // Both stay within the capacity, and the fuller one of
                    // the two is no less full than before.
                    if (top > capacity || top < fuller) continue;
                    // Half the rise of the two squared loads.
                    const double gain =
                            static_cast<double>(top - fuller) * static_cast<double>(top - lighter);
                    if (gain < best) continue;
                    if (gain > best) {
                        best = gain;
                        ties = 0;
                    }
                    if (rng() % ++ties != 0) continue;  // uniform among the equal ones
                    give = given;
                    take = taken;
                    partner = other;
                }
            }
        }
        if (partner < 0) return false;

        moved_.clear();
        for (int item : {give.first, give.second}) {
            if (item >= 0) moved_.push_back({item, first});
        }
        for (int item : {take.first, take.second}) {
            if (item >= 0) moved_.push_back({item, partner});
        }
        ctx.editor.begin_batch();
        for (const std::pair<int, int>& move : moved_) {
            ctx.editor.assign(move.first, move.second == first ? partner : first);
            entities.push_back(move.first);
        }
        ctx.editor.end_batch();
        return true;
    }

    void undo_last(optimaru::OperatorContext<BinPackingProblem>& ctx) override {
        ctx.editor.begin_batch();
        for (const std::pair<int, int>& move : moved_) ctx.editor.assign(move.first, move.second);
        ctx.editor.end_batch();
    }

private:
    struct Candidate {
        std::int64_t load;
        std::uint64_t tieBreak;
        int bin;
        bool operator<(const Candidate& other) const {
            if (load != other.load) return load < other.load;
            return tieBreak < other.tieBreak;
        }
    };

    // What one bin puts into a trade: no item, one or two (-1 = none).
    struct Part {
        int first = -1;
        int second = -1;
        std::int64_t weight = 0;
    };

    // Every part of a bin: nothing, each item, each pair. Of a bin with more
    // than kMaxItems items, a uniform sample of that many.
    static void parts(const BinPackingProblem& problem, std::vector<int>& items,
                      std::mt19937_64& rng, std::vector<Part>& out) {
        std::size_t count = items.size();
        if (count > kMaxItems) {
            count = kMaxItems;
            for (std::size_t i = 0; i < count; ++i)
                std::swap(items[i], items[i + rng() % (items.size() - i)]);
        }
        out.clear();
        out.push_back({});
        for (std::size_t a = 0; a < count; ++a) {
            const std::int64_t weight = problem.items[items[a]].weight;
            out.push_back({items[a], -1, weight});
            for (std::size_t b = a + 1; b < count; ++b)
                out.push_back({items[a], items[b], weight + problem.items[items[b]].weight});
        }
    }

    static std::size_t count(const Part& part) {
        return static_cast<std::size_t>(part.first >= 0) +
               static_cast<std::size_t>(part.second >= 0);
    }

    // Two parts of equal weight: do the same weights change hands (nothing
    // for nothing, an item for one as heavy, a pair for the same two weights)?
    static bool sameWeights(const BinPackingProblem& problem, const Part& a, const Part& b) {
        if (a.first < 0 || b.first < 0) return true;         // weights are positive: both are empty
        if ((a.second < 0) != (b.second < 0)) return false;  // one item for two
        if (a.second < 0) return true;
        const std::int64_t weight = problem.items[a.first].weight;
        return weight == problem.items[b.first].weight || weight == problem.items[b.second].weight;
    }

    int candidates_;

    // Scratch, reused across moves.
    std::vector<std::int64_t> load_;
    std::vector<std::vector<int>> members_;   // per bin
    std::vector<Candidate> used_;             // [0] the first bin, then the partners drawn
    std::vector<Part> give_;                  // the first bin's parts
    std::vector<Part> take_;                  // a partner's parts
    std::vector<std::pair<int, int>> moved_;  // (item, bin before the move)
};

}  // namespace bp

#endif  // BP_MOVES_HPP
