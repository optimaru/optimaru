#include "fp/solver.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <utility>

#include "fp/incremental_calculator.hpp"
#include "fp/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"

namespace fp {

namespace {

// Raw cleaning-window overlap minutes between every pair of lines (operator
// agnostic), from the current job lists. overlap[i][j] is what the
// operatorCleaningConflictCost constraint would charge if lines i and j
// shared an operator.
std::vector<std::vector<std::int64_t>> lineOverlapMatrix(PackagingSchedule& schedule) {
    ScoreCalculator::updateShadowVariables(schedule);
    const int lineCount = schedule.lineCount();
    std::vector<std::vector<std::pair<Minutes, Minutes>>> windows(lineCount);
    for (int l = 0; l < lineCount; ++l) {
        for (const int j : schedule.lines[l].jobs) {
            const Job& job = schedule.jobs[j];
            if (job.startCleaningDateTime < job.startProductionDateTime)
                windows[l].emplace_back(job.startCleaningDateTime, job.startProductionDateTime);
        }
    }
    std::vector<std::vector<std::int64_t>> overlap(lineCount,
                                                   std::vector<std::int64_t>(lineCount, 0));
    for (int a = 0; a < lineCount; ++a) {
        for (int b = a + 1; b < lineCount; ++b) {
            std::int64_t total = 0;
            for (const auto& [aStart, aEnd] : windows[a]) {
                for (const auto& [bStart, bEnd] : windows[b]) {
                    const Minutes start = std::max(aStart, bStart);
                    const Minutes end = std::min(aEnd, bEnd);
                    if (start < end) total += end - start;
                }
            }
            overlap[a][b] = total;
            overlap[b][a] = total;
        }
    }
    return overlap;
}

bool anySharedOperator(const PackagingSchedule& schedule) {
    const int lineCount = schedule.lineCount();
    for (int a = 0; a < lineCount; ++a) {
        for (int b = a + 1; b < lineCount; ++b) {
            if (schedule.lines[a].operator_index == schedule.lines[b].operator_index) return true;
        }
    }
    return false;
}

// The plugin path's Score: the calculator's three accumulators before the
// medium fold (level_scores()), each level rounded on its own. Decoding the
// folded engine soft instead would file a soft total of 10^10 or more under
// medium.
Score levelScoreOf(const optimaru::DecomposedObjective<PackagingSchedule>& calculator) {
    const auto levels = calculator.level_scores();
    return Score{static_cast<std::int64_t>(levels.hard),
                 optimaru::round_score_level(levels.medium, "medium"),
                 optimaru::round_score_level(levels.soft, "soft")};
}

}  // namespace

void assignMissingOperators(PackagingSchedule& schedule) {
    const int operatorCount = static_cast<int>(schedule.operators.size());
    if (operatorCount == 0) return;
    std::vector<int> used(operatorCount, 0);
    for (const Line& line : schedule.lines) {
        if (line.operator_index >= 0) ++used[line.operator_index];
    }
    for (Line& line : schedule.lines) {
        if (line.operator_index >= 0) continue;
        int best = 0;
        for (int o = 1; o < operatorCount; ++o) {
            if (used[o] < used[best]) best = o;
        }
        line.operator_index = best;
        ++used[best];
    }
}

bool improveOperatorAssignment(PackagingSchedule& schedule) {
    const int operatorCount = static_cast<int>(schedule.operators.size());
    const int lineCount = schedule.lineCount();
    if (operatorCount == 0 || lineCount < 2) return false;

    const auto overlap = lineOverlapMatrix(schedule);
    // Conflict charged to line l against the other lines under `assign`.
    auto lineCost = [&](const std::vector<int>& assign, int l, int op) {
        std::int64_t cost = 0;
        for (int m = 0; m < lineCount; ++m) {
            if (m != l && assign[m] == op) cost += overlap[l][m];
        }
        return cost;
    };

    std::vector<int> assign(lineCount);
    for (int l = 0; l < lineCount; ++l) assign[l] = schedule.lines[l].operator_index;

    bool changedAny = false;
    for (bool improved = true; improved;) {
        improved = false;
        for (int l = 0; l < lineCount; ++l) {
            if (assign[l] < 0) continue;  // no operators were ever assigned to this line
            const std::int64_t current = lineCost(assign, l, assign[l]);
            int bestOp = assign[l];
            std::int64_t bestCost = current;
            for (int o = 0; o < operatorCount; ++o) {
                if (o == assign[l]) continue;
                const std::int64_t cost = lineCost(assign, l, o);
                if (cost < bestCost) {
                    bestCost = cost;
                    bestOp = o;
                }
            }
            if (bestOp != assign[l]) {
                assign[l] = bestOp;
                improved = true;
                changedAny = true;
            }
        }
    }
    for (int l = 0; l < lineCount; ++l) schedule.lines[l].operator_index = assign[l];
    return changedAny;
}

void Solver::solve(PackagingSchedule& schedule) {
    ScoreCalculator::prepare(schedule);
    if (config_.ruinRecreateRewards.size() != 4) {
        throw std::invalid_argument("ruinRecreateRewards must contain exactly 4 values");
    }
    assignMissingOperators(schedule);

    const int movableJobs = static_cast<int>(schedule.engineToJob.size());
    if (schedule.jobs.empty() || schedule.lines.empty() || movableJobs == 0) {
        // Nothing searchable: score the instance as-is (with no
        // lines the jobs simply stay unassigned).
        ScoreCalculator::score(schedule);
        return;
    }

    // Auto rounds (R4): on the shared-operator bench 5 legs beat 3 (mean
    // medium -350 -> -163, 5W/1L over 6 seeds) and 2 (head-to-head 4/6) —
    // more alternations mean more operator re-optimizations and more
    // acceptor restarts within the same wall budget.
    const int rounds =
            config_.operatorRounds > 0
                    ? config_.operatorRounds
                    : (anySharedOperator(schedule) && !schedule.operators.empty() ? 5 : 1);
    const auto deadline = std::chrono::steady_clock::now() + config_.time_limit;

    for (int round = 0; round < rounds; ++round) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds(0)) break;

        optimaru::Settings engineConfig;
        engineConfig.seed = config_.seed + static_cast<std::uint64_t>(round) *
                                                   static_cast<std::uint64_t>(std::max(
                                                           1, config_.parallelSolverCount));
        engineConfig.score_checks = config_.score_checks;
        engineConfig.parallel_starts = std::max(1, config_.parallelSolverCount);
        if (config_.on_new_best) {
            auto callback = config_.on_new_best;
            engineConfig.on_new_best = [callback](std::int64_t step, const optimaru::Score& score) {
                callback(step, fromEngineScore(score));
            };
        }
        engineConfig.limits.time_limit = remaining / (rounds - round);
        engineConfig.limits.target_score = config_.target_score;
        engineConfig.limits.stall_time_limit = config_.stall_time_limit;
        engineConfig.limits.step_limit = config_.step_limit;
        engineConfig.limits.stall_step_limit = config_.stall_step_limit;
        engineConfig.search.acceptance.history_length =
                config_.history_length != 0
                        ? config_.history_length
                        : std::clamp<std::size_t>(8 * static_cast<std::size_t>(movableJobs), 400,
                                                  3200);
        auto& moves = engineConfig.search.moves;
        moves.relocate_weight = config_.relocate_weight;
        moves.list_swap_weight = config_.list_swap_weight;
        moves.two_opt_weight = config_.two_opt_weight;
        moves.or_opt_weight = config_.or_opt_weight;
        moves.tail_swap_weight = config_.tail_swap_weight;
        if (config_.nearbyProbability > 0 && movableJobs >= 2) {
            engineConfig.search.nearby.probability = config_.nearbyProbability;
            engineConfig.search.nearby.size = config_.nearbySize;
            // Element ids are engine ids; the meter reads only immutable
            // problem facts (products, cleanup matrix, id mapping), so the
            // lambda stays valid across the engine's worker copies. Missing
            // cleanup pairs rank as "very far" instead of throwing (the
            // meter must never evaluate score semantics).
            engineConfig.search.nearby.distance = [&schedule](int a, int b) {
                const Job& jobA = schedule.jobs[schedule.engineToJob[a]];
                const Job& jobB = schedule.jobs[schedule.engineToJob[b]];
                const Minutes ab = schedule.cleanup(jobA.productIndex, jobB.productIndex);
                const Minutes ba = schedule.cleanup(jobB.productIndex, jobA.productIndex);
                const double changeover = (ab == kMissingCleanup || ba == kMissingCleanup)
                                                  ? 1e9
                                                  : static_cast<double>(ab + ba);
                const double idealGap =
                        static_cast<double>(std::llabs(jobA.idealEndTime - jobB.idealEndTime));
                return changeover * 1e6 + idealGap;
            };
        }
        auto& lns = engineConfig.search.ruin_recreate;
        lns.enabled = config_.ruin_recreate && movableJobs >= 4;
        lns.unimproved_step_trigger = config_.ruinRecreateUnimprovedStepTrigger;
        lns.min_ruin_fraction = config_.ruinRecreateMinRuinFraction;
        lns.max_ruin_fraction = config_.ruinRecreateMaxRuinFraction;
        lns.max_ruin_count = config_.ruinRecreateMaxRuinCount;
        lns.operator_decay = config_.ruinRecreateOperatorDecay;
        lns.rewards = config_.ruinRecreateRewards;
        lns.polish_steps = config_.ruinRecreatePolishSteps;

        // Construction heuristic (cheapest insertion over lines + the
        // unassigned pool) + late-acceptance local search with ALNS kicks;
        // each start gets its own calculator instance (they are stateful).
        auto engine = optimaru::Solver<PackagingSchedule>(std::move(engineConfig));
        const optimaru::Score best = engine.solve_multistart(schedule, [] {
            return std::make_unique<FoodPackagingIncrementalScoreCalculator>();
        });

        if (best >= config_.target_score) break;
        // Keep running the remaining legs even when the operator assignment
        // reaches a fixed point: a leg still searches the job lists (fresh
        // acceptor + reseeded RNG = a restart-continuation), and breaking
        // here would abandon the remaining wall budget — on a shared-
        // operator instance the alternation hits the fixed point after leg 1
        // and would return with 2/3 of the time limit unused.
        if (round < rounds - 1) improveOperatorAssignment(schedule);
    }

    // Free posterior step: with the job lists fixed, a better operator
    // assignment can only reduce the conflict penalty (no-op when injective).
    improveOperatorAssignment(schedule);

    // The authoritative final score: a full recompute on the best solution
    // (also refreshes the jobs' derived fields for the caller).
    schedule.score = ScoreCalculator::score(schedule);
}

void Solver::solve(PackagingSchedule& schedule,
                   optimaru::DecomposedObjective<PackagingSchedule>& calculator) {
    ScoreCalculator::prepare(schedule);
    if (config_.ruinRecreateRewards.size() != 4) {
        throw std::invalid_argument("ruinRecreateRewards must contain exactly 4 values");
    }
    assignMissingOperators(schedule);

    const int movableJobs = static_cast<int>(schedule.engineToJob.size());
    if (!schedule.jobs.empty() && !schedule.lines.empty() && movableJobs != 0) {
        // One search leg, one start: the operator-alternation rounds and the
        // multi-start workers both need calculators of their own, and a
        // caller-assembled plugin cannot be cloned. The move mix, the
        // changeover-nearby selection and the ALNS kicks are the same.
        optimaru::Settings engineConfig;
        engineConfig.seed = config_.seed;
        engineConfig.score_checks = config_.score_checks;
        engineConfig.parallel_starts = 1;
        if (config_.on_new_best) {
            // Report the same three levels as the final schedule.score below.
            // The engine calls on_new_best right after the working solution
            // became the new best, so the calculator's accumulators hold
            // exactly that solution; the folded engine score it passes is not
            // decoded.
            auto callback = config_.on_new_best;
            engineConfig.on_new_best = [callback, &calculator](std::int64_t step,
                                                               const optimaru::Score&) {
                callback(step, levelScoreOf(calculator));
            };
        }
        engineConfig.limits.time_limit = config_.time_limit;
        engineConfig.limits.target_score = config_.target_score;
        engineConfig.limits.stall_time_limit = config_.stall_time_limit;
        engineConfig.limits.step_limit = config_.step_limit;
        engineConfig.limits.stall_step_limit = config_.stall_step_limit;
        engineConfig.search.acceptance.history_length =
                config_.history_length != 0
                        ? config_.history_length
                        : std::clamp<std::size_t>(8 * static_cast<std::size_t>(movableJobs), 400,
                                                  3200);
        auto& moves = engineConfig.search.moves;
        moves.relocate_weight = config_.relocate_weight;
        moves.list_swap_weight = config_.list_swap_weight;
        moves.two_opt_weight = config_.two_opt_weight;
        moves.or_opt_weight = config_.or_opt_weight;
        moves.tail_swap_weight = config_.tail_swap_weight;
        if (config_.nearbyProbability > 0 && movableJobs >= 2) {
            engineConfig.search.nearby.probability = config_.nearbyProbability;
            engineConfig.search.nearby.size = config_.nearbySize;
            engineConfig.search.nearby.distance = [&schedule](int a, int b) {
                const Job& jobA = schedule.jobs[schedule.engineToJob[a]];
                const Job& jobB = schedule.jobs[schedule.engineToJob[b]];
                const Minutes ab = schedule.cleanup(jobA.productIndex, jobB.productIndex);
                const Minutes ba = schedule.cleanup(jobB.productIndex, jobA.productIndex);
                const double changeover = (ab == kMissingCleanup || ba == kMissingCleanup)
                                                  ? 1e9
                                                  : static_cast<double>(ab + ba);
                const double idealGap =
                        static_cast<double>(std::llabs(jobA.idealEndTime - jobB.idealEndTime));
                return changeover * 1e6 + idealGap;
            };
        }
        auto& lns = engineConfig.search.ruin_recreate;
        lns.enabled = config_.ruin_recreate && movableJobs >= 4;
        lns.unimproved_step_trigger = config_.ruinRecreateUnimprovedStepTrigger;
        lns.min_ruin_fraction = config_.ruinRecreateMinRuinFraction;
        lns.max_ruin_fraction = config_.ruinRecreateMaxRuinFraction;
        lns.max_ruin_count = config_.ruinRecreateMaxRuinCount;
        lns.operator_decay = config_.ruinRecreateOperatorDecay;
        lns.rewards = config_.ruinRecreateRewards;
        lns.polish_steps = config_.ruinRecreatePolishSteps;

        auto engine = optimaru::Solver<PackagingSchedule>(std::move(engineConfig));
        engine.solve(schedule, calculator);
    }

    // Refresh the jobs' derived fields on the best solution (the plugin
    // components derive their own chain and never write these), then score it
    // with the caller's constraint assembly (custom weights and levels
    // included), leaving the calculator primed on it so constraint_totals()
    // explains exactly this score. The three levels come straight from the
    // calculator's accumulators (levelScoreOf() above, like every on_new_best
    // report) rather than from decoding the folded engine score, so a demoted
    // or re-weighted term lands on the level the assembly gave it.
    ScoreCalculator::updateShadowVariables(schedule);
    calculator.rebuild(schedule);
    schedule.score = levelScoreOf(calculator);
}

}  // namespace fp
