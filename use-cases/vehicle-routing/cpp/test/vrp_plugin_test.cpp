// Plugin-assembly (vrp/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 300 random route
//       layouts (unassigned visits included)
//   (b) incremental score == full recompute across 1500 random LIST moves
//       driven through the engine's ScoreDirector protocol (plugin path)
//   (c) partial assemblies: each component isolates its own term
//   (d) weight / isHard overrides
//   (e) constraintTotals() explains exactly the reported score
//   (f) a 2-second solve(plan, calculator) under FULL_ASSERT
//   (g) the assembly's move previews equal the applied score; a global
//       without the preview contract switches previews off
//   (h) a short time limit holds on the plugin path, whose previews cost
//       O(route) and whose strategies therefore read the termination
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/testing/preview_parity.hpp"
#include "vrp/constraints.hpp"
#include "vrp/demo_data.hpp"
#include "vrp/domain.hpp"
#include "vrp/score_calculator.hpp"
#include "vrp/solver.hpp"

using namespace vrp;

namespace {

int failures = 0;

void check(bool ok, const std::string& label) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}

Score scoreWith(DecomposedCalculator& calculator, VehicleRoutePlan& plan) {
    calculator.resetWorkingSolution(plan);
    return fromEngineScore(calculator.calculateScore());
}

// Spreads every visit over the vehicles at random; `poolRate` of them go to
// the unassigned pool list and another `poolRate` into no list at all.
void randomizeRoutes(VehicleRoutePlan& plan, std::mt19937_64& rng, double poolRate) {
    for (Vehicle& vehicle : plan.vehicles) vehicle.visitIndices.clear();
    plan.unassignedVisitIndices.clear();
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (int v = 0; v < static_cast<int>(plan.visits.size()); ++v) {
        const double roll = unit(rng);
        if (roll < poolRate) {
            plan.unassignedVisitIndices.push_back(v);
            continue;
        }
        if (roll < 2 * poolRate) continue;  // in no list at all
        plan.vehicles[rng() % plan.vehicles.size()].visitIndices.push_back(v);
    }
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute.
// -------------------------------------------------------------------------
void verifyAssemblyParity() {
    std::printf("== (a) Default assembly == monolithic score ==\n");
    VehicleRoutePlan plan = buildRandomInstance(40, 5, /*seed=*/11);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    int mismatches = 0;
    const int rounds = 300;
    for (int round = 0; round < rounds; ++round) {
        randomizeRoutes(plan, rng, /*poolRate=*/0.10);
        const Score plugin = scoreWith(calculator, plan);
        const Score monolithic = ScoreCalculator::calculateScore(plan);
        if (!(plugin == monolithic)) {
            if (++mismatches <= 3) {
                std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n", round,
                            plugin.toString().c_str(), monolithic.toString().c_str());
            }
        }
    }
    check(mismatches == 0, std::to_string(rounds) + " random route layouts score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of LIST
// mutations, driven through the ScoreDirector.
// -------------------------------------------------------------------------
void verifyIncremental() {
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    VehicleRoutePlan plan = buildRandomInstance(40, 5, /*seed=*/11);
    plan.rebuildUnassignedPool();

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<VehicleRoutePlan> director(plan, calculator);

    using Descriptor = optimaru::SolutionDescriptor<VehicleRoutePlan>;
    std::mt19937_64 rng(3);
    int mismatches = 0;
    const int steps = 1500;
    for (int step = 0; step < steps; ++step) {
        const int listCount = Descriptor::listCount(plan);
        const int from = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount));
        if (Descriptor::listSize(plan, from) == 0) continue;
        const int position = static_cast<int>(
                rng() % static_cast<std::uint64_t>(Descriptor::listSize(plan, from)));
        const int element = Descriptor::getElement(plan, from, position);
        director.removeListElement(from, position);

        const int to = static_cast<int>(rng() % static_cast<std::uint64_t>(listCount));
        const int insertAt = static_cast<int>(
                rng() % static_cast<std::uint64_t>(Descriptor::listSize(plan, to) + 1));
        director.insertListElement(to, insertAt, element);

        const Score incremental = fromEngineScore(director.calculateScore());
        const Score full = ScoreCalculator::calculateScore(plan);
        if (!(incremental == full)) {
            if (++mismatches <= 3) {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n", step,
                            incremental.toString().c_str(), full.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(steps) + " random list moves, incremental == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Each component in isolation carries exactly its own level.
// -------------------------------------------------------------------------
void verifyPartialAssemblies() {
    std::printf("== (c) Single-component assemblies ==\n");
    VehicleRoutePlan plan = buildRandomInstance(30, 4, /*seed=*/21);
    std::mt19937_64 rng(5);
    randomizeRoutes(plan, rng, /*poolRate=*/0.15);
    const Score full = ScoreCalculator::calculateScore(plan);

    {
        DecomposedCalculator calculator;
        calculator.emplace<VehicleCapacity>();
        calculator.emplace<ServiceFinishedAfterMaxEndTime>();
        const Score score = scoreWith(calculator, plan);
        check(score.hard == full.hard && score.medium == 0 && score.soft == 0,
              "the two hard components alone reproduce the hard level");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<MaximizeVisitsAssigned>();
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 0 && score.medium == full.medium && score.soft == 0,
              "maximizeVisitsAssigned alone reproduces the medium level");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<MinimizeTravelTime>();
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 0 && score.medium == 0 && score.soft == full.soft,
              "minimizeTravelTime alone reproduces the soft level");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides.
// -------------------------------------------------------------------------
void verifyOverrides() {
    std::printf("== (d) Weight / level overrides ==\n");
    VehicleRoutePlan plan = buildRandomInstance(30, 3, /*seed=*/31);
    std::mt19937_64 rng(7);
    randomizeRoutes(plan, rng, /*poolRate=*/0.05);

    DecomposedCalculator baseline;
    baseline.emplace<VehicleCapacity>();
    const Score base = scoreWith(baseline, plan);

    {
        DecomposedCalculator calculator;
        calculator.emplace<VehicleCapacity>(10.0);
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 10 * base.hard, "vehicleCapacity re-weighted to 10");
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<VehicleCapacity>();
        component->isHard = false;
        calculator.add(std::move(component));
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 0 && score.medium == 0 && score.soft == base.hard,
              "vehicleCapacity demoted to soft");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals(): the per-constraint breakdown adds up.
// -------------------------------------------------------------------------
void verifyConstraintTotals() {
    std::printf("== (e) constraintTotals() explains the score ==\n");
    VehicleRoutePlan plan = buildRandomInstance(25, 4, /*seed=*/9);
    std::mt19937_64 rng(5);
    randomizeRoutes(plan, rng, /*poolRate=*/0.10);

    const std::size_t componentCount = defaultConstraints().size();
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(plan);
    const optimaru::HardSoftScore score = calculator.calculateScore();

    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal& total : calculator.constraintTotals())
        (total.isHard ? hard : soft) += total.cost;
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9
                  && std::fabs(-soft - score.soft) < 1e-3,
          "per-constraint totals sum to the reported score");
    check(calculator.constraintTotals().size() == componentCount,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve() {
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    VehicleRoutePlan plan = buildRandomInstance(25, 4, /*seed=*/9);
    plan.rebuildUnassignedPool();

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    bool corrupted = false;
    try {
        Solver solver(config);
        solver.solve(plan, calculator);
    } catch (const optimaru::ScoreCorruptionError& e) {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted) {
        const Score recomputed = ScoreCalculator::calculateScore(plan);
        check(plan.score == recomputed, "plugin-reported score == monolithic full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (g) Move previews on the plugin path. The decomposed calculator serves
// them generically (candidate contents re-costed per component), and the one
// global in the default set answers through its preview contract. The demo
// plan has morning/afternoon windows that open after departure, i.e. it is a
// waiting plan: the monolithic calculator walks it, the assembly re-costs it,
// and both must match the applied score.
// -------------------------------------------------------------------------
struct PreviewlessGlobal final : VrpGlobalConstraint {
    PreviewlessGlobal() : VrpGlobalConstraint("previewless", 1.0, false) {}
    void reset(const VehicleRoutePlan&, const std::vector<std::vector<int>>&) override {}
    void onUnitChanged(const VehicleRoutePlan&, int, const std::vector<int>&) override {}
    double cost() const override { return 0.0; }
};

void verifyPreviews() {
    std::printf("== (g) Plugin previews == applied score ==\n");
    VehicleRoutePlan plan = buildRandomInstance(40, 5, /*seed=*/11);
    std::mt19937_64 rng(3);
    randomizeRoutes(plan, rng, /*poolRate=*/0.10);
    plan.rebuildUnassignedPool();

    {
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        optimaru::ScoreDirector<VehicleRoutePlan> director(plan, calculator);
        const optimaru::testing::PreviewParity parity =
                optimaru::testing::runPreviewParity(director, calculator, /*seed=*/5, /*trials=*/3000);
        std::printf("    %d moves checked, %d previewed, %d declined\n", parity.checked,
                    parity.previewed, parity.declined);
        check(parity.allEqual() && parity.previewed > 1000 && parity.declined == 0,
              "default assembly: every preview served and equal to the applied score");
    }
    {
        // Every optional policy on, at a weight that makes it bite, so the
        // preview re-costs the full component set.
        DecomposedCalculator calculator;
        ConstraintAssembly assembly = defaultConstraints();
        for (Vehicle& vehicle : plan.vehicles) {
            vehicle.maxStops = 6;
            vehicle.maxTravelTime = 3 * 3600;
            vehicle.fixedCost = 1000;
        }
        assembly.into(calculator);
        optimaru::ScoreDirector<VehicleRoutePlan> director(plan, calculator);
        const optimaru::testing::PreviewParity parity =
                optimaru::testing::runPreviewParity(director, calculator, /*seed=*/6, /*trials=*/2000);
        check(parity.allEqual() && parity.previewed > 500 && parity.declined == 0,
              "assembly with route policies: every preview equal to the applied score");
        for (Vehicle& vehicle : plan.vehicles) {
            vehicle.maxStops = 0;
            vehicle.maxTravelTime = 0;
            vehicle.fixedCost = 0;
        }
    }
    {
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        calculator.emplace<PreviewlessGlobal>();
        optimaru::ScoreDirector<VehicleRoutePlan> director(plan, calculator);
        const optimaru::testing::PreviewParity parity =
                optimaru::testing::runPreviewParity(director, calculator, /*seed=*/7, /*trials=*/300);
        check(parity.previewed == 0 && parity.declined > 0,
              "a global without the preview contract switches previews off");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) The time limit holds. A preview on the plugin path re-costs the
// candidate routes per component, so the iterated local search's first
// descent over a large plan is seconds of work; the strategies consult the
// termination between batches of evaluations, so a 1-second solve must end
// close to 1 second rather than when the descent happens to finish.
// -------------------------------------------------------------------------
// -------------------------------------------------------------------------
// (i) Piecewise previews by hand: the four summed components answer from
// the shared route sums for a candidate route pieced together from forward,
// reversed, pool and detached pieces, and agree with cost() on the same
// route written out; the lateness component declines exactly when the sums
// cannot settle it (a route that may run late, a plan with waiting); an
// assembly whose only summed component is the global previews correctly.
// -------------------------------------------------------------------------
void verifyPiecewiseByHand() {
    std::printf("== (i) Piecewise previews by hand ==\n");
    VehicleRoutePlan plan = buildRandomInstance(12, 2, /*seed=*/21);
    for (Visit& visit : plan.visits) {
        visit.minStartTime = 0;  // no waiting: the sums settle finish times
        visit.maxEndTime = 48 * 3600;
    }
    plan.vehicles[0].visitIndices = {0, 1, 2, 3, 4};
    plan.vehicles[1].visitIndices = {5, 6, 7, 8};
    plan.vehicles[1].returnToDepot = true;
    plan.unassignedVisitIndices = {9, 10};
    // 11 is in no list at all (the detached element of a repair's insert).
    const int pool = static_cast<int>(plan.vehicles.size());

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(plan);

    // Vehicle 0 would hold: its own [0,2) forward, vehicle 1's [1,4) reversed,
    // pool visit 9, the detached visit 11, then its own [3,5).
    const optimaru::ListPiece pieces[] = {
            {0, 0, 2, false}, {1, 1, 4, true}, {pool, 0, 1, false}, {-1, 11, 12, false},
            {0, 3, 5, false}};
    const std::vector<int> written = {0, 1, 8, 7, 6, 9, 11, 3, 4};
    const auto& constraints = calculator.unitConstraintList();
    int compared = 0;
    for (const auto& c : constraints) {
        if (!c->supportsPiecewisePreview()) continue;
        double piecewise = -1.0;
        const bool answered = c->piecewiseCost(plan, 0, pieces, 5, piecewise);
        const double walked = c->cost(plan, 0, written);
        check(answered && piecewise == walked,
              c->name + ": piecewise answer equals cost() of the written-out route");
        ++compared;
    }
    check(compared == 3, "three summed unit components compared");
    // The pool as a unit: every summed component costs it 0 from pieces too.
    for (const auto& c : constraints) {
        if (!c->supportsPiecewisePreview()) continue;
        double piecewise = -1.0;
        check(c->piecewiseCost(plan, pool, pieces, 5, piecewise) && piecewise == 0.0,
              c->name + ": the pool costs nothing");
    }
    // The global: unrouted penalty with vehicle 0 holding the pieces.
    for (const auto& g : calculator.globalConstraintList()) {
        if (!g->supportsPiecewisePreview()) continue;
        std::vector<int> members1 = plan.vehicles[1].visitIndices;
        const optimaru::PreviewedUnit changes[1] = {{0, &written, pieces, 5}};
        double piecewise = -1.0;
        const bool answered = g->piecewisePreviewCost(plan, changes, 1, piecewise);
        std::int64_t expected = 0;
        for (const Visit& visit : plan.visits) expected += visit.unassignedPenalty();
        for (int v : written) expected -= plan.visits[static_cast<std::size_t>(v)].unassignedPenalty();
        for (int v : members1) expected -= plan.visits[static_cast<std::size_t>(v)].unassignedPenalty();
        check(answered && piecewise == static_cast<double>(expected),
              g->name + ": piecewise answer equals the hand-counted unrouted penalty");
    }

    // Lateness: a deadline the candidate route cannot meet is declined, not
    // answered; cost() on the written-out route then reports the minutes.
    {
        plan.visits[4].maxEndTime = 60;  // a minute after departure
        calculator.resetWorkingSolution(plan);
        for (const auto& c : constraints) {
            if (c->name != "serviceFinishedAfterMaxEndTime") continue;
            double piecewise = -1.0;
            check(!c->piecewiseCost(plan, 0, pieces, 5, piecewise),
                  "lateness declines when the route may run late");
            check(c->cost(plan, 0, written) > 0.0, "... and cost() reports the lateness");
        }
        plan.visits[4].maxEndTime = 48 * 3600;
    }
    {
        plan.visits[6].minStartTime = plan.vehicles[0].departureTime + 3600;  // waiting
        calculator.resetWorkingSolution(plan);
        for (const auto& c : constraints) {
            if (c->name != "serviceFinishedAfterMaxEndTime") continue;
            double piecewise = -1.0;
            check(!c->piecewiseCost(plan, 0, pieces, 5, piecewise),
                  "lateness declines on a plan with waiting");
        }
        plan.visits[6].minStartTime = 0;
    }

    // An assembly whose only summed component is the global: its reset() is
    // what prepares the shared sums, and the previews still agree.
    {
        VehicleRoutePlan wide = buildRandomInstance(40, 5, /*seed=*/12);
        std::mt19937_64 rng(4);
        randomizeRoutes(wide, rng, /*poolRate=*/0.10);
        wide.rebuildUnassignedPool();
        DecomposedCalculator onlyGlobal;
        defaultConstraints().only({"maximizeVisitsAssigned", "maxStops"}).into(onlyGlobal);
        optimaru::ScoreDirector<VehicleRoutePlan> director(wide, onlyGlobal);
        const optimaru::testing::PreviewParity parity =
                optimaru::testing::runPreviewParity(director, onlyGlobal, /*seed=*/8, 1500);
        check(parity.allEqual() && parity.declined == 0 && parity.previewed > 500,
              "global-only summed assembly: every preview equals the applied score");
    }
    std::printf("\n");
}

void verifyTimeLimitOnPluginPath() {
    std::printf("== (h) Time limit on the plugin path ==\n");
    VehicleRoutePlan plan = buildRandomInstance(800, 40, /*seed=*/7);
    plan.rebuildUnassignedPool();

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(1);
    config.randomSeed = 1;
    // The construction heuristic runs before the clock-reading phase and is
    // not what this test is about: time from its result (the first best).
    auto started = std::chrono::steady_clock::now();
    bool constructed = false;
    config.onNewBest = [&](std::int64_t, const Score&) {
        if (!constructed) {
            constructed = true;
            started = std::chrono::steady_clock::now();
        }
    };

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    Solver(config).solve(plan, calculator);
    const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("    1 s limit, 800 visits: local search took %.2f s\n", seconds);
    // Generous for a loaded CI runner; without the termination check a 1 s
    // X-n895 solve on this path ran for 4.6 s.
    check(seconds < 2.5, "a 1 s plugin-path solve ends within 2.5 s");
    std::printf("\n");
}

}  // namespace

int main() {
    verifyAssemblyParity();
    verifyIncremental();
    verifyPartialAssemblies();
    verifyOverrides();
    verifyConstraintTotals();
    verifyFullAssertSolve();
    verifyPreviews();
    verifyPiecewiseByHand();
    verifyTimeLimitOnPluginPath();

    if (failures == 0) {
        std::printf("All vehicle-routing plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d vehicle-routing plugin check(s) FAILED.\n", failures);
    return 1;
}
