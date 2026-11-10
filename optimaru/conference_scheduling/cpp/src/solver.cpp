#include "cs/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "cs/incremental_calculator.hpp"
#include "cs/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"

namespace cs {

namespace {

// Shared engine wiring for both solve() overloads: termination, late
// acceptance, the move mix, the ALNS knobs and whatever guided selection the
// domain config asks for.
optimaru::Settings buildEngineConfig(const Solver::Config& config,
                                     const ConferenceSchedule& schedule) {
    optimaru::Settings engineConfig = optimaru::settings_from(config);
    engineConfig.parallel_starts = 1;  // parallel starts are fanned out above
    // history_length 0 = auto-scale with the instance (see Config).
    engineConfig.search.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(8 * schedule.talks.size(), 800, 3200);
    engineConfig.search.moves.change_weight = config.change_weight;
    engineConfig.search.moves.swap_weight = config.swap_weight;
    // as_move_weight < 0 = auto: the budget-share rule (weight * valueRange
    // ~ 0.2; see Config).
    engineConfig.search.ruin_recreate.as_move_weight =
            config.ruin_recreate_as_move_weight >= 0.0
                    ? config.ruin_recreate_as_move_weight
                    : 0.2 / std::max(1, schedule.slotRoomCount());
    engineConfig.search.descent_weight = config.descent_weight;

    // Per-talk type-compatible (timeslot, room) cells. Change moves draw from this
    // list with compatible_value_probability; the construction heuristic
    // additionally initializes the most constrained talks first.
    const int slotCount = schedule.timeslotCount();
    const int roomCount = schedule.roomCount();
    auto compatibleValues = std::make_shared<std::vector<std::vector<int>>>();
    compatibleValues->reserve(schedule.talks.size());
    for (const Talk& talk : schedule.talks) {
        std::vector<int> values;
        for (int s = 0; s < slotCount; ++s) {
            if (!ScoreCalculator::typeAllowsSlot(schedule, talk.talkType, s)) continue;
            for (int r = 0; r < roomCount; ++r) {
                if (ScoreCalculator::typeAllowsRoom(schedule, talk.talkType, r))
                    values.push_back(schedule.slotRoomOf(s, r));
            }
        }
        compatibleValues->push_back(std::move(values));
    }
    engineConfig.construction.difficulty = [compatibleValues](int talkIndex) {
        // Fewer compatible cells == harder; First Fit Decreasing.
        return -static_cast<double>((*compatibleValues)[talkIndex].size());
    };
    if (config.compatible_value_probability > 0.0) {
        const double probability = config.compatible_value_probability;
        engineConfig.search.value_hint = [compatibleValues, probability](std::mt19937_64& rng,
                                                                         int talkIndex) -> int {
            const std::vector<int>& values = (*compatibleValues)[talkIndex];
            if (values.empty()) return -1;  // fall back to the uniform draw
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            if (unit(rng) >= probability) return -1;
            return values[rng() % values.size()];
        };
    }

    return engineConfig;
}

}  // namespace

void Solver::solve(ConferenceSchedule& schedule) {
    ScoreCalculator::prepare(schedule);  // intern tags / flatten tables
    if (schedule.talks.empty() || schedule.slotRoomCount() == 0) {
        ScoreCalculator::score(schedule);
        return;
    }
    optimaru::check_config(config_);

    // Independent parallel starts, fanned out here rather than through the
    // engine's built-in multi-start: the budget is driven from this side (see
    // the restart segments below), and the engine's multi-start would instead
    // hand every thread one unsegmented full-budget run. Each start is a
    // single-start Solver of its own. (The school-timetabling canonical solver
    // fans out here for the same reason.)
    if (config_.parallel_solver_count > 1) {
        optimaru::solve_parallel_starts(
                config_, schedule, [](const Config& config, ConferenceSchedule& candidate, int) {
                    Solver worker(config);
                    worker.solve(candidate);
                });
        return;
    }

    optimaru::Settings engineConfig = buildEngineConfig(config_, schedule);

    // Construction heuristic + late-acceptance local search (+ ALNS kicks),
    // run as `restart_segments` consecutive segments over the one wall-clock
    // budget (iterated local search).
    //
    // Why segment at all: on this domain a single local-search run stops
    // paying long before the budget does. On the scaled quality-bench
    // instance the last new best lands around 3s of a 10s solve -- the
    // late-acceptance history has filled with the incumbent's own score and
    // the ALNS kick interval, which doubles after every kick that fails to
    // find a new best, has backed off past the remaining step count. Both are
    // per-run state, so restarting the phase resets them. Each segment starts
    // from the previous segment's best (the construction heuristic only
    // touches unassigned talks, and local search restores its best snapshot
    // before returning), so the sequence is monotone: a segment can only add
    // to the incumbent.
    //
    // Segmenting is skipped whenever a step- or unimproved-based termination
    // is configured, since those count per run and segmenting would silently
    // multiply them.
    const bool segmentable = config_.step_limit == 0 && config_.stall_step_limit == 0 &&
                             config_.stall_time_limit.count() == 0 &&
                             config_.time_limit.count() > 0;
    const int segments = segmentable ? std::max(1, config_.restart_segments) : 1;
    const auto deadline = std::chrono::steady_clock::now() + config_.time_limit;

    // The engine's step counter restarts with each segment; keep the
    // reported step numbers monotone across segments for on_new_best
    // consumers.
    std::int64_t stepOffset = 0;
    std::int64_t lastStep = 0;
    Score best = schedule.score;
    // One calculator for every segment (the engine resets it on the working
    // solution at each solve start), so the violation-guided entity
    // recommender below can bind to its live hot-talk view.
    ConferenceSchedulingIncrementalScoreCalculator calculator;
    for (int segment = 0; segment < segments; ++segment) {
        optimaru::Settings segmentConfig = engineConfig;
        if (segments > 1) {
            const auto now = std::chrono::steady_clock::now();
            const auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            if (segment > 0 && remaining.count() <= 0) break;
            segmentConfig.limits.time_limit =
                    std::max(std::chrono::milliseconds(1), remaining / (segments - segment));
            // A distinct stream per segment, kept far away from the
            // parallel-worker offsets above.
            segmentConfig.seed =
                    config_.seed + static_cast<std::uint64_t>(segment) * 0x9E3779B97F4A7C15ULL;
        }
        segmentConfig.on_new_best = [&lastStep, &stepOffset, this](std::int64_t step,
                                                                   const Score& sc) {
            lastStep = step;
            if (config_.on_new_best) config_.on_new_best(stepOffset + step, sc);
        };
        // Violation-guided entity selection (see Config::hot_talk_probability):
        // rejection-sample a few uniform draws against the calculator's O(1)
        // hot-talk check; -1 falls back to the uniform draw.
        if (config_.hot_talk_probability > 0.0) {
            const double probability = config_.hot_talk_probability;
            const int talkCount = static_cast<int>(schedule.talks.size());
            segmentConfig.search.entity_hint = [&calculator, probability,
                                                talkCount](std::mt19937_64& rng) -> int {
                std::uniform_real_distribution<double> unit(0.0, 1.0);
                if (unit(rng) >= probability) return -1;
                for (int attempt = 0; attempt < 4; ++attempt) {
                    const int talk =
                            static_cast<int>(rng() % static_cast<std::uint64_t>(talkCount));
                    if (calculator.isHotTalk(talk)) return talk;
                }
                return -1;
            };
        }

        auto engine = optimaru::Solver<ConferenceSchedule>(std::move(segmentConfig));
        if (config_.stop_flag) engine.share_stop_flag(config_.stop_flag);
        lastStep = 0;
        best = engine.solve(schedule, calculator);
        stepOffset += lastStep + 1;
        if (best >= config_.target_score) break;
        // A sibling start failed or reached the target: no further segments.
        if (config_.stop_flag && config_.stop_flag->load()) break;
    }

    // The authoritative final score: a full recompute on the best solution.
    schedule.score = ScoreCalculator::score(schedule);
}

void Solver::solve(ConferenceSchedule& schedule,
                   optimaru::DecomposedObjective<ConferenceSchedule>& calculator) {
    ScoreCalculator::prepare(schedule);
    optimaru::check_config(config_);
    if (!schedule.talks.empty() && schedule.slotRoomCount() != 0) {
        // One segment, one start: both the segmented restarts and the
        // parallel workers need a calculator of their own, and a
        // caller-assembled plugin cannot be cloned.
        optimaru::Settings engineConfig = buildEngineConfig(config_, schedule);
        auto engine = optimaru::Solver<ConferenceSchedule>(std::move(engineConfig));
        engine.solve(schedule, calculator);
    }

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraint_totals() explains exactly this score.
    calculator.rebuild(schedule);
    schedule.score = calculator.score();
}

}  // namespace cs
