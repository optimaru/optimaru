#include "op/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "op/incremental_calculator.hpp"
#include "op/score_calculator.hpp"
#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace op
{

namespace
{

// ---------------------------------------------------------------------------
// OrderConsolidateMove (see Config::orderConsolidateMoves)
// ---------------------------------------------------------------------------
// The order-level neighborhood the built-in element moves cannot reach. Every
// move it makes is atomic at whole-order granularity, in three tiers:
//
//  1. Consolidate — a random split order is gathered onto ONE trolley: the
//     holder with the most of it that still has room, else any other trolley
//     that can host the whole order, else (nothing can) its smallest holder
//     alone is folded into its largest. Every relocated pick goes in at its
//     cheapest-insertion position. Merges that would create a bucket shortage
//     are gated out in O(1) via the calculator's live bucket-demand table
//     rather than being applied and rolled back.
//  2. Relocate / swap a whole order — the endgame moves, once every order
//     already sits on a single trolley.
//  3. Single-pick cheapest-insertion relocate — last resort (see below).
//
// Why tier 1 must be atomic: done with single-element relocates a
// consolidation has to climb through intermediate +1000 states (the donor
// keeps its distinct-trolley mark until its last pick leaves), which late
// acceptance rarely tolerates. Done atomically the move trades a small
// distance increase for a 1000-point split win, so the acceptor sees the true
// net effect. Why targets outside the holders matter: the orders still split
// at the budget's end are the ones whose holders are all bucket-full while
// several trolleys stand idle — a spare trolley costs one round trip, the
// split costs 1000 (measured: restricting the target to the holders left
// every instance 1-2 splits above the floor of one trolley per order, which
// the full target search reaches on every seed).
class OrderConsolidateMove final
    : public optimaru::MoveOperator<OrderPickingSolution>
{
public:
    explicit OrderConsolidateMove(
        const OrderPickingIncrementalScoreCalculator *calculator)
        : calculator_(calculator)
    {
    }

    const char *name() const override { return "orderConsolidate"; }

    bool apply(optimaru::OperatorContext<OrderPickingSolution> &ctx,
               std::mt19937_64 &rng,
               std::vector<int> &entities) override
    {
        OrderPickingSolution &s = ctx.director.workingSolution();
        const int orderCount = static_cast<int>(s.orders.size());

        candidates_.clear();
        for (int o = 0; o < orderCount; ++o)
        {
            if (calculator_->distinctTrolleyCount(o) > 1)
                candidates_.push_back(o);
        }
        // Try a few random split orders through the feasibility gate; when
        // none can be merged anywhere, fall back to the order-level moves.
        for (int attempt = 0; attempt < 4 && !candidates_.empty(); ++attempt)
        {
            const std::size_t pick = rng() % candidates_.size();
            const int order = candidates_[pick];
            candidates_[pick] = candidates_.back();
            candidates_.pop_back();
            if (tryConsolidate(ctx, rng, entities, order))
                return true;
        }
        if (orderMoveFallback(ctx, rng, entities))
            return true;
        return relocateFallback(ctx, rng, entities);
    }

    void undoLast(optimaru::OperatorContext<OrderPickingSolution> &ctx) override
    {
        OrderPickingSolution &s = ctx.director.workingSolution();
        for (const auto &[trolley, list] : saved_)
        {
            ctx.director.beginListChange(trolley);
            s.trolleys[trolley].pickTaskIndices = list;
            ctx.director.endListChange(trolley);
        }
        saved_.clear();
    }

private:
    // Cheapest-insertion position for `location` in the trolley's route.
    static int cheapestPosition(const OrderPickingSolution &s,
                                const Trolley &trolley,
                                const WarehouseLocation &location)
    {
        const std::vector<int> &list = trolley.pickTaskIndices;
        const WarehouseLocation *previous = &trolley.location;
        const int size = static_cast<int>(list.size());
        int bestPosition = 0;
        int bestDelta = std::numeric_limits<int>::max();
        for (int p = 0; p <= size; ++p)
        {
            const WarehouseLocation &next
                = p < size ? s.pickTasks[list[p]].location : trolley.location;
            const int delta = calculateDistance(*previous, location)
                              + calculateDistance(location, next)
                              - calculateDistance(*previous, next);
            if (delta < bestDelta)
            {
                bestDelta = delta;
                bestPosition = p;
            }
            if (p < size)
                previous = &s.pickTasks[list[p]].location;
        }
        return bestPosition;
    }

    // Whether moving `addedVolume` more of `order` onto `trolley` keeps the
    // trolley within its buckets — O(1) from the calculator's live tables.
    bool fits(const OrderPickingSolution &s,
              int order,
              int trolley,
              std::int64_t addedVolume) const
    {
        const Trolley &holder = s.trolleys[trolley];
        const std::int64_t held
            = calculator_->orderVolumeOnTrolley(order, trolley);
        const std::int64_t delta
            = ScoreCalculator::calculateOrderRequiredBuckets(
                  held + addedVolume, holder.bucketCapacity)
              - ScoreCalculator::calculateOrderRequiredBuckets(
                  held, holder.bucketCapacity);
        return calculator_->requiredBucketsOnTrolley(trolley) + delta
               <= holder.bucketCount;
    }

    // Merge the order onto one trolley: the best holder that can take the
    // whole order, else any other trolley with room, else the smallest donor
    // folded into the largest holder. False when gated out.
    bool tryConsolidate(optimaru::OperatorContext<OrderPickingSolution> &ctx,
                        std::mt19937_64 &rng,
                        std::vector<int> &entities,
                        int order)
    {
        OrderPickingSolution &s = ctx.director.workingSolution();
        const int trolleyCount = static_cast<int>(s.trolleys.size());

        // The order's largest-volume holder — the natural merge target and
        // the anchor of the partial fallback.
        int largestHolder = -1;
        std::int64_t largestHolderVolume = -1;
        std::int64_t totalVolume = 0;
        for (int t = 0; t < trolleyCount; ++t)
        {
            if (calculator_->pickCountOnTrolley(order, t) == 0)
                continue;
            const std::int64_t volume
                = calculator_->orderVolumeOnTrolley(order, t);
            totalVolume += volume;
            if (volume > largestHolderVolume)
            {
                largestHolderVolume = volume;
                largestHolder = t;
            }
        }

        // Full-merge target, best first: the holder with the most of the
        // order already on it that still has room for the rest. Keeping the
        // fragments where the bulk already sits is the cheapest merge.
        int target = -1;
        std::int64_t targetVolume = -1;
        for (int t = 0; t < trolleyCount; ++t)
        {
            const std::int64_t volume
                = calculator_->orderVolumeOnTrolley(order, t);
            if (calculator_->pickCountOnTrolley(order, t) == 0
                || volume <= targetVolume)
                continue;
            if (fits(s, order, t, totalVolume - volume))
            {
                targetVolume = volume;
                target = t;
            }
        }

        // No holder has room — take any other trolley that can host the whole
        // order, scanned from a random offset so that repeated invocations
        // spread over the equivalent spare trolleys. This is what unblocks
        // the endgame: the orders still split at the 10s mark are the ones
        // whose holders are all bucket-full, while several trolleys stand
        // idle (a spare trolley costs one round trip, the split costs 1000).
        if (target < 0 && trolleyCount > 0)
        {
            const int start = static_cast<int>(
                rng() % static_cast<std::uint64_t>(trolleyCount));
            for (int i = 0; i < trolleyCount; ++i)
            {
                const int t = start + i < trolleyCount
                                  ? start + i
                                  : start + i - trolleyCount;
                if (calculator_->pickCountOnTrolley(order, t) != 0)
                    continue;
                if (fits(s, order, t, totalVolume))
                {
                    target = t;
                    break;
                }
            }
        }

        const bool full = target >= 0;
        int smallestDonor = -1;
        if (!full)
        {
            target = largestHolder;
            std::int64_t donorVolume = std::numeric_limits<std::int64_t>::max();
            for (int t = 0; t < trolleyCount; ++t)
            {
                if (t == target
                    || calculator_->pickCountOnTrolley(order, t) == 0)
                    continue;
                const std::int64_t volume
                    = calculator_->orderVolumeOnTrolley(order, t);
                if (volume < donorVolume)
                {
                    donorVolume = volume;
                    smallestDonor = t;
                }
            }
            if (!fits(s, order, target, donorVolume))
                return false;  // no merge is feasible for this order
        }

        // Pull the order's picks out of the donor trolleys, then cheapest-
        // insert every one of them into the target's route.
        moved_.clear();
        saved_.clear();
        for (int t = 0; t < trolleyCount; ++t)
        {
            if (t == target || calculator_->pickCountOnTrolley(order, t) == 0
                || (!full && t != smallestDonor))
                continue;
            detachOrder(ctx, order, t);
        }
        saved_.emplace_back(target, s.trolleys[target].pickTaskIndices);
        attachRange(ctx, target, 0, moved_.size());

        entities.insert(entities.end(), moved_.begin(), moved_.end());
        return true;
    }

    // Endgame order-level moves. Once every order sits on a single trolley
    // the split count is at its floor (one trolley per order), and every
    // element-granular cross-trolley move the built-in mix can make costs
    // +1000 — so the built-ins can only polish routes in place and the
    // assignment itself freezes. These two moves keep the split count exactly
    // where it is and are the only remaining way to change who carries what:
    // relocate a whole order onto another trolley with room, or swap two
    // whole orders between their trolleys.
    bool orderMoveFallback(optimaru::OperatorContext<OrderPickingSolution> &ctx,
                           std::mt19937_64 &rng,
                           std::vector<int> &entities)
    {
        OrderPickingSolution &s = ctx.director.workingSolution();
        const int trolleyCount = static_cast<int>(s.trolleys.size());
        const int orderCount = static_cast<int>(s.orders.size());
        if (trolleyCount < 2 || orderCount == 0)
            return false;

        for (int attempt = 0; attempt < 6; ++attempt)
        {
            const int order = static_cast<int>(
                rng() % static_cast<std::uint64_t>(orderCount));
            if (calculator_->distinctTrolleyCount(order) != 1)
                continue;  // split orders belong to the consolidation branch
            const int source = holderOf(order, trolleyCount);
            const std::int64_t volume
                = calculator_->orderVolumeOnTrolley(order, source);

            if ((rng() & 1u) == 0u)
            {
                // Relocate: the whole order onto the first trolley with room,
                // scanned from a random offset.
                const int start = static_cast<int>(
                    rng() % static_cast<std::uint64_t>(trolleyCount));
                for (int i = 0; i < trolleyCount; ++i)
                {
                    const int t = start + i < trolleyCount
                                      ? start + i
                                      : start + i - trolleyCount;
                    if (t == source || !fits(s, order, t, volume))
                        continue;
                    moved_.clear();
                    saved_.clear();
                    detachOrder(ctx, order, source);
                    saved_.emplace_back(t, s.trolleys[t].pickTaskIndices);
                    attachRange(ctx, t, 0, moved_.size());
                    entities.insert(
                        entities.end(), moved_.begin(), moved_.end());
                    return true;
                }
                continue;
            }

            // Swap: exchange this order with another whole order sitting on a
            // different trolley, when both trolleys can take the exchange.
            const int other = static_cast<int>(
                rng() % static_cast<std::uint64_t>(orderCount));
            if (other == order || calculator_->distinctTrolleyCount(other) != 1)
                continue;
            const int otherSource = holderOf(other, trolleyCount);
            if (otherSource == source)
                continue;
            const std::int64_t otherVolume
                = calculator_->orderVolumeOnTrolley(other, otherSource);
            if (!swapFits(s, source, volume, otherVolume)
                || !swapFits(s, otherSource, otherVolume, volume))
                continue;

            moved_.clear();
            saved_.clear();
            detachOrder(ctx, order, source);
            const std::size_t split = moved_.size();
            detachOrder(ctx, other, otherSource);
            attachRange(ctx, otherSource, 0, split);
            attachRange(ctx, source, split, moved_.size());
            entities.insert(entities.end(), moved_.begin(), moved_.end());
            return true;
        }
        return false;
    }

    // The single trolley holding `order` (only called when it has exactly
    // one).
    int holderOf(int order, int trolleyCount) const
    {
        for (int t = 0; t < trolleyCount; ++t)
        {
            if (calculator_->pickCountOnTrolley(order, t) != 0)
                return t;
        }
        return 0;
    }

    // Whether `trolley` still fits its buckets after giving up an order of
    // `outVolume` and taking one of `inVolume` (both whole orders, so the
    // bucket demand of each is a single ceil term).
    bool swapFits(const OrderPickingSolution &s,
                  int trolley,
                  std::int64_t outVolume,
                  std::int64_t inVolume) const
    {
        const Trolley &holder = s.trolleys[trolley];
        const std::int64_t delta
            = ScoreCalculator::calculateOrderRequiredBuckets(
                  inVolume, holder.bucketCapacity)
              - ScoreCalculator::calculateOrderRequiredBuckets(
                  outVolume, holder.bucketCapacity);
        return calculator_->requiredBucketsOnTrolley(trolley) + delta
               <= holder.bucketCount;
    }

    // Removes every pick of `order` from `from` into moved_, recording the
    // trolley's original list for the undo.
    void detachOrder(optimaru::OperatorContext<OrderPickingSolution> &ctx,
                     int order,
                     int from)
    {
        OrderPickingSolution &s = ctx.director.workingSolution();
        std::vector<int> &list = s.trolleys[from].pickTaskIndices;
        saved_.emplace_back(from, list);
        const std::vector<int> &old = saved_.back().second;
        ctx.director.beginListChange(from);
        auto keep = list.begin();
        for (int element : old)
        {
            if (s.pickTasks[element].orderIndex == order)
                moved_.push_back(element);
            else
                *keep++ = element;
        }
        list.erase(keep, list.end());
        ctx.director.endListChange(from);
    }

    // Cheapest-insertion of moved_[first, last) into `target`'s route. The
    // caller must already have recorded `target`'s original list.
    void attachRange(optimaru::OperatorContext<OrderPickingSolution> &ctx,
                     int target,
                     std::size_t first,
                     std::size_t last)
    {
        OrderPickingSolution &s = ctx.director.workingSolution();
        const Trolley &trolley = s.trolleys[target];
        std::vector<int> &list = s.trolleys[target].pickTaskIndices;
        ctx.director.beginListChange(target);
        for (std::size_t i = first; i < last; ++i)
        {
            const int element = moved_[i];
            list.insert(list.begin()
                            + cheapestPosition(
                                s, trolley, s.pickTasks[element].location),
                        element);
        }
        ctx.director.endListChange(target);
    }

    // Last resort when even the order-level moves find nothing: relocate one
    // random pick to its cheapest-insertion position on another random
    // trolley. Returning "no move possible" instead would freeze the adaptive
    // selection — a skipped apply gets no outcome reward, so the roulette
    // wheel would keep feeding steps to a dead operator (measured: the step
    // counter inflated ~30x and starved the built-in moves).
    bool relocateFallback(optimaru::OperatorContext<OrderPickingSolution> &ctx,
                          std::mt19937_64 &rng,
                          std::vector<int> &entities)
    {
        OrderPickingSolution &s = ctx.director.workingSolution();
        const int elementCount = static_cast<int>(s.pickTasks.size());
        const int trolleyCount = static_cast<int>(s.trolleys.size());
        if (elementCount == 0 || trolleyCount < 2)
            return false;

        int element = -1;
        int source = -1;
        for (int attempt = 0; attempt < 8 && element < 0; ++attempt)
        {
            const int e = static_cast<int>(
                rng() % static_cast<std::uint64_t>(elementCount));
            if (ctx.elementList[e] >= 0)
            {
                element = e;
                source = ctx.elementList[e];
            }
        }
        if (element < 0)
            return false;
        int destination = static_cast<int>(
            rng() % static_cast<std::uint64_t>(trolleyCount - 1));
        if (destination >= source)
            ++destination;

        saved_.clear();
        saved_.emplace_back(source, s.trolleys[source].pickTaskIndices);
        saved_.emplace_back(destination,
                            s.trolleys[destination].pickTaskIndices);
        ctx.director.removeListElement(source, ctx.elementPosition[element]);
        const Trolley &destinationTrolley = s.trolleys[destination];
        ctx.director.insertListElement(
            destination,
            cheapestPosition(
                s, destinationTrolley, s.pickTasks[element].location),
            element);
        entities.push_back(element);
        return true;
    }

    const OrderPickingIncrementalScoreCalculator *calculator_;
    std::vector<int> candidates_;
    std::vector<int> moved_;
    std::vector<std::pair<int, std::vector<int>>> saved_;
};

}  // namespace

namespace
{

// Shared engine wiring for both solve() overloads: termination, late
// acceptance, the LIST move mix, the ALNS knobs and the distance-nearby
// selection (all pure problem-fact lookups, so they apply to the plugin path
// too).
optimaru::SolverConfig buildEngineConfig(const Solver::Config &config,
                                         const OrderPickingSolution &solution)
{
    optimaru::SolverConfig engineConfig;
    engineConfig.randomSeed = config.randomSeed;
    engineConfig.environmentMode = config.environmentMode;
    engineConfig.multiStartCount = 1;
    engineConfig.onNewBest = config.onNewBest;
    engineConfig.termination.spentLimit = config.spentLimit;
    engineConfig.termination.bestScoreLimit = config.bestScoreLimit;
    engineConfig.termination.unimprovedSpentLimit = config.unimprovedSpentLimit;
    engineConfig.termination.stepCountLimit = config.stepCountLimit;
    engineConfig.termination.unimprovedStepCountLimit
        = config.unimprovedStepCountLimit;
    // lateAcceptanceSize 0 = auto-scale with the instance (see Config).
    engineConfig.localSearch.acceptor.lateAcceptanceSize
        = config.lateAcceptanceSize != 0
              ? config.lateAcceptanceSize
              : std::clamp<std::size_t>(
                    4 * solution.pickTasks.size(), 200, 1600);
    auto &moves = engineConfig.localSearch.moveSelector;
    moves.relocateMoveWeight = config.relocateMoveWeight;
    moves.listSwapMoveWeight = config.listSwapMoveWeight;
    moves.twoOptMoveWeight = config.twoOptMoveWeight;
    moves.orOptMoveWeight = config.orOptMoveWeight;
    moves.tailSwapMoveWeight = config.tailSwapMoveWeight;
    auto &lns = engineConfig.localSearch.ruinRecreate;
    lns.enabled = config.ruinRecreate;
    lns.unimprovedStepTrigger = config.ruinRecreateUnimprovedStepTrigger;
    lns.minRuinFraction = config.ruinRecreateMinRuinFraction;
    lns.maxRuinFraction = config.ruinRecreateMaxRuinFraction;
    lns.maxRuinCount = config.ruinRecreateMaxRuinCount;
    lns.operatorDecay = config.ruinRecreateOperatorDecay;
    lns.rewards = config.ruinRecreateRewards;
    lns.polishSteps = config.ruinRecreatePolishSteps;

    // Warehouse-distance nearby neighborhoods over the pick tasks (see
    // Config::distanceNearby). The meter reads a snapshot of the pick
    // locations — a pure lookup.
    if (config.distanceNearby)
    {
        auto locations = std::make_shared<std::vector<WarehouseLocation>>();
        locations->reserve(solution.pickTasks.size());
        for (const PickTask &task : solution.pickTasks)
            locations->push_back(task.location);
        engineConfig.localSearch.nearby.distanceMeter
            = [locations](int a, int b)
        {
            return static_cast<double>(
                calculateDistance((*locations)[a], (*locations)[b]));
        };
        engineConfig.localSearch.nearby.size = std::max(
            1,
            std::min(config.distanceNearbySize,
                     static_cast<int>(solution.pickTasks.size()) - 1));
        engineConfig.localSearch.nearby.probability
            = config.distanceNearbyProbability;
    }

    return engineConfig;
}

}  // namespace

void Solver::solve(OrderPickingSolution &solution)
{
    if (solution.trolleys.empty() || solution.pickTasks.empty())
    {
        ScoreCalculator::calculateScore(solution);
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }

    // Independent parallel starts (the school-timetabling pattern): the
    // order-consolidation operator binds to its worker's calculator, so the
    // engine's built-in multi-start — which runs the threads with the
    // built-in moves only — cannot be used. Worker i solves its own copy
    // with seed + i; the best final score wins.
    const int workerCount = std::max(1, config_.parallelSolverCount);
    if (workerCount > 1)
    {
        std::vector<OrderPickingSolution> candidates(workerCount, solution);
        std::vector<std::exception_ptr> failures(workerCount);
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (int i = 0; i < workerCount; ++i)
        {
            workers.emplace_back(
                [this, i, &candidates, &failures]
                {
                    try
                    {
                        Config workerConfig = config_;
                        workerConfig.parallelSolverCount = 1;
                        workerConfig.randomSeed
                            += static_cast<std::uint64_t>(i);
                        if (i != 0)
                            workerConfig.onNewBest = nullptr;
                        Solver worker(std::move(workerConfig));
                        worker.solve(candidates[i]);
                    }
                    catch (...)
                    {
                        failures[i] = std::current_exception();
                    }
                });
        }
        for (std::thread &worker : workers)
            worker.join();
        for (const std::exception_ptr &failure : failures)
        {
            if (failure)
                std::rethrow_exception(failure);
        }
        int best = 0;
        for (int i = 1; i < workerCount; ++i)
        {
            if (candidates[i].score > candidates[best].score)
                best = i;
        }
        solution = std::move(candidates[best]);
        return;
    }

    optimaru::SolverConfig engineConfig = buildEngineConfig(config_, solution);
    // Construction heuristic (cheapest insertion) + late-acceptance local
    // search (+ ALNS kicks), with the order-consolidation operator joining
    // the built-in move mix under adaptive selection.
    OrderPickingIncrementalScoreCalculator calculator;
    optimaru::Solver<OrderPickingSolution> engine(std::move(engineConfig));
    if (config_.orderConsolidateMoves)
    {
        engine.addMoveOperator(
            std::make_unique<OrderConsolidateMove>(&calculator));
    }
    engine.solve(solution, calculator);

    // The authoritative final score: a full recompute on the best solution.
    // Also refreshes the shadow variables (trolley / previous / next).
    solution.score = ScoreCalculator::calculateScore(solution);
}

void Solver::solve(
    OrderPickingSolution &solution,
    optimaru::DecomposedScoreCalculator<OrderPickingSolution> &calculator)
{
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }
    if (!solution.pickTasks.empty() && !solution.trolleys.empty())
    {
        // No OrderConsolidateMove here: that operator reads the monolithic
        // calculator's live per-(trolley, order) volume table, which a
        // caller-assembled plugin does not expose. The built-in LIST move mix
        // plus the ALNS kicks remain.
        optimaru::Solver<OrderPickingSolution> engine(
            buildEngineConfig(config_, solution));
        engine.solve(solution, calculator);
    }

    // Refresh the shadow variables (trolley / previous / next) on the best
    // solution, then score it with the caller's constraint assembly, leaving
    // the calculator primed on it so constraintTotals() explains exactly this
    // score.
    ScoreCalculator::updateShadowVariables(solution);
    calculator.resetWorkingSolution(solution);
    solution.score = calculator.calculateScore();
}

}  // namespace op
