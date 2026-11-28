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
//   (j) the opt-in costs: shipping prices order a group's boxes, small
//       instances priced by board and shipping solved to the enumerated
//       optimum, and the default solve unchanged by facts they read
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
#include "bs/constraints.hpp"
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
    // The facts of the shipping prices.
    const auto priced = [&base] {
        BoxSizingProblem p = base();
        p.rateTables.push_back({std::nullopt, {{1000, 4.0}, {5000, 6.0}}});
        return p;
    };
    {
        BoxSizingProblem p = priced();
        p.boxes[0].dimDivisor = 0;
        check(prepareRefuses(p, "box 'B0': dim_divisor must be at least 1, not 0"),
              "a divisor below 1");
    }
    {
        BoxSizingProblem p = priced();
        p.rateTables.push_back(p.rateTables[0]);
        check(prepareRefuses(p, "the rate table without a zone is given twice"), "a zone twice");
    }
    {
        BoxSizingProblem p = priced();
        p.rateTables[0].breaks.clear();
        check(prepareRefuses(p, "the rate table without a zone has no breaks"), "no breaks");
    }
    {
        BoxSizingProblem p = priced();
        p.rateTables[0].breaks[1].maxWeight = 1000;
        check(prepareRefuses(p, "strictly ascending"), "breaks out of order");
    }
    {
        BoxSizingProblem p = priced();
        p.rateTables[0].breaks[0].price = -1.0;
        check(prepareRefuses(p, "a break's price must be finite and >= 0"), "a negative price");
    }
    {
        BoxSizingProblem p = priced();
        p.parcels[0].zone = "far";
        check(prepareRefuses(p, "parcel 'P0': no rate table has zone 'far'"), "an unpriced zone");
        p.rateTables.clear();
        check(prepareRefuses(p, "parcel 'P0': no rate table has zone 'far'"),
              "a zone without any rate table");
    }
    {
        BoxSizingProblem p = priced();
        p.rateTables[0].zone = "near";
        check(prepareRefuses(p, "parcel 'P0': no zone, and no rate table without a zone"),
              "a parcel without a zone and no table for it");
    }
    {
        // 300 x 200 x 100 at divisor 1,000 bills 6,000, above the top break.
        BoxSizingProblem p = priced();
        p.boxes[0].dimDivisor = 1000;
        check(prepareRefuses(p,
                             "parcel 'P0' bills 6000 in box 'B0', above the top break (5000) of "
                             "the rate table without a zone: add a break that covers it"),
              "a billed weight past the table");
        p.boxes[0].dimDivisor = 2000;  // 3,000
        p.prepared = false;
        check(!prepareRefuses(p, ""), "within the table");
    }
    {
        // Billed weights that an exact total cannot hold, only once weight is billed.
        BoxSizingProblem p = base();
        p.parcels[0].weight = ScoreCalculator::kMaxWeight;
        p.parcels[0].quantity = 10000;
        check(!prepareRefuses(p, ""), "without billing, a heavy parcel is not refused");
        p.boxes[0].dimDivisor = 5000;
        p.prepared = false;
        check(prepareRefuses(p, "use a coarser weight unit"), "with billing, it is");
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
        // The cover is greedy, and candidates that do not nest can fool it.
        // Cube takes the six small parcels, Tall three of them and two rods,
        // Flat the other three and two plates. The cover opens Cube (six),
        // then one of the two (two more), and at a limit of two the last two
        // parcels need a third size: the start is one size over the limit,
        // although Tall and Flat together take everything. The search gets
        // there by closing Cube.
        BoxSizingProblem p;
        addBox(p, 20, 20, 20);                                // Cube
        addBox(p, 10, 10, 40);                                // Tall
        addBox(p, 5, 30, 30);                                 // Flat
        for (int i = 0; i < 3; ++i) addParcel(p, 8, 8, 18);   // Cube or Tall
        for (int i = 0; i < 3; ++i) addParcel(p, 4, 15, 18);  // Cube or Flat
        for (int i = 0; i < 2; ++i) addParcel(p, 8, 8, 35);   // Tall only
        for (int i = 0; i < 2; ++i) addParcel(p, 4, 25, 28);  // Flat only
        p.maxBoxTypes = 2;
        BoxSizingProblem start = p;
        Solver::Config construction;
        construction.time_limit = std::chrono::seconds(0);
        Solver(construction).solve(start);
        check(start.boxTypesUsed() == 3 && start.score.hard == -1,
              "a greedy cover can start one size over the limit");
        Solver::Config config;
        config.time_limit = std::chrono::seconds(20);
        config.step_limit = 200;
        config.seed = 1;
        Solver(config).solve(p);
        check(p.score.hard == 0 && p.boxTypesUsed() == 2 &&
                      p.assignment() == std::vector<int>{1, 1, 1, 2, 2, 2, 1, 1, 2, 2},
              "and the search closes the size that was one too many");
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
        // S and L in use at a limit of 2, with XL as a fourth candidate. A
        // swap that opens M and closes L leaves the large parcel without a
        // size, and the only other one that takes it is XL: the plan would be
        // S, M and XL, three sizes at a limit of two. It is not made, and
        // nothing is written. (Opening XL and closing L, or M for S, is.)
        BoxSizingProblem p = nested();
        addBox(p, 40, 40, 40);  // XL
        p.maxBoxTypes = 2;
        const auto ranking = std::make_shared<const BoxRanking>(defaultRanking(p));
        p.parcels[0].box = 0;
        p.parcels[1].box = 2;
        p.parcels[2].box = 2;
        MoveHarness harness(p);
        SwapBoxMove swap(ranking);
        const Score before = harness.editor.score();
        int overLimit = 0, applied = 0;
        for (int trial = 0; trial < 200; ++trial) {
            if (!harness.apply(swap)) continue;
            ++applied;
            if (p.boxTypesUsed() > 2) ++overLimit;
            swap.undo_last(harness.ctx);
        }
        check(applied > 0 && overLimit == 0 && p.assignment() == std::vector<int>{0, 2, 2} &&
                      harness.editor.score() == before,
              "SwapBoxMove at the limit never proposes a plan with a size too many");
        p.maxBoxTypes.reset();
        bool three = false;
        for (int trial = 0; trial < 200 && !three; ++trial) {
            if (!harness.apply(swap)) continue;
            three = p.boxTypesUsed() == 3;
            swap.undo_last(harness.ctx);
        }
        check(three, "without a limit the same swap may bring a second size in");
    }
    {
        // Two sizes of one price (the same volume, no cost) rank by index, so
        // every start and every platform breaks the tie the same way.
        BoxSizingProblem p;
        addBox(p, 20, 10, 10);
        addBox(p, 10, 20, 10);
        addBox(p, 10, 10, 10);
        addParcel(p, 10, 10, 10, -1, 3);
        addParcel(p, 20, 10, 10, -1, 2);
        const BoxRanking ranking = defaultRanking(p);
        constructGreedy(p, ranking);
        check(ranking.order == std::vector<int>{2, 0, 1} &&
                      ranking.rank == std::vector<int>{1, 2, 0} &&
                      p.assignment() == std::vector<int>{2, 0},
              "sizes of one price keep their order: the lower index first");
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
    // An improving step is one that ends above the step before it, also
    // below the best: seven steps down, one up, seven down are fifteen steps
    // without a refill, and the eighth after the step up brings the third.
    for (int step = 0; step < 7; ++step) acceptor.on_step_end({0, -1008.0});
    acceptor.on_step_end({0, -1007.0});
    for (int step = 0; step < 7; ++step) acceptor.on_step_end({0, -1007.0});
    const bool waited = acceptor.reheats() == 2;
    acceptor.on_step_end({0, -1007.0});
    check(waited && acceptor.reheats() == 3,
          "a step up from the step before, below the best too, starts the count again");
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

// -------------------------------------------------------------------------
// (j) The opt-in costs: board area and shipping by billed weight.
// -------------------------------------------------------------------------

// The cheapest set of at most `limit` sizes under `air` x the void volume and
// the box cost, plus `board` x the board area and `shipping` x the shipping
// cost, each parcel in its cheapest size of the set; -infinity when no set
// takes every parcel.
double pricedOptimum(BoxSizingProblem& problem, int limit, double air, double board,
                     double shipping) {
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
            const auto quantity = static_cast<double>(problem.parcels[p].quantity);
            double cheapest = std::numeric_limits<double>::infinity();
            for (int b = 0; b < boxCount; ++b) {
                if (!(set >> b & 1u) || !problem.fits(p, b)) continue;
                const double price = air * (static_cast<double>(problem.voidOf(p, b)) +
                                            quantity * problem.boxes[b].cost) +
                                     quantity * (board * static_cast<double>(problem.boxArea[b]) +
                                                 shipping * problem.shippingPrice(p, b));
                cheapest = std::min(cheapest, price);
            }
            feasible = std::isfinite(cheapest);
            cost += cheapest;
        }
        if (feasible) best = std::max(best, -cost);
    }
    return best;
}

void verifyOptInCosts() {
    std::printf("== (j) Board area and shipping by billed weight ==\n");
    {
        // A light parcel and a heavy one that both fit S (1,000 mm3) and L
        // (27,000 mm3). Divisor 10: S bills 100, L 2,700. Breaks at 1,000
        // (price 1) and 3,000 (price 9). The light parcel pays 1 in S and 9
        // in L; the heavy one (2,000) pays 9 in both. With the shipping cost
        // alone, the two parcels' boxes come in different orders.
        BoxSizingProblem p;
        addBox(p, 30, 30, 30);  // B0: L, first by index
        addBox(p, 10, 10, 10);  // B1: S
        for (Box& box : p.boxes) box.dimDivisor = 10;
        p.rateTables.push_back({std::nullopt, {{1000, 1.0}, {3000, 9.0}}});
        addParcel(p, 10, 10, 10, -1, 1, /*weight*/ 50);
        addParcel(p, 10, 10, 10, -1, 1, /*weight*/ 2000);
        ScoreCalculator::prepare(p);
        const BoxRanking ranking = BoxRanking::of(p, Pricing{0.0, 0.0, 0.0, 0.0, 1.0});
        check(ranking.perGroup && ranking.groups.size() == 2,
              "shipping prices: two groups for one set of boxes that take them");
        check(ranking.cheapestFor(p, 0) == 1 && ranking.cheapestFor(p, 1) == 1,
              "the light parcel's cheapest is S by price, the heavy one's S by the smaller volume");
        const BoxRanking unpriced = BoxRanking::of(p, Pricing{0.0, 0.0, 0.0, 0.0, 0.0});
        check(!unpriced.perGroup && unpriced.groups.size() == 1 && unpriced.cheapestFor(p, 0) == 0,
              "without a shipping weight: one group, one order (ties to the lower index)");
    }
    {
        // Two candidates the same volume: a cube and a flat box. The board
        // area takes the cube.
        BoxSizingProblem p;
        addBox(p, 400, 400, 25);   // B0: 4,000,000 mm3, board 2 (160,000 + 10,000 + 10,000)
        addBox(p, 200, 200, 100);  // B1: 4,000,000 mm3, board 2 (40,000 + 20,000 + 20,000)
        addParcel(p, 100, 100, 20, -1, 10);
        Solver::Config construction;
        construction.time_limit = std::chrono::seconds(0);
        DecomposedCalculator calculator;
        defaultConstraints().weight("boardAreaCost", 1.0).into(calculator);
        Solver(construction).solve(p, calculator);
        check(p.parcels[0].box == 1 && p.boardArea() == 10 * 160000,
              "the board area sends the parcel to the cube");
    }
    {
        int optimal = 0;
        int changed = 0;  // the weights ship a parcel in another box than the default solve
        const int instances = 8;
        for (int i = 0; i < instances; ++i) {
            const int limit = 2 + i % 3;
            BoxSizingProblem p = randomInstance(12, 50, limit, 300 + i, i % 2 == 0 ? 0.0 : 5000.0);
            // Parcels half as long: most fit several sizes, so the set is a
            // choice and not forced by the largest parcels.
            for (Parcel& parcel : p.parcels) {
                parcel.length /= 2;
                parcel.width /= 2;
                parcel.height /= 2;
            }
            addRates(p);
            // A shipping price unit is worth 20 litres of air, a mm2 of board
            // 200 mm3; one instance in four prices only the board, one only
            // the shipping, and one prices the shipping alone (no air, no
            // box cost).
            const double air = i % 4 == 3 ? 0.0 : 1.0;
            const double board = i % 4 == 0 || i % 4 == 1 ? 200.0 : 0.0;
            const double shipping = i % 4 == 1 ? 0.0 : i % 4 == 3 ? 1.0 : 20000000.0;
            const double optimum = pricedOptimum(p, limit, air, board, shipping);
            Solver::Config config;
            config.time_limit = std::chrono::seconds(20);
            config.step_limit = 20000;
            config.seed = 1;
            DecomposedCalculator calculator;
            defaultConstraints()
                    .weight("voidVolumeCost", air)
                    .weight("boxCost", air)
                    .weight("boardAreaCost", board)
                    .weight("shippingCost", shipping)
                    .into(calculator);
            BoxSizingProblem plain = p;
            Solver(config).solve(plain);
            Solver(config).solve(p, calculator);
            if (plain.assignment() != p.assignment()) ++changed;
            if (p.score.hard == 0 && std::fabs(p.score.soft - optimum) <= 1e-9 * -optimum) {
                ++optimal;
            } else {
                std::printf("    instance %d, limit %d: %s, the optimum is %.1f\n", i, limit,
                            p.score.str().c_str(), optimum);
            }
        }
        check(optimal == instances, std::to_string(optimal) + " of " + std::to_string(instances) +
                                            " priced instances (12 sizes, 50 parcels, three "
                                            "zones) at the enumerated optimum");
        // Board and billed weight grow with the box, as the air does, so the
        // set that ships the least air is often the cheapest by them too.
        check(changed > 0,
              std::to_string(changed) + " of them solved otherwise without the opt-in costs");
    }
    {
        // Rate tables and divisors at the default weights: the same solve,
        // on the default path and on the default assembly.
        for (bool assembly : {false, true}) {
            BoxSizingProblem plain = randomInstance(20, 200, 4, 77, 250.0);
            BoxSizingProblem priced = plain;
            addRates(priced);
            Solver::Config config;
            config.time_limit = std::chrono::seconds(20);
            config.step_limit = 5000;
            config.seed = 2;
            for (BoxSizingProblem* p : {&plain, &priced}) {
                DecomposedCalculator calculator;
                addDefaultConstraints(calculator);
                if (assembly) {
                    Solver(config).solve(*p, calculator);
                } else {
                    Solver(config).solve(*p);
                }
            }
            check(plain.assignment() == priced.assignment() && plain.score == priced.score,
                  std::string(assembly ? "default assembly" : "default path") +
                          ": rate tables at weight 0 change nothing");
        }
    }
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
    verifyOptInCosts();
    if (failures == 0) {
        std::printf("All box-sizing checks passed.\n");
        return 0;
    }
    std::printf("%d box-sizing check(s) FAILED.\n", failures);
    return 1;
}
