// Plugin-assembly (vrp/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedObjective
//       reproduces the monolithic score bit-for-bit on 300 random route
//       layouts (unassigned visits included)
//   (b) incremental score == full recompute across 1500 random sequence moves
//       driven through the engine's Editor protocol (plugin path)
//   (c) partial assemblies: each component isolates its own term
//   (d) weight / level overrides
//   (e) constraint_totals() explains exactly the reported score
//   (f) a 2-second solve(plan, calculator) under ScoreChecks::Full
//   (g) the assembly's move previews equal the applied score; a global
//       without the preview contract switches previews off
//   (h) a short time limit holds on the plugin path, whose previews cost
//       O(route) and whose strategies therefore read the termination
//   (i) piecewise previews by hand
//   (j) every component's raw cost by hand: one positive and one zero case
//   (k) the catalog (info()) == each component's kDefaultWeight / kDefaultLevel
//   (l) the medium level: fold, hard() / soft() / weight on
//       unassignedVisitsCost, and incremental == full and previews ==
//       applied score with those overrides
//   (m) on_new_best on the plugin path reports the same levels as plan.score
//   (n) repeated components and the offset / clip knobs: a hand-calculated
//       repeat (travel + 0.5 x travel^2), threshold / literal / negative
//       offsets by hand (an idle vehicle counts in literal mode, the pool
//       never does), the global's total shaped once, the literal formula
//       refused on hard at reset and kept on soft and medium, incremental ==
//       a fresh recompute over a random walk, every preview == the applied
//       score under ScoreChecks::Full and a ScoreChecks::Full solve with repeated,
//       shifted components
//   (o) negative thresholds (a fixed charge per non-zero vehicle, real-valued
//       radius included): previews == applied score under ScoreChecks::Full, a
//       ScoreChecks::Full solve, and after a 20000-step solve plan.score == a fresh
//       recompute == the last (incremental) on_new_best report
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/model/editor.hpp"
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
    calculator.rebuild(plan);
    return fromEngineScore(calculator.score());
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
        const Score monolithic = ScoreCalculator::score(plan);
        if (!(plugin == monolithic)) {
            if (++mismatches <= 3) {
                std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n", round,
                            plugin.str().c_str(), monolithic.str().c_str());
            }
        }
    }
    check(mismatches == 0, std::to_string(rounds) + " random route layouts score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of sequence
// mutations, driven through the Editor.
// -------------------------------------------------------------------------
void verifyIncremental() {
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    VehicleRoutePlan plan = buildRandomInstance(40, 5, /*seed=*/11);
    plan.rebuildUnassignedPool();

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::Editor<VehicleRoutePlan> editor(plan, calculator);

    using Descriptor = optimaru::SolutionTraits<VehicleRoutePlan>;
    std::mt19937_64 rng(3);
    int mismatches = 0;
    const int steps = 1500;
    for (int step = 0; step < steps; ++step) {
        const int list_count = Descriptor::list_count(plan);
        const int from = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
        if (Descriptor::list_size(plan, from) == 0) continue;
        const int position = static_cast<int>(
                rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, from)));
        const int element = Descriptor::element_at(plan, from, position);
        editor.remove(from, position);

        const int to = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
        const int insert_at = static_cast<int>(
                rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, to) + 1));
        editor.insert(to, insert_at, element);

        const Score incremental = fromEngineScore(editor.score());
        const Score full = ScoreCalculator::score(plan);
        if (!(incremental == full)) {
            if (++mismatches <= 3) {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n", step,
                            incremental.str().c_str(), full.str().c_str());
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
    const Score full = ScoreCalculator::score(plan);

    {
        DecomposedCalculator calculator;
        calculator.emplace<VehicleCapacityCost>();
        calculator.emplace<MaxEndTimeCost>();
        const Score score = scoreWith(calculator, plan);
        check(score.hard == full.hard && score.medium == 0 && score.soft == 0,
              "the two hard components alone reproduce the hard level");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<UnassignedVisitsCost>();
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 0 && score.medium == full.medium && score.soft == 0,
              "unassignedVisitsCost alone reproduces the medium level");
    }
    {
        DecomposedCalculator calculator;
        calculator.emplace<TravelTimeCost>();
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 0 && score.medium == 0 && score.soft == full.soft,
              "travelTimeCost alone reproduces the soft level");
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
    baseline.emplace<VehicleCapacityCost>();
    const Score base = scoreWith(baseline, plan);

    {
        DecomposedCalculator calculator;
        calculator.emplace<VehicleCapacityCost>(10.0);
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 10 * base.hard, "vehicleCapacityCost re-weighted to 10");
    }
    {
        DecomposedCalculator calculator;
        auto component = std::make_unique<VehicleCapacityCost>();
        component->level = optimaru::Level::Soft;
        calculator.add(std::move(component));
        const Score score = scoreWith(calculator, plan);
        check(score.hard == 0 && score.medium == 0 && score.soft == base.hard,
              "vehicleCapacityCost demoted to soft");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraint_totals(): the per-constraint breakdown adds up.
// -------------------------------------------------------------------------
void verifyConstraintTotals() {
    std::printf("== (e) constraintTotals() explains the score ==\n");
    VehicleRoutePlan plan = buildRandomInstance(25, 4, /*seed=*/9);
    std::mt19937_64 rng(5);
    randomizeRoutes(plan, rng, /*poolRate=*/0.10);

    const std::size_t componentCount = defaultConstraints().size();
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(plan);
    const optimaru::Score score = calculator.score();

    // A medium total is in medium units (not multiplied by the fold), so it
    // is folded here the way the calculator folds its medium accumulator.
    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal& total : calculator.constraint_totals()) {
        if (total.level == optimaru::Level::Hard)
            hard += total.cost;
        else if (total.level == optimaru::Level::Medium)
            soft += total.cost * static_cast<double>(kMediumUnit);
        else
            soft += total.cost;
    }
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9 &&
                  std::fabs(-soft - score.soft) < 1e-3,
          "per-constraint totals sum to the reported score");
    check(calculator.constraint_totals().size() == componentCount,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) ScoreChecks::Full solve on the plugin path.
// -------------------------------------------------------------------------
void verifyFullAssertSolve() {
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    VehicleRoutePlan plan = buildRandomInstance(25, 4, /*seed=*/9);
    plan.rebuildUnassignedPool();

    Solver::Config config;
    config.time_limit = std::chrono::seconds(2);
    config.seed = 3;
    config.score_checks = optimaru::ScoreChecks::Full;
    config.ruin_recreate_unimproved_step_trigger = 200;
    config.ruin_recreate_polish_steps = 100;

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    bool corrupted = false;
    try {
        Solver solver(config);
        solver.solve(plan, calculator);
    } catch (const optimaru::ScoreMismatchError& e) {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted) {
        const Score recomputed = ScoreCalculator::score(plan);
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
    PreviewlessGlobal() : VrpGlobalConstraint("previewless", 1.0, optimaru::Level::Soft) {}
    void reset(const VehicleRoutePlan&, const std::vector<std::vector<int>>&) override {}
    void on_unit_changed(const VehicleRoutePlan&, int, const std::vector<int>&) override {}
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
        optimaru::Editor<VehicleRoutePlan> editor(plan, calculator);
        const optimaru::testing::PreviewParity parity = optimaru::testing::run_preview_parity(
                editor, calculator, /*seed=*/5, /*trials=*/3000);
        std::printf("    %d moves checked, %d previewed, %d declined\n", parity.checked,
                    parity.previewed, parity.declined);
        check(parity.all_equal() && parity.previewed > 1000 && parity.declined == 0,
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
        optimaru::Editor<VehicleRoutePlan> editor(plan, calculator);
        const optimaru::testing::PreviewParity parity = optimaru::testing::run_preview_parity(
                editor, calculator, /*seed=*/6, /*trials=*/2000);
        check(parity.all_equal() && parity.previewed > 500 && parity.declined == 0,
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
        optimaru::Editor<VehicleRoutePlan> editor(plan, calculator);
        const optimaru::testing::PreviewParity parity = optimaru::testing::run_preview_parity(
                editor, calculator, /*seed=*/7, /*trials=*/300);
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
    calculator.rebuild(plan);

    // Vehicle 0 would hold: its own [0,2) forward, vehicle 1's [1,4) reversed,
    // pool visit 9, the detached visit 11, then its own [3,5).
    const optimaru::ListPiece pieces[] = {{0, 0, 2, false},
                                          {1, 1, 4, true},
                                          {pool, 0, 1, false},
                                          {-1, 11, 12, false},
                                          {0, 3, 5, false}};
    const std::vector<int> written = {0, 1, 8, 7, 6, 9, 11, 3, 4};
    const auto& constraints = calculator.unit_constraint_list();
    int compared = 0;
    for (const auto& c : constraints) {
        if (!c->supports_piecewise_preview()) continue;
        double piecewise = -1.0;
        const bool answered = c->piecewise_cost(plan, 0, pieces, 5, piecewise);
        const double walked = c->cost(plan, 0, written);
        check(answered && piecewise == walked,
              c->name + ": piecewise answer equals cost() of the written-out route");
        ++compared;
    }
    check(compared == 3, "three summed unit components compared");
    // The pool as a unit: every summed component costs it 0 from pieces too.
    for (const auto& c : constraints) {
        if (!c->supports_piecewise_preview()) continue;
        double piecewise = -1.0;
        check(c->piecewise_cost(plan, pool, pieces, 5, piecewise) && piecewise == 0.0,
              c->name + ": the pool costs nothing");
    }
    // The global: unrouted penalty with vehicle 0 holding the pieces.
    for (const auto& g : calculator.global_constraint_list()) {
        if (!g->supports_piecewise_preview()) continue;
        std::vector<int> members1 = plan.vehicles[1].visitIndices;
        const optimaru::PreviewedUnit changes[1] = {{0, &written, pieces, 5}};
        double piecewise = -1.0;
        const bool answered = g->piecewise_preview_cost(plan, changes, 1, piecewise);
        std::int64_t expected = 0;
        for (const Visit& visit : plan.visits) expected += visit.unassignedPenalty();
        for (int v : written)
            expected -= plan.visits[static_cast<std::size_t>(v)].unassignedPenalty();
        for (int v : members1)
            expected -= plan.visits[static_cast<std::size_t>(v)].unassignedPenalty();
        check(answered && piecewise == static_cast<double>(expected),
              g->name + ": piecewise answer equals the hand-counted unrouted penalty");
    }

    // Lateness: a deadline the candidate route cannot meet is declined, not
    // answered; cost() on the written-out route then reports the minutes.
    {
        plan.visits[4].maxEndTime = 60;  // a minute after departure
        calculator.rebuild(plan);
        for (const auto& c : constraints) {
            if (c->name != "maxEndTimeCost") continue;
            double piecewise = -1.0;
            check(!c->piecewise_cost(plan, 0, pieces, 5, piecewise),
                  "lateness declines when the route may run late");
            check(c->cost(plan, 0, written) > 0.0, "... and cost() reports the lateness");
        }
        plan.visits[4].maxEndTime = 48 * 3600;
    }
    {
        plan.visits[6].minStartTime = plan.vehicles[0].departureTime + 3600;  // waiting
        calculator.rebuild(plan);
        for (const auto& c : constraints) {
            if (c->name != "maxEndTimeCost") continue;
            double piecewise = -1.0;
            check(!c->piecewise_cost(plan, 0, pieces, 5, piecewise),
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
        defaultConstraints().only({"unassignedVisitsCost", "maxStopsCost"}).into(onlyGlobal);
        optimaru::Editor<VehicleRoutePlan> editor(wide, onlyGlobal);
        const optimaru::testing::PreviewParity parity =
                optimaru::testing::run_preview_parity(editor, onlyGlobal, /*seed=*/8, 1500);
        check(parity.all_equal() && parity.declined == 0 && parity.previewed > 500,
              "global-only summed assembly: every preview equals the applied score");
    }
    std::printf("\n");
}

void verifyTimeLimitOnPluginPath() {
    std::printf("== (h) Time limit on the plugin path ==\n");
    VehicleRoutePlan plan = buildRandomInstance(800, 40, /*seed=*/7);
    plan.rebuildUnassignedPool();

    Solver::Config config;
    config.time_limit = std::chrono::seconds(1);
    config.seed = 1;
    // The construction heuristic runs before the clock-reading phase and is
    // not what this test is about: time from its result (the first best).
    auto started = std::chrono::steady_clock::now();
    bool constructed = false;
    config.on_new_best = [&](std::int64_t, const Score&) {
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

// -------------------------------------------------------------------------
// (j) Raw costs by hand: one positive and one zero case per component.
// -------------------------------------------------------------------------
// Four visits, two vehicles, an explicit driving matrix of 100 s per index
// step (location k at longitude 0.001 * k on the equator, home = location 0):
// visit k sits at location k + 1, demand 3, 60 s of service, window [0, 24 h],
// the automatic unassigned penalty (60 s -> 1 minute). Vehicles of capacity
// 10 leave at 0 with open routes. With vehicle 0 on visits 0 1 2 3 the
// arrivals are 100, 260, 420, 580, the route ends at 640 and drives 400 s.
VehicleRoutePlan buildHandInstance() {
    VehicleRoutePlan plan;
    plan.name = "hand";
    for (int k = 0; k < 5; ++k) plan.locations.push_back(Location{0.0, 0.001 * k, k});
    for (int v = 0; v < 2; ++v) {
        Vehicle vehicle;
        vehicle.id = std::to_string(v);
        vehicle.capacity = 10;
        vehicle.homeLocationIndex = 0;
        vehicle.departureTime = 0;
        plan.vehicles.push_back(vehicle);
    }
    for (int k = 0; k < 4; ++k) {
        Visit visit;
        visit.id = "v" + std::to_string(k);
        visit.name = visit.id;
        visit.locationIndex = k + 1;
        visit.demand = 3;
        visit.minStartTime = 0;
        visit.maxEndTime = 24 * 3600;
        visit.serviceDuration = 60;
        plan.visits.push_back(visit);
    }
    plan.drivingSeconds.assign(5, std::vector<Seconds>(5, 0));
    for (int a = 0; a < 5; ++a)
        for (int b = 0; b < 5; ++b) plan.drivingSeconds[a][b] = 100 * std::abs(a - b);
    return plan;
}

// Vehicle 0 serves visits 0 1 2 3; vehicle 1 and the pool are empty.
VehicleRoutePlan handRouted() {
    VehicleRoutePlan plan = buildHandInstance();
    plan.vehicles[0].visitIndices = {0, 1, 2, 3};
    return plan;
}

// The raw per-unit costs (vehicle 0, vehicle 1, pool; one entry for the
// global) of one registry name, read off the default assembly.
std::vector<double> rawCosts(VehicleRoutePlan plan, const std::string& name) {
    plan.rebuildUnassignedPool();
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(plan);
    calculator.score();
    for (const auto& entry : calculator.constraint_costs())
        if (entry.name == name) return entry.unit_costs;
    throw std::logic_error("no constraint " + name);
}

bool near(const std::vector<double>& got, const std::vector<double>& want) {
    if (got.size() != want.size()) return false;
    for (std::size_t i = 0; i < got.size(); ++i)
        if (std::fabs(got[i] - want[i]) > 1e-6 * std::max(1.0, std::fabs(want[i]))) return false;
    return true;
}

void byHand(const std::string& name, const std::function<void(VehicleRoutePlan&)>& positive,
            const std::vector<double>& positiveCosts,
            const std::function<void(VehicleRoutePlan&)>& zero,
            const std::vector<double>& zeroCosts) {
    VehicleRoutePlan up = handRouted();
    positive(up);
    check(near(rawCosts(up, name), positiveCosts), name + " positive");
    VehicleRoutePlan down = handRouted();
    zero(down);
    check(near(rawCosts(down, name), zeroCosts), name + " zero");
}

void verifyRawCostsByHand() {
    std::printf("== (j) Raw costs by hand ==\n");
    constexpr double pi = 3.14159265358979323846;
    // On the equator the haversine distance is R * dLongitude (radians), and
    // the equirectangular projection keeps the visits on one line.
    const double meterPerStep = 6371000.0 * 0.001 * pi / 180.0;
    const std::vector<double> none{0.0, 0.0, 0.0};
    const auto keep = [](VehicleRoutePlan&) {};
    const auto park = [](VehicleRoutePlan& p) { p.vehicles[0].visitIndices.clear(); };

    // Demand 4 x 3 = 12 on capacity 10 -> 2.
    byHand(
            "vehicleCapacityCost", keep, {2.0, 0.0, 0.0},
            [](VehicleRoutePlan& p) { p.vehicles[0].capacity = 12; }, none);
    // Visit 2 finishes at 480 against 400 (80 s -> 2 min), visit 3 at 640
    // against 580 (60 s -> 1 min).
    byHand(
            "maxEndTimeCost",
            [](VehicleRoutePlan& p) {
                p.visits[2].maxEndTime = 400;
                p.visits[3].maxEndTime = 580;
            },
            {3.0, 0.0, 0.0}, [](VehicleRoutePlan& p) { p.visits[3].maxEndTime = 640; }, none);
    // 4 legs of 100 s, open; returnToDepot adds visit 3 -> home = 400 s.
    byHand("travelTimeCost", keep, {400.0, 0.0, 0.0}, park, none);
    byHand(
            "travelTimeCost", [](VehicleRoutePlan& p) { p.vehicles[0].returnToDepot = true; },
            {800.0, 0.0, 0.0},
            [](VehicleRoutePlan& p) {
                p.vehicles[0].returnToDepot = true;
                p.vehicles[0].visitIndices.clear();  // a parked vehicle drives 0
            },
            none);
    byHand(
            "maxTravelTimeCost", [](VehicleRoutePlan& p) { p.vehicles[0].maxTravelTime = 300; },
            {100.0, 0.0, 0.0}, [](VehicleRoutePlan& p) { p.vehicles[0].maxTravelTime = 400; },
            none);
    byHand(
            "maxRouteDurationCost",
            [](VehicleRoutePlan& p) { p.vehicles[0].maxRouteDuration = 600; }, {40.0, 0.0, 0.0},
            [](VehicleRoutePlan& p) { p.vehicles[0].maxRouteDuration = 640; }, none);
    byHand(
            "vehicleMaxEndTimeCost", [](VehicleRoutePlan& p) { p.vehicles[0].maxEndTime = 600; },
            {40.0, 0.0, 0.0}, [](VehicleRoutePlan& p) { p.vehicles[0].maxEndTime = 640; }, none);
    byHand(
            "maxStopsCost", [](VehicleRoutePlan& p) { p.vehicles[0].maxStops = 3; },
            {1.0, 0.0, 0.0}, [](VehicleRoutePlan& p) { p.vehicles[0].maxStops = 4; }, none);
    // The farthest visit (3, at location 4) is 4 steps from home.
    byHand(
            "maxRouteRadiusCost",
            [&](VehicleRoutePlan& p) {
                p.vehicles[0].maxRouteRadiusMeters = 4 * meterPerStep - 25.0;
            },
            {25.0, 0.0, 0.0},
            [&](VehicleRoutePlan& p) {
                p.vehicles[0].maxRouteRadiusMeters = 4 * meterPerStep + 1.0;
            },
            none);
    // Four visits on one line, 3 steps from the first to the last: radius 1.5.
    byHand(
            "enclosingCircleRadiusCost", keep, {1.5 * meterPerStep, 0.0, 0.0},
            [](VehicleRoutePlan& p) { p.vehicles[0].visitIndices = {0}; }, none);
    // Target 16 * 1.0 against 12 delivered; 16 * 0.75 = 12 is exactly met.
    byHand(
            "minCapacityUtilizationCost",
            [](VehicleRoutePlan& p) {
                p.vehicles[0].capacity = 16;
                p.vehicles[0].minLoadRatio = 1.0;
            },
            {4.0, 0.0, 0.0},
            [](VehicleRoutePlan& p) {
                p.vehicles[0].capacity = 16;
                p.vehicles[0].minLoadRatio = 0.75;
            },
            none);
    byHand("routeDurationCost", keep, {640.0, 0.0, 0.0}, park, none);
    // 100 + 260 + 420 + 580.
    byHand("latencyCost", keep, {1360.0, 0.0, 0.0}, park, none);
    byHand(
            "vehicleFixedCost", [](VehicleRoutePlan& p) { p.vehicles[0].fixedCost = 50.0; },
            {50.0, 0.0, 0.0},
            [](VehicleRoutePlan& p) { p.vehicles[1].fixedCost = 50.0; },  // vehicle 1 stays unused
            none);
    // Visits 2 and 3 unrouted: 1 automatic minute + 90 explicit minutes.
    byHand("unassignedVisitsCost",
           [](VehicleRoutePlan& p) {
               p.vehicles[0].visitIndices = {0, 1};
               p.visits[3].unassignedPenaltyMinutes = 90;
           },
           {91.0}, keep, {0.0});
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (k) The catalog read off the default assembly == the components' constants.
// -------------------------------------------------------------------------
template <typename Component>
bool rowIs(const optimaru::ConstraintInfo& row, const char* name, optimaru::Level level,
           double weight, bool is_global) {
    return row.name == name && row.default_level == Component::kDefaultLevel &&
           row.default_weight == Component::kDefaultWeight && row.default_level == level &&
           row.default_weight == weight && row.level_flippable && !row.reward &&
           row.is_global == is_global && row.default_enabled == (weight > 0.0);
}

void verifyCatalog() {
    std::printf("== (k) Catalog == component constants ==\n");
    using optimaru::Level;
    const auto rows = defaultConstraints().info();
    check(rows.size() == 14, "14 catalog rows");
    if (rows.size() != 14) return;
    // Unit constraints first, then the global, in emplace order: the
    // list_constraints() order.
    check(rowIs<VehicleCapacityCost>(rows[0], "vehicleCapacityCost", Level::Hard, 1.0, false) &&
                  rowIs<MaxEndTimeCost>(rows[1], "maxEndTimeCost", Level::Hard, 1.0, false) &&
                  rowIs<TravelTimeCost>(rows[2], "travelTimeCost", Level::Soft, 1.0, false) &&
                  rowIs<MaxTravelTimeCost>(rows[3], "maxTravelTimeCost", Level::Hard, 1.0, false) &&
                  rowIs<MaxRouteDurationCost>(rows[4], "maxRouteDurationCost", Level::Hard, 1.0,
                                              false) &&
                  rowIs<VehicleMaxEndTimeCost>(rows[5], "vehicleMaxEndTimeCost", Level::Hard, 1.0,
                                               false) &&
                  rowIs<MaxStopsCost>(rows[6], "maxStopsCost", Level::Hard, 1.0, false) &&
                  rowIs<MaxRouteRadiusCost>(rows[7], "maxRouteRadiusCost", Level::Hard, 1.0,
                                            false) &&
                  rowIs<EnclosingCircleRadiusCost>(rows[8], "enclosingCircleRadiusCost",
                                                   Level::Soft, 0.0, false) &&
                  rowIs<MinCapacityUtilizationCost>(rows[9], "minCapacityUtilizationCost",
                                                    Level::Soft, 1.0, false) &&
                  rowIs<RouteDurationCost>(rows[10], "routeDurationCost", Level::Soft, 0.0,
                                           false) &&
                  rowIs<LatencyCost>(rows[11], "latencyCost", Level::Soft, 0.0, false) &&
                  rowIs<VehicleFixedCost>(rows[12], "vehicleFixedCost", Level::Soft, 1.0, false) &&
                  rowIs<UnassignedVisitsCost>(rows[13], "unassignedVisitsCost", Level::Medium, 1.0,
                                              true),
          "info() == the components' kDefaultWeight / kDefaultLevel");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (l) The medium level: unassignedVisitsCost.
// -------------------------------------------------------------------------
optimaru::DecomposedObjective<VehicleRoutePlan>::LevelScores levelsWith(ConstraintAssembly assembly,
                                                                        VehicleRoutePlan& plan) {
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    calculator.rebuild(plan);
    calculator.score();
    return calculator.level_scores();
}

void verifyMediumLevel() {
    std::printf("== (l) Medium level ==\n");
    // Vehicle 0 serves visits 0 and 1; visits 2 and 3 are dropped: 2 medium
    // minutes, 200 s of driving.
    VehicleRoutePlan plan = buildHandInstance();
    plan.vehicles[0].visitIndices = {0, 1};
    plan.rebuildUnassignedPool();
    const Score monolithic = ScoreCalculator::score(plan);
    check(monolithic.hard == 0 && monolithic.medium == -2 && monolithic.soft == -200,
          "the hand plan drops 2 medium minutes");

    {
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        calculator.rebuild(plan);
        const optimaru::Score engine = calculator.score();
        check(engine.hard == 0 && engine.soft == -200.0 - 2.0 * static_cast<double>(kMediumUnit),
              "default: medium folded into soft x 10^9");
        check(engine.hard == toEngineScore(monolithic).hard &&
                      engine.soft == toEngineScore(monolithic).soft,
              "default: the fold equals the monolithic Score adapter");
        const auto levels = calculator.level_scores();
        check(levels.hard == 0 && levels.medium == -2.0 && levels.soft == -200.0,
              "levelScores() reports the unfolded levels");
    }
    {
        const auto levels = levelsWith(defaultConstraints().hard("unassignedVisitsCost"), plan);
        check(levels.hard == -2 && levels.medium == 0.0 && levels.soft == -200.0,
              "hard(): one hard unit per raw minute");
    }
    {
        const auto levels = levelsWith(defaultConstraints().soft("unassignedVisitsCost"), plan);
        check(levels.hard == 0 && levels.medium == 0.0 && levels.soft == -202.0,
              "soft(): one soft unit per raw minute, outside the fold");
    }
    {
        const auto levels =
                levelsWith(defaultConstraints().weight("unassignedVisitsCost", 3), plan);
        check(levels.hard == 0 && levels.medium == -6.0 && levels.soft == -200.0,
              "weight 3 stays in medium units");
    }
    {
        const auto levels = levelsWith(
                defaultConstraints().level("travelTimeCost", optimaru::Level::Medium), plan);
        check(levels.hard == 0 && levels.medium == -202.0 && levels.soft == 0.0,
              "level(Medium): travelTimeCost moves onto medium, one medium unit per second");
    }
    {
        const auto levels = levelsWith(defaultConstraints()
                                               .level("travelTimeCost", optimaru::Level::Medium)
                                               .weight("travelTimeCost", 0.5),
                                       plan);
        check(levels.hard == 0 && levels.medium == -102.0 && levels.soft == 0.0,
              "a weight on a moved constraint is in medium units");
    }
    std::printf("\n");
}

// Incremental == full recompute and every move preview == the applied score
// with the medium term overridden: a random walk of list moves through the
// Editor protocol, then the preview harness on the same assembly. The
// sequence-shape domain previews through the global's piecewise contract, so this is
// the path a medium term takes in the engine's move evaluation.
void verifyMediumIncremental() {
    std::printf("== (l) Medium overrides: incremental == full, previews == applied ==\n");
    const std::vector<std::pair<std::string, std::function<ConstraintAssembly()>>> variants{
            {"default", [] { return defaultConstraints(); }},
            {"hard", [] { return defaultConstraints().hard("unassignedVisitsCost"); }},
            {"soft", [] { return defaultConstraints().soft("unassignedVisitsCost"); }},
            {"weight 3", [] { return defaultConstraints().weight("unassignedVisitsCost", 3); }},
            {"with travelTimeCost on medium",
             [] { return defaultConstraints().level("travelTimeCost", optimaru::Level::Medium); }},
    };
    using Descriptor = optimaru::SolutionTraits<VehicleRoutePlan>;
    for (const auto& variant : variants) {
        VehicleRoutePlan plan = buildRandomInstance(30, 4, /*seed=*/17);
        std::mt19937_64 layout(23);
        randomizeRoutes(plan, layout, /*poolRate=*/0.15);
        plan.rebuildUnassignedPool();
        DecomposedCalculator calculator;
        variant.second().into(calculator);
        optimaru::Editor<VehicleRoutePlan> editor(plan, calculator);
        std::mt19937_64 rng(41);
        int mismatches = 0;
        int mediumSteps = 0;
        for (int step = 0; step < 600; ++step) {
            const int list_count = Descriptor::list_count(plan);
            const int from = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
            if (Descriptor::list_size(plan, from) == 0) continue;
            const int position = static_cast<int>(
                    rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, from)));
            const int element = Descriptor::element_at(plan, from, position);
            editor.remove(from, position);
            const int to = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
            const int insert_at = static_cast<int>(
                    rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, to) + 1));
            editor.insert(to, insert_at, element);

            const optimaru::Score incremental = editor.score();
            DecomposedCalculator fresh;
            variant.second().into(fresh);
            fresh.rebuild(plan);
            const optimaru::Score full = fresh.score();
            if (fresh.level_scores().medium != 0.0) ++mediumSteps;
            if (incremental.hard != full.hard || std::fabs(incremental.soft - full.soft) > 1e-6)
                ++mismatches;
        }
        check(mismatches == 0, "unassignedVisitsCost " + variant.first +
                                       ": 600 random list moves, incremental == full recompute");
        if (variant.first != "hard" && variant.first != "soft")
            check(mediumSteps > 0, variant.first + ": the walk visits medium states");

        const optimaru::testing::PreviewParity parity = optimaru::testing::run_preview_parity(
                editor, calculator, /*seed=*/9, /*trials=*/1500);
        check(parity.all_equal() && parity.declined == 0 && parity.previewed > 500,
              "unassignedVisitsCost " + variant.first +
                      ": every preview served and equal to the applied score");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (m) on_new_best on the plugin path reports the levels plan.score reports.
// -------------------------------------------------------------------------
// With travelTimeCost at weight 10^7 the soft total passes 10^9, which a
// decoded fold would split into medium + (soft mod 10^9). The last new best
// is the returned solution, so its report must equal plan.score exactly.
void verifyPluginCallbackLevels() {
    std::printf("== (m) Plugin-path onNewBest levels == plan.score ==\n");
    VehicleRoutePlan plan = buildRandomInstance(25, 4, /*seed=*/9);
    plan.rebuildUnassignedPool();

    Solver::Config config;
    config.time_limit = std::chrono::seconds(10);
    config.step_limit = 3000;
    config.seed = 5;
    std::vector<Score> reports;
    config.on_new_best = [&](std::int64_t, const Score& best) { reports.push_back(best); };

    constexpr double kWeight = 1e7;
    DecomposedCalculator calculator;
    defaultConstraints().weight("travelTimeCost", kWeight).into(calculator);
    Solver(config).solve(plan, calculator);

    check(!reports.empty(), "onNewBest fired");
    check(plan.score.soft <= -kMediumUnit, "the final soft total is 10^9 or more");
    check(!reports.empty() && reports.back() == plan.score,
          "the last onNewBest score == plan.score (" + plan.score.str() + ")");
    // Folded back, the reports are the engine's successive bests: strictly
    // improving in its (hard, soft) order.
    bool improving = true;
    for (std::size_t i = 1; i < reports.size(); ++i)
        improving = improving && toEngineScore(reports[i - 1]) < toEngineScore(reports[i]);
    check(improving, std::to_string(reports.size()) +
                             " reports, strictly improving once folded into the engine score");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (n) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with add_instances(), as the Python bindings
// build it from a pick list.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string& name) {
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

// The hand instance with a third vehicle: vehicle 0 (capacity 4) on visits
// 0 1 drives 200 s and carries 6 (2 over), vehicle 1 on visit 3 drives
// 400 s, vehicle 2 stays idle, visit 2 waits in the pool with 5 penalty
// minutes. Raw per unit (3 vehicles, pool): travel [200, 400, 0, 0],
// capacity [2, 0, 0, 0]; unassigned total 5.
VehicleRoutePlan repeatInstance() {
    VehicleRoutePlan plan = buildHandInstance();
    Vehicle idle = plan.vehicles[1];
    idle.id = "2";
    plan.vehicles.push_back(idle);
    plan.vehicles[0].capacity = 4;
    plan.vehicles[0].visitIndices = {0, 1};
    plan.vehicles[1].visitIndices = {3};
    plan.visits[2].unassignedPenaltyMinutes = 5;
    plan.rebuildUnassignedPool();
    return plan;
}

DecomposedCalculator::LevelScores levelsOf(ConstraintAssembly assembly, VehicleRoutePlan plan) {
    return levelsWith(std::move(assembly), plan);
}

// Every shaped flavour at once, on a unit constraint answering piecewise
// (travel, capacity), one that does not (latency, maxEndTime) and the global.
ConstraintAssembly repeatedAssembly() {
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("travelTimeCost", 3000.0).clip("travelTimeCost", false);
    assembly.offset("unassignedVisitsCost", 1.0);
    assembly.add_instances(std::move(copyOf("travelTimeCost")
                                             .weight("travelTimeCost", 0.001)
                                             .exponent("travelTimeCost", 2.0)
                                             .offset("travelTimeCost", 2000.0)));
    // The literal capacity copy sits on soft: hard refuses the literal
    // formula with an offset (hard 0 must mean no violation).
    assembly.add_instances(std::move(copyOf("vehicleCapacityCost")
                                             .soft("vehicleCapacityCost")
                                             .weight("vehicleCapacityCost", 2.0)
                                             .offset("vehicleCapacityCost", 1.0)
                                             .clip("vehicleCapacityCost", false)));
    assembly.add_instances(std::move(copyOf("vehicleCapacityCost")
                                             .weight("vehicleCapacityCost", 2.0)
                                             .offset("vehicleCapacityCost", 1.0)));
    assembly.add_instances(std::move(copyOf("unassignedVisitsCost")
                                             .weight("unassignedVisitsCost", 2.0)
                                             .offset("unassignedVisitsCost", 3.0)
                                             .clip("unassignedVisitsCost", false)));
    assembly.add_instances(std::move(copyOf("maxEndTimeCost").offset("maxEndTimeCost", -2.0)));
    assembly.add_instances(std::move(copyOf("latencyCost")
                                             .weight("latencyCost", 0.01)
                                             .offset("latencyCost", 5000.0)
                                             .clip("latencyCost", false)));
    return assembly;
}

void verifyRepeatsAndOffsets() {
    std::printf("== (n) Repeated components, offset / clip ==\n");
    {
        VehicleRoutePlan plan = repeatInstance();
        ConstraintAssembly assembly = copyOf("travelTimeCost");
        assembly.add_instances(std::move(copyOf("travelTimeCost")
                                                 .weight("travelTimeCost", 0.5)
                                                 .exponent("travelTimeCost", 2.0)));
        DecomposedCalculator calculator;
        assembly.into(calculator);
        calculator.rebuild(plan);
        const optimaru::Score score = calculator.score();
        check(score.hard == 0 && score.soft == -(600.0 + 0.5 * (200.0 * 200.0 + 400.0 * 400.0)),
              "travel + 0.5 x travel^2 per vehicle: -(600 + 100000) soft");
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].unit_costs == entries[1].unit_costs &&
                      entries[0].unit_costs == std::vector<double>{200.0, 400.0, 0.0, 0.0} &&
                      entries[1].weight == 0.5 && entries[1].exponent == 2.0 &&
                      entries[1].default_weight == 1.0,
              "two instances, the same raw costs, each with its own knobs");
        check(entries.size() == 2 && entries[0].scoped && entries[1].scoped &&
                      entries[0].units == std::vector<int>{0, 1, 2} &&
                      entries[1].units == std::vector<int>{0, 1, 2},
              "a per-vehicle component evaluates the three vehicles, not the pool");
    }
    {
        // Threshold at 300 s: only vehicle 1's 400 - 300 = 100 counts.
        check(levelsOf(copyOf("travelTimeCost").offset("travelTimeCost", 300.0), repeatInstance())
                              .soft == -100.0,
              "threshold offset 300: 100");
        // Literal: (200 - 300) + (400 - 300) + (0 - 300) = -300, the idle
        // vehicle included and the pool not (it would make -600).
        check(levelsOf(copyOf("travelTimeCost")
                               .offset("travelTimeCost", 300.0)
                               .clip("travelTimeCost", false),
                       repeatInstance())
                              .soft == 300.0,
              "literal offset 300: -100 + 100 - 300 -> +300 soft, the pool not charged");
        // A negative threshold offset adds 50 to every non-zero vehicle.
        check(levelsOf(copyOf("travelTimeCost").offset("travelTimeCost", -50.0), repeatInstance())
                              .soft == -700.0,
              "threshold offset -50: 250 + 450, the idle vehicle stays 0");
        check(levelsOf(copyOf("travelTimeCost")
                               .offset("travelTimeCost", -50.0)
                               .clip("travelTimeCost", false),
                       repeatInstance())
                              .soft == -750.0,
              "literal offset -50: 250 + 450 + 50");
        // The literal capacity pick on soft: (2 - 2.5) + (0 - 2.5) x 2 =
        // -5.5, the idle vehicle included: +5.5 soft (not rounded).
        const auto literal = levelsOf(copyOf("vehicleCapacityCost")
                                              .soft("vehicleCapacityCost")
                                              .offset("vehicleCapacityCost", 2.5)
                                              .clip("vehicleCapacityCost", false),
                                      repeatInstance());
        check(literal.hard == 0 && literal.soft == 5.5,
              "capacity literal offset 2.5 on soft: -0.5 - 2.5 - 2.5 -> +5.5 soft");
        // A hard threshold pick rounds per vehicle: travel moved to hard at
        // weight 0.005 beyond 100 s: llround(0.5) + llround(1.5) = 3 (a
        // rounded total would give 2).
        check(levelsOf(copyOf("travelTimeCost")
                               .hard("travelTimeCost")
                               .weight("travelTimeCost", 0.005)
                               .offset("travelTimeCost", 100.0),
                       repeatInstance())
                              .hard == -3,
              "travel on hard, threshold offset 100 at weight 0.005: llround per vehicle, -3 hard");
        // The global's total is shaped once: (5 - 2)^2 = 9 medium minutes.
        const auto global = levelsOf(copyOf("unassignedVisitsCost")
                                             .offset("unassignedVisitsCost", 2.0)
                                             .exponent("unassignedVisitsCost", 2.0),
                                     repeatInstance());
        check(global.medium == -9.0 && global.hard == 0 && global.soft == 0.0,
              "unassigned threshold offset 2, exponent 2: -(5 - 2)^2 medium");
        const auto reward = levelsOf(copyOf("unassignedVisitsCost")
                                             .offset("unassignedVisitsCost", 8.0)
                                             .clip("unassignedVisitsCost", false),
                                     repeatInstance());
        check(reward.medium == 3.0, "unassigned literal offset 8: 5 - 8 -> +3 medium");
        // The medium instances add up: 5 + 2 x 5.
        const auto twice = levelsOf(
                std::move(copyOf("unassignedVisitsCost")
                                  .add_instances(
                                          std::move(copyOf("unassignedVisitsCost")
                                                            .weight("unassignedVisitsCost", 2.0)))),
                repeatInstance());
        check(twice.medium == -15.0, "a repeated medium pick: -5 - 10 medium");
    }
    {
        // On a level that counts toward the hard score the literal formula
        // with an offset is refused at reset, whichever sign and setter
        // order: +2.5 would reward the vehicles below it (+7 hard, so one
        // vehicle's spare capacity would cancel another's overload), -2.5
        // would charge three vehicles that hold no violation. Medium folds
        // into soft here, so it keeps the literal formula.
        auto resetRefusal = [](ConstraintAssembly assembly) {
            VehicleRoutePlan plan = repeatInstance();
            try {
                levelsWith(std::move(assembly), plan);
            } catch (const std::logic_error& error) {
                return std::string(error.what());
            }
            return std::string();
        };
        auto refusal = [](const std::string& name) {
            return "Constraint '" + name +
                   "' cannot use clip=false with a non-zero offset on a level that counts toward "
                   "the hard score (hard 0 must mean no violation)";
        };
        check(resetRefusal(copyOf("vehicleCapacityCost")
                                   .offset("vehicleCapacityCost", 2.5)
                                   .clip("vehicleCapacityCost", false)) ==
                              refusal("vehicleCapacityCost") &&
                      resetRefusal(copyOf("vehicleCapacityCost")
                                           .clip("vehicleCapacityCost", false)
                                           .offset("vehicleCapacityCost", -2.5)) ==
                              refusal("vehicleCapacityCost"),
              "capacity literal offset +-2.5 on hard (its default): refused at reset");
        check(resetRefusal(copyOf("travelTimeCost")
                                   .offset("travelTimeCost", 300.0)
                                   .clip("travelTimeCost", false)
                                   .hard("travelTimeCost")) == refusal("travelTimeCost"),
              "travel literal offset moved to hard afterwards: refused at reset");
        ConstraintAssembly repeated = copyOf("vehicleCapacityCost");
        repeated.add_instances(std::move(copyOf("vehicleCapacityCost")
                                                 .offset("vehicleCapacityCost", 1.0)
                                                 .clip("vehicleCapacityCost", false)));
        check(resetRefusal(std::move(repeated)) == refusal("vehicleCapacityCost"),
              "a repeated capacity instance with the literal offset on hard: refused at reset");
        check(resetRefusal(copyOf("vehicleCapacityCost").offset("vehicleCapacityCost", 2.5))
                              .empty() &&
                      resetRefusal(copyOf("vehicleCapacityCost").clip("vehicleCapacityCost", false))
                              .empty() &&
                      resetRefusal(copyOf("unassignedVisitsCost")
                                           .offset("unassignedVisitsCost", 8.0)
                                           .clip("unassignedVisitsCost", false))
                              .empty(),
              "allowed: a threshold offset or clip=false with offset 0 on hard, the literal "
              "offset on the into-soft medium");
    }
    using Descriptor = optimaru::SolutionTraits<VehicleRoutePlan>;
    {
        // Random walk: incremental == a fresh calculator of the same assembly.
        VehicleRoutePlan plan = buildRandomInstance(30, 4, /*seed=*/17);
        std::mt19937_64 layout(29);
        randomizeRoutes(plan, layout, /*poolRate=*/0.15);
        plan.rebuildUnassignedPool();
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        optimaru::Editor<VehicleRoutePlan> editor(plan, calculator);
        std::mt19937_64 rng(43);
        int mismatches = 0;
        double drift = 0.0;
        const int steps = 1500;
        for (int step = 0; step < steps; ++step) {
            const int list_count = Descriptor::list_count(plan);
            const int from = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
            if (Descriptor::list_size(plan, from) == 0) continue;
            const int position = static_cast<int>(
                    rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, from)));
            const int element = Descriptor::element_at(plan, from, position);
            editor.remove(from, position);
            const int to = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
            const int insert_at = static_cast<int>(
                    rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, to) + 1));
            editor.insert(to, insert_at, element);

            const optimaru::Score incremental = editor.score();
            DecomposedCalculator fresh;
            repeatedAssembly().into(fresh);
            fresh.rebuild(plan);
            const optimaru::Score full = fresh.score();
            drift = std::max(drift, std::fabs(full.soft - incremental.soft));
            if (!optimaru::scores_agree(incremental, full)) ++mismatches;
        }
        std::printf("    largest soft drift %.3g\n", drift);
        check(mismatches == 0, std::to_string(steps) +
                                       " random list moves with repeated, shifted components: "
                                       "incremental == fresh");
    }
    {
        // Every preview == the applied score, re-checked under ScoreChecks::Full.
        VehicleRoutePlan plan = buildRandomInstance(30, 4, /*seed=*/19);
        std::mt19937_64 layout(31);
        randomizeRoutes(plan, layout, /*poolRate=*/0.10);
        plan.rebuildUnassignedPool();
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        optimaru::Editor<VehicleRoutePlan> editor(plan, calculator, optimaru::ScoreChecks::Full);
        editor.score();
        optimaru::testing::PreviewParity parity;
        bool corrupted = false;
        try {
            parity = optimaru::testing::run_preview_parity(editor, calculator, /*seed=*/13,
                                                           /*trials=*/3000);
        } catch (const optimaru::ScoreMismatchError& error) {
            corrupted = true;
            std::printf("    %s\n", error.what());
        }
        std::printf("    %d moves checked, %d previewed, %d declined\n", parity.checked,
                    parity.previewed, parity.declined);
        check(!corrupted && parity.all_equal() && parity.declined == 0 && parity.previewed > 1000,
              "repeated, shifted components: every preview served and equal to the applied "
              "score (FULL_ASSERT)");
    }
    {
        VehicleRoutePlan plan = buildRandomInstance(25, 4, /*seed=*/9);
        plan.rebuildUnassignedPool();
        Solver::Config config;
        config.time_limit = std::chrono::seconds(2);
        config.seed = 3;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruin_recreate_unimproved_step_trigger = 200;
        config.ruin_recreate_polish_steps = 100;
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        bool corrupted = false;
        try {
            Solver solver(config);
            solver.solve(plan, calculator);
        } catch (const optimaru::ScoreMismatchError& e) {
            corrupted = true;
            std::printf("  ScoreCorruptionError: %s\n", e.what());
        }
        check(!corrupted, "2s FULL_ASSERT solve with repeated, shifted components");
        // plan.score rounds each level on its own (the fractional weights
        // leave fractions), exactly as the solver's levelScore does.
        const auto levels = levelsWith(repeatedAssembly(), plan);
        check(!corrupted && plan.score.hard == levels.hard &&
                      plan.score.medium == optimaru::round_score_level(levels.medium, "medium") &&
                      plan.score.soft == optimaru::round_score_level(levels.soft, "soft"),
              "the reported levels == a fresh recompute of the same assembly");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (o) Negative thresholds. The threshold with an offset < 0 is a fixed
// charge per vehicle with a non-zero raw cost: it jumps at 0, so a raw cost
// that should be 0 must be exactly 0 on every path. The enclosing radius is
// real-valued (meters); the travel and route times are whole seconds; the
// capacity threshold sits on hard. The calculator previews these instances
// through cost() on the written-out route (never the shared sums) and costs
// every applied route with cost(), so previews, the incremental score and a
// fresh recompute agree; the last on_new_best report (the incremental score of
// the best solution) is plan.score (a fresh recompute of it).
// -------------------------------------------------------------------------
ConstraintAssembly negativeThresholdAssembly() {
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("travelTimeCost", -60.0);
    assembly.offset("vehicleCapacityCost", -0.5);
    assembly.weight("enclosingCircleRadiusCost", 0.01).offset("enclosingCircleRadiusCost", -60.0);
    assembly.weight("routeDurationCost", 0.001).offset("routeDurationCost", -60.0);
    assembly.offset("unassignedVisitsCost", -1.0);
    assembly.add_instances(std::move(copyOf("travelTimeCost")
                                             .weight("travelTimeCost", 1e-4)
                                             .exponent("travelTimeCost", 2.0)
                                             .offset("travelTimeCost", -30.0)));
    return assembly;
}

void verifyNegativeThresholds() {
    std::printf("== (o) Negative thresholds: a fixed charge, computed exactly ==\n");
    {
        VehicleRoutePlan plan = buildRandomInstance(30, 5, /*seed=*/23);
        std::mt19937_64 layout(37);
        randomizeRoutes(plan, layout, /*poolRate=*/0.15);
        plan.rebuildUnassignedPool();
        DecomposedCalculator calculator;
        negativeThresholdAssembly().into(calculator);
        optimaru::Editor<VehicleRoutePlan> editor(plan, calculator, optimaru::ScoreChecks::Full);
        editor.score();
        optimaru::testing::PreviewParity parity;
        bool corrupted = false;
        try {
            parity = optimaru::testing::run_preview_parity(editor, calculator, /*seed=*/71,
                                                           /*trials=*/3000);
        } catch (const optimaru::ScoreMismatchError& error) {
            corrupted = true;
            std::printf("    %s\n", error.what());
        }
        std::printf("    %d moves checked, %d previewed, %d declined\n", parity.checked,
                    parity.previewed, parity.declined);
        check(!corrupted && parity.all_equal() && parity.declined == 0 && parity.previewed > 1000,
              "negative thresholds: every preview equal to the applied score (FULL_ASSERT)");
    }
    {
        VehicleRoutePlan plan = buildRandomInstance(30, 5, /*seed=*/23);
        plan.rebuildUnassignedPool();
        Solver::Config config;
        config.time_limit = std::chrono::seconds(2);
        config.seed = 11;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.ruin_recreate_unimproved_step_trigger = 200;
        config.ruin_recreate_polish_steps = 100;
        DecomposedCalculator calculator;
        negativeThresholdAssembly().into(calculator);
        bool corrupted = false;
        try {
            Solver(config).solve(plan, calculator);
        } catch (const optimaru::ScoreMismatchError& e) {
            corrupted = true;
            std::printf("  ScoreCorruptionError: %s\n", e.what());
        }
        check(!corrupted, "2s FULL_ASSERT solve with negative thresholds");
    }
    {
        VehicleRoutePlan plan = buildRandomInstance(30, 5, /*seed=*/23);
        plan.rebuildUnassignedPool();
        Solver::Config config;
        config.time_limit = std::chrono::seconds(10);
        config.step_limit = 20000;
        config.seed = 13;
        std::vector<Score> reports;
        config.on_new_best = [&](std::int64_t, const Score& best) { reports.push_back(best); };
        DecomposedCalculator calculator;
        negativeThresholdAssembly().into(calculator);
        Solver(config).solve(plan, calculator);
        const auto levels = levelsWith(negativeThresholdAssembly(), plan);
        check(plan.score.hard == levels.hard &&
                      plan.score.medium == optimaru::round_score_level(levels.medium, "medium") &&
                      plan.score.soft == optimaru::round_score_level(levels.soft, "soft"),
              "20000 steps: plan.score == a fresh recompute of the same assembly");
        check(!reports.empty() && reports.back() == plan.score,
              "the last onNewBest report (incremental) == plan.score (fresh)");
    }
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
    verifyRawCostsByHand();
    verifyCatalog();
    verifyMediumLevel();
    verifyMediumIncremental();
    verifyPluginCallbackLevels();
    verifyRepeatsAndOffsets();
    verifyNegativeThresholds();

    if (failures == 0) {
        std::printf("All vehicle-routing plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d vehicle-routing plugin check(s) FAILED.\n", failures);
    return 1;
}
