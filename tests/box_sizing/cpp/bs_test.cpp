// Box-sizing unit tests (engine test/ style: check() + non-zero exit on any
// failure):
//   (a) every default constraint verified in isolation against hand-computed
//       scores, a violating (positive) and a clean (zero) case each, through
//       the monolithic ScoreCalculator AND the incremental calculator
//   (b) incremental score == full recompute across a long random walk driven
//       through the engine's Editor protocol, with whole and fractional costs
//   (c) what prepare() and a solve refuse
//   (d) the greedy construction: the sizes it opens, the limit, seeds
//   (e) the three moves: what they do, and that undo restores the solution
//   (f) a small instance solved under ScoreChecks::Full
//   (g) small instances solved to the optimum an enumeration of every set of
//       sizes finds; the search improves on the construction where it can
//   (h) the acceptor: late acceptance that starts over just below its best
//   (i) the parallel multi-start path returns a consistent best solution
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "bs/accept/reheating_late_acceptance.hpp"
#include "bs/construct/construction.hpp"
#include "bs/model/domain.hpp"
#include "bs/model/pricing.hpp"
#include "bs/score/incremental_calculator.hpp"
#include "bs/score/score_calculator.hpp"
#include "bs/search/moves/close_box_move.hpp"
#include "bs/search/moves/open_box_move.hpp"
#include "bs/search/moves/swap_box_move.hpp"
#include "bs/solver.hpp"
#include "bs_fixtures.hpp"
#include "optimaru/solution/editor.hpp"
#include "optimaru/solution/neighborhoods.hpp"
#include "optimaru/testing/check.hpp"

using namespace bs;
using namespace bs_fixtures;

namespace {

using optimaru::testing::check;
using optimaru::testing::failures;

// Both calculators on the same problem: the full recompute and a rebuild of
// the incremental one.
void checkScore(const std::string& label, BoxSizingProblem& problem, long long expectedHard,
                double expectedSoft) {
    const Score full = ScoreCalculator::score(problem);
    BoxSizingIncrementalScoreCalculator calculator;
    calculator.rebuild(problem);
    const Score incremental = calculator.score();
    const bool ok = full.hard == expectedHard && std::fabs(full.soft - expectedSoft) < 1e-9 &&
                    incremental == full;
    std::printf("  [%s] %-52s got=%s expected=%lldhard/%gsoft\n", ok ? "PASS" : "FAIL",
                label.c_str(), full.str().c_str(), expectedHard, expectedSoft);
    if (!ok) ++failures;
}

bool refuses(const std::function<void()>& call, const std::string& needle) {
    try {
        call();
    } catch (const std::invalid_argument& error) {
        const bool found = std::string(error.what()).find(needle) != std::string::npos;
        if (!found) std::printf("    refused with: %s\n", error.what());
        return found;
    }
    return false;
}

// -------------------------------------------------------------------------
// (a) Each constraint in isolation.
// -------------------------------------------------------------------------
void verifyConstraints() {
    std::printf("== (a) Verifying each constraint in isolation ==\n");

    // hard 1: parcelFitCost. Box 300 x 200 x 100; a parcel 350 x 90 x 250
    // sorts to (90, 250, 350) against (100, 200, 300): 0 + 50 + 50 = 100. Its
    // volume is larger than the box's, so the void counts 0.
    {
        BoxSizingProblem p;
        addBox(p, 300, 200, 100);
        addParcel(p, 350, 90, 250, 0, /*quantity*/ 7);
        checkScore("parcelFitCost: two sides 50 too long", p, -100, 0);
    }
    // A parcel that fits only when turned: 100 x 300 x 200 in 300 x 200 x 100.
    {
        BoxSizingProblem p;
        addBox(p, 300, 200, 100);
        addParcel(p, 100, 300, 200, 0);
        checkScore("parcelFitCost: fits in another rotation", p, 0, 0);
    }
    // hard 2: boxWeightCost. 1,200 g in a box that carries 1,000 g: 200. The
    // quantity does not multiply it, and two parcels are not added up.
    {
        BoxSizingProblem p;
        addBox(p, 100, 100, 100, /*maxWeight*/ 1000);
        addParcel(p, 100, 100, 100, 0, /*quantity*/ 5, /*weight*/ 1200);
        addParcel(p, 100, 100, 100, 0, /*quantity*/ 1, /*weight*/ 900);
        checkScore("boxWeightCost: 200 g over", p, -200, 0);
    }
    {
        BoxSizingProblem p;
        addBox(p, 100, 100, 100);  // no maxWeight
        addParcel(p, 100, 100, 100, 0, 1, /*weight*/ 999999);
        checkScore("boxWeightCost: no limit, no cost", p, 0, 0);
    }
    // hard 3: boxTypeLimitCost. Three sizes in use, two allowed: 1.
    {
        BoxSizingProblem p;
        for (int b = 0; b < 3; ++b) addBox(p, 100, 100, 100);
        for (int b = 0; b < 3; ++b) addParcel(p, 100, 100, 100, b);
        p.maxBoxTypes = 2;
        checkScore("boxTypeLimitCost: three sizes, two allowed", p, -1, 0);
        p.maxBoxTypes = 3;
        checkScore("boxTypeLimitCost: at the limit", p, 0, 0);
    }
    // A required size counts without a parcel: one in use + one required = 2.
    {
        BoxSizingProblem p;
        addBox(p, 100, 100, 100);
        addBox(p, 100, 100, 100, std::nullopt, 0.0, /*required*/ true);
        addParcel(p, 100, 100, 100, 0);
        p.maxBoxTypes = 1;
        checkScore("boxTypeLimitCost: a required size counts", p, -1, 0);
    }
    // soft 4: voidVolumeCost. Box 10 x 10 x 10 = 1000; parcels of 600 (x 3)
    // and 1000 (x 2): 3 * 400 + 2 * 0 = 1200.
    {
        BoxSizingProblem p;
        addBox(p, 10, 10, 10);
        addParcel(p, 10, 10, 6, 0, /*quantity*/ 3);
        addParcel(p, 10, 10, 10, 0, /*quantity*/ 2);
        checkScore("voidVolumeCost: 3 x 400", p, 0, -1200);
    }
    // soft 5: boxCost. 4 + 2 shipments in a box of cost 2.5: 15; the box
    // nobody ships in costs nothing.
    {
        BoxSizingProblem p;
        addBox(p, 10, 10, 10, std::nullopt, /*cost*/ 2.5);
        addBox(p, 10, 10, 10, std::nullopt, /*cost*/ 100.0);
        addParcel(p, 10, 10, 10, 0, /*quantity*/ 4);
        addParcel(p, 10, 10, 10, 0, /*quantity*/ 2);
        checkScore("boxCost: 6 shipments x 2.5", p, 0, -15);
    }
    // An unassigned parcel contributes nothing.
    {
        BoxSizingProblem p;
        addBox(p, 10, 10, 10, std::nullopt, 3.0);
        addParcel(p, 10, 10, 5, -1, 9);
        checkScore("an unassigned parcel scores nothing", p, 0, 0);
    }
    // The summary getters, on the void case plus a second box.
    {
        BoxSizingProblem p;
        addBox(p, 10, 10, 10);
        addBox(p, 20, 10, 10);
        addParcel(p, 10, 10, 6, 0, 3);
        addParcel(p, 20, 10, 10, 1, 2);
        check(p.voidVolume() == 1200 && p.boxTypesUsed() == 2 &&
                      p.parcelsPerBox() == std::vector<std::int64_t>{3, 2} &&
                      std::fabs(p.fillRatio() - 5800.0 / 7000.0) < 1e-12,
              "void 1200, two sizes, shipments 3 and 2, fill ratio 5800 / 7000");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental == full recompute on a random walk.
// -------------------------------------------------------------------------
void verifyIncremental() {
    std::printf("== (b) Incremental == full recompute ==\n");
    for (double costStep : {0.0, 1.0, 0.37}) {
        BoxSizingProblem p = randomInstance(9, 40, 3, 5, costStep);
        p.boxes[2].required = true;
        p.boxes[1].maxWeight = 4000;
        BoxSizingIncrementalScoreCalculator calculator;
        optimaru::Editor<BoxSizingProblem> editor(p, calculator);
        std::mt19937_64 rng(17);
        int mismatches = 0;
        const int steps = 4000;
        for (int step = 0; step < steps; ++step) {
            const int parcel = static_cast<int>(rng() % p.parcels.size());
            const int box = static_cast<int>(rng() % (p.boxes.size() + 1)) - 1;  // -1 too
            editor.assign(parcel, box);
            const Score incremental = editor.score();
            BoxSizingProblem copy = p;
            if (!(incremental == ScoreCalculator::score(copy))) ++mismatches;
        }
        check(mismatches == 0, std::to_string(steps) + " random assignments, cost step " +
                                       std::to_string(costStep) + ": identical scores");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Refusals.
// -------------------------------------------------------------------------
void verifyRefusals() {
    std::printf("== (c) What prepare() and a solve refuse ==\n");
    const auto base = [] {
        BoxSizingProblem p;
        addBox(p, 300, 200, 100);
        addParcel(p, 100, 100, 100);
        return p;
    };
    const auto prepareRefuses = [](BoxSizingProblem p, const std::string& needle) {
        return refuses([&p] { ScoreCalculator::prepare(p); }, needle);
    };
    {
        BoxSizingProblem p = base();
        p.boxes.clear();
        check(prepareRefuses(p, "at least one candidate box"), "no candidate box");
    }
    {
        BoxSizingProblem p = base();
        addBox(p, 1, 1, 1);
        p.boxes[1].id = p.boxes[0].id;
        check(prepareRefuses(p, "box ids must be unique"), "a repeated box id");
    }
    {
        BoxSizingProblem p = base();
        p.boxes[0].width = 0;
        check(prepareRefuses(p, "box 'B0': width must be between 1 and 2000000"), "a side of 0");
    }
    {
        BoxSizingProblem p = base();
        p.parcels[0].height = 2000001;
        check(prepareRefuses(p, "parcel 'P0': height must be between"), "a side above the bound");
    }
    {
        BoxSizingProblem p = base();
        p.parcels[0].quantity = 0;
        check(prepareRefuses(p, "quantity must be at least 1"), "a quantity of 0");
    }
    {
        BoxSizingProblem p = base();
        p.parcels[0].weight = -1;
        check(prepareRefuses(p, "weight must be between 0"), "a negative weight");
    }
    {
        BoxSizingProblem p = base();
        p.boxes[0].cost = -0.5;
        check(prepareRefuses(p, "cost must be finite and >= 0"), "a negative cost");
        p.boxes[0].cost = std::numeric_limits<double>::infinity();
        check(prepareRefuses(p, "cost must be finite and >= 0"), "an infinite cost");
    }
    {
        BoxSizingProblem p = base();
        p.maxBoxTypes = 0;
        check(prepareRefuses(p, "max_box_types must be at least 1"), "max_box_types 0");
    }
    {
        BoxSizingProblem p = base();
        p.parcels[0].box = 1;
        check(prepareRefuses(p, "parcel 'P0' has box 1, outside -1..0"), "a box out of range");
    }
    {
        // 2000000^3 = 8e18 per box: two shipments pass 2^53 by far.
        BoxSizingProblem p;
        addBox(p, 2000000, 2000000, 2000000);
        addParcel(p, 1, 1, 1, -1, 2);
        check(prepareRefuses(p, "use a coarser length unit"), "a volume a double cannot hold");
    }
    {
        BoxSizingProblem p = base();
        addParcel(p, 301, 100, 100);
        check(refuses([&p] { Solver().solve(p); },
                      "parcel 'P1' (301 x 100 x 100, weight 0) fits none of the candidate boxes"),
              "a solve refuses a parcel no candidate takes by size");
        check(!refuses([&p] { ScoreCalculator::score(p); }, ""), "scoring it is not refused");
    }
    {
        BoxSizingProblem p = base();
        p.boxes[0].maxWeight = 500;
        p.parcels[0].weight = 501;
        check(refuses([&p] { Solver().solve(p); }, "fits none of the candidate boxes"),
              "a solve refuses a parcel no candidate takes by weight");
    }
    {
        BoxSizingProblem p = base();
        addBox(p, 300, 200, 100, std::nullopt, 0.0, true);
        p.boxes[0].required = true;
        p.maxBoxTypes = 1;
        check(refuses([&p] { Solver().solve(p); }, "max_box_types is 1, but 2 boxes are required"),
              "a solve refuses more required boxes than the limit");
    }
    std::printf("\n");
}

// Three sizes that nest (S in M in L) and parcels for each: the instance of
// the construction and move checks. Volumes 1000, 8000, 27000.
BoxSizingProblem nested() {
    BoxSizingProblem p;
    addBox(p, 10, 10, 10);                          // S
    addBox(p, 20, 20, 20);                          // M
    addBox(p, 30, 30, 30);                          // L
    addParcel(p, 10, 10, 10, -1, /*quantity*/ 10);  // fits S
    addParcel(p, 20, 20, 15, -1, /*quantity*/ 5);   // fits M
    addParcel(p, 30, 30, 30, -1, /*quantity*/ 1);   // fits L only
    return p;
}

BoxRanking defaultRanking(BoxSizingProblem& problem) {
    ScoreCalculator::prepare(problem);
    return BoxRanking::of(problem, Pricing{});
}

// -------------------------------------------------------------------------
// (d) The greedy construction.
// -------------------------------------------------------------------------
void verifyConstruction() {
    std::printf("== (d) The greedy construction ==\n");
    {
        BoxSizingProblem p = nested();
        constructGreedy(p, defaultRanking(p));
        check(p.assignment() == std::vector<int>{0, 1, 2},
              "no limit: every parcel in its own size");
    }
    {
        // Two sizes: L is needed. S saves 10 * 26000, M saves 10 * 19000 +
        // 5 * 19000 = 285000: M opens, and the ten small parcels go to M.
        BoxSizingProblem p = nested();
        p.maxBoxTypes = 2;
        constructGreedy(p, defaultRanking(p));
        check(p.assignment() == std::vector<int>{1, 1, 2},
              "limit 2: L, then M by the larger saving");
    }
    {
        BoxSizingProblem p = nested();
        p.maxBoxTypes = 1;
        constructGreedy(p, defaultRanking(p));
        check(p.assignment() == std::vector<int>{2, 2, 2}, "limit 1: the size that takes them all");
    }
    {
        // A fixed cost per size. After L, S saves 10 * 26000 = 260000 and M
        // saves 15 * 19000 = 285000; once M is open, S saves 10 * 7000 = 70000.
        // At 50000 a size both pay; at 200000 only M does; at 300000 neither.
        const auto sizesAt = [](double fixedCost) {
            BoxSizingProblem p = nested();
            ScoreCalculator::prepare(p);
            constructGreedy(p, BoxRanking::of(p, Pricing{1.0, 1.0, fixedCost}));
            return p.assignment();
        };
        check(sizesAt(50000.0) == std::vector<int>{0, 1, 2} &&
                      sizesAt(200000.0) == std::vector<int>{1, 1, 2} &&
                      sizesAt(300000.0) == std::vector<int>{2, 2, 2},
              "a fixed cost per size stops the greedy where a size no longer pays");
    }
    {
        // A required size is open from the start, and a seed stays put.
        BoxSizingProblem p = nested();
        p.boxes[1].required = true;
        p.maxBoxTypes = 2;
        p.parcels[0].box = 2;  // seeded in L
        constructGreedy(p, defaultRanking(p));
        check(p.assignment() == std::vector<int>{2, 1, 2},
              "a seed keeps its box, the others use the open sizes");
    }
    {
        // A cost changes the order: M at a cost of 30000 per box is dearer
        // than L (27000 + 0), so nothing ships in M.
        BoxSizingProblem p = nested();
        p.boxes[1].cost = 30000.0;
        constructGreedy(p, defaultRanking(p));
        check(p.assignment() == std::vector<int>{0, 2, 2}, "a dear size is left out");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) The moves.
// -------------------------------------------------------------------------
struct MoveHarness {
    BoxSizingProblem& problem;
    BoxSizingIncrementalScoreCalculator calculator;
    optimaru::Editor<BoxSizingProblem> editor;
    optimaru::Neighborhoods nearby;
    std::vector<int> none;
    optimaru::OperatorContext<BoxSizingProblem> ctx;
    std::mt19937_64 rng{3};

    explicit MoveHarness(BoxSizingProblem& p)
        : problem(p), editor(p, calculator), ctx{editor, nearby, none, none} {}

    bool apply(optimaru::MoveOperator<BoxSizingProblem>& move) {
        std::vector<int> entities;
        return move.apply(ctx, rng, entities);
    }
};

void verifyMoves() {
    std::printf("== (e) The moves on the set of sizes ==\n");
    {
        // S, M, L in use; only M can close without a parcel left over when
        // the draw picks it. Whatever closes, undo restores the solution.
        BoxSizingProblem p = nested();
        const auto ranking = std::make_shared<const BoxRanking>(defaultRanking(p));
        p.parcels[0].box = 0;
        p.parcels[1].box = 1;
        p.parcels[2].box = 2;
        MoveHarness harness(p);
        CloseBoxMove close(ranking);
        int closedOne = 0, restored = 0, scored = 0;
        for (int trial = 0; trial < 20; ++trial) {
            const std::vector<int> before = p.assignment();
            const Score scoreBefore = harness.editor.score();
            if (!harness.apply(close)) continue;
            ++closedOne;
            BoxSizingProblem copy = p;
            if (harness.editor.score() == ScoreCalculator::score(copy) && p.boxTypesUsed() == 2)
                ++scored;
            close.undo_last(harness.ctx);
            if (p.assignment() == before && harness.editor.score() == scoreBefore) ++restored;
        }
        check(closedOne > 0 && scored == closedOne && restored == closedOne,
              "CloseBoxMove takes one size out of use, scores it, and undo restores it");
    }
    {
        // Closing L is no move: its parcel fits no other candidate.
        BoxSizingProblem p;
        addBox(p, 10, 10, 10);
        addBox(p, 30, 30, 30);
        addParcel(p, 30, 30, 30, 1);
        const auto ranking = std::make_shared<const BoxRanking>(defaultRanking(p));
        MoveHarness harness(p);
        CloseBoxMove close(ranking);
        check(!harness.apply(close) && p.assignment() == std::vector<int>{1},
              "CloseBoxMove: no move when a parcel fits no other size");
    }
    {
        // Everything in L; opening S or M pulls in the parcels it serves cheaper.
        BoxSizingProblem p = nested();
        const auto ranking = std::make_shared<const BoxRanking>(defaultRanking(p));
        for (Parcel& parcel : p.parcels) parcel.box = 2;
        MoveHarness harness(p);
        OpenBoxMove open(ranking);
        const Score before = harness.editor.score();
        const bool applied = harness.apply(open);
        const std::vector<int> after = p.assignment();
        const bool pulled =
                after == std::vector<int>{0, 2, 2} || after == std::vector<int>{1, 1, 2};
        const bool better = harness.editor.score() > before;
        open.undo_last(harness.ctx);
        check(applied && pulled && better && p.assignment() == std::vector<int>{2, 2, 2} &&
                      harness.editor.score() == before,
              "OpenBoxMove pulls in what a new size serves cheaper; undo restores");
        p.maxBoxTypes = 1;
        check(!harness.apply(open), "OpenBoxMove: no move at the limit");
    }
    {
        // Limit 2, S and L in use: the only swap is M for S (L is needed),
        // which moves the M parcel out of L and the S parcels into M.
        BoxSizingProblem p = nested();
        p.maxBoxTypes = 2;
        const auto ranking = std::make_shared<const BoxRanking>(defaultRanking(p));
        p.parcels[0].box = 0;
        p.parcels[1].box = 2;
        p.parcels[2].box = 2;
        MoveHarness harness(p);
        SwapBoxMove swap(ranking);
        const Score before = harness.editor.score();
        bool found = false;
        for (int trial = 0; trial < 20 && !found; ++trial) {
            if (!harness.apply(swap)) continue;
            if (p.assignment() == std::vector<int>{1, 1, 2}) {
                found = harness.editor.score() > before && p.boxTypesUsed() == 2;
            }
            if (!found) swap.undo_last(harness.ctx);
        }
        check(found, "SwapBoxMove trades S for M at the limit and the score improves");
    }
    {
        // A required size is never the one that closes.
        BoxSizingProblem p = nested();
        p.boxes[0].required = true;
        p.boxes[1].required = true;
        p.boxes[2].required = true;
        const auto ranking = std::make_shared<const BoxRanking>(defaultRanking(p));
        p.parcels[0].box = 0;
        p.parcels[1].box = 1;
        p.parcels[2].box = 2;
        MoveHarness harness(p);
        CloseBoxMove close(ranking);
        SwapBoxMove swap(ranking);
        check(!harness.apply(close) && !harness.apply(swap), "required sizes do not close");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) A solve under ScoreChecks::Full.
// -------------------------------------------------------------------------
void verifyFullAssertSolve() {
    std::printf("== (f) FULL_ASSERT solve ==\n");
    BoxSizingProblem p = randomInstance(10, 40, 3, 21, 0.37);
    Solver::Config config;
    config.time_limit = std::chrono::seconds(20);
    config.step_limit = 6000;
    config.seed = 3;
    config.score_checks = optimaru::ScoreChecks::Full;
    config.ruin_recreate_unimproved_step_trigger = 200;
    config.ruin_recreate_polish_steps = 100;
    bool corrupted = false;
    try {
        Solver(config).solve(p);
    } catch (const optimaru::ScoreMismatchError& error) {
        corrupted = true;
        std::printf("  ScoreMismatchError: %s\n", error.what());
    }
    check(!corrupted, "6,000 FULL_ASSERT steps without a score mismatch");
    check(p.score.hard == 0 && p.boxTypesUsed() <= 3, "feasible, within three sizes");
    std::printf("\n");
}

// The best soft score over every set of at most `limit` sizes, each parcel in
// its cheapest size of the set; -infinity when no set takes every parcel.
double enumeratedOptimum(BoxSizingProblem& problem, int limit) {
    ScoreCalculator::prepare(problem);
    const int boxCount = problem.boxCount();
    double best = -std::numeric_limits<double>::infinity();
    for (std::uint32_t set = 1; set < (1u << boxCount); ++set) {
        int sizes = 0;
        for (int b = 0; b < boxCount; ++b) sizes += static_cast<int>(set >> b & 1u);
        if (sizes > limit) continue;
        double cost = 0.0;
        bool feasible = true;
        for (int p = 0; p < problem.parcelCount() && feasible; ++p) {
            double cheapest = std::numeric_limits<double>::infinity();
            for (int b = 0; b < boxCount; ++b) {
                if (!(set >> b & 1u) || !problem.fits(p, b)) continue;
                const double price =
                        static_cast<double>(problem.voidOf(p, b)) +
                        static_cast<double>(problem.parcels[p].quantity) * problem.boxes[b].cost;
                cheapest = std::min(cheapest, price);
            }
            feasible = std::isfinite(cheapest);
            cost += cheapest;
        }
        if (feasible) best = std::max(best, -cost);
    }
    return best;
}

// -------------------------------------------------------------------------
// (g) Small instances against the enumerated optimum.
// -------------------------------------------------------------------------
void verifyOptimum() {
    std::printf("== (g) The optimum of small instances ==\n");
    int optimal = 0, improved = 0, worse = 0;
    const int instances = 12;
    for (int i = 0; i < instances; ++i) {
        const int limit = 2 + i % 4;
        BoxSizingProblem p = randomInstance(14, 60, limit, 100 + i, i % 2 == 0 ? 0.0 : 5000.0);
        const double optimum = enumeratedOptimum(p, limit);

        BoxSizingProblem greedy = p;
        Solver::Config construction;
        construction.time_limit = std::chrono::seconds(0);
        Solver(construction).solve(greedy);

        Solver::Config config;
        config.time_limit = std::chrono::seconds(20);
        config.step_limit = 20000;
        config.seed = 1;
        Solver(config).solve(p);
        if (p.score.hard == 0 && std::fabs(p.score.soft - optimum) < 1e-6) {
            ++optimal;
        } else {
            std::printf("    instance %d, limit %d: %s, the optimum is %.0f\n", i, limit,
                        p.score.str().c_str(), optimum);
        }
        if (p.score > greedy.score) ++improved;
        if (p.score < greedy.score) ++worse;
    }
    check(optimal == instances, std::to_string(optimal) + " of " + std::to_string(instances) +
                                        " instances (14 sizes, 60 parcels) at the optimum");
    check(worse == 0, "the search never returns less than its construction");
    std::printf("  (the search improved on the construction in %d of %d)\n", improved, instances);
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) The acceptor: late acceptance that starts over below its best.
// -------------------------------------------------------------------------
void verifyAcceptor() {
    std::printf("== (h) ReheatingLateAcceptance ==\n");
    std::mt19937_64 rng(1);
    const Score start{0, -1000.0};
    ReheatingLateAcceptance acceptor(/*historyLength*/ 4, start);
    const auto accepts = [&](const Score& candidate, const Score& current) {
        return acceptor.accepts(candidate, current, current, nullptr, 0, 0.0, rng);
    };
    check(accepts({0, -1000.0}, start) && accepts({0, -990.0}, start) &&
                  !accepts({0, -1001.0}, start),
          "from the start it is a descent: equal and better are taken, worse is not");
    // Two histories (8 steps) without an improving step: the history becomes
    // the best score less the first gap, 0.5% of 1000 = 5.
    for (int step = 0; step < 8; ++step) acceptor.on_step_end(start);
    check(acceptor.reheats() == 1, "it starts over after two histories without an improvement");
    check(accepts({0, -1005.0}, start) && !accepts({0, -1005.5}, start),
          "then a step down to 0.5% below the best is taken, and no further");
    check(!accepts({-1, 0.0}, start), "never a step that gives up feasibility");
    // The walk goes down and the history follows it: once it holds the new
    // scores, the old bar is gone.
    for (int step = 0; step < 4; ++step) acceptor.on_step_end({0, -1002.0});
    check(accepts({0, -1002.0}, {0, -1002.0}) && !accepts({0, -1004.0}, {0, -1002.0}),
          "the history fills with what the walk finds");
    // Four more steps without an improvement make two histories again: the
    // next gap is 1%, of the best score seen (-1000).
    for (int step = 0; step < 4; ++step) acceptor.on_step_end({0, -1002.0});
    check(acceptor.reheats() == 2 && accepts({0, -1010.0}, {0, -1002.0}) &&
                  !accepts({0, -1010.5}, {0, -1002.0}),
          "the second time the gap is 1% of the best score seen");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) Parallel starts.
// -------------------------------------------------------------------------
void verifyParallelStarts() {
    std::printf("== (i) Parallel starts ==\n");
    BoxSizingProblem p = randomInstance(30, 300, 5, 77);
    Solver::Config config;
    config.time_limit = std::chrono::seconds(20);
    config.step_limit = 3000;
    config.seed = 5;
    config.parallel_solver_count = 3;
    Solver(config).solve(p);
    BoxSizingProblem copy = p;
    check(p.score == ScoreCalculator::score(copy), "the reported score is the solution's score");
    check(p.score.hard == 0 && p.boxTypesUsed() <= 5, "feasible, within five sizes");
    bool allAssigned = true;
    for (const Parcel& parcel : p.parcels) allAssigned = allAssigned && parcel.isAssigned();
    check(allAssigned, "every parcel has a box");
    std::printf("\n");
}

}  // namespace

int main() {
    verifyConstraints();
    verifyIncremental();
    verifyRefusals();
    verifyConstruction();
    verifyMoves();
    verifyFullAssertSolve();
    verifyOptimum();
    verifyAcceptor();
    verifyParallelStarts();
    if (failures == 0) {
        std::printf("All box-sizing checks passed.\n");
        return 0;
    }
    std::printf("%d box-sizing check(s) FAILED.\n", failures);
    return 1;
}
