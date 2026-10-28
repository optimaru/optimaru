#include "mnt/solver.hpp"

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

#include "mnt/incremental_calculator.hpp"
#include "mnt/score_calculator.hpp"
#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"

namespace mnt
{

namespace
{

// Per-job start-date window, in startDates indices.
//
// The (crew, date) composite value range is dateCount x crewCount wide, but
// only a narrow slice of the date axis is worth proposing for a given job:
//   lo  = first start that respects minStartDate      (below it: hard 2)
//   hi  = last start that still ends by maxEndDate    (above it: hard 3)
//   top = last start that still ends by idealEndDate  (above it: soft 5, at
//         1'000'000 per day the single most expensive term in the model)
// Since endDateFor(dur, i) is nondecreasing in i, each bound is a binary
// search. `hi` only enters through the clamping of `top`, so the search
// itself proposes dates from [lo, top].
struct DateWindows
{
    std::vector<int> lo, top;
};

DateWindows buildDateWindows(const MaintenanceSchedule &schedule)
{
    const int dateCount = schedule.dateCount();
    DateWindows w;
    w.lo.reserve(schedule.jobs.size());
    w.top.reserve(schedule.jobs.size());
    for (const Job &job : schedule.jobs)
    {
        int lo = 0;
        if (job.minStartDate != kNoDate)
        {
            lo = static_cast<int>(std::lower_bound(schedule.startDates.begin(),
                                                   schedule.startDates.end(),
                                                   job.minStartDate)
                                  - schedule.startDates.begin());
        }
        // endDateByDur row of this duration: nondecreasing over the date axis.
        const std::int64_t *ends
            = schedule.endDateByDur.data()
              + static_cast<std::size_t>(job.durationInDays) * dateCount;
        auto lastEndingBy = [&](std::int64_t limit)
        {
            return static_cast<int>(
                       std::upper_bound(ends, ends + dateCount, limit) - ends)
                   - 1;
        };
        int hi = job.maxEndDate != kNoDate ? lastEndingBy(job.maxEndDate)
                                           : dateCount - 1;
        int top
            = job.idealEndDate != kNoDate ? lastEndingBy(job.idealEndDate) : hi;
        // Degenerate windows (no placement satisfies the bounds) fall back to
        // the full date axis so the search can still find the least-bad spot.
        if (lo > hi || hi < 0)
        {
            lo = 0;
            hi = dateCount - 1;
        }
        top = std::clamp(top, lo, hi);
        w.lo.push_back(lo);
        w.top.push_back(top);
    }
    return w;
}

}  // namespace

void Solver::solve(MaintenanceSchedule &schedule)
{
    ScoreCalculator::prepare(schedule);  // intern tags / build date tables
    if (schedule.jobs.empty() || schedule.crewDateCount() == 0)
    {
        ScoreCalculator::calculateScore(schedule);
        return;
    }
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }

    // Independent parallel starts (the school-timetabling pattern): worker i
    // solves its own copy with seed + i and its own calculator. The guided
    // selection hooks below read live calculator state, so the engine's
    // built-in multi-start (which shares one config across threads) cannot be
    // used. The best final score wins.
    const int workerCount = std::max(1, config_.parallelSolverCount);
    if (workerCount > 1)
    {
        std::vector<MaintenanceSchedule> candidates(workerCount, schedule);
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
        schedule = std::move(candidates[best]);
        return;
    }

    optimaru::SolverConfig engineConfig;
    engineConfig.randomSeed = config_.randomSeed;
    engineConfig.environmentMode = config_.environmentMode;
    engineConfig.onNewBest = config_.onNewBest;
    engineConfig.termination.spentLimit = config_.spentLimit;
    engineConfig.termination.bestScoreLimit = config_.bestScoreLimit;
    engineConfig.termination.unimprovedSpentLimit
        = config_.unimprovedSpentLimit;
    engineConfig.termination.stepCountLimit = config_.stepCountLimit;
    engineConfig.termination.unimprovedStepCountLimit
        = config_.unimprovedStepCountLimit;
    // lateAcceptanceSize 0 = auto-scale with the instance (see Config).
    engineConfig.localSearch.acceptor.lateAcceptanceSize
        = config_.lateAcceptanceSize != 0
              ? config_.lateAcceptanceSize
              : std::clamp<std::size_t>(8 * schedule.jobs.size(), 400, 3200);
    engineConfig.localSearch.moveSelector.changeMoveWeight
        = config_.changeMoveWeight;
    engineConfig.localSearch.moveSelector.swapMoveWeight
        = config_.swapMoveWeight;
    auto &lns = engineConfig.localSearch.ruinRecreate;
    lns.enabled = config_.ruinRecreate;
    lns.unimprovedStepTrigger = config_.ruinRecreateUnimprovedStepTrigger;
    lns.minRuinFraction = config_.ruinRecreateMinRuinFraction;
    lns.maxRuinFraction = config_.ruinRecreateMaxRuinFraction;
    lns.maxRuinCount = config_.ruinRecreateMaxRuinCount;
    lns.operatorDecay = config_.ruinRecreateOperatorDecay;
    lns.rewards = config_.ruinRecreateRewards;
    lns.polishSteps = config_.ruinRecreatePolishSteps;
    lns.asMoveWeight = config_.ruinRecreateAsMoveWeight;
    lns.asMoveMaxRuin = config_.ruinRecreateAsMoveMaxRuin;
    engineConfig.localSearch.descentWeight = config_.descentWeight;
    engineConfig.localSearch.descentMaxSweeps = config_.descentMaxSweeps;

    // The single calculator instance of this solve. Created before the engine
    // so the guided-selection hooks can bind to it; the engine resets it on
    // the working solution at solve start.
    MaintenanceSchedulingIncrementalScoreCalculator calculator;

    // Guided value selection: a change move draws the date half of the
    // composite value uniformly from [lo, top] — the job's own start window,
    // truncated at the latest start that still ends by idealEndDate — instead
    // of from the whole (date x crew) grid. That removes the two hard
    // violations and the 1'000'000-per-day afterIdeal term from the proposal
    // distribution entirely. Concentrating the draw harder on `top` (a
    // min-of-k bias) was measured clearly worse: the same start dates then
    // pile up and the tagConflict term degrades.
    std::shared_ptr<const DateWindows> windows;
    if (config_.windowValueProbability > 0.0)
    {
        windows
            = std::make_shared<const DateWindows>(buildDateWindows(schedule));
        const double probability = config_.windowValueProbability;
        const int crewCount = schedule.crewCount();
        const int freeCrewCandidates = config_.freeCrewCandidates;
        const MaintenanceSchedule *s = &schedule;
        engineConfig.localSearch.valueRecommender
            = [windows,
               probability,
               crewCount,
               freeCrewCandidates,
               s,
               &calculator](std::mt19937_64 &rng, int jobIndex) -> int
        {
            if (probability < 1.0)
            {
                std::uniform_real_distribution<double> unit(0.0, 1.0);
                if (unit(rng) >= probability)
                    return -1;  // fall back to the engine's uniform draw
            }
            const int lo = windows->lo[jobIndex];
            const int top = windows->top[jobIndex];
            const unsigned span = static_cast<unsigned>(top - lo) + 1u;
            const int dateIndex = lo + static_cast<int>(rng() % span);
            int crewIndex = static_cast<int>(rng() % crewCount);
            if (freeCrewCandidates > 0)
            {
                // Prefer a crew with no job on any day of this placement:
                // crewConflict is hard, so a colliding crew makes the whole
                // proposal dead on arrival.
                const int begin
                    = static_cast<int>(s->startDates[dateIndex] - s->dayBase);
                const int end = static_cast<int>(
                    s->endDateFor(s->jobs[jobIndex].durationInDays, dateIndex)
                    - s->dayBase);
                for (int attempt = 0; attempt < freeCrewCandidates; ++attempt)
                {
                    if (calculator.crewFree(crewIndex, begin, end))
                        break;
                    crewIndex = static_cast<int>(rng() % crewCount);
                }
            }
            return dateIndex * crewCount + crewIndex;
        };
    }

    // Violation-guided entity selection: with probability hotJobProbability
    // the moved job is drawn from the jobs currently sitting in a tagConflict
    // — the dominant soft term. A hot (tag, day) cell is picked from the
    // calculator's live hot set, then a job carrying that tag and covering
    // that day. Once tagConflict is 0 the hot set is empty and this falls
    // back to uniform exploration by itself.
    std::shared_ptr<const std::vector<std::vector<int>>> tagJobs;
    if (config_.hotJobProbability > 0.0 && schedule.tagCount > 0)
    {
        auto lists = std::make_shared<std::vector<std::vector<int>>>(
            schedule.tagCount);
        for (int j = 0; j < static_cast<int>(schedule.jobs.size()); ++j)
        {
            for (int tag : schedule.jobs[j].tagIds)
                (*lists)[tag].push_back(j);
        }
        tagJobs = lists;
        // Integer coin flip: this hook runs on every move, so the probability
        // test is a single 64-bit compare instead of a double distribution.
        const std::uint64_t hotThreshold = static_cast<std::uint64_t>(
            config_.hotJobProbability
            * static_cast<double>(std::numeric_limits<std::uint64_t>::max()));
        const MaintenanceSchedule *s = &schedule;
        engineConfig.localSearch.entityRecommender =
            [&calculator, tagJobs, hotThreshold, s](std::mt19937_64 &rng) -> int
        {
            // Cheap first: with no tagConflict there is nothing to guide
            // toward.
            if (!calculator.hasHotTagCells() || rng() >= hotThreshold)
                return -1;
            const int cell = calculator.hotTagCell(rng);
            const int tag = cell / s->dayCount;  // one division, not two
            const std::int64_t day = s->dayBase + (cell - tag * s->dayCount);
            const std::vector<int> &members = (*tagJobs)[tag];
            // At least two members cover this day (that is what made the cell
            // hot), so a few draws find one; giving up returns -1 and the
            // engine falls back to a uniform entity.
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                const int j = members[rng() % members.size()];
                const int crewDate = s->jobs[j].crewDate;
                if (crewDate < 0)
                    continue;
                const int dateIndex = s->dateIndexOf(crewDate);
                if (s->startDates[dateIndex] <= day
                    && day < s->endDateFor(s->jobs[j].durationInDays,
                                           dateIndex))
                    return j;
            }
            return -1;
        };
    }

    // Construction heuristic + late-acceptance local search (+ ALNS kicks).
    optimaru::Solver<MaintenanceSchedule> engine(std::move(engineConfig));
    engine.solve(schedule, calculator);

    // The authoritative final score: a full recompute on the best solution.
    schedule.score = ScoreCalculator::calculateScore(schedule);
}

void Solver::solve(
    MaintenanceSchedule &schedule,
    optimaru::DecomposedScoreCalculator<MaintenanceSchedule> &calculator)
{
    ScoreCalculator::prepare(schedule);  // intern tags / build date tables
    if (config_.ruinRecreateRewards.size() != 4)
    {
        throw std::invalid_argument(
            "ruinRecreateRewards must contain exactly 4 values");
    }
    if (!schedule.jobs.empty() && schedule.crewDateCount() != 0)
    {
        // No guided value/entity hooks here: both read the monolithic
        // calculator's live occupancy and hot-cell state, which a
        // caller-assembled plugin does not expose. The built-in move mix plus
        // the ALNS kicks remain.
        optimaru::SolverConfig engineConfig;
        engineConfig.randomSeed = config_.randomSeed;
        engineConfig.environmentMode = config_.environmentMode;
        engineConfig.onNewBest = config_.onNewBest;
        engineConfig.termination.spentLimit = config_.spentLimit;
        engineConfig.termination.bestScoreLimit = config_.bestScoreLimit;
        engineConfig.termination.unimprovedSpentLimit
            = config_.unimprovedSpentLimit;
        engineConfig.termination.stepCountLimit = config_.stepCountLimit;
        engineConfig.termination.unimprovedStepCountLimit
            = config_.unimprovedStepCountLimit;
        engineConfig.localSearch.acceptor.lateAcceptanceSize
            = config_.lateAcceptanceSize != 0
                  ? config_.lateAcceptanceSize
                  : std::clamp<std::size_t>(
                        8 * schedule.jobs.size(), 400, 3200);
        engineConfig.localSearch.moveSelector.changeMoveWeight
            = config_.changeMoveWeight;
        engineConfig.localSearch.moveSelector.swapMoveWeight
            = config_.swapMoveWeight;
        auto &lns = engineConfig.localSearch.ruinRecreate;
        lns.enabled = config_.ruinRecreate;
        lns.unimprovedStepTrigger = config_.ruinRecreateUnimprovedStepTrigger;
        lns.minRuinFraction = config_.ruinRecreateMinRuinFraction;
        lns.maxRuinFraction = config_.ruinRecreateMaxRuinFraction;
        lns.maxRuinCount = config_.ruinRecreateMaxRuinCount;
        lns.operatorDecay = config_.ruinRecreateOperatorDecay;
        lns.rewards = config_.ruinRecreateRewards;
        lns.polishSteps = config_.ruinRecreatePolishSteps;
        lns.asMoveWeight = config_.ruinRecreateAsMoveWeight;
        lns.asMoveMaxRuin = config_.ruinRecreateAsMoveMaxRuin;
        engineConfig.localSearch.descentWeight = config_.descentWeight;
        engineConfig.localSearch.descentMaxSweeps = config_.descentMaxSweeps;

        optimaru::Solver<MaintenanceSchedule> engine(std::move(engineConfig));
        engine.solve(schedule, calculator);
    }

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score.
    calculator.resetWorkingSolution(schedule);
    schedule.score = calculator.calculateScore();
}

}  // namespace mnt
