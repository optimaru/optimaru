// Bed-allocation demo: rebuilds the Java quickstart's DemoDataGenerator
// instance (1 department, 10 SAME_GENDER rooms with 13 beds, 28 nights,
// ~75 stays with the generator's gender/age/equipment distributions), solves
// for N seconds and asserts the result is feasible (hard == 0).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "ba/domain.hpp"
#include "ba/score_calculator.hpp"
#include "ba/solver.hpp"

using namespace ba;

namespace
{

const std::vector<std::string> SPECIALTIES
    = {"Specialty1", "Specialty2", "Specialty3"};
const std::vector<std::string> EQUIPMENT_TYPES
    = {"telemetry", "television", "oxygen", "nitrogen"};

// Draws the index of the first cumulative threshold >= a unit draw
// (the Java generator's Pair<Float, T> tables).
int drawIndex(std::mt19937_64 &rng, const std::vector<double> &cumulative)
{
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const double draw = unit(rng);
    for (std::size_t i = 0; i < cumulative.size(); ++i)
    {
        if (draw <= cumulative[i])
            return static_cast<int>(i);
    }
    return static_cast<int>(cumulative.size()) - 1;
}

void addDistinctEquipment(std::mt19937_64 &rng,
                          std::vector<std::string> &target,
                          const std::vector<double> &cumulative,
                          int count)
{
    while (static_cast<int>(target.size()) < count)
    {
        const std::string &equipment
            = EQUIPMENT_TYPES[drawIndex(rng, cumulative)];
        if (std::find(target.begin(), target.end(), equipment) == target.end())
        {
            target.push_back(equipment);
        }
    }
}

BedPlan generateDemoData()
{
    std::mt19937_64 rng(0);
    BedPlan plan;

    // Department: one, offering Specialty1 at priority 1 and the others at 2.
    Department department;
    department.id = "1";
    department.name = "Department";
    department.specialtyToPriority
        = {{"Specialty1", 1}, {"Specialty2", 2}, {"Specialty3", 2}};
    plan.departments.push_back(department);

    // Rooms: 10, all SAME_GENDER (the generator applies it to every room);
    // capacities 80% -> 1, 10% -> 2, 10% -> 3.
    const int roomCount = 10;
    for (int r = 0; r < roomCount; ++r)
    {
        Room room;
        room.id = std::to_string(r);
        room.name = "Room" + std::to_string(r);
        room.department = 0;
        room.genderLimitation = GenderLimitation::SAME_GENDER;
        plan.rooms.push_back(room);
    }
    {
        std::vector<int> order(roomCount);
        for (int r = 0; r < roomCount; ++r)
            order[r] = r;
        std::shuffle(order.begin(), order.end(), rng);
        for (int i = 0; i < roomCount; ++i)
            plan.rooms[order[i]].capacity = i < 8 ? 1 : (i == 8 ? 2 : 3);

        // 76% of the rooms carry equipment; count distribution 11% one /
        // 16% two / 42% three / 31% four.
        std::shuffle(order.begin(), order.end(), rng);
        for (int i = 0; i < static_cast<int>(0.76 * roomCount); ++i)
        {
            const int equipmentCount
                = drawIndex(rng, {0.11, 0.27, 0.69, 1.0}) + 1;
            std::vector<std::string> pool = EQUIPMENT_TYPES;
            std::shuffle(pool.begin(), pool.end(), rng);
            pool.resize(equipmentCount);
            plan.rooms[order[i]].equipment = pool;
        }
    }

    // Beds: one per unit of room capacity.
    for (int r = 0; r < roomCount; ++r)
    {
        for (int i = 0; i < plan.rooms[r].capacity; ++i)
        {
            Bed bed;
            bed.id = std::to_string(r) + "-bed" + std::to_string(i);
            bed.room = r;
            bed.indexInRoom = i;
            plan.beds.push_back(bed);
        }
    }

    // Stays: one timeline per room over 28 nights (days 0..27), stay lengths
    // 5% one night / 25% two / 65% three / 5% four, one free night between
    // consecutive stays of a timeline.
    const int lastDay = 27;
    for (int r = 0; r < roomCount; ++r)
    {
        int currentDay = 0;
        while (currentDay < lastDay)
        {
            const int numDays = drawIndex(rng, {0.05, 0.30, 0.95, 1.0}) + 1;
            const int departure = std::min(currentDay + numDays, lastDay);

            Stay stay;
            const int index = static_cast<int>(plan.stays.size());
            stay.id = "stay-" + std::to_string(index);
            stay.patientName = "patient-" + std::to_string(index);
            stay.arrivalDay = currentDay;
            stay.departureDay = departure;
            stay.specialty = SPECIALTIES[drawIndex(rng, {0.27, 0.63, 1.0})];
            plan.stays.push_back(stay);

            currentDay = departure + 1;
        }
    }

    // Patients: 50/50 gender; the generator's age buckets; preferred maximum
    // room capacity 34% -> 1 / 34% -> 2 / 32% -> 3; required equipment 12%
    // none / 47% one / 41% two; preferred equipment 29% one / 53% two /
    // 16% three / 2% four.
    const std::vector<std::pair<int, int>> ageBuckets = {{0, 10},
                                                         {11, 20},
                                                         {21, 30},
                                                         {31, 40},
                                                         {41, 50},
                                                         {51, 60},
                                                         {61, 70},
                                                         {71, 80},
                                                         {81, 90},
                                                         {91, 100},
                                                         {101, 109}};
    const std::vector<double> ageCumulative
        = {0.10, 0.19, 0.26, 0.36, 0.45, 0.53, 0.61, 0.74, 0.82, 0.91, 1.0};
    for (Stay &stay : plan.stays)
    {
        stay.patientGender = (rng() % 2 == 0) ? Gender::MALE : Gender::FEMALE;

        const auto [minAge, maxAge] = ageBuckets[drawIndex(rng, ageCumulative)];
        stay.patientAge
            = minAge
              + static_cast<int>(
                  rng() % static_cast<std::uint64_t>(maxAge - minAge + 1));

        stay.patientPreferredMaximumRoomCapacity
            = drawIndex(rng, {0.34, 0.68, 1.0}) + 1;

        const int requiredCount = drawIndex(rng, {0.12, 0.59, 1.0});
        if (requiredCount == 1)
        {
            addDistinctEquipment(
                rng, stay.patientRequiredEquipment, {0.22, 0.47, 0.72, 1.0}, 1);
        }
        else if (requiredCount == 2)
        {
            addDistinctEquipment(
                rng, stay.patientRequiredEquipment, {0.13, 0.29, 0.49, 1.0}, 2);
        }

        const int preferredCount = drawIndex(rng, {0.29, 0.82, 0.98, 1.0}) + 1;
        addDistinctEquipment(rng,
                             stay.patientPreferredEquipment,
                             {0.32, 0.62, 0.90, 1.0},
                             preferredCount);
    }

    return plan;
}

void printSummary(const BedPlan &plan)
{
    int unassigned = 0;
    std::vector<int> staysPerRoom(plan.rooms.size(), 0);
    for (const Stay &stay : plan.stays)
    {
        if (stay.bed < 0 || stay.bed >= plan.bedCount())
            ++unassigned;
        else
            ++staysPerRoom[plan.beds[stay.bed].room];
    }
    std::printf("  stays per room:");
    for (std::size_t r = 0; r < plan.rooms.size(); ++r)
    {
        std::printf(" %s=%d(x%d)",
                    plan.rooms[r].name.c_str(),
                    staysPerRoom[r],
                    plan.rooms[r].capacity);
    }
    std::printf(
        "\n  unassigned (no bed) stays: %d / %zu\n\n", unassigned, plan.stays.size());

    std::printf("  [patient assignments: first 8]\n");
    for (std::size_t s = 0; s < plan.stays.size() && s < 8; ++s)
    {
        const Stay &stay = plan.stays[s];
        const bool assigned = stay.bed >= 0 && stay.bed < plan.bedCount();
        std::printf("    %s  %c/%3d y  day %2d-%2d  %s  -> %s\n",
                    stay.patientName.c_str(),
                    stay.patientGender == Gender::MALE ? 'M' : 'F',
                    stay.patientAge,
                    stay.arrivalDay,
                    stay.departureDay,
                    stay.specialty.c_str(),
                    assigned ? plan.beds[stay.bed].id.c_str() : "(no bed)");
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char **argv)
{
    const int seconds = (argc > 1) ? std::atoi(argv[1]) : 10;

    BedPlan plan = generateDemoData();
    std::printf("== bed allocation demo (%zu departments, %zu rooms, %d beds, 28 nights, "
                "%zu stays, %d s) ==\n",
                plan.departments.size(),
                plan.rooms.size(),
                plan.bedCount(),
                plan.stays.size(),
                seconds);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(seconds);
    config.randomSeed = 42;
    config.onNewBest = [](std::int64_t step, const Score &score)
    {
        std::printf("    new best @step %-8lld %s\n",
                    static_cast<long long>(step),
                    score.toString().c_str());
    };
    Solver solver(config);
    solver.solve(plan);

    std::printf("\n  final score: %s  (feasible=%s)\n\n",
                plan.score.toString().c_str(),
                plan.score.isFeasible() ? "true" : "false");
    printSummary(plan);

    if (plan.score.hard == 0)
    {
        std::printf("  reached hard 0: OK\n");
        return 0;
    }
    std::printf("  failed to reach hard 0 (hard=%lld)\n",
                static_cast<long long>(plan.score.hard));
    return 1;
}
