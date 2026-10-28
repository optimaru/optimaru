// Plugin-assembly (ba/constraints.hpp) tests, engine test/ style: check() + a
// non-zero exit on any failure.
//   (a) the default assembly on the engine's DecomposedScoreCalculator
//       reproduces the monolithic score bit-for-bit on 500 random
//       assignments (uninitialized stays and the "no bed" sentinel included)
//   (b) incremental score == full recompute across 3000 random moves driven
//       through the engine's ScoreDirector protocol (plugin path)
//   (c) partial assemblies: removing one component removes exactly that term
//   (d) weight / isHard overrides
//   (e) constraintTotals() explains exactly the reported score
//   (f) a 2-second solve(plan, calculator) under FULL_ASSERT
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "ba/constraints.hpp"
#include "ba/domain.hpp"
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
                std::int64_t expectedHard,
                std::int64_t expectedMedium,
                std::int64_t expectedSoft)
{
    const bool ok = actual.hard == expectedHard
                    && actual.medium == expectedMedium
                    && actual.soft == expectedSoft;
    std::printf("  [%s] %-52s got=%s expected=%lldhard/%lldmedium/%lldsoft\n",
                ok ? "PASS" : "FAIL",
                label.c_str(),
                actual.toString().c_str(),
                static_cast<long long>(expectedHard),
                static_cast<long long>(expectedMedium),
                static_cast<long long>(expectedSoft));
    if (!ok)
        ++failures;
}

// --- instance helpers (the ba_test shapes) ---

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

// Random mid-size instance (the ba_test shape): two departments with age
// limits, six rooms covering every gender limitation, mixed equipment.
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

// Scores `plan`'s current assignment with a freshly reset assembly, decoded
// back into the domain's HardMediumSoft levels.
Score scoreWith(DecomposedCalculator &calculator, BedPlan &plan)
{
    calculator.resetWorkingSolution(plan);
    return fromEngineScore(calculator.calculateScore());
}

// -------------------------------------------------------------------------
// (a) The default assembly == the monolithic full recompute, on random
// assignments over the whole value range (real beds, the "no bed" sentinel
// and uninitialized).
// -------------------------------------------------------------------------
void verifyAssemblyParity()
{
    std::printf("== (a) Default assembly == monolithic score ==\n");
    BedPlan plan = buildRandomInstance(/*stayCount*/ 30, /*seed*/ 7);
    ScoreCalculator::prepare(plan);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    int mismatches = 0;
    const int rounds = 500;
    for (int round = 0; round < rounds; ++round)
    {
        for (Stay &stay : plan.stays)
        {
            const double roll = unit(rng);
            if (roll < 0.05)
                stay.bed = -1;  // uninitialized
            else if (roll < 0.15)
                stay.bed = plan.noBedValue();
            else
                stay.bed = static_cast<int>(rng() % plan.bedCount());
        }
        const Score plugin = scoreWith(calculator, plan);
        const Score monolithic = ScoreCalculator::calculateScore(plan);
        if (!(plugin == monolithic))
        {
            if (++mismatches <= 3)
            {
                std::printf("  MISMATCH @round %d: plugin=%s monolithic=%s\n",
                            round,
                            plugin.toString().c_str(),
                            monolithic.toString().c_str());
            }
        }
    }
    check(mismatches == 0,
          std::to_string(rounds) + " random assignments score identically");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (b) Incremental score == full recompute across a random walk of change and
// swap moves, driven through the ScoreDirector so the plugin path's
// before/set/after hook contract is exercised exactly like in the engine.
// -------------------------------------------------------------------------
void verifyIncremental()
{
    std::printf("== (b) Plugin incremental == full recompute ==\n");
    BedPlan plan = buildRandomInstance(/*stayCount*/ 30, /*seed*/ 7);
    ScoreCalculator::prepare(plan);

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    optimaru::ScoreDirector<BedPlan> director(plan, calculator);
    BedPlan shadow = plan;

    std::mt19937_64 rng(123);
    const int stayCount = static_cast<int>(plan.stays.size());
    const int valueCount = plan.bedCount() + 1;  // + the "no bed" sentinel
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    int mismatches = 0;
    const int steps = 3000;
    for (int step = 0; step < steps; ++step)
    {
        if (unit(rng) < 0.75)
        {
            const int stayIndex = static_cast<int>(rng() % stayCount);
            const int value = (unit(rng) < 0.05)
                                  ? -1
                                  : static_cast<int>(rng() % valueCount);
            director.changeVariable(stayIndex, value);
        }
        else
        {
            const int a = static_cast<int>(rng() % stayCount);
            const int b = static_cast<int>(rng() % stayCount);
            const int va = plan.stays[a].bed;
            const int vb = plan.stays[b].bed;
            director.changeVariable(a, vb);
            director.changeVariable(b, va);
        }
        for (int i = 0; i < stayCount; ++i)
            shadow.stays[i].bed = plan.stays[i].bed;
        const Score full = ScoreCalculator::calculateScore(shadow);
        const Score incremental = fromEngineScore(director.calculateScore());
        if (!(full == incremental))
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
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (c) Partial assemblies: each component in isolation on the hand-computed
// instances of the Java ConstraintProvider tests (6-night stays).
// -------------------------------------------------------------------------
void verifyPartialAssemblies()
{
    std::printf("== (c) Single-component assemblies ==\n");

    {  // femaleInMaleRoom alone: 50 * 6 nights, nothing else.
        BedPlan plan = makeBase();
        const int bed
            = addBed(plan, addRoom(plan, GenderLimitation::MALE_ONLY));
        addStay(plan, 0, 5, Gender::FEMALE, bed);
        ScoreCalculator::prepare(plan);
        DecomposedCalculator calculator;
        calculator.emplace<FemaleInMaleRoom>();
        checkScore(
            "femaleInMaleRoom alone", scoreWith(calculator, plan), -300, 0, 0);
    }
    {  // sameBedInSameNight alone: two full-overlap stays, 1000 * 6.
        BedPlan plan = makeBase();
        const int bed = addBed(plan, addRoom(plan));
        addStay(plan, 0, 5, Gender::MALE, bed);
        addStay(plan, 0, 5, Gender::MALE, bed);
        ScoreCalculator::prepare(plan);
        DecomposedCalculator calculator;
        calculator.emplace<SameBedInSameNight>();
        checkScore("sameBedInSameNight alone",
                   scoreWith(calculator, plan),
                   -6000,
                   0,
                   0);
    }
    {  // differentGenderInSameGenderRoomInSameNight alone: the pair spans two
        // beds of one SAME_GENDER room, which is why it is a global.
        BedPlan plan = makeBase();
        const int room
            = addRoom(plan, GenderLimitation::SAME_GENDER, /*capacity*/ 2);
        addStay(plan, 0, 5, Gender::FEMALE, addBed(plan, room));
        addStay(plan, 0, 5, Gender::MALE, addBed(plan, room));
        ScoreCalculator::prepare(plan);
        DecomposedCalculator calculator;
        calculator.emplace<DifferentGenderInSameGenderRoomInSameNight>();
        checkScore("differentGenderInSameGenderRoom alone",
                   scoreWith(calculator, plan),
                   -6000,
                   0,
                   0);
    }
    {  // assignEveryPatientToABed alone: the sentinel costs 6 medium.
        BedPlan plan = makeBase();
        addBed(plan, addRoom(plan));
        addStay(plan, 0, 5, Gender::MALE, plan.noBedValue());
        ScoreCalculator::prepare(plan);
        DecomposedCalculator calculator;
        calculator.emplace<AssignEveryPatientToABed>();
        checkScore("assignEveryPatientToABed alone",
                   scoreWith(calculator, plan),
                   0,
                   -6,
                   0);
    }
    {  // The same stay in a real bed costs nothing on that component.
        BedPlan plan = makeBase();
        const int bed = addBed(plan, addRoom(plan));
        addStay(plan, 0, 5, Gender::MALE, bed);
        ScoreCalculator::prepare(plan);
        DecomposedCalculator calculator;
        calculator.emplace<AssignEveryPatientToABed>();
        checkScore("assignEveryPatientToABed silent in a real bed",
                   scoreWith(calculator, plan),
                   0,
                   0,
                   0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (d) Metadata overrides: the level and the weight are assembly-time
// metadata, not part of the cost.
// -------------------------------------------------------------------------
void verifyOverrides()
{
    std::printf("== (d) Weight / level overrides ==\n");
    BedPlan plan = makeBase();
    const int bed = addBed(plan, addRoom(plan, GenderLimitation::MALE_ONLY));
    addStay(plan, 0, 5, Gender::FEMALE, bed);
    ScoreCalculator::prepare(plan);

    {
        DecomposedCalculator calculator;
        calculator.emplace<FemaleInMaleRoom>(10.0);
        checkScore("femaleInMaleRoom re-weighted to 10",
                   scoreWith(calculator, plan),
                   -60,
                   0,
                   0);
    }
    {
        // Demoted to soft: the hard level empties out, the soft level takes
        // the same 300 penalty units.
        DecomposedCalculator calculator;
        auto component = std::make_unique<FemaleInMaleRoom>();
        component->isHard = false;
        calculator.add(std::move(component));
        checkScore("femaleInMaleRoom demoted to soft",
                   scoreWith(calculator, plan),
                   0,
                   0,
                   -300);
    }
    {
        // exponent 2 shapes the raw cost (6 nights) before the weight:
        // 50 * 36 = 1800.
        DecomposedCalculator calculator;
        auto component = std::make_unique<FemaleInMaleRoom>();
        component->exponent = 2.0;
        calculator.add(std::move(component));
        checkScore("femaleInMaleRoom with exponent 2",
                   scoreWith(calculator, plan),
                   -1800,
                   0,
                   0);
    }
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (e) constraintTotals(): the per-constraint breakdown adds up to the score
// the same assembly reports.
// -------------------------------------------------------------------------
void verifyConstraintTotals()
{
    std::printf("== (e) constraintTotals() explains the score ==\n");
    BedPlan plan = buildRandomInstance(/*stayCount*/ 24, /*seed*/ 13);
    ScoreCalculator::prepare(plan);
    std::mt19937_64 rng(5);
    for (Stay &stay : plan.stays)
        stay.bed = static_cast<int>(rng() % (plan.bedCount() + 1));

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);
    calculator.resetWorkingSolution(plan);
    const optimaru::HardSoftScore score = calculator.calculateScore();

    double hard = 0.0;
    double soft = 0.0;
    for (const optimaru::ConstraintTotal &total : calculator.constraintTotals())
        (total.isHard ? hard : soft) += total.cost;
    check(std::fabs(-hard - static_cast<double>(score.hard)) < 1e-9
              && std::fabs(-soft - score.soft) < 1e-3,
          "per-constraint totals sum to the reported score");
    check(calculator.constraintTotals().size() == 12,
          "one total per assembled component");
    std::printf("\n");
}

// -------------------------------------------------------------------------
// (f) FULL_ASSERT solve on the plugin path: the engine cross-checks the
// incremental score against a full reset + recompute after every move.
// -------------------------------------------------------------------------
void verifyFullAssertSolve()
{
    std::printf("== (f) FULL_ASSERT plugin solve ==\n");
    BedPlan plan = buildRandomInstance(/*stayCount*/ 20, /*seed*/ 11);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(2);
    config.randomSeed = 3;
    config.environmentMode = optimaru::EnvironmentMode::FULL_ASSERT;
    config.ruinRecreateUnimprovedStepTrigger = 200;
    config.ruinRecreatePolishSteps = 100;

    DecomposedCalculator calculator;
    addDefaultConstraints(calculator);

    bool corrupted = false;
    try
    {
        Solver solver(config);
        solver.solve(plan, calculator);
    }
    catch (const optimaru::ScoreCorruptionError &e)
    {
        corrupted = true;
        std::printf("  ScoreCorruptionError: %s\n", e.what());
    }
    check(!corrupted, "2s FULL_ASSERT plugin solve without score corruption");
    if (!corrupted)
    {
        const Score recomputed = ScoreCalculator::calculateScore(plan);
        check(plan.score == recomputed,
              "plugin-reported score == monolithic full recompute");
    }
    std::printf("\n");
}

}  // namespace

int main()
{
    verifyAssemblyParity();
    verifyIncremental();
    verifyPartialAssemblies();
    verifyOverrides();
    verifyConstraintTotals();
    verifyFullAssertSolve();

    if (failures == 0)
    {
        std::printf("All bed-allocation plugin checks PASSED.\n");
        return 0;
    }
    std::printf("%d bed-allocation plugin check(s) FAILED.\n", failures);
    return 1;
}
