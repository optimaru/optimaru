#include "fab/solver.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "fab/decoder.hpp"
#include "fab/incremental_calculator.hpp"
#include "fab/moves/priority_swap.hpp"
#include "fab/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/accept/acceptors.hpp"
#include "optimaru/worker_threads.hpp"

namespace fab
{

void beginSearch(SemiconductorSchedule &schedule)
{
    ScoreCalculator::prepare(schedule);
    const int n = schedule.rowCount();
    const std::vector<int> choices = defaultChoices(schedule);
    std::vector<int> selected;
    std::vector<int> ranks;
    if (schedule.warmStart)
    {
        seedState(schedule, choices, selected, ranks);
    }
    else
    {
        selected = choices;
        ranks.resize(static_cast<std::size_t>(n));
        std::iota(ranks.begin(), ranks.end(), 0);
    }
    for (int r = 0; r < n; ++r)
    {
        schedule.rows[static_cast<std::size_t>(r)].value
            = schedule.valueOf(ranks[static_cast<std::size_t>(r)], selected[static_cast<std::size_t>(r)]);
    }
    // A complete seed (every operation assigned; the list is validated
    // unique) stands for itself while the values stay at the seed state.
    schedule.seedComplete = schedule.warmStart && static_cast<int>(schedule.assignments.size()) == n;
    schedule.seedValues.clear();
    schedule.seedAssignments.clear();
    if (schedule.seedComplete)
    {
        for (const OperationRow &row : schedule.rows)
            schedule.seedValues.push_back(row.value);
        schedule.seedAssignments = schedule.assignments;
    }
    schedule.searching = true;
}

void endSearch(SemiconductorSchedule &schedule)
{
    bool seed = schedule.seedComplete && schedule.seedValues.size() == schedule.rows.size();
    for (std::size_t r = 0; seed && r < schedule.rows.size(); ++r)
        seed = schedule.rows[r].value == schedule.seedValues[r];
    schedule.assignments = seed ? schedule.seedAssignments : decodeValues(schedule);
    schedule.searching = false;
    schedule.warmStart = false;  // a seed is used once
    schedule.seedComplete = false;
    schedule.seedValues.clear();
    schedule.seedAssignments.clear();
    ScoreCalculator::score(schedule);
}

namespace
{

// Leaves search mode without touching the assignment list, for a solve that
// failed after beginSearch() (for example the decoder giving up). Otherwise
// the schedule would stay in search mode, and the plugin path would re-decode
// the decision values while the monolithic calculator scores `assignments`.
void abandonSearch(SemiconductorSchedule &schedule)
{
    schedule.searching = false;
    schedule.seedComplete = false;
    schedule.seedValues.clear();
    schedule.seedAssignments.clear();
}

void validate(const Solver::Config &config)
{
    if (config.ruinRecreateRewards.size() != 4)
        throw std::invalid_argument("ruinRecreateRewards must contain exactly 4 values");
    if (config.parallelSolverCount < 1)
        throw std::invalid_argument("parallelSolverCount must be positive");
    if (config.machineMoveProbability < 0.0 || config.machineMoveProbability > 1.0)
        throw std::invalid_argument("machineMoveProbability must be in [0, 1]");
    if (!(config.simulatedAnnealingTemperatureFraction > 0.0))
        throw std::invalid_argument("simulatedAnnealingTemperatureFraction must be > 0");
}

// The change move's value: another machine for the row (rank kept), or a
// small priority shift (machine kept). -1 = the engine's uniform draw.
std::function<int(std::mt19937_64 &, int)> value_hint(const SemiconductorSchedule *working,
                                                            double machineMoveProbability)
{
    return [working, machineMoveProbability](std::mt19937_64 &rng, int row) -> int
    {
        const SemiconductorSchedule &s = *working;
        const int value = s.rows[static_cast<std::size_t>(row)].value;
        if (value < 0)
            return -1;
        const int rank = s.rankOf(value);
        const int slot = s.slotOf(value);
        const int options = s.optionCountOfRow(row);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        if (options > 1 && unit(rng) < machineMoveProbability)
        {
            const int shift = 1 + static_cast<int>(rng() % static_cast<std::uint64_t>(options - 1));
            return s.valueOf(rank, (slot % options + shift) % options);
        }
        const int n = s.rowCount();
        if (n < 2)
            return -1;
        int magnitude = 1;  // geometric 1..8
        while ((rng() & 1u) != 0 && magnitude < 8)
            ++magnitude;
        const int sign = (rng() & 1u) != 0 ? 1 : -1;
        int shifted = std::clamp(rank + sign * magnitude, 0, n - 1);
        if (shifted == rank)
            shifted = std::clamp(rank - sign * magnitude, 0, n - 1);
        return shifted == rank ? -1 : s.valueOf(shifted, slot);
    };
}

}  // namespace

void Solver::runSingle(SemiconductorSchedule &schedule,
                       optimaru::Objective<SemiconductorSchedule> &calculator) const
{
    optimaru::Settings engineConfig;
    engineConfig.seed = config_.seed;
    engineConfig.score_checks = config_.score_checks;
    engineConfig.on_new_best = config_.on_new_best;
    engineConfig.limits.time_limit = config_.time_limit;
    engineConfig.limits.target_score = config_.target_score;
    engineConfig.limits.stall_time_limit = config_.stall_time_limit;
    engineConfig.limits.step_limit = config_.step_limit;
    engineConfig.limits.stall_step_limit = config_.stall_step_limit;

    auto &acceptor = engineConfig.search.acceptance;
    acceptor.rule = config_.rule;
    acceptor.history_length = config_.history_length != 0
                                      ? config_.history_length
                                      : std::clamp<std::size_t>(16 * schedule.rows.size(), 400, 3200);
    acceptor.start_temperature = config_.start_temperature;

    engineConfig.search.moves.change_weight = config_.change_weight;
    engineConfig.search.moves.swap_weight = config_.swap_weight;
    engineConfig.search.custom_move_probability = config_.custom_move_probability;
    // The schedule outlives the engine run: the recommender reads the
    // working values through it.
    engineConfig.search.value_hint = value_hint(&schedule, config_.machineMoveProbability);

    auto &lns = engineConfig.search.ruin_recreate;
    lns.enabled = config_.ruin_recreate;
    lns.unimproved_step_trigger = config_.ruinRecreateUnimprovedStepTrigger;
    lns.min_ruin_fraction = config_.ruinRecreateMinRuinFraction;
    lns.max_ruin_fraction = config_.ruinRecreateMaxRuinFraction;
    lns.max_ruin_count = config_.ruinRecreateMaxRuinCount;
    lns.operator_decay = config_.ruinRecreateOperatorDecay;
    lns.rewards = config_.ruinRecreateRewards;
    lns.polish_steps = config_.ruinRecreatePolishSteps;

    const bool autoTemperature = config_.rule == optimaru::AcceptanceRule::SimulatedAnnealing
                                 && config_.start_temperature.hard == 0
                                 && config_.start_temperature.soft == 0.0;
    optimaru::Solver<SemiconductorSchedule> engine(std::move(engineConfig));
    if (config_.stop_flag)
        engine.share_stop_flag(config_.stop_flag);
    if (autoTemperature)
    {
        const double fraction = config_.simulatedAnnealingTemperatureFraction;
        engine.set_acceptor_factory(
            [fraction](const optimaru::Score &start) -> std::unique_ptr<optimaru::Acceptor>
            {
                const double soft = std::max(1.0, fraction * std::fabs(start.soft));
                return std::make_unique<optimaru::SimulatedAnnealing>(optimaru::Score{0, soft});
            });
    }
    if (config_.prioritySwapMoves)
        engine.add_move_operator(std::make_unique<PrioritySwapMove>());
    engine.solve(schedule, calculator);
}

void Solver::solve(SemiconductorSchedule &schedule)
{
    validate(config_);
    beginSearch(schedule);
    try
    {
        searchFromBegun(schedule);
    }
    catch (...)
    {
        abandonSearch(schedule);
        throw;
    }
}

void Solver::searchFromBegun(SemiconductorSchedule &schedule)
{
    if (schedule.rows.empty())
    {
        endSearch(schedule);
        return;
    }
    const int workerCount = config_.parallelSolverCount;
    if (workerCount > 1)
    {
        // Domain-level starts: the value recommender and the priority swap
        // read the working schedule, so every start needs its own copy.
        std::vector<SemiconductorSchedule> candidates(static_cast<std::size_t>(workerCount), schedule);
        std::vector<Score> scores(static_cast<std::size_t>(workerCount));
        std::vector<std::exception_ptr> failures(static_cast<std::size_t>(workerCount));
        // Raised by the first worker that fails: every worker's engine checks
        // it once a step, so the others stop instead of using up their budget.
        auto stop = std::make_shared<std::atomic<bool>>(false);
        // A worker that cannot be started (std::system_error) stops and joins
        // the ones already running before the error is rethrown here.
        optimaru::run_worker_threads(
            workerCount, *stop,
            [this, &candidates, &scores, &failures, &stop](int i)
            {
                try
                {
                    Config workerConfig = config_;
                    workerConfig.parallelSolverCount = 1;
                    workerConfig.stop_flag = stop;
                    workerConfig.seed += static_cast<std::uint64_t>(i);
                    if (i != 0)
                        workerConfig.on_new_best = nullptr;
                    SemiconductorIncrementalScoreCalculator calculator;
                    SemiconductorSchedule &copy = candidates[static_cast<std::size_t>(i)];
                    Solver(std::move(workerConfig)).runSingle(copy, calculator);
                    scores[static_cast<std::size_t>(i)] = ScoreCalculator::searchStateScore(copy);
                }
                catch (...)
                {
                    failures[static_cast<std::size_t>(i)] = std::current_exception();
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
            if (scores[static_cast<std::size_t>(i)] > scores[static_cast<std::size_t>(best)])
                best = i;
        }
        schedule = std::move(candidates[static_cast<std::size_t>(best)]);
    }
    else
    {
        SemiconductorIncrementalScoreCalculator calculator;
        runSingle(schedule, calculator);
    }
    endSearch(schedule);
}

void Solver::solve(SemiconductorSchedule &schedule,
                   optimaru::DecomposedObjective<SemiconductorSchedule> &calculator)
{
    validate(config_);
    beginSearch(schedule);
    try
    {
        if (!schedule.rows.empty())
            runSingle(schedule, calculator);
        endSearch(schedule);  // assignments + the default totals
    }
    catch (...)
    {
        abandonSearch(schedule);
        throw;
    }
    // Score with the caller's assembly, in data mode, and leave the
    // calculator primed on the result.
    calculator.rebuild(schedule);
    schedule.score = calculator.score();
}

}  // namespace fab
