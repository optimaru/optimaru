#include "st/canonical_solver.hpp"

#include <algorithm>
#include <memory>
#include <type_traits>
#include <random>
#include <utility>
#include <vector>

#include "st/score/canonical_incremental_calculator.hpp"
#include "st/score/canonical_score_calculator.hpp"
#include "st/search/moves/hot_source.hpp"
#include "st/search/moves/reassign.hpp"
#include "st/search/moves/smart_class_swap.hpp"
#include "st/search/moves/unassign.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/utils/random.hpp"

namespace st::canon {

namespace {

// Guided-search tables shared by both solve() overloads; the smart operator
// needs them after the engine config is built.
struct GuidedSetup {
    std::shared_ptr<std::vector<std::vector<int>>> allowedSlots;
    std::shared_ptr<std::vector<int>> classOf;
    std::vector<std::vector<int>> lessonsOfClass;
};

// Everything of the engine configuration that does not depend on which
// calculator drives the search: termination, late acceptance, the move mix,
// the adaptive LNS, allowed-slot value guidance, same-class nearby
// neighborhoods and the FFD construction order. The entity recommender
// (violation-guided selection) binds to the concrete calculator, so each
// solve() overload wires it separately.
GuidedSetup configureEngine(const Solver::Config& config, const Timetable& table,
                            optimaru::Settings& engineConfig) {
    engineConfig = optimaru::settings_from(config);
    // history_length 0 = auto-scale with the instance (see Config).
    engineConfig.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(4 * table.lessons.size(), 400, 3200);
    engineConfig.moves.change_weight = config.change_weight;
    engineConfig.moves.swap_weight = config.swap_weight;
    engineConfig.strategies.descent_weight = config.descent_weight;
    engineConfig.strategies.descent_max_sweeps = config.descent_max_sweeps;

    GuidedSetup setup;

    // Guided value selection: change moves only draw slots the lesson is
    // allowed on (its allowedSlot table), so the search never wanders into
    // disallowedSlotCost territory. Lessons allowed nowhere fall back to
    // uniform-random (-1).
    setup.allowedSlots = std::make_shared<std::vector<std::vector<int>>>();
    setup.allowedSlots->reserve(table.lessons.size());
    for (const Lesson& lesson : table.lessons) {
        std::vector<int> slots;
        for (int s = 0; s < table.slotCount(); ++s) {
            if (Timetable::slotAllowed(lesson, s)) slots.push_back(s);
        }
        setup.allowedSlots->push_back(std::move(slots));
    }
    const std::shared_ptr<std::vector<std::vector<int>>>& allowedSlots = setup.allowedSlots;
    engineConfig.selection.value_hint = [allowedSlots](std::mt19937_64& rng,
                                                       int lessonIndex) -> int {
        const std::vector<int>& slots = (*allowedSlots)[lessonIndex];
        if (slots.empty()) return -1;
        return slots[rng() % slots.size()];
    };

    // Class membership tables, shared by the nearby meter and the smart
    // same-class swap operator.
    setup.classOf = std::make_shared<std::vector<int>>();
    setup.classOf->reserve(table.lessons.size());
    setup.lessonsOfClass.assign(std::max(1, table.classCount), {});
    for (int i = 0; i < static_cast<int>(table.lessons.size()); ++i) {
        const int classGroup = table.lessons[i].classGroupId;
        setup.classOf->push_back(classGroup);
        setup.lessonsOfClass[classGroup].push_back(i);
    }

    // Same-class nearby neighborhoods (see Config::same_class_nearby). The
    // neighbor list of a lesson is its classmates (distance 0) up to the
    // largest class size, so a swap partner drawn nearby always shares the
    // class and the LNS related ruin removes one class's cluster.
    if (config.same_class_nearby) {
        std::size_t maxClassSize = 0;
        for (const std::vector<int>& members : setup.lessonsOfClass)
            maxClassSize = std::max(maxClassSize, members.size());
        const std::shared_ptr<std::vector<int>>& classOf = setup.classOf;
        engineConfig.selection.nearby.distance = [classOf](int a, int b) {
            return (*classOf)[a] == (*classOf)[b] ? 0.0 : 1.0;
        };
        engineConfig.selection.nearby.size = std::max(1, static_cast<int>(maxClassSize) - 1);
        engineConfig.selection.nearby.probability = config.same_class_nearby_probability;
    }

    // First Fit Decreasing: initialize the most constrained lessons first —
    // block members, team teaching / merged lessons (many teachers), special
    // room demand, and lessons with few allowed slots.
    auto difficulty = std::make_shared<std::vector<double>>();
    difficulty->reserve(table.lessons.size());
    for (std::size_t i = 0; i < table.lessons.size(); ++i) {
        const Lesson& lesson = table.lessons[i];
        int demand = 0;
        for (int d : lesson.roomTypeDemand) demand += d;
        (*difficulty)
                .push_back(
                        (lesson.blockGroup >= 0 ? 100.0 : 0.0) +
                        10.0 * static_cast<double>(lesson.teachers.size()) + 5.0 * demand +
                        static_cast<double>(table.slotCount() - (*setup.allowedSlots)[i].size()));
    }
    engineConfig.construction.difficulty = [difficulty](int lessonIndex) {
        return (*difficulty)[lessonIndex];
    };

    return setup;
}

// The use case's own moves (in Python: config.custom_moves), which a step asks for
// after the built-in strategies (optimaru::Solver::choose_move). To add a move:
// write it in search/moves/ and register it here, behind a switch of the Config.
// `hot` tells the moves which lessons break a hard constraint: the monolithic
// calculator itself, or a DecomposedHotSource over a constraint assembly.
template <typename HotSource>
void registerMoves(optimaru::Solver<Timetable>& engine, const Solver::Config& config,
                   const GuidedSetup& setup, HotSource* hot, bool allowUnassigned) {
    if (config.smart_class_swap_moves) {
        engine.add_move_operator(std::make_unique<SmartClassSwapMove<HotSource>>(
                setup.lessonsOfClass, *setup.classOf, setup.allowedSlots,
                config.smart_class_swap_candidates, hot));
    }
    if constexpr (std::is_same_v<HotSource, DecomposedHotSource>) {
        if (allowUnassigned) {
            engine.add_move_operator(std::make_unique<UnassignMove<HotSource>>(hot));
            // The way back in. Without it the search only ever drops (dropping is
            // guided by the hot lessons, re-placing is not) and the run ends with
            // far more unassigned than the instance requires.
            engine.add_move_operator(std::make_unique<ReassignMove<HotSource>>(
                    setup.allowedSlots, config.reassign_candidates));
        }
    }
}

}  // namespace

void Solver::solve(Timetable& table) {
    ScoreCalculator::prepare(table);  // normalize ids / derived tables
    if (table.lessons.empty() || table.slotCount() == 0) {
        ScoreCalculator::score(table, config_.weights);
        return;
    }
    optimaru::check_config(config_);

    // Independent parallel starts: start i runs this solve() on its own copy
    // with seed + i, one start and a calculator of its own — the
    // guided-selection hooks below read live calculator state, so the engine's
    // built-in multi-start (which shares the config across threads) cannot be
    // used. The best final score wins.
    if (config_.parallel_solver_count > 1) {
        optimaru::solve_parallel_starts(config_, table,
                                        [](const Config& config, Timetable& candidate, int) {
                                            Solver(config).solve(candidate);
                                        });
        return;
    }

    optimaru::Settings engineConfig;
    const GuidedSetup setup = configureEngine(config_, table, engineConfig);

    // The single calculator instance of this (single-start) solve. Created
    // before the engine so the guided-selection hooks can bind to it; the
    // engine resets it on the working solution at solve start.
    CanonicalIncrementalScoreCalculator calculator(config_.weights);

    // Violation-guided entity selection (the es guided-moves pattern): with
    // probability hot_lesson_probability the moved lesson is drawn from the
    // lessons currently involved in a hard violation. Once feasible there are
    // no hot lessons, so this falls back to uniform exploration by itself.
    if (config_.hot_lesson_probability > 0.0) {
        const double probability = config_.hot_lesson_probability;
        engineConfig.selection.entity_hint = [&calculator,
                                              probability](std::mt19937_64& rng) -> int {
            if (optimaru::random_unit(rng) >= probability) return -1;
            return calculator.hotLesson(rng);
        };
    }

    // Construction heuristic + late-acceptance local search (+ ALNS kicks).
    optimaru::Solver<Timetable> engine(std::move(engineConfig));
    if (config_.stop_flag) engine.share_stop_flag(config_.stop_flag);
    registerMoves(engine, config_, setup, &calculator, /*allowUnassigned=*/false);
    engine.solve(table, calculator);

    // The authoritative final score: a full recompute on the best solution.
    table.score = ScoreCalculator::score(table, config_.weights);
}

void Solver::solve(Timetable& table, optimaru::DecomposedObjective<Timetable>& calculator) {
    ScoreCalculator::prepare(table);  // normalize ids / derived tables
    if (table.lessons.empty() || table.slotCount() == 0) {
        calculator.rebuild(table);
        table.score = calculator.score();
        return;
    }
    optimaru::check_config(config_);

    optimaru::Settings engineConfig;
    const GuidedSetup setup = configureEngine(config_, table, engineConfig);

    // Violation-guided entity selection, fed by the decomposed calculator's
    // penalized units instead of the monolithic hot sets (see
    // DecomposedHotSource). Falls back to uniform exploration once feasible.
    DecomposedHotSource hotSource(&calculator);
    if (config_.hot_lesson_probability > 0.0) {
        const double probability = config_.hot_lesson_probability;
        engineConfig.selection.entity_hint = [&hotSource,
                                              probability](std::mt19937_64& rng) -> int {
            if (optimaru::random_unit(rng) >= probability) return -1;
            return hotSource.hotLesson(rng);
        };
    }

    optimaru::Solver<Timetable> engine(std::move(engineConfig));
    if (config_.stop_flag) engine.share_stop_flag(config_.stop_flag);
    // Unassigning is only worth a slot in the move mix when the assembly prices it as a soft
    // penalty; with unassignedLessonsCost hard, it is always a hard loss.
    registerMoves(engine, config_, setup, &hotSource, allowsUnassigned(calculator));
    engine.solve(table, calculator);

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraint_totals() explains exactly this score (the es pattern).
    calculator.rebuild(table);
    table.score = calculator.score();
}

}  // namespace st::canon
