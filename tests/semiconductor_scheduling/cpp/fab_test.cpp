// fab_test — the decoder, every constraint by hand (a positive and a zero
// case each), the incremental evaluator against the full recompute of every
// constraint on random move sequences, the solver's contracts and the int64
// overflow checks. check() style: non-zero exit on any failure.
#include <chrono>
#include <cstdio>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "fab/constraints.hpp"
#include "fab/decoder.hpp"
#include "fab/incremental_calculator.hpp"
#include "fab/score_calculator.hpp"
#include "fab/solver.hpp"
#include "fab_test_support.hpp"
#include "optimaru/model/editor.hpp"

using namespace fab;
using namespace fabtest;

namespace {

// --------------------------------------------------------------------------
// Fixtures (the Python tests' instances)
// --------------------------------------------------------------------------

// tests/test_semiconductor_scheduling.py::_fab_problem
Builder fabProblem(bool withAssignments) {
    Builder b;
    b.machine("M1", "PHOTO", 1, 1, {{3, 5}}).machine("M2", "PHOTO").machine("F1", "DIFF", 2, 2);
    b.setup("PHOTO", "X", "Y", 3).transport("PHOTO", "DIFF", 3);
    b.lot("L1", 2, 12, 2).op("PHOTO", "X", {{"M1", 4, 1}}, {}, 0, 3).op("DIFF", "X", {{"F1", 5}});
    b.lot("L2", 0, 30).op("PHOTO", "Y", {{"M1", 4}}).op("DIFF", "Y", {{"F1", 5}});
    b.lot("L3", 0, 30).op("DIFF", "X", {{"F1", 5}});
    if (withAssignments) {
        b.assign("L1", "PHOTO", "M1", 0, 4).assign("L1", "DIFF", "F1", 10, 15);
        b.assign("L2", "PHOTO", "M1", 2, 6).assign("L2", "DIFF", "F1", 11, 16);
        b.assign("L3", "DIFF", "F1", 12, 17);
    }
    return b;
}

// tests/test_semiconductor_scheduling.py::test_fab_resource_pin_and_cycle_...
Builder resourceProblem() {
    Builder b;
    b.machine("M1", "G1").machine("M2", "G2").resource("RETICLE", 1, {{3, 5}});
    b.lot("L1", 0, 20, 1, 6).op("O1", "F", {{"M1", 4}}, {"RETICLE"}, 4).op("O2", "F", {{"M2", 2}});
    b.lot("L2", 0, 20).op("O1", "F", {{"M1", 4}}, {"RETICLE"});
    b.pin("L1", "O1", "M1", 1, 5);
    b.assign("L1", "O1", "M1", 0, 4).assign("L1", "O2", "M2", 6, 8).assign("L2", "O1", "M1", 2, 6);
    return b;
}

// tests/test_initial_solution.py::_queue_problem: L1 must start B within one
// time unit of finishing A, and L2 holds M2 first.
Builder queueProblem() {
    Builder b;
    b.machine("M1", "A").machine("M2", "B");
    b.lot("L1", 0, 50).op("A", "F", {{"M1", 2}}, {}, 0, 1).op("B", "F", {{"M2", 2}});
    b.lot("L2", 0, 50).op("X", "F", {{"M2", 10}});
    return b;
}

// The decoder's 10 000-pass guard: a resource down on every odd unit step,
// so a two-unit operation that needs it never fits. L2 adds a second row.
Builder horizonProblem() {
    Builder b;
    b.machine("M1", "A").machine("M2", "B");
    std::vector<TimeWindow> windows;
    for (int k = 0; k < 10003; ++k) windows.push_back({2 * k + 1, 2 * k + 2});
    b.resource("R", 1, windows);
    b.lot("L1", 0, 50).op("O", "F", {{"M1", 2}}, {"R"});
    b.lot("L2", 0, 50).op("O", "F", {{"M2", 3}});
    return b;
}

// Two independent one-step lots; M1 is the preferred tool for both.
Builder choiceProblem() {
    Builder b;
    b.machine("M1", "A").machine("M2", "A");
    b.lot("L1", 0, 50).op("O", "F", {{"M1", 3}, {"M2", 3, 5}});
    b.lot("L2", 0, 50).op("O", "F", {{"M1", 3}, {"M2", 3, 5}});
    return b;
}

// A feasible schedule on which every term but cycle time and makespan is 0.
Builder zeroProblem() {
    Builder b;
    b.machine("M1", "G1", 1, 1, {{20, 25}}).machine("M2", "G1").machine("F1", "G2", 2, 2);
    b.resource("R", 1, {{30, 35}});
    b.setup("G1", "A", "B", 2).transport("G1", "G2", 1);
    b.lot("L1", 0, 50, 1, 40).op("O1", "A", {{"M1", 4}}, {"R"}, 1, 10).op("O2", "X", {{"F1", 5}});
    b.lot("L2", 0, 50).op("O1", "B", {{"M1", 3}}).op("O2", "X", {{"F1", 5}});
    b.lot("L3", 0, 50).op("O1", "B", {{"M2", 7}});
    b.pin("L1", "O1", "M1", 0, 4);
    b.assign("L1", "O1", "M1", 0, 4).assign("L2", "O1", "M1", 6, 9);
    b.assign("L1", "O2", "F1", 10, 15, "B1").assign("L2", "O2", "F1", 10, 15, "B1");
    b.assign("L3", "O1", "M2", 0, 7);
    return b;
}

RawTotals rawOf(Builder b) {
    SemiconductorSchedule s = b.build();
    return ScoreCalculator::rawCosts(s);
}

std::int64_t startOf(const std::vector<Assignment>& assignments, const SemiconductorSchedule& s,
                     const std::string& operation) {
    for (const Assignment& a : assignments) {
        if (s.lots[static_cast<std::size_t>(a.lot)]
                    .operations[static_cast<std::size_t>(a.operation)]
                    .id == operation)
            return a.start;
    }
    return -1;
}

bool sameAssignments(const std::vector<Assignment>& a, const std::vector<Assignment>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].lot != b[i].lot || a[i].operation != b[i].operation ||
            a[i].machine != b[i].machine || a[i].start != b[i].start || a[i].end != b[i].end ||
            a[i].batchId != b[i].batchId)
            return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// (a) the decoder
// --------------------------------------------------------------------------

void testDecoder() {
    {
        SemiconductorSchedule s = queueProblem().build();
        beginSearch(s);
        const auto decoded = decodeValues(s);
        check(startOf(decoded, s, "A") == 0 && startOf(decoded, s, "B") == 2 &&
                      startOf(decoded, s, "X") == 4,
              "queue problem at default ranks decodes {A:0, B:2, X:4}");
    }
    {
        // A complete seed the decoder cannot keep: left-shifting A makes B
        // wait 8 (max queue 1 -> 7).
        Builder b = queueProblem();
        b.assign("L2", "X", "M2", 0, 10)
                .assign("L1", "A", "M1", 7, 9)
                .assign("L1", "B", "M2", 10, 12);
        SemiconductorSchedule s = b.build();
        check(raw(ScoreCalculator::rawCosts(s), Term::MaxQueueTimeCost) == 0,
              "queue seed keeps max queue time");
        std::vector<int> selected, ranks;
        seedState(s, std::vector<int>(static_cast<std::size_t>(s.rowCount()), 0), selected, ranks);
        check(ranks == std::vector<int>{1, 2, 0}, "seed ranks follow the seeded start times");
        const auto decoded =
                decode(s, selected, std::vector<std::int64_t>(ranks.begin(), ranks.end()));
        check(raw(rawCostsOf(s, decoded), Term::MaxQueueTimeCost) == 7,
              "decoding the queue seed costs max queue 7");
    }
    {
        // A partial seed ranks the left-out rows last.
        Builder b = queueProblem();
        b.assign("L2", "X", "M2", 3, 13);
        SemiconductorSchedule s = b.build();
        s.warmStart = true;
        beginSearch(s);
        check(!s.seedComplete, "a partial seed is not complete");
        const auto decoded = decodeValues(s);
        check(startOf(decoded, s, "X") == 0 && startOf(decoded, s, "A") == 0 &&
                      startOf(decoded, s, "B") == 10,
              "partial seed decodes {X:0, A:0, B:10}");
    }
    {
        // A batch tool merges two lots into one run with one id.
        Builder b;
        b.machine("F1", "DIFF", 2);
        b.lot("L1", 0, 50).op("O", "X", {{"F1", 5}});
        b.lot("L2", 0, 50).op("O", "X", {{"F1", 5}});
        SemiconductorSchedule s = b.build();
        beginSearch(s);
        const auto decoded = decodeValues(s);
        check(decoded.size() == 2 && decoded[0].start == 0 && decoded[0].end == 5 &&
                      decoded[1].start == 0 && decoded[1].end == 5,
              "a batch tool runs both lots 0-5");
        check(decoded[0].batchId == std::optional<std::string>("B001") &&
                      decoded[1].batchId == std::optional<std::string>("B001"),
              "the merged run shares batch id B001");
    }
    {
        // A pin is an obstacle and is appended when selected.
        Builder b = choiceProblem();
        b.pin("L1", "O", "M2", 2, 5);
        b.assign("L2", "O", "M1", 0, 3);
        SemiconductorSchedule s = b.build();
        s.warmStart = true;
        beginSearch(s);
        const auto decoded = decodeValues(s);
        bool pinKept = false;
        for (const Assignment& a : decoded)
            pinKept |= a.lot == 0 && a.machine == 1 && a.start == 2 && a.end == 5;
        check(decoded.size() == 2 && pinKept, "a left-out pinned operation stays on its pin");
    }
    {
        // A repeated machine option: the decoder runs the chosen (first)
        // option 0-3, the scorer reads the last one (5) -> eligibility 1.
        Builder b;
        b.machine("M1", "A");
        b.lot("L1", 0, 50).op("O", "F", {{"M1", 3}, {"M1", 5, 1}});
        SemiconductorSchedule s = b.build();
        beginSearch(s);
        const auto decoded = decodeValues(s);
        check(decoded.size() == 1 && decoded[0].start == 0 && decoded[0].end == 3,
              "repeated option decodes 0-3");
        const RawTotals r = rawCostsOf(s, decoded);
        check(raw(r, Term::MachineEligibilityCost) == 1 && raw(r, Term::MachinePreferenceCost) == 1,
              "repeated option: eligibility 1 and the last option's preference");
        s.searching = false;
        s.assignments = decoded;
        std::vector<int> selected, ranks;
        seedState(s, {1}, selected, ranks);
        check(selected == std::vector<int>{0}, "the seed maps (M1, 3) back to option 0");
    }
    {
        SemiconductorSchedule s = horizonProblem().build();
        beginSearch(s);
        bool thrown = false;
        try {
            decodeValues(s);
        } catch (const std::runtime_error&) {
            thrown = true;
        }
        check(thrown, "the decoder gives up after 10 000 passes");
    }
}

// --------------------------------------------------------------------------
// (b) every constraint by hand: a positive and a zero case
// --------------------------------------------------------------------------

void testHandCalculated() {
    const RawTotals fab = rawOf(fabProblem(true));
    check(raw(fab, Term::MachineEligibilityCost) == 0, "fab: eligibility 0");
    check(raw(fab, Term::LotRoutePrecedenceCost) == 2,
          "fab: L1 starts 2 before its release -> precedence 2");
    check(raw(fab, Term::MachineCapacityCost) == 2,
          "fab: M1 runs [0,4) and [2,6) -> machine capacity 2");
    check(raw(fab, Term::BatchCapacityCost) == 3,
          "fab: F1 holds 3 lots on [12,15) -> batch capacity 3");
    check(raw(fab, Term::BatchFamilyConflictCost) == 8,
          "fab: X/Y overlap 4 + 4 on F1 -> family conflict 8");
    check(raw(fab, Term::MissingSetupTimeCost) == 5,
          "fab: X->Y needs 3 after 4, Y starts at 2 -> setup 5");
    check(raw(fab, Term::MachineUnavailabilityCost) == 3,
          "fab: M1 down [3,5): 1 + 2 -> unavailability 3");
    check(raw(fab, Term::MaxQueueTimeCost) == 3, "fab: L1 waits 6 of max 3 -> max queue 3");
    check(raw(fab, Term::MissingTransportTimeCost) == 0,
          "fab: waits 6 and 5 cover transport 3 -> 0");
    check(raw(fab, Term::MachinePreferenceCost) == 1, "fab: L1 PHOTO on M1 costs 1");
    check(raw(fab, Term::MinBatchSizeCost) == 3, "fab: three singleton F1 runs of min 2 -> 3");
    check(raw(fab, Term::MachineLoadImbalanceCost) == 8, "fab: PHOTO loads 8 and 0 -> 8");
    check(raw(fab, Term::WeightedTardinessCost) == 6, "fab: L1 3 late x priority 2 -> 6");
    check(raw(fab, Term::CycleTimeCost) == 46, "fab: 13 + 16 + 17 -> cycle 46");
    check(raw(fab, Term::MakespanCost) == 17, "fab: last lot ends at 17");
    check(raw(fab, Term::MinQueueTimeCost) == 0 && raw(fab, Term::PinnedAssignmentsCost) == 0 &&
                  raw(fab, Term::AuxiliaryResourceCapacityCost) == 0 &&
                  raw(fab, Term::MaxCycleTimeCost) == 0,
          "fab: no min queue, pins, resources or cycle limits");
    {
        Builder b = fabProblem(true);
        b.s.assignments[1].start = 6;
        b.s.assignments[1].end = 11;
        check(raw(rawOf(b), Term::MissingTransportTimeCost) == 1,
              "fab: L1 DIFF at 6 waits 2 of transport 3 -> 1");
    }
    {
        RawItems items;
        SemiconductorSchedule s = fabProblem(true).build();
        ScoreCalculator::rawCosts(s, &items);
        check(items[termIndex(Term::MakespanCost)] == std::vector<std::int64_t>{17},
              "makespan is one item");
        check(items[termIndex(Term::MachineCapacityCost)] == std::vector<std::int64_t>{2, 0},
              "machine capacity: one item per unary machine");
    }

    const RawTotals res = rawOf(resourceProblem());
    check(raw(res, Term::MinQueueTimeCost) == 2, "resources: L1 waits 2 of min 4 -> min queue 2");
    check(raw(res, Term::AuxiliaryResourceCapacityCost) == 2,
          "resources: RETICLE double-booked [2,4) -> 2");
    check(raw(res, Term::AuxiliaryResourceUnavailabilityCost) == 3,
          "resources: RETICLE down [3,5): 1 + 2 -> 3");
    check(raw(res, Term::PinnedAssignmentsCost) == 1,
          "resources: L1 O1 moved off its pin 1-5 -> 1");
    check(raw(res, Term::MaxCycleTimeCost) == 2, "resources: L1 ends 8, max cycle 6 -> 2");

    // Eligibility: a missing operation, a wrong duration, the last option.
    {
        Builder b = fabProblem(true);
        b.s.assignments.pop_back();  // L3 DIFF unassigned
        check(raw(rawOf(b), Term::MachineEligibilityCost) == 1,
              "eligibility: a missing operation costs 1");
    }
    {
        Builder b = fabProblem(true);
        b.s.assignments[0].end = 3;  // L1 PHOTO takes 4 on M1
        check(raw(rawOf(b), Term::MachineEligibilityCost) == 1,
              "eligibility: a wrong duration costs 1");
    }
    {
        Builder b = fabProblem(true);
        b.s.assignments[0].machine = b.machines.at("M2");  // not eligible
        const RawTotals r = rawOf(b);
        check(raw(r, Term::MachineEligibilityCost) == 1 && raw(r, Term::MachinePreferenceCost) == 0,
              "eligibility: an ineligible machine costs 1 and no preference");
    }
    // Min batch size keyed by batch id, empty id = start.
    {
        Builder b;
        b.machine("F1", "G", 3, 3);
        for (const char* id : {"A", "B", "C", "D"}) b.lot(id, 0, 50).op("O", "X", {{"F1", 5}});
        b.assign("A", "O", "F1", 0, 5, "RUN1").assign("B", "O", "F1", 5, 10, "RUN1");
        b.assign("C", "O", "F1", 0, 5, "").assign("D", "O", "F1", 0, 5);
        check(raw(rawOf(b), Term::MinBatchSizeCost) == 2,
              "min batch: groups RUN1 {A,B} and start 0 {C,D} -> 1 + 1");
    }
    // Duplicated required resources count twice.
    {
        Builder b;
        b.machine("M1", "G").resource("R", 1, {{1, 2}});
        b.lot("L", 0, 50).op("O", "F", {{"M1", 4}}, {"R", "R"});
        b.assign("L", "O", "M1", 0, 4);
        const RawTotals r = rawOf(b);
        check(raw(r, Term::AuxiliaryResourceCapacityCost) == 4,
              "a resource listed twice is used twice: 4");
        check(raw(r, Term::AuxiliaryResourceUnavailabilityCost) == 2,
              "and its downtime counts twice: 2");
    }
    // A route overlap is charged by precedence, min queue and transport.
    {
        Builder b;
        b.machine("M1", "G").machine("M2", "H");
        b.lot("L", 0, 50).op("O1", "F", {{"M1", 4}}).op("O2", "F", {{"M2", 4}});
        b.assign("L", "O1", "M1", 0, 4).assign("L", "O2", "M2", 1, 5);
        const RawTotals r = rawOf(b);
        check(raw(r, Term::LotRoutePrecedenceCost) == 3 && raw(r, Term::MinQueueTimeCost) == 3 &&
                      raw(r, Term::MissingTransportTimeCost) == 3,
              "a 3-unit route overlap costs 3 in precedence, min queue and transport");
    }

    // Zero cases.
    const RawTotals zero = rawOf(zeroProblem());
    for (int t = 0; t < kTermCount; ++t) {
        const Term term = static_cast<Term>(t);
        if (term == Term::CycleTimeCost || term == Term::MakespanCost) continue;
        check(zero[static_cast<std::size_t>(t)] == 0,
              "zero fixture: term " + std::to_string(t) + " is 0");
    }
    check(raw(zero, Term::CycleTimeCost) == 15 + 15 + 7 && raw(zero, Term::MakespanCost) == 15,
          "zero fixture: cycle 37, makespan 15");
    {
        Builder b = zeroProblem();
        b.s.assignments.clear();
        const RawTotals r = rawOf(b);
        check(raw(r, Term::CycleTimeCost) == 0 && raw(r, Term::MakespanCost) == 0 &&
                      raw(r, Term::WeightedTardinessCost) == 0,
              "nothing completed: cycle time, makespan and tardiness 0");
        check(raw(r, Term::MachineEligibilityCost) == 5 && raw(r, Term::PinnedAssignmentsCost) == 1,
              "nothing assigned: 5 operations missing, the pin is broken");
    }
    // The default score of the fab fixture.
    {
        SemiconductorSchedule s = fabProblem(true).build();
        const Score score = ScoreCalculator::score(s);
        check(score.hard == -(2 + 2 + 3 + 8 + 5 + 3 + 3), "fab: hard = -26");
        check(score.soft == -(1.0 + 3 + 8 + 10 * 6 + 46 + 17), "fab: soft = -135");
        check(s.makespan == 17 && s.totalWeightedTardiness == 6 && s.totalCycleTime == 46 &&
                      s.scored,
              "fab: totals stored");
    }
}

// --------------------------------------------------------------------------
// (c) incremental == full, per constraint, on random move sequences
// --------------------------------------------------------------------------

void testIncrementalMatchesFull(int instances, int moves) {
    int mismatches = 0;
    std::int64_t movesDone = 0;
    TermHits hits;
    for (int seed = 0; seed < instances; ++seed) {
        SemiconductorSchedule s = randomInstance(static_cast<std::uint64_t>(seed) * 7919 + 1);
        s.warmStart = seed % 3 == 0;
        beginSearch(s);
        SemiconductorIncrementalScoreCalculator calculator;
        optimaru::Editor<SemiconductorSchedule> editor(s, calculator);
        std::mt19937_64 rng(static_cast<std::uint64_t>(seed));
        const int n = s.rowCount();
        // Every constraint's raw cost, then the score.
        auto verify = [&](const char* what) {
            const RawTotals incremental = calculator.rawTotals();
            const RawTotals full = ScoreCalculator::searchStateRawCosts(s);
            const bool same =
                    incremental == full && editor.score() == ScoreCalculator::searchStateScore(s);
            if (!same && mismatches++ < 5)
                std::printf("  mismatch: instance %d after %s\n", seed, what);
            if (!atSeed(s)) hits.add(full);
        };
        verify("reset");
        for (int m = 0; m < moves; ++m, ++movesDone) {
            const int kind = static_cast<int>(rng() % 6);
            const int row = static_cast<int>(rng() % static_cast<std::uint64_t>(n));
            const int value = s.rows[static_cast<std::size_t>(row)].value;
            if (kind == 0) {
                editor.assign(row, randomValue(s, rng, true));
            } else if (kind == 1 && value >= 0) {
                editor.assign(row, s.valueOf(s.rankOf(value),
                                             static_cast<int>(rng() % s.maxOptionCount)));
            } else if (kind == 2 && n > 1) {
                const int other = static_cast<int>(rng() % static_cast<std::uint64_t>(n));
                const int v2 = s.rows[static_cast<std::size_t>(other)].value;
                if (value >= 0 && v2 >= 0) {
                    editor.begin_batch();
                    editor.assign(row, s.valueOf(s.rankOf(v2), s.slotOf(value)));
                    editor.assign(other, s.valueOf(s.rankOf(value), s.slotOf(v2)));
                    editor.end_batch();
                }
            } else if (kind == 3) {
                editor.begin_batch();
                for (int k = 0; k < 3; ++k) {
                    const int r = static_cast<int>(rng() % static_cast<std::uint64_t>(n));
                    editor.assign(r, randomValue(s, rng, false));
                }
                editor.end_batch();
            } else if (kind == 4) {
                // A rejected move: apply, score, undo by the inverse write.
                editor.assign(row, randomValue(s, rng, false));
                editor.score();
                editor.assign(row, value);
            } else if (s.seedComplete && rng() % 4 == 0) {
                // Back to the seed state.
                editor.begin_batch();
                for (int r = 0; r < n; ++r)
                    editor.assign(r, s.seedValues[static_cast<std::size_t>(r)]);
                editor.end_batch();
            }
            verify("a move");
        }
    }
    check(mismatches == 0, "incremental == full, per constraint, on " + std::to_string(instances) +
                                   " instances x " + std::to_string(moves) + " moves (" +
                                   std::to_string(mismatches) + " mismatches)");
    check(hits.missing().empty(),
          "every constraint is non-zero in some decoded state of the walk (never: " +
                  hits.missing() + ")");
    std::printf(
            "  incremental vs full: %lld moves checked; the rarest term is non-zero in %lld "
            "decoded states\n",
            static_cast<long long>(movesDone), static_cast<long long>(hits.fewest()));
}

// The suffix decode really is a suffix: a change late in the order decodes
// only the steps after it.
void testSuffixDecode() {
    SemiconductorSchedule s = fabProblem(false).build();
    beginSearch(s);
    SemiconductorIncrementalScoreCalculator calculator;
    optimaru::Editor<SemiconductorSchedule> editor(s, calculator);
    editor.score();
    const std::int64_t full = calculator.evaluator().decodedSteps();
    // The row with the highest rank (identity ranks: the last row) is ready
    // late; moving its slot re-decodes at most the tail.
    const int last = s.rowCount() - 1;
    editor.assign(last, s.valueOf(s.rankOf(s.rows[static_cast<std::size_t>(last)].value), 0));
    editor.score();
    const std::int64_t after = calculator.evaluator().decodedSteps();
    check(after - full < full, "a late change re-decodes fewer steps than a full decode");
}

// --------------------------------------------------------------------------
// (d) the solver
// --------------------------------------------------------------------------

Score scoreWithAssembly(DecomposedCalculator& calculator, SemiconductorSchedule& schedule) {
    calculator.rebuild(schedule);
    return calculator.score();
}

Solver::Config zeroBudget() {
    Solver::Config config;
    config.time_limit = std::chrono::milliseconds(0);
    return config;
}

void testSolver() {
    {
        SemiconductorSchedule s = queueProblem().build();
        Solver(zeroBudget()).solve(s);
        check(startOf(s.assignments, s, "A") == 0 && startOf(s.assignments, s, "B") == 2 &&
                      startOf(s.assignments, s, "X") == 4,
              "zero budget returns the default decode");
        check(s.scored && !s.searching && !s.warmStart, "zero budget: scored in data mode");
    }
    {
        Builder b = queueProblem();
        b.assign("L2", "X", "M2", 0, 10)
                .assign("L1", "A", "M1", 7, 9)
                .assign("L1", "B", "M2", 10, 12, "RUN");
        SemiconductorSchedule s = b.build();
        const std::vector<Assignment> seed = s.assignments;
        s.warmStart = true;
        Solver(zeroBudget()).solve(s);
        check(sameAssignments(s.assignments, seed), "zero budget returns a complete seed verbatim");
        check(s.score.hard == 0 && s.makespan == 12 && !s.warmStart,
              "the seed is scored as supplied");
    }
    for (int seed = 0; seed < 5; ++seed) {
        Builder b = queueProblem();
        b.assign("L2", "X", "M2", 0, 10)
                .assign("L1", "A", "M1", 7, 9)
                .assign("L1", "B", "M2", 10, 12);
        SemiconductorSchedule s = b.build();
        SemiconductorSchedule copy = s;
        const Score seedScore = ScoreCalculator::score(copy);
        s.warmStart = true;
        Solver::Config config;
        config.step_limit = 200;
        config.seed = static_cast<std::uint64_t>(seed);
        Solver(config).solve(s);
        check(s.score >= seedScore,
              "a solve is never worse than its seed (seed " + std::to_string(seed) + ")");
    }
    {
        SemiconductorSchedule s = randomInstance(42);
        Solver::Config config;
        config.step_limit = 500;
        config.parallelSolverCount = 4;
        config.seed = 3;
        Solver(config).solve(s);
        SemiconductorSchedule rescored = s;
        check(ScoreCalculator::score(rescored) == s.score,
              "parallel starts: the stored score rescored");
        check(static_cast<int>(s.assignments.size()) == s.rowCount(),
              "parallel starts: every row assigned");
    }
    {
        bool corrupted = false;
        try {
            for (int seed = 0; seed < 8; ++seed) {
                SemiconductorSchedule s = randomInstance(1000 + static_cast<std::uint64_t>(seed));
                s.warmStart = seed % 2 == 0;
                Solver::Config config;
                config.score_checks = optimaru::ScoreChecks::Full;
                config.step_limit = 400;
                config.seed = static_cast<std::uint64_t>(seed);
                Solver(config).solve(s);
            }
        } catch (const std::exception& error) {
            std::printf("  %s\n", error.what());
            corrupted = true;
        }
        check(!corrupted, "FULL_ASSERT solves report no score corruption");
    }
    for (const bool warm : {false, true}) {
        for (const bool plugin : {false, true}) {
            // A failed solve leaves search mode: both scoring paths score the
            // assignment list again, as before the solve.
            Builder b = horizonProblem();
            b.assign("L1", "O", "M1", 0, 2).assign("L2", "O", "M2", 0, 3);
            SemiconductorSchedule s = b.build();
            s.warmStart = warm;
            SemiconductorSchedule copy = s;
            const Score before = ScoreCalculator::score(copy);
            Solver::Config config;
            config.step_limit = 20;
            bool thrown = false;
            try {
                DecomposedCalculator calculator;
                addDefaultConstraints(calculator);
                if (plugin)
                    Solver(config).solve(s, calculator);
                else
                    Solver(config).solve(s);
            } catch (const std::runtime_error&) {
                thrown = true;
            }
            DecomposedCalculator again;
            addDefaultConstraints(again);
            again.rebuild(s);
            SemiconductorSchedule rescored = s;
            const std::string what =
                    std::string(warm ? "warm" : "cold") + (plugin ? ", plugin" : "");
            check(thrown && !s.searching && !s.seedComplete && s.seedValues.empty() &&
                          s.seedAssignments.empty(),
                  "a failed solve leaves search mode (" + what + ")");
            check(again.score() == before && ScoreCalculator::score(rescored) == before,
                  "after a failed solve both paths score the assignments (" + what + ")");
        }
    }
    {
        // A short solve improves the queue problem's seed with idle time.
        Builder b = choiceProblem();
        b.assign("L1", "O", "M1", 4, 7).assign("L2", "O", "M1", 7, 10);
        SemiconductorSchedule s = b.build();
        SemiconductorSchedule copy = s;
        const Score seedScore = ScoreCalculator::score(copy);
        s.warmStart = true;
        Solver::Config config;
        config.step_limit = 100;
        config.seed = 1;
        Solver(config).solve(s);
        check(s.score > seedScore, "the search improves a seed with idle time");
    }
}

// Times and costs are exact int64; what leaves int64 throws instead of
// wrapping.
void testCheckedArithmetic() {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    auto throws = [](auto&& f) {
        try {
            f();
        } catch (const std::overflow_error&) {
            return true;
        }
        return false;
    };
    check(checked::add(kMax, -1) == kMax - 1 && checked::sub(-1, kMin) == kMax &&
                  checked::mul(kMin, 1) == kMin && checked::mul(-3, 4) == -12 &&
                  checked::mul(-3, -4) == 12 &&
                  checked::mul(3037000499, 3037000499) == 3037000499LL * 3037000499LL,
          "checked arithmetic: in-range results are exact");
    check(throws([&] { checked::add(kMax, 1); }) && throws([&] { checked::add(kMin, -1); }) &&
                  throws([&] { checked::sub(kMin, 1); }) &&
                  throws([&] { checked::sub(0, kMin); }) &&
                  throws([&] { checked::mul(kMax, 2); }) &&
                  throws([&] { checked::mul(-1, kMin); }) &&
                  throws([&] { checked::mul(kMin, -1); }) &&
                  throws([&] { checked::mul(3037000500, 3037000500); }) &&
                  throws([&] { checked::mul(-3037000500, 3037000500); }),
          "checked arithmetic: out-of-range results throw");

    auto oneLot = [](std::int64_t priority) {
        Builder b;
        b.machine("M1", "G");
        b.lot("L1", 0, 1, priority).op("O", "F", {{"M1", Time{1} << 30}});
        b.assign("L1", "O", "M1", 0, Time{1} << 30);
        return b.build();
    };
    SemiconductorSchedule exact = oneLot(std::int64_t{1} << 30);
    ScoreCalculator::score(exact);
    check(exact.totalWeightedTardiness == (std::int64_t{1} << 30) * ((std::int64_t{1} << 30) - 1),
          "a 2^60 tardiness is exact");
    SemiconductorSchedule huge = oneLot(std::int64_t{1} << 40);
    check(throws([&] { ScoreCalculator::score(huge); }),
          "a tardiness beyond int64 throws (monolith)");
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    check(throws([&] { scoreWithAssembly(calculator, huge); }),
          "a tardiness beyond int64 throws (plugin path)");
    check(throws([&] { Solver(zeroBudget()).solve(huge); }) && !huge.searching,
          "a tardiness beyond int64 throws (solve) and leaves search mode");
}

void testCatalog() {
    const auto rows = defaultConstraints().info();
    check(rows.size() == 20, "20 constraints in the catalog");
    int hard = 0;
    for (const auto& row : rows) {
        hard += row.default_level == optimaru::Level::Hard ? 1 : 0;
        check(row.level_flippable && !row.reward && row.is_global && row.default_enabled,
              row.name + ": flippable, not a reward, global, enabled");
    }
    check(hard == 14, "14 hard constraints by default");
}

}  // namespace

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string(argv[1]) == "--quick";
    testDecoder();
    testHandCalculated();
    testIncrementalMatchesFull(quick ? 40 : 300, quick ? 200 : 1000);
    testSuffixDecode();
    testSolver();
    testCheckedArithmetic();
    testCatalog();
    return finish("fab_test");
}
