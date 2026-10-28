// Bed-allocation unit tests (engine test/ style: check() + non-zero exit on any
// failure):
//   (a) every constraint verified in isolation against hand-computed scores
//       (the Java BedAllocationConstraintProviderTest cases, scaled by the
//       constraint weights)
//   (b) incremental score == full recompute across a long random walk driven
//       through the engine's ScoreDirector protocol (before/set/after)
//   (c) a small instance solved under EnvironmentMode::FULL_ASSERT
//   (d) a plain solve reaches hard 0 and initializes every stay
//   (e) a parallelSolverCount > 1 solve returns a consistently scored winner
//   (f) stays with no night in house occupy nothing in the per-night
//       occupancy counters
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "ba/domain.hpp"
#include "ba/incremental_calculator.hpp"
#include "ba/score_calculator.hpp"
#include "ba/solver.hpp"
#include "optimaru/api/score/director/score_director.hpp"

using namespace ba;

namespace
{

int failures = 0;

void check(bool ok, const std::string &label)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok)
        ++failures;
}

void checkScore(const std::string &label,
                const Score &actual,
                long expectedHard,
                long expectedMedium,
                long expectedSoft)
{
    const bool ok
        = (actual.hard == expectedHard && actual.medium == expectedMedium
           && actual.soft == expectedSoft);
    std::printf("  [%s] %-44s got=%s expected=%ldhard/%ldmedium/%ldsoft\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                expectedHard,
                expectedMedium,
                expectedSoft);
    if (!ok)
        ++failures;
}

// Base problem: one department offering the "default" specialty at priority
// 1, so the specialty constraints stay silent unless a test opts in.
BedPlan makeBase()
{
    BedPlan plan;
    Department department;
    department.id = "d0";
    department.name = "Dept";
    department.specialtyToPriority = {{"default", 1}};
    plan.departments.push_back(department);
    return plan;
}

int addRoom(BedPlan &plan,
            GenderLimitation genderLimitation = GenderLimitation::ANY_GENDER,
            int capacity = 1,
            std::vector<std::string> equipment = {},
            int department = 0)
{
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

int addBed(BedPlan &plan, int roomIndex)
{
    Bed bed;
    bed.id = "b" + std::to_string(plan.beds.size());
    bed.room = roomIndex;
    bed.indexInRoom = 0;
    plan.beds.push_back(bed);
    return static_cast<int>(plan.beds.size()) - 1;
}

// Days 0..5 == the Java tests' ZERO_NIGHT..FIVE_NIGHT (nightCount 6).
int addStay(BedPlan &plan,
            int arrivalDay,
            int departureDay,
            Gender gender = Gender::MALE,
            int bedValue = -1,
            const std::string &specialty = "default")
{
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

// -------------------------------------------------------------------------
// (a) Each constraint in isolation, full recompute vs hand-computed scores.
// The Java ConstraintProvider tests all use a 6-night stay (penalizesBy 6);
// here the expectations are multiplied by the constraint weights.
// -------------------------------------------------------------------------
void verifyConstraints()
{
    std::printf("== (a) Verifying each constraint in isolation ==\n");

    // hard 2: femaleInMaleRoom — 50 * 6 nights.
    {
        BedPlan plan = makeBase();
        const int bed
            = addBed(plan, addRoom(plan, GenderLimitation::MALE_ONLY));
        addStay(plan, 0, 5, Gender::FEMALE, bed);
        checkScore("femaleInMaleRoom",
                   ScoreCalculator::calculateScore(plan),
                   -300,
                   0,
                   0);
    }
    // hard 3: maleInFemaleRoom — 50 * 6 nights.
    {
        BedPlan plan = makeBase();
        const int bed
            = addBed(plan, addRoom(plan, GenderLimitation::FEMALE_ONLY));
        addStay(plan, 0, 5, Gender::MALE, bed);
        checkScore("maleInFemaleRoom",
                   ScoreCalculator::calculateScore(plan),
                   -300,
                   0,
                   0);
    }
    // The matching gender costs nothing.
    {
        BedPlan plan = makeBase();
        const int bed
            = addBed(plan, addRoom(plan, GenderLimitation::FEMALE_ONLY));
        addStay(plan, 0, 5, Gender::FEMALE, bed);
        checkScore("female in FEMALE_ONLY ok",
                   ScoreCalculator::calculateScore(plan),
                   0,
                   0,
                   0);
    }
    // hard 1: sameBedInSameNight — full overlap: 1000 * 6 nights.
    {
        BedPlan plan = makeBase();
        const int bed = addBed(plan, addRoom(plan));
        addStay(plan, 0, 5, Gender::MALE, bed);
        addStay(plan, 0, 5, Gender::MALE, bed);
        checkScore("sameBedInSameNight full overlap",
                   ScoreCalculator::calculateScore(plan),
                   -6000,
                   0,
                   0);
    }
    // hard 1: partial overlap [0,5] x [3,9] -> 3 shared nights.
    {
        BedPlan plan = makeBase();
        const int bed = addBed(plan, addRoom(plan));
        addStay(plan, 0, 5, Gender::MALE, bed);
        addStay(plan, 3, 9, Gender::MALE, bed);
        checkScore("sameBedInSameNight partial overlap",
                   ScoreCalculator::calculateScore(plan),
                   -3000,
                   0,
                   0);
    }
    // hard 1: disjoint stays in one bed cost nothing.
    {
        BedPlan plan = makeBase();
        const int bed = addBed(plan, addRoom(plan));
        addStay(plan, 0, 2, Gender::MALE, bed);
        addStay(plan, 4, 6, Gender::MALE, bed);
        checkScore("sameBedInSameNight disjoint",
                   ScoreCalculator::calculateScore(plan),
                   0,
                   0,
                   0);
    }
    // hard 4: differentGenderInSameGenderRoomInSameNight — 1000 * 6.
    {
        BedPlan plan = makeBase();
        const int room
            = addRoom(plan, GenderLimitation::SAME_GENDER, /*capacity*/ 2);
        addStay(plan, 0, 5, Gender::FEMALE, addBed(plan, room));
        addStay(plan, 0, 5, Gender::MALE, addBed(plan, room));
        checkScore("differentGenderInSameGenderRoom",
                   ScoreCalculator::calculateScore(plan),
                   -6000,
                   0,
                   0);
    }
    // hard 4: same gender in a SAME_GENDER room costs nothing.
    {
        BedPlan plan = makeBase();
        const int room
            = addRoom(plan, GenderLimitation::SAME_GENDER, /*capacity*/ 2);
        addStay(plan, 0, 5, Gender::MALE, addBed(plan, room));
        addStay(plan, 0, 5, Gender::MALE, addBed(plan, room));
        checkScore("same gender in SAME_GENDER room ok",
                   ScoreCalculator::calculateScore(plan),
                   0,
                   0,
                   0);
    }
    // hard 4: mixed genders in an ANY_GENDER room cost nothing.
    {
        BedPlan plan = makeBase();
        const int room
            = addRoom(plan, GenderLimitation::ANY_GENDER, /*capacity*/ 2);
        addStay(plan, 0, 5, Gender::FEMALE, addBed(plan, room));
        addStay(plan, 0, 5, Gender::MALE, addBed(plan, room));
        checkScore("mixed genders in ANY_GENDER room ok",
                   ScoreCalculator::calculateScore(plan),
                   0,
                   0,
                   0);
    }
    // hard 5: departmentMinimumAge — 100 * 6 nights.
    {
        BedPlan plan = makeBase();
        plan.departments[0].minimumAge = 18;
        const int bed = addBed(plan, addRoom(plan));
        const int stay = addStay(plan, 0, 5, Gender::MALE, bed);
        plan.stays[stay].patientAge = 5;
        checkScore("departmentMinimumAge",
                   ScoreCalculator::calculateScore(plan),
                   -600,
                   0,
                   0);
    }
    // hard 6: departmentMaximumAge — 100 * 6 nights.
    {
        BedPlan plan = makeBase();
        plan.departments[0].maximumAge = 18;
        const int bed = addBed(plan, addRoom(plan));
        const int stay = addStay(plan, 0, 5, Gender::MALE, bed);
        plan.stays[stay].patientAge = 42;
        checkScore("departmentMaximumAge",
                   ScoreCalculator::calculateScore(plan),
                   -600,
                   0,
                   0);
    }
    // hard 7: requiredPatientEquipment — the room misses "television" but
    // has "telemetry": 50 * 6 nights * 1 present (the quickstart's
    // test-pinned semantics: weigh by the PRESENT required equipment).
    {
        BedPlan plan = makeBase();
        const int bed = addBed(
            plan,
            addRoom(plan, GenderLimitation::ANY_GENDER, 1, {"telemetry"}));
        const int stay = addStay(plan, 0, 5, Gender::MALE, bed);
        plan.stays[stay].patientRequiredEquipment = {"television", "telemetry"};
        checkScore("requiredPatientEquipment partial",
                   ScoreCalculator::calculateScore(plan),
                   -300,
                   0,
                   0);
    }
    // hard 7: a room with NONE of the required equipment costs 0 (the Java
    // match weigher counts the present ones, which is 0 here).
    {
        BedPlan plan = makeBase();
        const int bed = addBed(plan, addRoom(plan));
        const int stay = addStay(plan, 0, 5, Gender::MALE, bed);
        plan.stays[stay].patientRequiredEquipment = {"television"};
        checkScore("requiredPatientEquipment none present",
                   ScoreCalculator::calculateScore(plan),
                   0,
                   0,
                   0);
    }
    // hard 7: a room with ALL the required equipment costs 0 (filter off).
    {
        BedPlan plan = makeBase();
        const int bed = addBed(plan,
                               addRoom(plan,
                                       GenderLimitation::ANY_GENDER,
                                       1,
                                       {"telemetry", "television"}));
        const int stay = addStay(plan, 0, 5, Gender::MALE, bed);
        plan.stays[stay].patientRequiredEquipment = {"television"};
        checkScore("requiredPatientEquipment all present",
                   ScoreCalculator::calculateScore(plan),
                   0,
                   0,
                   0);
    }
    // medium 8: assignEveryPatientToABed — the "no bed" sentinel.
    {
        BedPlan plan = makeBase();
        addBed(plan, addRoom(plan));
        addStay(plan, 0, 5, Gender::MALE, /*bed*/ 1);  // noBedValue() == 1
        checkScore("assignEveryPatientToABed (no bed)",
                   ScoreCalculator::calculateScore(plan),
                   0,
                   -6,
                   0);
    }
    // Uninitialized stays (bed == -1) contribute nothing.
    {
        BedPlan plan = makeBase();
        addBed(plan, addRoom(plan));
        addStay(plan, 0, 5, Gender::MALE, -1);
        checkScore("uninitialized contributes nothing",
                   ScoreCalculator::calculateScore(plan),
                   0,
                   0,
                   0);
    }
    // soft 9: preferredMaximumRoomCapacity — 8 * 6 nights.
    {
        BedPlan plan = makeBase();
        const int bed = addBed(
            plan, addRoom(plan, GenderLimitation::ANY_GENDER, /*capacity*/ 6));
        const int stay = addStay(plan, 0, 5, Gender::MALE, bed);
        plan.stays[stay].patientPreferredMaximumRoomCapacity = 3;
        checkScore("preferredMaximumRoomCapacity",
                   ScoreCalculator::calculateScore(plan),
                   0,
                   0,
                   -48);
    }
    // soft 9: a room within the preferred capacity costs nothing.
    {
        BedPlan plan = makeBase();
        const int bed = addBed(
            plan, addRoom(plan, GenderLimitation::ANY_GENDER, /*capacity*/ 3));
        const int stay = addStay(plan, 0, 5, Gender::MALE, bed);
        plan.stays[stay].patientPreferredMaximumRoomCapacity = 3;
        checkScore("preferredMaximumRoomCapacity within",
                   ScoreCalculator::calculateScore(plan),
                   0,
                   0,
                   0);
    }
    // soft 10: departmentSpecialty — 10 * 6 nights for the stay whose
    // specialty the department does not offer (the Java test's two-stay
    // setup: spec1 offered, spec2 not).
    {
        BedPlan plan = makeBase();
        plan.departments[0].specialtyToPriority = {{"spec1", 1}};
        const int bed
            = addBed(plan, addRoom(plan, GenderLimitation::ANY_GENDER, 2));
        addStay(plan, 0, 5, Gender::MALE, bed, "spec1");
        const int bed2 = addBed(plan, plan.beds[bed].room);
        addStay(plan, 0, 5, Gender::MALE, bed2, "spec2");
        checkScore("departmentSpecialty",
                   ScoreCalculator::calculateScore(plan),
                   0,
                   0,
                   -60);
    }
    // soft 11: departmentSpecialtyNotFirstPriority — 10 * (2 - 1) * 6 for
    // the priority-2 specialty; the priority-1 one costs nothing.
    {
        BedPlan plan = makeBase();
        plan.departments[0].specialtyToPriority = {{"spec1", 2}, {"spec2", 1}};
        const int bed
            = addBed(plan, addRoom(plan, GenderLimitation::ANY_GENDER, 2));
        addStay(plan, 0, 5, Gender::MALE, bed, "spec1");
        const int bed2 = addBed(plan, plan.beds[bed].room);
        addStay(plan, 0, 5, Gender::MALE, bed2, "spec2");
        checkScore("departmentSpecialtyNotFirstPriority",
                   ScoreCalculator::calculateScore(plan),
                   0,
                   0,
                   -60);
    }
    // soft 12: preferredPatientEquipment — 50 * 6 nights * 1 missing.
    {
        BedPlan plan = makeBase();
        const int bed = addBed(
            plan,
            addRoom(plan, GenderLimitation::ANY_GENDER, 1, {"telemetry"}));
        const int stay = addStay(plan, 0, 5, Gender::MALE, bed);
        plan.stays[stay].patientPreferredEquipment
            = {"television", "telemetry"};
        checkScore("preferredPatientEquipment",
                   ScoreCalculator::calculateScore(plan),
                   0,
                   0,
                   -300);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// Random mid-size instance for the incremental cross-check and the solves.
// -------------------------------------------------------------------------
BedPlan buildRandomInstance(int stayCount, std::uint64_t seed)
{
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

    const std::vector<std::string> equipmentPool
        = {"telemetry", "television", "oxygen", "nitrogen"};
    const GenderLimitation limitations[] = {GenderLimitation::ANY_GENDER,
                                            GenderLimitation::MALE_ONLY,
                                            GenderLimitation::FEMALE_ONLY,
                                            GenderLimitation::SAME_GENDER,
                                            GenderLimitation::SAME_GENDER,
                                            GenderLimitation::SAME_GENDER};
    for (int r = 0; r < 6; ++r)
    {
        std::vector<std::string> roomEquipment;
        for (const std::string &equipment : equipmentPool)
        {
            if (rng() % 2 == 0)
                roomEquipment.push_back(equipment);
        }
        const int capacity = 1 + static_cast<int>(rng() % 3);
        const int room = addRoom(plan,
                                 limitations[r],
                                 capacity,
                                 roomEquipment,
                                 static_cast<int>(rng() % 2));
        for (int i = 0; i < capacity; ++i)
            addBed(plan, room);
    }

    const std::vector<std::string> specialties = {"spec1", "spec2", "spec3"};
    for (int s = 0; s < stayCount; ++s)
    {
        const int arrival = static_cast<int>(rng() % 18);
        const int departure = arrival + static_cast<int>(rng() % 5);
        const int stay = addStay(plan,
                                 arrival,
                                 departure,
                                 rng() % 2 == 0 ? Gender::MALE : Gender::FEMALE,
                                 -1,
                                 specialties[rng() % specialties.size()]);
        plan.stays[stay].patientAge = 1 + static_cast<int>(rng() % 99);
        if (rng() % 2 == 0)
        {
            plan.stays[stay].patientPreferredMaximumRoomCapacity
                = 1 + static_cast<int>(rng() % 3);
        }
        for (const std::string &equipment : equipmentPool)
        {
            if (rng() % 4 == 0)
                plan.stays[stay].patientRequiredEquipment.push_back(equipment);
            if (rng() % 3 == 0)
                plan.stays[stay].patientPreferredEquipment.push_back(equipment);
        }
    }
    return plan;
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of change and
// swap moves (including unassignments to -1 and to the "no bed" sentinel),
// driven through the ScoreDirector so the before/set/after hook contract is
// exercised exactly like in the engine.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Verifying incremental scoring == full recompute ==\n");
    BedPlan plan = buildRandomInstance(/*stayCount*/ 30, /*seed*/ 7);

    BedAllocationIncrementalScoreCalculator calc;
    optimaru::ScoreDirector<BedPlan> director(plan, calc);
    BedPlan shadow = plan;

    std::mt19937_64 rng(123);
    const int stayCount = static_cast<int>(plan.stays.size());
    const int valueCount = plan.bedCount() + 1;  // beds + "no bed" sentinel
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            // ChangeMove; 10% of the time back to uninitialized (-1), like
            // the construction heuristic's starting state. The random value
            // range includes the sentinel.
            const int stay = static_cast<int>(rng() % stayCount);
            const int value = (unit(rng) < 0.10)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(stay, value);
        }
        else
        {
            // SwapMove.
            const int a = static_cast<int>(rng() % stayCount);
            const int b = static_cast<int>(rng() % stayCount);
            const int va = plan.stays[a].bed;
            const int vb = plan.stays[b].bed;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        // Full recompute on the same assignment.
        for (int i = 0; i < stayCount; ++i)
            shadow.stays[i].bed = plan.stays[i].bed;
        const optimaru::HardSoftScore full
            = ScoreCalculator::calculateEngineScore(shadow);
        const optimaru::HardSoftScore incremental = director.calculateScore();
        if (full.hard != incremental.hard
            || std::fabs(full.soft - incremental.soft) > 1e-9)
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @step %d: incremental=%s full=%s\n",
                            step,
                            incremental.toString().c_str(),
                            full.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(steps)
              + " random moves, incremental == full recompute");

    // The three-level round trip through the engine encoding is exact.
    const Score decoded
        = fromEngineScore(ScoreCalculator::calculateEngineScore(plan));
    ScoreCalculator::calculateScore(plan);
    check(decoded == plan.score, "fromEngineScore(toEngineScore(s)) == s");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) FULL_ASSERT solve: the engine cross-checks the incremental score
// against a full reset + recompute after every single move and throws
// ScoreCorruptionError on any divergence.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (c) FULL_ASSERT solve ==\n");
    BedPlan plan = buildRandomInstance(/*stayCount*/ 20, /*seed*/ 11);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    // Kick early so the LNS ruin/recreate path also runs under FULL_ASSERT.
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    bool corrupted = false;
    Score finalScore{};
    try
    {
        Solver solver(config);
        solver.solve(plan);
        finalScore = plan.score;
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(plan);
        check(finalScore == recomputed,
              "solver-reported score == full recompute");
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) A plain solve reaches hard 0 (the "no bed" sentinel guarantees a
// feasible assignment exists) and leaves no stay uninitialized.
// -------------------------------------------------------------------------
void verifyFeasibleSolve()
{
    std::printf("== (d) Plain solve reaches hard 0 ==\n");
    BedPlan plan = buildRandomInstance(/*stayCount*/ 24, /*seed*/ 21);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 5;
    Solver solver(config);
    solver.solve(plan);

    check(plan.score.hard == 0, "small instance reaches 0hard");
    bool allInitialized = true;
    for (const Stay &stay : plan.stays)
    {
        if (stay.bed < 0 || stay.bed > plan.bedCount())
            allInitialized = false;
    }
    check(allInitialized, "every stay holds a bed or the \"no bed\" sentinel");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) parallelSolverCount > 1: every worker solves its own copy with its own
// calculator and nearby tables, and the winner's reported score still equals
// a full recompute on the returned solution.
// -------------------------------------------------------------------------
void verifyParallelSolve()
{
    std::printf("== (e) Parallel multi-start solve ==\n");
    BedPlan plan = buildRandomInstance(/*stayCount*/ 24, /*seed*/ 31);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(1);
    config.randomSeed = 5;
    config.parallelSolverCount = 4;
    Solver solver(config);
    solver.solve(plan);

    BedPlan recomputed = plan;
    check(ScoreCalculator::calculateScore(recomputed) == plan.score,
          "4 workers: reported score == full recompute");
    check(plan.score.hard == 0, "4 workers: reaches 0hard");
    bool allInitialized = true;
    for (const Stay &stay : plan.stays)
    {
        if (stay.bed < 0 || stay.bed > plan.bedCount())
            allInitialized = false;
    }
    check(allInitialized, "4 workers: every stay initialized");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) Degenerate stays (departureDay < arrivalDay, i.e. no night in house)
// occupy nothing: the per-night occupancy counters must skip them, exactly
// like Stay::sameNightCount clamps their overlap to 0.
// -------------------------------------------------------------------------
void verifyEmptyNightStays()
{
    std::printf("== (f) Stays without a night ==\n");
    BedPlan plan = makeBase();
    const int room = addRoom(plan, GenderLimitation::SAME_GENDER, 2);
    const int bed = addBed(plan, room);
    addBed(plan, room);
    // Two zero-night stays sharing the bed and one normal stay over them.
    addStay(plan, 5, 4, Gender::MALE, bed);
    addStay(plan, 5, 4, Gender::FEMALE, bed);
    addStay(plan, 0, 9, Gender::MALE, bed);

    BedAllocationIncrementalScoreCalculator calc;
    optimaru::ScoreDirector<BedPlan> director(plan, calc);
    BedPlan shadow = plan;
    check(ScoreCalculator::calculateEngineScore(shadow).hard
              == director.calculateScore().hard,
          "zero-night stays: incremental == full recompute");

    // Moving one of them through the director must stay consistent.
    director.changeVariable(0, plan.noBedValue());
    director.changeVariable(1, bed + 1);
    for (std::size_t i = 0; i < plan.stays.size(); ++i)
        shadow.stays[i].bed = plan.stays[i].bed;
    const optimaru::HardSoftScore full
        = ScoreCalculator::calculateEngineScore(shadow);
    const optimaru::HardSoftScore incremental = director.calculateScore();
    check(full.hard == incremental.hard
              && std::fabs(full.soft - incremental.soft) < 1e-9,
          "zero-night stays: incremental == full after moves");
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyConstraints();
    verifyIncremental();
    verifyFullAssertSolve();
    verifyFeasibleSolve();
    verifyParallelSolve();
    verifyEmptyNightStays();

    if (failures == 0)
    {
        std::printf("All bed-allocation checks PASSED.\n");
        return 0;
    }
    std::printf("%d bed-allocation check(s) FAILED.\n", failures);
    return 1;
}
