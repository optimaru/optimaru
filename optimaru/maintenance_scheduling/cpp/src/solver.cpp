#include "mnt/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "mnt/incremental_calculator.hpp"
#include "mnt/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/worker_threads.hpp"

namespace mnt
{

namespace
{

// Per-job start-date window, in startDates indices.
//
// The (crew, date) composite value range is dateCount x crewCount wide, but
// only a narrow slice of the date axis is worth proposing for a given job:
//   lo  = first start that respects minStartDate      (below it: hard 2,
//         minStartDateCost)
//   hi  = last start that still ends by maxEndDate    (above it: hard 3,
//         maxEndDateCost)
//   top = last start that still ends by idealEndDate  (above it: soft 5,
//         afterIdealEndDateCost, at 1'000'000 per day the single most
//         expensive term in the model)
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

// Shared engine wiring for both solve() overloads: termination, late
// acceptance, the assignment move mix and the ALNS knobs.
optimaru::Settings buildEngineConfig(const Solver::Config &config,
                                         const MaintenanceSchedule &schedule)
{
    optimaru::Settings engineConfig;
    engineConfig.seed = config.seed;
    engineConfig.score_checks = config.score_checks;
    engineConfig.on_new_best = config.on_new_best;
    engineConfig.limits.time_limit = config.time_limit;
    engineConfig.limits.target_score = config.target_score;
    engineConfig.limits.stall_time_limit = config.stall_time_limit;
    engineConfig.limits.step_limit = config.step_limit;
    engineConfig.limits.stall_step_limit
        = config.stall_step_limit;
    // history_length 0 = auto-scale with the instance (see Config).
    engineConfig.search.acceptance.history_length
        = config.history_length != 0
              ? config.history_length
              : std::clamp<std::size_t>(8 * schedule.jobs.size(), 400, 3200);
    engineConfig.search.moves.change_weight
        = config.change_weight;
    engineConfig.search.moves.swap_weight
        = config.swap_weight;
    auto &lns = engineConfig.search.ruin_recreate;
    lns.enabled = config.ruin_recreate;
    lns.unimproved_step_trigger = config.ruinRecreateUnimprovedStepTrigger;
    lns.min_ruin_fraction = config.ruinRecreateMinRuinFraction;
    lns.max_ruin_fraction = config.ruinRecreateMaxRuinFraction;
    lns.max_ruin_count = config.ruinRecreateMaxRuinCount;
    lns.operator_decay = config.ruinRecreateOperatorDecay;
    lns.rewards = config.ruinRecreateRewards;
    lns.polish_steps = config.ruinRecreatePolishSteps;
    lns.as_move_weight = config.ruinRecreateAsMoveWeight;
    lns.as_move_max_ruin = config.ruinRecreateAsMoveMaxRuin;
    engineConfig.search.descent_weight = config.descent_weight;
    engineConfig.search.descent_max_sweeps = config.descent_max_sweeps;
    return engineConfig;
}

}  // namespace

void Solver::solve(MaintenanceSchedule &schedule)
{
    ScoreCalculator::prepare(schedule);  // intern tags / build date tables
    if (schedule.jobs.empty() || schedule.crewDateCount() == 0)
    {
        ScoreCalculator::score(schedule);
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
        // Raised by the first worker that fails: every worker's engine checks
        // it once a step, so the others stop instead of using up their budget.
        auto stop = std::make_shared<std::atomic<bool>>(false);
        // A worker that cannot be started (std::system_error) stops and joins
        // the ones already running before the error is rethrown here.
        optimaru::run_worker_threads(
            workerCount, *stop,
            [this, &candidates, &failures, &stop](int i)
            {
                try
                {
                    Config workerConfig = config_;
                    workerConfig.parallelSolverCount = 1;
                    workerConfig.stop_flag = stop;
                    workerConfig.seed
                        += static_cast<std::uint64_t>(i);
                    if (i != 0)
                        workerConfig.on_new_best = nullptr;
                    Solver worker(std::move(workerConfig));
                    worker.solve(candidates[i]);
                }
                catch (...)
                {
                    failures[i] = std::current_exception();
                    stop->store(true);
                }
            });
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

    optimaru::Settings engineConfig = buildEngineConfig(config_, schedule);

    // The single calculator instance of this solve. Created before the engine
    // so the guided-selection hooks can bind to it; the engine resets it on
    // the working solution at solve start.
    MaintenanceSchedulingIncrementalScoreCalculator calculator;

    // Guided value selection: a change move draws the date half of the
    // composite value uniformly from [lo, top] — the job's own start window,
    // truncated at the latest start that still ends by idealEndDate — instead
    // of from the whole (date x crew) grid. That removes the two hard
    // violations and the 1'000'000-per-day afterIdealEndDateCost term from
    // the proposal distribution entirely. Concentrating the draw harder on
    // `top` (a min-of-k bias) was measured clearly worse: the same start
    // dates then pile up and the tagConflictCost term degrades.
    std::shared_ptr<const DateWindows> windows;
    if (config_.windowValueProbability > 0.0)
    {
        windows
            = std::make_shared<const DateWindows>(buildDateWindows(schedule));
        const double probability = config_.windowValueProbability;
        const int crewCount = schedule.crewCount();
        const int freeCrewCandidates = config_.freeCrewCandidates;
        const MaintenanceSchedule *s = &schedule;
        engineConfig.search.value_hint
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
                // crewConflictCost is hard, so a colliding crew makes the whole
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
    // the moved job is drawn from the jobs currently sitting in a
    // tagConflictCost — the dominant soft term. A hot (tag, day) cell is
    // picked from the calculator's live hot set, then a job carrying that tag
    // and covering that day. Once tagConflictCost is 0 the hot set is empty
    // and this falls back to uniform exploration by itself.
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
        engineConfig.search.entity_hint =
            [&calculator, tagJobs, hotThreshold, s](std::mt19937_64 &rng) -> int
        {
            // Cheap first: with no tagConflictCost there is nothing to guide
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
    if (config_.stop_flag)
        engine.share_stop_flag(config_.stop_flag);
    engine.solve(schedule, calculator);

    // The authoritative final score: a full recompute on the best solution.
    schedule.score = ScoreCalculator::score(schedule);
}

void Solver::solve(
    MaintenanceSchedule &schedule,
    optimaru::DecomposedObjective<MaintenanceSchedule> &calculator)
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
        optimaru::Settings engineConfig
            = buildEngineConfig(config_, schedule);

        optimaru::Solver<MaintenanceSchedule> engine(std::move(engineConfig));
        if (config_.stop_flag)
            engine.share_stop_flag(config_.stop_flag);
        engine.solve(schedule, calculator);
    }

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraint_totals() explains exactly this score.
    calculator.rebuild(schedule);
    schedule.score = calculator.score();
}

}  // namespace mnt
