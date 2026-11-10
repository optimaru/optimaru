// Plugin-assembly (ba/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedObjective
//       reproduces the monolithic score bit-for-bit on 500 random
//       assignments (uninitialized stays and the "no bed" sentinel included)
//   (b) incremental score == full recompute across 3000 random moves driven
//       through the engine's Editor protocol (plugin path)
//   (c) partial assemblies: removing one component removes exactly that term
//   (d) weight / level / exponent overrides
//   (e) constraint_totals() explains exactly the reported score
//   (f) a 2-second solve(plan, calculator) under ScoreChecks::Full
//   (g) every component's raw cost by hand (constraint_costs()): one positive
//       and one zero case each
//   (h) the catalog read off the default assembly == the components'
//       kDefaultWeight / kDefaultLevel
//   (i) the medium level: the fold, hard() / soft() / weight on
//       unassignedStaysCost, and incremental == full with those overrides
//   (j) repeated components and the offset / clip knobs: a hand-calculated
//       repeat, threshold and literal offsets by hand (the per-bed
//       constraints evaluate the real beds, an empty one included, and never
//       the "no bed" sentinel, which is the only unit of
//       unassignedStaysCost), the global's total shaped once, the literal
//       formula refused on hard at reset and kept on soft (and on the
//       into_soft medium), incremental == a fresh recompute over a random
//       walk and a ScoreChecks::Full solve with repeated, shifted components
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ba/constraints.hpp"
#include "ba/domain.hpp"
#include "ba/score_calculator.hpp"
#include "ba/solver.hpp"
#include "optimaru/model/editor.hpp"

using namespace ba;

namespace {

int failures = 0;

void check(bool ok, const std::string& label) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++failures;
}

void checkScore(const std::string& label, const Score& actual, std::int64_t expectedHard,
                std::int64_t expectedMedium, std::int64_t expectedSoft) {
    const bool ok = actual.hard == expectedHard && actual.medium == expectedMedium &&
                    actual.soft == expectedSoft;
    std::printf("  [%s] %-52s got=%s expected=%lldhard/%lldmedium/%lldsoft\n", ok ? "PASS" : "FAIL",
                label.c_str(), actual.str().c_str(), static_cast<long long>(expectedHard),
                static_cast<long long>(expectedMedium), static_cast<long long>(expectedSoft));
    if (!ok) ++failures;
}

// --- instance helpers (the ba_test shapes) ---

BedPlan makeBase() {
    BedPlan plan;
    Department department;
    department.id = "d0";
    department.name = "Dept";
    department.specialtyToPriority = {{"default", 1}};
    plan.departments.push_back(department);
    return plan;
}

int addRoom(BedPlan& plan, GenderLimitation genderLimitation = GenderLimitation::ANY_GENDER,
            int capacity = 1, std::vector<std::string> equipment = {}, int department = 0) {
    Room room;
    room.id = "r" + std::to_string(plan.rooms.size());
    room.name = room.id;
    room.department = department;
    room.capacity = capacity;
    room.genderLimitation = genderLimitation;
    room.equipment = std::move(equipment);
    plan.rooms.push_back(room);
    return static_cast<int>(plan.rooms.size()) - 1;
}

int addBed(BedPlan& plan, int roomIndex) {
    Bed bed;
    bed.id = "b" + std::to_string(plan.beds.size());
    bed.room = roomIndex;
    bed.indexInRoom = 0;
    plan.beds.push_back(bed);
    return static_cast<int>(plan.beds.size()) - 1;
}

int addStay(BedPlan& plan, int arrivalDay, int departureDay, Gender gender = Gender::MALE,
            int bedValue = -1, const std::string& specialty = "default") {
    Stay stay;
    stay.id = "s" + std::to_string(plan.stays.size());
    stay.patientName = stay.id;
    stay.patientGender = gender;
    stay.patientAge = 30;
    stay.arrivalDay = arrivalDay;
    stay.departureDay = departureDay;
    stay.specialty = specialty;
    stay.bed = bedValue;
    plan.stays.push_back(stay);
    return static_cast<int>(plan.stays.size()) - 1;
}

// Random mid-size instance (the ba_test shape): two departments with age
// limits, six rooms covering every gender limitation, mixed equipment.
BedPlan buildRandomInstance(int stayCount, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    BedPlan plan;

    Department young;
    young.id = "d0";
    young.name = "Underage";
    young.maximumAge = 20;
    young.specialtyToPriority = {{"spec1", 1}, {"spec2", 2}};
    plan.departments.push_back(young);

    Department adult;
    adult.id = "d1";
    adult.name = "Adult";
    adult.minimumAge = 18;
    adult.specialtyToPriority = {{"spec2", 1}};
    plan.departments.push_back(adult);

    const std::vector<std::string> equipmentPool = {"telemetry", "television", "oxygen",
                                                    "nitrogen"};
    const GenderLimitation limitations[] = {
            GenderLimitation::ANY_GENDER,  GenderLimitation::MALE_ONLY,
            GenderLimitation::FEMALE_ONLY, GenderLimitation::SAME_GENDER,
            GenderLimitation::SAME_GENDER, GenderLimitation::SAME_GENDER};
    for (int r = 0; r < 6; ++r) {
        std::vector<std::string> roomEquipment;
        for (const std::string& equipment : equipmentPool) {
            if (rng() % 2 == 0) roomEquipment.push_back(equipment);
        }
        const int capacity = 1 + static_cast<int>(rng() % 3);
        const int room =
                addRoom(plan, limitations[r], capacity, roomEquipment, static_cast<int>(rng() % 2));
        for (int i = 0; i < capacity; ++i) addBed(plan, room);
    }

    const std::vector<std::string> specialties = {"spec1", "spec2", "spec3"};
    for (int s = 0; s < stayCount; ++s) {
        const int arrival = static_cast<int>(rng() % 18);
        const int departure = arrival + static_cast<int>(rng() % 5);
        const int stay =
                addStay(plan, arrival, departure, rng() % 2 == 0 ? Gender::MALE : Gender::FEMALE,
                        -1, specialties[rng() % specialties.size()]);
        plan.stays[stay].patientAge = 1 + static_cast<int>(rng() % 99);
        if (rng() % 2 == 0) {
            plan.stays[stay].patientPreferredMaximumRoomCapacity = 1 + static_cast<int>(rng() % 3);
        }
        for (const std::string& equipment : equipmentPool) {
            if (rng() % 4 == 0) plan.stays[stay].patientRequiredEquipment.push_back(equipment);
            if (rng() % 3 == 0) plan.stays[stay].patientPreferredEquipment.push_back(equipment);
        }
    }
    return plan;
}

// Scores `plan`'s current assignment with a freshly reset assembly, decoded
// back into the domain's hard / medium / soft levels.
Score scoreWith(DecomposedCalculator& calculator, BedPlan& plan) {
    calculator.rebuild(plan);
    return fromEngineScore(calculator.score());
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute, on random
// assignments over the whole value range (real beds, the "no bed" sentinel
// and uninitialized).
// -------------------------------------------------------------------------
void verifyAssemblyParity() {
    std::printf("== (a) Default assembly == monolithic score ==\n");
    BedPlan plan = buildRandomInstance(/*stayCount*/ 30, /*seed*/ 7);
    ScoreCalculator::prepare(plan);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    int mismatches = 0;
    const int rounds = 500;
    for (int round = 0; round < rounds; ++round) {
        for (Stay& stay : plan.stays) {
            const double roll = unit(rng);
            if (roll < 0.05)
                stay.bed = -1;  // uninitialized
            else if (roll < 0.15)
                stay.bed = plan.noBedValue();
            else
                stay.bed = static_cast<int>(rng() % plan.bedCount());
        }
        const Score plugin = scoreWith(calculator, plan);
        const Score monolithic = ScoreCalculator::score(plan);
        if (!(plugin == monolithic)) {
            if (++mismatches <= 3) {
                std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n", round,
                            plugin.str().c_str(), monolithic.str().c_str());
            }
        }
    }
    check(mismatches == 0, std::to_string(rounds) + " random assignments score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of change and
// swap moves, driven through the Editor so the plugin path's
// before/set/after hook contract is exercised exactly like in the engine.
// -------------------------------------------------------------------------
void verifyIncremental() {
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    BedPlan plan = buildRandomInstance(/*stayCount*/ 30, /*seed*/ 7);
    ScoreCalculator::prepare(plan);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::Editor<BedPlan> editor(plan, calculator);
    BedPlan shadow = plan;

    std::mt19937_64 rng(123);
    const int stayCount = static_cast<int>(plan.stays.size());
    const int value_count = plan.bedCount() + 1;  // + the "no bed" sentinel
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step) {
        if (unit(rng) < 0.75) {
            const int stayIndex = static_cast<int>(rng() % stayCount);
            const int value = (unit(rng) < 0.05) ? -1 : static_cast<int>(rng() % value_count);
            editor.assign(stayIndex, value);
        } else {
            const int a = static_cast<int>(rng() % stayCount);
            const int b = static_cast<int>(rng() % stayCount);
            const int va = plan.stays[a].bed;
            const int vb = plan.stays[b].bed;
            editor.assign(a, vb);
            editor.assign(b, va);
        }
        for (int i = 0; i < stayCount; ++i) shadow.stays[i].bed = plan.stays[i].bed;
        const Score full = ScoreCalculator::score(shadow);
        const Score incremental = fromEngineScore(editor.score());
        if (!(full == incremental)) {
            if (++mismatches <= 3) {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n", step,
                            incremental.str().c_str(), full.str().c_str());
            }
        }
    }
    check(mismatches == 0, std::to_string(steps) + " random moves, incremental == full recompute");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Partial assemblies: each component in isolation on hand-computed
// instances (6-night stays).
// -------------------------------------------------------------------------
void verifyPartialAssemblies() {
    std::printf("== (c) Single-component assemblies ==\n");

    {  // femaleInMaleRoomCost alone: 50 * 6 nights, nothing else.
        BedPlan plan = makeBase();
        const int bed = addBed(plan, addRoom(plan, GenderLimitation::MALE_ONLY));
        addStay(plan, 0, 5, Gender::FEMALE, bed);
        ScoreCalculator::prepare(plan);
        DecomposedCalculator calculator;
        calculator.emplace<FemaleInMaleRoomCost>();
        checkScore("femaleInMaleRoomCost alone", scoreWith(calculator, plan), -300, 0, 0);
    }
    {  // bedConflictCost alone: two full-overlap stays, 1000 * 6.
        BedPlan plan = makeBase();
        const int bed = addBed(plan, addRoom(plan));
        addStay(plan, 0, 5, Gender::MALE, bed);
        addStay(plan, 0, 5, Gender::MALE, bed);
        ScoreCalculator::prepare(plan);
        DecomposedCalculator calculator;
        calculator.emplace<BedConflictCost>();
        checkScore("bedConflictCost alone", scoreWith(calculator, plan), -6000, 0, 0);
    }
    {  // mixedGenderRoomCost alone: the pair spans two beds of one
        // SAME_GENDER room, which is why it is a global.
        BedPlan plan = makeBase();
        const int room = addRoom(plan, GenderLimitation::SAME_GENDER, /*capacity*/ 2);
        addStay(plan, 0, 5, Gender::FEMALE, addBed(plan, room));
        addStay(plan, 0, 5, Gender::MALE, addBed(plan, room));
        ScoreCalculator::prepare(plan);
        DecomposedCalculator calculator;
        calculator.emplace<MixedGenderRoomCost>();
        checkScore("mixedGenderRoomCost alone", scoreWith(calculator, plan), -6000, 0, 0);
    }
    {  // unassignedStaysCost alone: the sentinel costs 6 medium.
        BedPlan plan = makeBase();
        addBed(plan, addRoom(plan));
        addStay(plan, 0, 5, Gender::MALE, plan.noBedValue());
        ScoreCalculator::prepare(plan);
        DecomposedCalculator calculator;
        calculator.emplace<UnassignedStaysCost>();
        checkScore("unassignedStaysCost alone", scoreWith(calculator, plan), 0, -6, 0);
    }
    {  // The same stay in a real bed costs nothing on that component.
        BedPlan plan = makeBase();
        const int bed = addBed(plan, addRoom(plan));
        addStay(plan, 0, 5, Gender::MALE, bed);
        ScoreCalculator::prepare(plan);
        DecomposedCalculator calculator;
        calculator.emplace<UnassignedStaysCost>();
        checkScore("unassignedStaysCost silent in a real bed", scoreWith(calculator, plan), 0, 0,
                   0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides: the level and the weight are assembly-time
// metadata, not part of the cost.
// -------------------------------------------------------------------------
void verifyOverrides() {
    std::printf("== (d) Weight / level overrides ==\n");
    BedPlan plan = makeBase();
    const int bed = addBed(plan, addRoom(plan, GenderLimitation::MALE_ONLY));
    addStay(plan, 0, 5, Gender::FEMALE, bed);
    ScoreCalculator::prepare(plan);

    {
        DecomposedCalculator calculator;
        calculator.emplace<FemaleInMaleRoomCost>(10.0);
        checkScore("femaleInMaleRoomCost re-weighted to 10", scoreWith(calculator, plan), -60, 0,
                   0);
    }
    {
        // Demoted to soft: the hard level empties out, the soft level takes
        // the same 300 penalty units.
        DecomposedCalculator calculator;
        auto component = std::make_unique<FemaleInMaleRoomCost>();
        component->level = optimaru::Level::Soft;
        calculator.add(std::move(component));
        checkScore("femaleInMaleRoomCost demoted to soft", scoreWith(calculator, plan), 0, 0, -300);
    }
    {
        // exponent 2 shapes the raw cost (6 nights) before the weight:
        // 50 * 36 = 1800.
        DecomposedCalculator calculator;
        auto component = std::make_unique<FemaleInMaleRoomCost>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore("femaleInMaleRoomCost with exponent 2", scoreWith(calculator, plan), -1800, 0,
                   0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraint_totals(): the per-constraint breakdown adds up to the score
// the same assembly reports.
// -------------------------------------------------------------------------
void verifyConstraintTotals() {
    std::printf("== (e) constraintTotals() explains the score ==\n");
    BedPlan plan = buildRandomInstance(/*stayCount*/ 24, /*seed*/ 13);
    ScoreCalculator::prepare(plan);
    std::mt19937_64 rng(5);
    for (Stay& stay : plan.stays) stay.bed = static_cast<int>(rng() % (plan.bedCount() + 1));

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(plan);
    const optimaru::Score score = calculator.score();

    // A medium total is in medium units (cost is not multiplied by the
    // fold), so it folds into soft the way the score does.
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
    check(calculator.constraint_totals().size() == 12, "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) ScoreChecks::Full solve on the plugin path: the engine cross-checks the
// incremental score against a full reset + recompute after every move.
// -------------------------------------------------------------------------
void verifyFullAssertSolve() {
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    BedPlan plan = buildRandomInstance(/*stayCount*/ 20, /*seed*/ 11);

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
// (g) Raw costs by hand: every component's per-unit raw cost (beds in order,
// then the "no bed" sentinel; one entry for the global), read through
// constraint_costs() off the default assembly. One positive and one zero case
// per component; the stays span 6 nights ([0, 5]) unless stated otherwise.
// -------------------------------------------------------------------------
std::vector<double> rawCosts(BedPlan plan, const std::string& name) {
    ScoreCalculator::prepare(plan);
    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.rebuild(plan);
    calculator.score();
    for (const auto& entry : calculator.constraint_costs()) {
        if (entry.name == name) return entry.unit_costs;
    }
    std::printf("  no constraint named %s\n", name.c_str());
    return {};
}

void byHand(const std::string& name, const std::function<BedPlan()>& positive,
            const std::vector<double>& positiveCosts, const std::function<BedPlan()>& zero,
            const std::vector<double>& zeroCosts) {
    check(rawCosts(positive(), name) == positiveCosts, name + " positive");
    check(rawCosts(zero(), name) == zeroCosts, name + " zero");
}

// One room holding two beds (b0, b1) — units {b0, b1, sentinel}.
BedPlan twoBedRoom(GenderLimitation limitation = GenderLimitation::ANY_GENDER, int capacity = 2,
                   std::vector<std::string> equipment = {}) {
    BedPlan plan = makeBase();
    const int room = addRoom(plan, limitation, capacity, std::move(equipment));
    addBed(plan, room);
    addBed(plan, room);
    return plan;
}

void verifyRawCostsByHand() {
    std::printf("== (g) Raw costs by hand ==\n");
    const std::vector<double> none{0.0, 0.0, 0.0};

    // b0 holds [0, 5] and [3, 9]: 3 shared nights; b1 holds one stay alone.
    byHand(
            "bedConflictCost",
            [] {
                BedPlan p = twoBedRoom();
                addStay(p, 0, 5, Gender::MALE, 0);
                addStay(p, 3, 9, Gender::MALE, 0);
                addStay(p, 0, 5, Gender::MALE, 1);
                return p;
            },
            {3.0, 0.0, 0.0},
            [] {  // back to back in b0: no shared night.
                BedPlan p = twoBedRoom();
                addStay(p, 0, 2, Gender::MALE, 0);
                addStay(p, 3, 9, Gender::MALE, 0);
                return p;
            },
            none);
    // A female stay (6 nights) in b0 of a MALE_ONLY room; a male one in b1.
    byHand(
            "femaleInMaleRoomCost",
            [] {
                BedPlan p = twoBedRoom(GenderLimitation::MALE_ONLY);
                addStay(p, 0, 5, Gender::FEMALE, 0);
                addStay(p, 0, 5, Gender::MALE, 1);
                return p;
            },
            {6.0, 0.0, 0.0},
            [] {  // the same female stay in a FEMALE_ONLY room.
                BedPlan p = twoBedRoom(GenderLimitation::FEMALE_ONLY);
                addStay(p, 0, 5, Gender::FEMALE, 0);
                return p;
            },
            none);
    byHand(
            "maleInFemaleRoomCost",
            [] {
                BedPlan p = twoBedRoom(GenderLimitation::FEMALE_ONLY);
                addStay(p, 0, 5, Gender::FEMALE, 0);
                addStay(p, 0, 5, Gender::MALE, 1);
                return p;
            },
            {0.0, 6.0, 0.0},
            [] {
                BedPlan p = twoBedRoom(GenderLimitation::MALE_ONLY);
                addStay(p, 0, 5, Gender::MALE, 1);
                return p;
            },
            none);
    // The global: a female [0, 5] in b0 and a male [3, 9] in b1 of one
    // SAME_GENDER room share 3 nights.
    byHand(
            "mixedGenderRoomCost",
            [] {
                BedPlan p = twoBedRoom(GenderLimitation::SAME_GENDER);
                addStay(p, 0, 5, Gender::FEMALE, 0);
                addStay(p, 3, 9, Gender::MALE, 1);
                return p;
            },
            {3.0},
            [] {  // the same pair in an ANY_GENDER room.
                BedPlan p = twoBedRoom(GenderLimitation::ANY_GENDER);
                addStay(p, 0, 5, Gender::FEMALE, 0);
                addStay(p, 3, 9, Gender::MALE, 1);
                return p;
            },
            {0.0});
    // Minimum age 18: a 5-year-old in b0 (6 nights), a 30-year-old in b1.
    byHand(
            "departmentMinAgeCost",
            [] {
                BedPlan p = twoBedRoom();
                p.departments[0].minimumAge = 18;
                p.stays[addStay(p, 0, 5, Gender::MALE, 0)].patientAge = 5;
                addStay(p, 0, 5, Gender::MALE, 1);
                return p;
            },
            {6.0, 0.0, 0.0},
            [] {  // exactly the minimum age is allowed.
                BedPlan p = twoBedRoom();
                p.departments[0].minimumAge = 18;
                p.stays[addStay(p, 0, 5, Gender::MALE, 0)].patientAge = 18;
                return p;
            },
            none);
    byHand(
            "departmentMaxAgeCost",
            [] {
                BedPlan p = twoBedRoom();
                p.departments[0].maximumAge = 18;
                addStay(p, 0, 5, Gender::MALE, 0);  // age 30
                return p;
            },
            {6.0, 0.0, 0.0},
            [] {
                BedPlan p = twoBedRoom();
                p.departments[0].maximumAge = 18;
                p.stays[addStay(p, 0, 5, Gender::MALE, 0)].patientAge = 18;
                return p;
            },
            none);
    // The room has telemetry and oxygen, the stay needs television too: one
    // item missing, so the charge is the 2 PRESENT items x 6 nights.
    byHand(
            "requiredPatientEquipmentCost",
            [] {
                BedPlan p = twoBedRoom(GenderLimitation::ANY_GENDER, 2, {"telemetry", "oxygen"});
                p.stays[addStay(p, 0, 5, Gender::MALE, 1)].patientRequiredEquipment = {
                        "television", "telemetry", "oxygen"};
                return p;
            },
            {0.0, 12.0, 0.0},
            [] {  // all required items present.
                BedPlan p = twoBedRoom(GenderLimitation::ANY_GENDER, 2, {"telemetry", "oxygen"});
                p.stays[addStay(p, 0, 5, Gender::MALE, 1)].patientRequiredEquipment = {"telemetry",
                                                                                       "oxygen"};
                return p;
            },
            none);
    {  // The kept quirk: a room with NONE of the required items costs 0.
        BedPlan p = twoBedRoom();
        p.stays[addStay(p, 0, 5, Gender::MALE, 0)].patientRequiredEquipment = {"television"};
        check(rawCosts(p, "requiredPatientEquipmentCost") == none,
              "requiredPatientEquipmentCost none present (quirk kept)");
    }
    // Two stays (6 + 3 nights) on the sentinel, one in b0: 9 on the sentinel.
    byHand(
            "unassignedStaysCost",
            [] {
                BedPlan p = twoBedRoom();
                addStay(p, 0, 5, Gender::MALE, p.noBedValue());
                addStay(p, 0, 2, Gender::MALE, p.noBedValue());
                addStay(p, 0, 5, Gender::MALE, 0);
                return p;
            },
            {0.0, 0.0, 9.0},
            [] {  // every stay in a bed; an uninitialized stay is not on the
                // sentinel and costs nothing either.
                BedPlan p = twoBedRoom();
                addStay(p, 0, 5, Gender::MALE, 0);
                addStay(p, 0, 5, Gender::MALE, -1);
                return p;
            },
            none);
    // Room capacity 3, the patient prefers at most 2.
    byHand(
            "preferredMaxRoomCapacityCost",
            [] {
                BedPlan p = twoBedRoom(GenderLimitation::ANY_GENDER, 3);
                p.stays[addStay(p, 0, 5, Gender::MALE, 0)].patientPreferredMaximumRoomCapacity = 2;
                return p;
            },
            {6.0, 0.0, 0.0},
            [] {
                BedPlan p = twoBedRoom(GenderLimitation::ANY_GENDER, 3);
                p.stays[addStay(p, 0, 5, Gender::MALE, 0)].patientPreferredMaximumRoomCapacity = 3;
                return p;
            },
            none);
    // The department offers only "default" (priority 1).
    byHand(
            "departmentSpecialtyCost",
            [] {
                BedPlan p = twoBedRoom();
                addStay(p, 0, 5, Gender::MALE, 1, "spec9");
                return p;
            },
            {0.0, 6.0, 0.0},
            [] {
                BedPlan p = twoBedRoom();
                addStay(p, 0, 5, Gender::MALE, 1, "default");
                return p;
            },
            none);
    // Offered at priority 3: (3 - 1) x 6 nights.
    byHand(
            "departmentSpecialtyPriorityCost",
            [] {
                BedPlan p = twoBedRoom();
                p.departments[0].specialtyToPriority = {{"spec1", 3}};
                addStay(p, 0, 5, Gender::MALE, 0, "spec1");
                return p;
            },
            {12.0, 0.0, 0.0},
            [] {  // priority 1, and a specialty not offered at all (silent here:
                // departmentSpecialtyCost prices it).
                BedPlan p = twoBedRoom();
                p.departments[0].specialtyToPriority = {{"spec1", 1}};
                addStay(p, 0, 5, Gender::MALE, 0, "spec1");
                addStay(p, 0, 5, Gender::MALE, 1, "spec9");
                return p;
            },
            none);
    // The room has telemetry; the stay prefers television and oxygen too:
    // 2 missing x 6 nights.
    byHand(
            "preferredPatientEquipmentCost",
            [] {
                BedPlan p = twoBedRoom(GenderLimitation::ANY_GENDER, 2, {"telemetry"});
                p.stays[addStay(p, 0, 5, Gender::MALE, 0)].patientPreferredEquipment = {
                        "television", "oxygen", "telemetry"};
                return p;
            },
            {12.0, 0.0, 0.0},
            [] {
                BedPlan p = twoBedRoom(GenderLimitation::ANY_GENDER, 2, {"telemetry"});
                p.stays[addStay(p, 0, 5, Gender::MALE, 0)].patientPreferredEquipment = {
                        "telemetry"};
                return p;
            },
            none);
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (h) The catalog read off the default assembly == the components' constants.
// -------------------------------------------------------------------------
template <typename Component>
bool rowIs(const optimaru::ConstraintInfo& row, const char* name, optimaru::Level level,
           double weight, bool is_global) {
    return row.name == name && row.default_level == Component::kDefaultLevel &&
           row.default_weight == Component::kDefaultWeight && row.default_level == level &&
           row.default_weight == weight && row.level_flippable && !row.reward &&
           row.is_global == is_global && row.default_enabled;
}

void verifyCatalog() {
    std::printf("== (h) Catalog == component constants ==\n");
    using optimaru::Level;
    const auto rows = defaultConstraints().info();
    check(rows.size() == 12, "12 catalog rows");
    if (rows.size() != 12) return;
    // Unit constraints first, then the global, in emplace order: the
    // list_constraints() order (mixedGenderRoomCost last).
    check(rowIs<BedConflictCost>(rows[0], "bedConflictCost", Level::Hard, 1000.0, false) &&
                  rowIs<FemaleInMaleRoomCost>(rows[1], "femaleInMaleRoomCost", Level::Hard, 50.0,
                                              false) &&
                  rowIs<MaleInFemaleRoomCost>(rows[2], "maleInFemaleRoomCost", Level::Hard, 50.0,
                                              false) &&
                  rowIs<DepartmentMinAgeCost>(rows[3], "departmentMinAgeCost", Level::Hard, 100.0,
                                              false) &&
                  rowIs<DepartmentMaxAgeCost>(rows[4], "departmentMaxAgeCost", Level::Hard, 100.0,
                                              false) &&
                  rowIs<RequiredPatientEquipmentCost>(rows[5], "requiredPatientEquipmentCost",
                                                      Level::Hard, 50.0, false) &&
                  rowIs<UnassignedStaysCost>(rows[6], "unassignedStaysCost", Level::Medium, 1.0,
                                             false) &&
                  rowIs<PreferredMaxRoomCapacityCost>(rows[7], "preferredMaxRoomCapacityCost",
                                                      Level::Soft, 8.0, false) &&
                  rowIs<DepartmentSpecialtyCost>(rows[8], "departmentSpecialtyCost", Level::Soft,
                                                 10.0, false) &&
                  rowIs<DepartmentSpecialtyPriorityCost>(rows[9], "departmentSpecialtyPriorityCost",
                                                         Level::Soft, 10.0, false) &&
                  rowIs<PreferredPatientEquipmentCost>(rows[10], "preferredPatientEquipmentCost",
                                                       Level::Soft, 50.0, false) &&
                  rowIs<MixedGenderRoomCost>(rows[11], "mixedGenderRoomCost", Level::Hard, 1000.0,
                                             true),
          "info() == the components' kDefaultWeight / kDefaultLevel");
    std::vector<std::string> infoNames;
    for (const auto& row : rows) infoNames.push_back(row.name);
    check(infoNames == defaultConstraints().names(), "info() rows in names() order");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (i) The medium level: unassignedStaysCost.
// -------------------------------------------------------------------------
optimaru::DecomposedObjective<BedPlan>::LevelScores levelsWith(ConstraintAssembly assembly,
                                                               BedPlan& plan) {
    DecomposedCalculator calculator;
    std::move(assembly).into(calculator);
    calculator.rebuild(plan);
    calculator.score();
    return calculator.level_scores();
}

void verifyMediumLevel() {
    std::printf("== (i) Medium level ==\n");
    // One stay (6 nights) on the sentinel, one in b0 missing one preferred
    // item: 6 medium nights and 50 * 6 = 300 soft.
    BedPlan plan = twoBedRoom();
    addStay(plan, 0, 5, Gender::MALE, plan.noBedValue());
    plan.stays[addStay(plan, 0, 5, Gender::MALE, 0)].patientPreferredEquipment = {"television"};
    const Score monolithic = ScoreCalculator::score(plan);
    checkScore("the hand plan", monolithic, 0, -6, -300);

    {
        DecomposedCalculator calculator;
        addDefaultConstraints(calculator);
        calculator.rebuild(plan);
        const optimaru::Score engine = calculator.score();
        check(engine.hard == 0 && engine.soft == -300.0 - 6.0 * static_cast<double>(kMediumUnit),
              "default: medium folded into soft x 10^9");
        check(engine.hard == toEngineScore(monolithic).hard &&
                      engine.soft == toEngineScore(monolithic).soft,
              "default: the fold equals the monolithic Score adapter");
        const auto levels = calculator.level_scores();
        check(levels.hard == 0 && levels.medium == -6.0 && levels.soft == -300.0,
              "levelScores() reports the unfolded levels");
    }
    {
        const auto levels = levelsWith(defaultConstraints().hard("unassignedStaysCost"), plan);
        check(levels.hard == -6 && levels.medium == 0.0 && levels.soft == -300.0,
              "hard(): one hard unit per raw night");
    }
    {
        const auto levels = levelsWith(defaultConstraints().soft("unassignedStaysCost"), plan);
        check(levels.hard == 0 && levels.medium == 0.0 && levels.soft == -306.0,
              "soft(): one soft unit per raw night, outside the fold");
    }
    {
        const auto levels = levelsWith(defaultConstraints().weight("unassignedStaysCost", 3), plan);
        check(levels.hard == 0 && levels.medium == -18.0 && levels.soft == -300.0,
              "weight 3 stays in medium units");
    }
    {
        const auto levels = levelsWith(defaultConstraints().level("preferredPatientEquipmentCost",
                                                                  optimaru::Level::Medium),
                                       plan);
        check(levels.hard == 0 && levels.medium == -306.0 && levels.soft == 0.0,
              "level(Medium): preferredPatientEquipmentCost moves onto medium");
    }
    {
        const auto levels =
                levelsWith(defaultConstraints()
                                   .level("preferredPatientEquipmentCost", optimaru::Level::Medium)
                                   .weight("preferredPatientEquipmentCost", 1),
                           plan);
        check(levels.hard == 0 && levels.medium == -12.0 && levels.soft == 0.0,
              "a weight on a moved constraint is in medium units");
    }
    {
        // solve(plan, calculator) reports the assembly's levels: two
        // overlapping stays for one bed, unassignedStaysCost demoted to soft.
        // Sharing costs 6000 hard, so one stay goes without a bed and its 6
        // nights land on SOFT (not medium).
        BedPlan crowded = makeBase();
        addBed(crowded, addRoom(crowded));
        addStay(crowded, 0, 5);
        addStay(crowded, 0, 5);
        Solver::Config config;
        config.time_limit = std::chrono::milliseconds(300);
        config.seed = 1;
        DecomposedCalculator calculator;
        ConstraintAssembly{}
                .emplace<BedConflictCost>()
                .emplace<UnassignedStaysCost>()
                .soft("unassignedStaysCost")
                .into(calculator);
        Solver(config).solve(crowded, calculator);
        checkScore("solve() with unassignedStaysCost demoted", crowded.score, 0, 0, -6);
    }
    std::printf("\n");
}

// Incremental == full recompute with the medium term overridden, through the
// Editor protocol (change and swap moves over the whole value range).
void verifyMediumIncremental() {
    std::printf("== (i) Medium overrides: incremental == full ==\n");
    const std::vector<std::pair<std::string, std::function<ConstraintAssembly()>>> variants{
            {"default", [] { return defaultConstraints(); }},
            {"hard", [] { return defaultConstraints().hard("unassignedStaysCost"); }},
            {"soft", [] { return defaultConstraints().soft("unassignedStaysCost"); }},
            {"weight 3", [] { return defaultConstraints().weight("unassignedStaysCost", 3); }},
            {"with preferredMaxRoomCapacityCost on medium",
             [] {
                 return defaultConstraints().level("preferredMaxRoomCapacityCost",
                                                   optimaru::Level::Medium);
             }},
    };
    for (const auto& variant : variants) {
        BedPlan plan = buildRandomInstance(/*stayCount*/ 24, /*seed*/ 17);
        ScoreCalculator::prepare(plan);
        DecomposedCalculator calculator;
        variant.second().into(calculator);
        optimaru::Editor<BedPlan> editor(plan, calculator);
        std::mt19937_64 rng(41);
        const int stayCount = static_cast<int>(plan.stays.size());
        const int value_count = plan.bedCount() + 1;
        int mismatches = 0;
        int mediumSteps = 0;
        for (int step = 0; step < 1500; ++step) {
            if (rng() % 4 != 0) {
                editor.assign(static_cast<int>(rng() % stayCount),
                              static_cast<int>(rng() % value_count));
            } else {
                const int a = static_cast<int>(rng() % stayCount);
                const int b = static_cast<int>(rng() % stayCount);
                const int va = plan.stays[a].bed;
                const int vb = plan.stays[b].bed;
                editor.assign(a, vb);
                editor.assign(b, va);
            }
            const optimaru::Score incremental = editor.score();
            DecomposedCalculator fresh;
            variant.second().into(fresh);
            fresh.rebuild(plan);
            const optimaru::Score full = fresh.score();
            if (fresh.level_scores().medium != 0.0) ++mediumSteps;
            if (incremental.hard != full.hard || std::fabs(incremental.soft - full.soft) > 1e-6)
                ++mismatches;
        }
        check(mismatches == 0, "unassignedStaysCost " + variant.first +
                                       ": 1500 random moves, incremental == full recompute");
        if (variant.first != "hard" && variant.first != "soft") {
            check(mediumSteps > 0, variant.first + ": the walk visits medium states");
        }
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (j) Repeated components and offsets. A repeat is a fresh copy of a
// default component (another defaultConstraints(), the one name kept, its
// knobs re-stated) appended with add_instances(), as the Python bindings
// build it from a pick list.
// -------------------------------------------------------------------------
ConstraintAssembly copyOf(const std::string& name) {
    ConstraintAssembly copy = defaultConstraints();
    copy.only({name});
    return copy;
}

// Three beds (two rooms of capacity 2) and four 6-night stays preferring a
// single room: s0 in b0, s1 in b1, b2 empty, s2 and s3 on the "no bed"
// sentinel. Raw per unit (b0, b1, b2, sentinel): preferredMaxRoomCapacityCost
// [6, 6, 0, 0] (evaluated on b0..b2), unassignedStaysCost [0, 0, 0, 12]
// (evaluated on the sentinel), every other unit constraint 0,
// mixedGenderRoomCost 0.
BedPlan repeatInstance() {
    BedPlan plan = makeBase();
    const int r0 = addRoom(plan, GenderLimitation::ANY_GENDER, 2);
    const int r1 = addRoom(plan, GenderLimitation::ANY_GENDER, 2);
    addBed(plan, r0);
    addBed(plan, r0);
    addBed(plan, r1);
    for (int value : {0, 1, 3, 3}) {
        const int stay = addStay(plan, 0, 5, Gender::MALE, value);
        plan.stays[stay].patientPreferredMaximumRoomCapacity = 1;
    }
    ScoreCalculator::prepare(plan);
    return plan;
}

ConstraintAssembly repeatedAssembly() {
    ConstraintAssembly assembly = defaultConstraints();
    assembly.offset("preferredMaxRoomCapacityCost", 3.0);
    assembly.add_instances(std::move(copyOf("preferredMaxRoomCapacityCost")
                                             .weight("preferredMaxRoomCapacityCost", 0.25)
                                             .exponent("preferredMaxRoomCapacityCost", 2.0)
                                             .offset("preferredMaxRoomCapacityCost", 4.0)
                                             .clip("preferredMaxRoomCapacityCost", false)));
    // The literal bed-conflict copy sits on soft: hard refuses the literal
    // formula with an offset (hard 0 must mean no violation).
    assembly.add_instances(std::move(copyOf("bedConflictCost")
                                             .soft("bedConflictCost")
                                             .weight("bedConflictCost", 2.0)
                                             .offset("bedConflictCost", 1.0)
                                             .clip("bedConflictCost", false)));
    assembly.add_instances(std::move(copyOf("bedConflictCost")
                                             .weight("bedConflictCost", 2.0)
                                             .offset("bedConflictCost", 1.0)));
    assembly.add_instances(std::move(copyOf("unassignedStaysCost")
                                             .exponent("unassignedStaysCost", 2.0)
                                             .offset("unassignedStaysCost", 4.0)));
    assembly.add_instances(std::move(copyOf("unassignedStaysCost")
                                             .soft("unassignedStaysCost")
                                             .weight("unassignedStaysCost", 3.0)
                                             .offset("unassignedStaysCost", -2.0)
                                             .clip("unassignedStaysCost", false)));
    assembly.add_instances(std::move(copyOf("mixedGenderRoomCost")
                                             .soft("mixedGenderRoomCost")
                                             .weight("mixedGenderRoomCost", 1.0)
                                             .offset("mixedGenderRoomCost", 2.0)
                                             .clip("mixedGenderRoomCost", false)));
    assembly.offset("departmentSpecialtyCost", -1.0);
    return assembly;
}

void verifyRepeatsAndOffsets() {
    std::printf("== (j) Repeated components, offset / clip ==\n");
    {
        BedPlan plan = repeatInstance();
        ConstraintAssembly assembly = copyOf("preferredMaxRoomCapacityCost");
        assembly.add_instances(std::move(copyOf("preferredMaxRoomCapacityCost")
                                                 .weight("preferredMaxRoomCapacityCost", 0.5)
                                                 .exponent("preferredMaxRoomCapacityCost", 2.0)));
        const auto levels = levelsWith(std::move(assembly), plan);
        check(levels.hard == 0 && levels.medium == 0.0 && levels.soft == -(8.0 * 12 + 0.5 * 2 * 36),
              "8 x nights + 0.5 x nights^2 per bed: -132 soft");
        DecomposedCalculator calculator;
        ConstraintAssembly twice = copyOf("preferredMaxRoomCapacityCost");
        twice.add_instances(std::move(copyOf("preferredMaxRoomCapacityCost")
                                              .weight("preferredMaxRoomCapacityCost", 0.5)
                                              .exponent("preferredMaxRoomCapacityCost", 2.0)));
        std::move(twice).into(calculator);
        calculator.rebuild(plan);
        calculator.score();
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 2 && entries[0].unit_costs == entries[1].unit_costs &&
                      entries[0].unit_costs == std::vector<double>{6, 6, 0, 0} &&
                      entries[0].scoped && entries[0].units == std::vector<int>{0, 1, 2} &&
                      entries[1].weight == 0.5 && entries[1].exponent == 2.0 &&
                      entries[1].default_weight == 8.0,
              "two instances, the same raw costs, each with its own knobs");
    }
    {
        BedPlan plan = repeatInstance();
        // Threshold 2 per bed: 2 x (6 - 2) x 8.
        check(levelsWith(copyOf("preferredMaxRoomCapacityCost")
                                 .offset("preferredMaxRoomCapacityCost", 2.0),
                         plan)
                              .soft == -8.0 * 8,
              "threshold offset 2: 8 x 2 x 4");
        // Literal: the empty bed b2 (raw 0) is evaluated, so it is rewarded;
        // the sentinel is no bed and is not.
        check(levelsWith(copyOf("preferredMaxRoomCapacityCost")
                                 .offset("preferredMaxRoomCapacityCost", 2.0)
                                 .clip("preferredMaxRoomCapacityCost", false),
                         plan)
                              .soft == -8.0 * (8 - 2),
              "literal offset 2: 8 x (2 x 4 - 2), the empty bed included, not the sentinel");
        // A negative threshold offset adds to the non-zero beds only.
        check(levelsWith(copyOf("preferredMaxRoomCapacityCost")
                                 .offset("preferredMaxRoomCapacityCost", -1.0),
                         plan)
                              .soft == -8.0 * 14,
              "threshold offset -1: 8 x 2 x 7, the empty bed stays 0");
        // The literal bed-conflict pick on soft: (0 - 0.5) x 3 = -1.5 on each
        // of the three beds, the empty one included: +4.5 soft (not rounded).
        const auto conflict = levelsWith(copyOf("bedConflictCost")
                                                 .soft("bedConflictCost")
                                                 .weight("bedConflictCost", 3.0)
                                                 .offset("bedConflictCost", 0.5)
                                                 .clip("bedConflictCost", false),
                                         plan);
        check(conflict.hard == 0 && conflict.medium == 0.0 && conflict.soft == 4.5,
              "bed conflict literal offset 0.5 at weight 3 on soft: 3 beds x (-1.5) -> +4.5 soft");
        // The scoped medium constraint evaluates the sentinel only: 12 - 20.
        const auto scoped = levelsWith(copyOf("unassignedStaysCost")
                                               .offset("unassignedStaysCost", 20.0)
                                               .clip("unassignedStaysCost", false),
                                       plan);
        check(scoped.medium == 8.0 && scoped.hard == 0 && scoped.soft == 0.0,
              "unassigned literal offset 20: (12 - 20) on the sentinel only, +8 medium");
        DecomposedCalculator calculator;
        copyOf("unassignedStaysCost").into(calculator);
        calculator.rebuild(plan);
        calculator.score();
        const auto entries = calculator.constraint_costs();
        check(entries.size() == 1 && entries[0].scoped &&
                      entries[0].units == std::vector<int>{plan.noBedValue()},
              "unassignedStaysCost reports the sentinel as its one evaluated unit");
        // The global's total (0) is shaped once: (0 - 2) x 1000, on soft.
        const auto mixed = levelsWith(copyOf("mixedGenderRoomCost")
                                              .soft("mixedGenderRoomCost")
                                              .offset("mixedGenderRoomCost", 2.0)
                                              .clip("mixedGenderRoomCost", false),
                                      plan);
        check(mixed.hard == 0 && mixed.soft == 2000.0,
              "mixed-gender literal offset 2 on the single total, on soft: +2000 soft");
    }
    {
        // On hard (the default level of bedConflictCost and
        // mixedGenderRoomCost) the literal formula with an offset is refused
        // at reset, whichever sign and setter order: +0.5 would reward the
        // three beds (+6 hard, a feasible-looking score), -0.5 would charge
        // beds that hold no conflict.
        auto resetRefusal = [](ConstraintAssembly assembly) {
            BedPlan plan = repeatInstance();
            try {
                levelsWith(std::move(assembly), plan);
            } catch (const std::logic_error& error) {
                return std::string(error.what());
            }
            return std::string();
        };
        auto refusal = [](const std::string& name) {
            return "Constraint '" + name +
                   "' cannot use clip=false with a non-zero offset on a level that counts "
                   "toward the hard score (hard 0 must mean no violation)";
        };
        check(resetRefusal(copyOf("bedConflictCost")
                                   .offset("bedConflictCost", 0.5)
                                   .clip("bedConflictCost", false)) == refusal("bedConflictCost") &&
                      resetRefusal(copyOf("bedConflictCost")
                                           .clip("bedConflictCost", false)
                                           .offset("bedConflictCost", -0.5)) ==
                              refusal("bedConflictCost"),
              "bed conflict literal offset +-0.5 on hard: refused at reset");
        check(resetRefusal(copyOf("mixedGenderRoomCost")
                                   .offset("mixedGenderRoomCost", 2.0)
                                   .clip("mixedGenderRoomCost", false)) ==
                      refusal("mixedGenderRoomCost"),
              "mixed-gender literal offset on its default hard level: refused at reset");
        check(resetRefusal(copyOf("preferredMaxRoomCapacityCost")
                                   .offset("preferredMaxRoomCapacityCost", 2.0)
                                   .clip("preferredMaxRoomCapacityCost", false)
                                   .hard("preferredMaxRoomCapacityCost")) ==
                              refusal("preferredMaxRoomCapacityCost") &&
                      resetRefusal(copyOf("unassignedStaysCost")
                                           .hard("unassignedStaysCost")
                                           .offset("unassignedStaysCost", 20.0)
                                           .clip("unassignedStaysCost", false)) ==
                              refusal("unassignedStaysCost"),
              "a soft / medium literal pick moved to hard (before or after): refused at reset");
        check(resetRefusal(copyOf("bedConflictCost").offset("bedConflictCost", 0.5)).empty() &&
                      resetRefusal(copyOf("bedConflictCost").clip("bedConflictCost", false))
                              .empty() &&
                      resetRefusal(copyOf("unassignedStaysCost")
                                           .offset("unassignedStaysCost", 20.0)
                                           .clip("unassignedStaysCost", false))
                              .empty(),
              "allowed: a hard threshold offset, hard clip=false with offset 0, and the "
              "literal formula on the into_soft medium");
    }
    {
        // Random walk: incremental == a fresh calculator of the same assembly.
        BedPlan plan = buildRandomInstance(/*stayCount*/ 30, /*seed*/ 7);
        ScoreCalculator::prepare(plan);
        DecomposedCalculator calculator;
        repeatedAssembly().into(calculator);
        optimaru::Editor<BedPlan> editor(plan, calculator);
        std::mt19937_64 rng(321);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const int stayCount = static_cast<int>(plan.stays.size());
        const int value_count = plan.bedCount() + 1;
        int mismatches = 0;
        int mediumSteps = 0;
        double drift = 0.0;
        const int steps = 3000;
        for (int step = 0; step < steps; ++step) {
            if (unit(rng) < 0.75) {
                const int stay = static_cast<int>(rng() % stayCount);
                const int value = unit(rng) < 0.05 ? -1 : static_cast<int>(rng() % value_count);
                editor.assign(stay, value);
            } else {
                const int a = static_cast<int>(rng() % stayCount);
                const int b = static_cast<int>(rng() % stayCount);
                const int va = plan.stays[a].bed;
                const int vb = plan.stays[b].bed;
                editor.assign(a, vb);
                editor.assign(b, va);
            }
            BedPlan shadow = plan;
            DecomposedCalculator fresh;
            repeatedAssembly().into(fresh);
            fresh.rebuild(shadow);
            const optimaru::Score full = fresh.score();
            if (fresh.level_scores().medium != 0.0) ++mediumSteps;
            const optimaru::Score incremental = editor.score();
            drift = std::max(drift, std::fabs(full.soft - incremental.soft));
            if (!optimaru::scores_agree(incremental, full)) ++mismatches;
        }
        std::printf("    largest soft drift %.3g\n", drift);
        check(mismatches == 0,
              std::to_string(steps) +
                      " random moves with repeated, shifted components: incremental == fresh");
        check(mediumSteps > 0, "the walk visits medium states");
    }
    {
        BedPlan plan = buildRandomInstance(/*stayCount*/ 20, /*seed*/ 11);
        Solver::Config config;
        config.time_limit = std::chrono::seconds(2);
        config.seed = 5;
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
        DecomposedCalculator fresh;
        repeatedAssembly().into(fresh);
        check(!corrupted && plan.score == scoreWith(fresh, plan),
              "the reported score == a fresh recompute of the same assembly");
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
    verifyRawCostsByHand();
    verifyCatalog();
    verifyMediumLevel();
    verifyMediumIncremental();
    verifyRepeatsAndOffsets();

    if (failures == 0) {
        std::printf("All bed-allocation plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d bed-allocation plugin check(s) FAILED.\n", failures);
    return 1;
}
