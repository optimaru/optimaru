// Pickup-and-delivery tests: the coupling constraints, the incremental
// calculator against the full recompute, and the engine integration.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "pdp/demo_data.hpp"
#include "pdp/domain.hpp"
#include "pdp/haversine.hpp"
#include "pdp/incremental_calculator.hpp"
#include "pdp/score_calculator.hpp"
#include "pdp/solver.hpp"
#include "optimaru/testing/check.hpp"

using namespace pdp;

namespace {

using optimaru::testing::check;
using optimaru::testing::failures;

// A vehicle with the four fields these tests set; every other field keeps its
// default (no route policy, an empty route).
Vehicle vehicleAt(std::string id, int capacity, int home, Seconds departure) {
    Vehicle vehicle;
    vehicle.id = std::move(id);
    vehicle.capacity = capacity;
    vehicle.homeLocationIndex = home;
    vehicle.departureTime = departure;
    return vehicle;
}

// A two-request instance with one vehicle, built by hand so the constraint
// expectations are exact. `demand` is per request; the vehicle's capacity is
// 10, so demand 4 leaves both requests carryable at once and demand 6 does not
// — which is how the capacity case is isolated from the coupling cases.
PdpRoutePlan buildTinyInstance(int demand = 4) {
    PdpRoutePlan plan;
    plan.name = "tiny";
    const int home = addLocation(plan, 43.77, 11.25);
    const int p1 = addLocation(plan, 43.78, 11.26);
    const int d1 = addLocation(plan, 43.79, 11.27);
    const int p2 = addLocation(plan, 43.76, 11.24);
    const int d2 = addLocation(plan, 43.75, 11.23);

    plan.vehicles = {vehicleAt("1", 10, home, hourMinute(7, 0))};
    plan.nodes = {
            Node{"p1", p1, hourMinute(8, 0), hourMinute(18, 0), 300, 0, true, -1, NULL_TIME},
            Node{"d1", d1, hourMinute(8, 0), hourMinute(18, 0), 300, 0, false, -1, NULL_TIME},
            Node{"p2", p2, hourMinute(8, 0), hourMinute(18, 0), 300, 1, true, -1, NULL_TIME},
            Node{"d2", d2, hourMinute(8, 0), hourMinute(18, 0), 300, 1, false, -1, NULL_TIME},
    };
    plan.deliveries = {Delivery{"r1", 0, 1, demand}, Delivery{"r2", 2, 3, demand}};
    HaversineDrivingTimeCalculator::initDrivingTimeMatrix(plan);
    return plan;
}

void testCouplingConstraints() {
    std::printf("== PDP coupling constraints ==\n");
    const std::int64_t penalty = ScoreCalculator::kCouplingPenalty;

    // Legal: p1 d1 p2 d2 on one vehicle.
    PdpRoutePlan legal = buildTinyInstance();
    legal.vehicles[0].nodeIndices = {0, 1, 2, 3};
    legal.rebuildUnassignedPool();
    Score s = ScoreCalculator::score(legal);
    check(s.hard == 0, "a properly ordered pair is feasible");
    check(s.medium == 0, "nothing unassigned when every stop is routed");
    check(s.soft < 0, "travel time is penalized on the soft level");
    {
        // The route is open by default: home -> p1 -> d1 -> p2 -> d2 with no
        // leg back. Turning returnToDepot on adds exactly d2 -> home.
        const std::vector<int>& route = legal.vehicles[0].nodeIndices;
        Seconds open = 0;
        int previous = legal.vehicles[0].homeLocationIndex;
        for (int nodeIndex : route) {
            const int location = legal.nodes[static_cast<std::size_t>(nodeIndex)].locationIndex;
            open += legal.drivingTime(previous, location);
            previous = location;
        }
        const Seconds back = legal.drivingTime(previous, legal.vehicles[0].homeLocationIndex);
        check(back > 0, "the closing leg of the tiny instance is not free");
        check(-s.soft == open, "an open route charges no leg back home");
        legal.vehicles[0].returnToDepot = true;
        const Score closed = ScoreCalculator::score(legal);
        check(-closed.soft == open + back, "returnToDepot adds exactly the last-stop -> home leg");
        legal.vehicles[0].returnToDepot = false;
        ScoreCalculator::score(legal);
    }

    // Dropoff before pickup.
    PdpRoutePlan reversed = buildTinyInstance();
    reversed.vehicles[0].nodeIndices = {1, 0, 2, 3};
    reversed.rebuildUnassignedPool();
    check(ScoreCalculator::score(reversed).hard == -penalty,
          "dropoff before pickup is penalized once");

    // Half-assigned: p1 routed, d1 left out.
    PdpRoutePlan half = buildTinyInstance();
    half.vehicles[0].nodeIndices = {0, 2, 3};
    half.rebuildUnassignedPool();
    check(ScoreCalculator::score(half).hard == -penalty, "a half-routed request is penalized once");

    // Both stops unassigned: medium, not hard.
    PdpRoutePlan dropped = buildTinyInstance();
    dropped.vehicles[0].nodeIndices = {2, 3};
    dropped.rebuildUnassignedPool();
    Score droppedScore = ScoreCalculator::score(dropped);
    check(droppedScore.hard == 0, "a fully dropped request is not hard-infeasible");
    check(droppedScore.medium == -10,
          "a dropped request costs its two service durations (5+5 min)");

    // Capacity: with demand 6 each, carrying both at once is 12 > capacity 10.
    PdpRoutePlan overloaded = buildTinyInstance(/*demand=*/6);
    overloaded.vehicles[0].nodeIndices = {0, 2, 1, 3};  // p1 p2 d1 d2
    overloaded.rebuildUnassignedPool();
    check(ScoreCalculator::score(overloaded).hard == -2,
          "the running-load peak is what breaks capacity (12 > 10)");

    // The same demand carried one request after the other fits fine — the
    // distinction that makes this a running-load check and not a route total.
    PdpRoutePlan sequential = buildTinyInstance(/*demand=*/6);
    sequential.vehicles[0].nodeIndices = {0, 1, 2, 3};
    sequential.rebuildUnassignedPool();
    check(ScoreCalculator::score(sequential).hard == 0,
          "the same demand is feasible when the requests do not overlap");

    // Split across vehicles.
    PdpRoutePlan split = buildTinyInstance();
    split.vehicles.push_back(vehicleAt("2", 10, 0, hourMinute(7, 0)));
    split.vehicles[0].nodeIndices = {0, 2, 3};
    split.vehicles[1].nodeIndices = {1};
    split.rebuildUnassignedPool();
    check(ScoreCalculator::score(split).hard == -penalty,
          "a pair split across two vehicles is penalized once");
    std::printf("\n");
}

void testIncrementalMatchesFullRecompute() {
    std::printf("== Incremental calculator vs full recompute ==\n");
    PdpRoutePlan plan = buildRandomInstance(25, 4, /*seed=*/11);
    plan.rebuildUnassignedPool();

    PdpIncrementalScoreCalculator calculator;
    calculator.rebuild(plan);

    using Descriptor = optimaru::SolutionTraits<PdpRoutePlan>;
    std::mt19937_64 rng(3);
    bool ok = true;
    for (int step = 0; step < 500 && ok; ++step) {
        const int list_count = Descriptor::list_count(plan);
        const int from = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));
        if (Descriptor::list_size(plan, from) == 0) continue;
        const int position = static_cast<int>(
                rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, from)));
        const int to = static_cast<int>(rng() % static_cast<std::uint64_t>(list_count));

        calculator.before_list_edit(from);
        const int element = Descriptor::remove_element(plan, from, position);
        calculator.after_list_edit(from);

        // Score in the transient half-assigned state too: the engine does
        // exactly this between a remove and the matching insert.
        if (!(calculator.current_score() == ScoreCalculator::score(plan))) {
            ok = false;
            std::printf("    step %d (mid-move): incremental %s != full %s\n", step,
                        calculator.current_score().str().c_str(), plan.score.str().c_str());
            break;
        }

        const int insert_at = static_cast<int>(
                rng() % static_cast<std::uint64_t>(Descriptor::list_size(plan, to) + 1));
        calculator.before_list_edit(to);
        Descriptor::insert_element(plan, to, insert_at, element);
        calculator.after_list_edit(to);

        const Score incremental = calculator.current_score();
        const Score full = ScoreCalculator::score(plan);
        if (!(incremental == full)) {
            ok = false;
            std::printf("    step %d: incremental %s != full %s\n", step, incremental.str().c_str(),
                        full.str().c_str());
        }
    }
    check(ok, "incremental == full recompute over 500 random list moves");
    std::printf("\n");
}

void testSolveIntegrity() {
    std::printf("== Engine integration ==\n");
    PdpRoutePlan plan = buildRandomInstance(40, 5, /*seed=*/5);

    Solver::Config config;
    config.time_limit = std::chrono::seconds(5);
    config.seed = 5;
    Solver(config).solve(plan);

    std::vector<int> seen(plan.nodes.size(), 0);
    for (const Vehicle& vehicle : plan.vehicles) {
        for (int nodeIndex : vehicle.nodeIndices) ++seen[static_cast<std::size_t>(nodeIndex)];
    }
    for (int nodeIndex : plan.unassignedNodeIndices) ++seen[static_cast<std::size_t>(nodeIndex)];
    bool conserved = true;
    for (int count : seen) conserved = conserved && count == 1;
    check(conserved, "every stop appears exactly once (routes + pool)");

    const Score reported = plan.score;
    check(reported == ScoreCalculator::score(plan), "reported score matches a full recompute");
    check(plan.score.feasible(),
          "the solution is feasible: every pair coupled, ordered and in "
          "capacity");

    int routed = 0;
    for (const Delivery& delivery : plan.deliveries) {
        if (plan.nodes[static_cast<std::size_t>(delivery.pickupNodeIndex)].vehicleIndex >= 0)
            ++routed;
    }
    std::printf("    40 requests / 5 vehicles in 5s: %s (%d/%d routed)\n", plan.score.str().c_str(),
                routed, static_cast<int>(plan.deliveries.size()));
    std::printf("\n");
}

void testFullAssert() {
    std::printf("== FULL_ASSERT (incremental score consistency) ==\n");
    PdpRoutePlan plan = buildRandomInstance(15, 3, /*seed=*/9);

    Solver::Config config;
    config.time_limit = std::chrono::seconds(3);
    config.seed = 9;
    config.score_checks = optimaru::ScoreChecks::Full;
    config.ruin_recreate_unimproved_step_trigger = 50;
    config.ruin_recreate_polish_steps = 20;

    bool threw = false;
    try {
        Solver(config).solve(plan);
    } catch (const optimaru::ScoreMismatchError&) {
        threw = true;
    }
    check(!threw, "pair moves + pair ruin + recreate pass FULL_ASSERT");
    std::printf("\n");
}

// Tightens an instance until requests genuinely have to be dropped: fewer
// seats and narrower windows than the generator's defaults.
PdpRoutePlan tighten(PdpRoutePlan plan, int capacity, Seconds windowMinutes) {
    for (Vehicle& vehicle : plan.vehicles) {
        vehicle.capacity = capacity;
        // The A/B below was measured on classic round trips. Open routes (the
        // default) loosen the soft landscape enough that a 4-second budget no
        // longer separates the two configurations reliably, so the fixture
        // keeps the closed routes the measurements were taken on.
        vehicle.returnToDepot = true;
    }
    for (Node& node : plan.nodes) node.maxEndTime = node.minStartTime + windowMinutes * 60;
    return plan;
}

// What the pair-aware operators are actually for.
//
// Measured across instance tightness (5 seeds each, 4s budget): on a LOOSE
// instance they are a wash (3 wins / 2 losses) — the built-in element moves
// already route every request, so the two configurations only differ by noise
// in travel time. Their value shows up once capacity and time windows force
// requests to be dropped: 4/1 on both the medium and tight settings, and the
// gap is on the MEDIUM level, not soft. On the tightest seeds the built-ins
// leave roughly twice as many requests unrouted (-320 vs -160 medium).
//
// That is the expected shape, and it is the reason the operators exist:
// moving a request into or out of the unassigned pool is only reachable as a
// pair move. A single stop moved into the pool leaves a half-assigned request,
// which is hard-infeasible and therefore always rejected — so without a pair
// move the set of routed requests is frozen at whatever the construction
// heuristic produced, and the search can only reorder within it.
//
// The check runs on a step budget, not wall clock, and compares the MEDIUM
// level only. With 4s per solve the outcome depended on the machine: a slow
// CI runner got 2/3 and failed, twice on soft-level ties of the medium level
// the claim is about. At 100k steps every seed routes more requests with the
// pair-aware operators (5/0), and the same seed gives the same result anywhere.
void testPairOperatorsMatter() {
    std::printf("== Pair-aware operators vs built-ins alone (tight instance) ==\n");
    int pairWins = 0;
    int builtinWins = 0;
    const int seeds = 5;
    for (int seed = 0; seed < seeds; ++seed) {
        PdpRoutePlan without = tighten(buildRandomInstance(60, 3, /*seed=*/300 + seed),
                                       /*capacity=*/6,
                                       /*windowMinutes=*/120);
        PdpRoutePlan with = without;

        Solver::Config base;
        base.step_limit = 100'000;
        base.time_limit = std::chrono::minutes(1);  // safety net only
        base.seed = static_cast<std::uint64_t>(seed);

        Solver::Config withoutConfig = base;
        withoutConfig.pair_moves = false;
        withoutConfig.pair_ruin = false;
        Solver(withoutConfig).solve(without);

        Solver(base).solve(with);

        std::printf("    seed %d: built-ins only %-28s | pair-aware %s\n", seed,
                    without.score.str().c_str(), with.score.str().c_str());
        if (with.score.medium > without.score.medium)
            ++pairWins;
        else if (without.score.medium > with.score.medium)
            ++builtinWins;
    }
    check(pairWins > builtinWins, "pair-aware operators route more requests on a tight instance");
    std::printf("\n");
}

// A line, built by hand so each insertion is exact: every stop sits at 0 and
// each vehicle at its entry of `homes`, one seat each. A request has to be
// picked up by `pickupClose` and dropped off two service times after that.
constexpr Seconds kLineService = 1000;

struct LineRequest {
    int requiredVehicleIndex;
    Seconds pickupClose;
};

PdpRoutePlan buildLine(const std::vector<Seconds>& homes,
                       const std::vector<LineRequest>& requests) {
    PdpRoutePlan plan;
    plan.name = "line";
    std::vector<Seconds> places = homes;
    places.push_back(0);
    for (std::size_t i = 0; i < places.size(); ++i) addLocation(plan, 0.0, 0.0);
    const int stops = static_cast<int>(homes.size());
    for (std::size_t v = 0; v < homes.size(); ++v) {
        plan.vehicles.push_back(vehicleAt(std::to_string(v), 1, static_cast<int>(v), 0));
    }
    for (std::size_t r = 0; r < requests.size(); ++r) {
        const int request = static_cast<int>(r);
        const std::string id = std::to_string(r);
        const Seconds close = requests[r].pickupClose;
        plan.nodes.push_back(
                Node{"p" + id, stops, 0, close, kLineService, request, true, -1, NULL_TIME});
        plan.nodes.push_back(Node{"d" + id, stops, 0, close + 2 * kLineService, kLineService,
                                  request, false, -1, NULL_TIME});
        plan.deliveries.push_back(
                Delivery{id, 2 * request, 2 * request + 1, 1, 0, requests[r].requiredVehicleIndex});
    }
    plan.drivingSeconds.assign(places.size(), std::vector<Seconds>(places.size(), 0));
    for (std::size_t a = 0; a < places.size(); ++a) {
        for (std::size_t b = 0; b < places.size(); ++b)
            plan.drivingSeconds[a][b] = std::abs(places[a] - places[b]);
    }
    return plan;
}

// No search at all, so these are the construction's own decisions. The default
// (monolithic) score has no requiredVehicleCost, so the checks look at the
// routes.
void testConstructionRespectsRequiredVehicle() {
    std::printf("== Construction and the required vehicle ==\n");
    Solver::Config config;
    config.time_limit = std::chrono::milliseconds(0);

    // The bound request is right next to vehicle 0 and bound to vehicle 1.
    PdpRoutePlan detour = buildLine({0, 1000}, {{1, 100000}});
    Solver(config).solve(detour);
    check(detour.vehicles[0].nodeIndices.empty() &&
                  detour.vehicles[1].nodeIndices == std::vector<int>({0, 1}),
          "a bound request goes on its vehicle, not on the cheapest one");

    // Vehicle 0 has room for one of the two. The free one has the tighter
    // window and the shorter trip, so taking the requests in window order
    // would strand the bound one although vehicle 1 can serve the free one.
    PdpRoutePlan contested = buildLine({0, 100}, {{-1, 1200}, {0, 1300}});
    Solver(config).solve(contested);
    check(contested.vehicles[0].nodeIndices == std::vector<int>({2, 3}) &&
                  contested.vehicles[1].nodeIndices == std::vector<int>({0, 1}) &&
                  contested.score.feasible(),
          "a bound request is placed before a free one can take its slot");

    // Vehicle 1 is too far away to make the window; vehicle 0 is not.
    PdpRoutePlan unreachable = buildLine({0, 5000}, {{1, 1200}});
    Solver(config).solve(unreachable);
    check(unreachable.vehicles[0].nodeIndices.empty() &&
                  unreachable.vehicles[1].nodeIndices.empty() &&
                  unreachable.unassignedNodeIndices.size() == 2,
          "a bound request with no feasible slot stays unassigned");

    PdpRoutePlan unknown = buildLine({0}, {{1, 100000}});
    bool refused = false;
    try {
        Solver(config).solve(unknown);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    check(refused, "a required vehicle outside the plan is refused");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// stop_at_first_feasible's best-score limit (0, INT64_MIN, INT64_MIN): the
// engine compares hard only (soft -inf), so no soft score, however low,
// misses the stop. Through toEngineScore the sentinel folded into a finite
// engine soft that a soft score below it never reached.
// -------------------------------------------------------------------------
void verifyStopAtFirstFeasibleLimit() {
    std::printf("== stop_at_first_feasible's limit is hard-only ==\n");
    const optimaru::Score limit = toEngineScoreLimit(Score{0, kNoLevelFloor, kNoLevelFloor});
    check(limit.hard == 0 && std::isinf(limit.soft) && limit.soft < 0.0,
          "(0, INT64_MIN, INT64_MIN) -> engine limit (0, -inf)");
    check(optimaru::Score{0, -1e30} >= limit && toEngineScore(Score{0, -5, -(1LL << 62)}) >= limit,
          "a feasible score with any soft (-1e30; medium -5, soft -2^62) reaches it");
    check(!(optimaru::Score{-1, 0.0} >= limit), "an infeasible score does not");
    check(toEngineScoreLimit(Score{0, kNoLevelFloor, 0}).soft ==
                          -std::numeric_limits<double>::infinity() &&
                  toEngineScoreLimit(Score{0, 0, kNoLevelFloor}).soft ==
                          -std::numeric_limits<double>::infinity(),
          "INT64_MIN on medium or on soft drops the soft-side floor");
    const Score other{-2, -3, -5};
    check(toEngineScoreLimit(other) == toEngineScore(other), "any other limit folds like a score");
    std::printf("\n");
}

}  // namespace

int main() {
    verifyStopAtFirstFeasibleLimit();
    testCouplingConstraints();
    testIncrementalMatchesFullRecompute();
    testSolveIntegrity();
    testFullAssert();
    testPairOperatorsMatter();
    testConstructionRespectsRequiredVehicle();

    if (failures == 0) {
        std::printf("All pickup-delivery tests PASSED.\n");
        return 0;
    }
    std::printf("%d pickup-delivery test(s) FAILED.\n", failures);
    return 1;
}
