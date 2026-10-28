// Engine-only quality bench: solves a dumped comparison instance (see
// benchmarks/bed-allocation/dump_instances.py) for N seconds with one seed and prints the
// authoritative re-scored levels. This is the A/B half of the
// `bed-allocation-compare` harness — same instances, same single-threaded
// engine path, without the CP-SAT run or the Python round-trip, so tuning
// rounds can interleave two binaries cheaply.
//
//   ba_quality_bench <instance.batxt> [seconds] [seed]
//
// Output (one line, parse-friendly):
//   RESULT file=<name> seed=<s> seconds=<t> hard=<h> medium=<m> soft=<x>
//   laststep=<n>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#include "ba/domain.hpp"
#include "ba/score_calculator.hpp"
#include "ba/solver.hpp"

namespace
{

int parseOptionalInt(const std::string &token, std::optional<int> &out)
{
    if (token == "-")
    {
        out.reset();
        return 0;
    }
    out = std::stoi(token);
    return 0;
}

ba::GenderLimitation parseLimitation(const std::string &name)
{
    if (name == "ANY_GENDER")
        return ba::GenderLimitation::ANY_GENDER;
    if (name == "MALE_ONLY")
        return ba::GenderLimitation::MALE_ONLY;
    if (name == "FEMALE_ONLY")
        return ba::GenderLimitation::FEMALE_ONLY;
    if (name == "SAME_GENDER")
        return ba::GenderLimitation::SAME_GENDER;
    throw std::runtime_error("unknown gender limitation: " + name);
}

ba::Gender parseGender(const std::string &name)
{
    if (name == "MALE")
        return ba::Gender::MALE;
    if (name == "FEMALE")
        return ba::Gender::FEMALE;
    throw std::runtime_error("unknown gender: " + name);
}

ba::BedPlan loadPlan(const std::string &path)
{
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot open " + path);
    std::string magic;
    in >> magic;
    if (magic != "BA1")
        throw std::runtime_error("bad magic in " + path);

    ba::BedPlan plan;
    std::string section;
    std::size_t count = 0;

    in >> section >> count;
    if (section != "departments")
        throw std::runtime_error("expected departments");
    plan.departments.resize(count);
    for (ba::Department &dept : plan.departments)
    {
        std::string minAge, maxAge;
        std::size_t specCount = 0;
        in >> dept.id >> dept.name >> minAge >> maxAge >> specCount;
        parseOptionalInt(minAge, dept.minimumAge);
        parseOptionalInt(maxAge, dept.maximumAge);
        for (std::size_t i = 0; i < specCount; ++i)
        {
            std::string specialty;
            int priority = 0;
            in >> specialty >> priority;
            dept.specialtyToPriority[specialty] = priority;
        }
    }

    in >> section >> count;
    if (section != "rooms")
        throw std::runtime_error("expected rooms");
    plan.rooms.resize(count);
    for (ba::Room &room : plan.rooms)
    {
        std::string limitation;
        std::size_t equipCount = 0;
        in >> room.id >> room.name >> room.department >> room.capacity
            >> limitation >> equipCount;
        room.genderLimitation = parseLimitation(limitation);
        room.equipment.resize(equipCount);
        for (std::string &equipment : room.equipment)
            in >> equipment;
    }

    in >> section >> count;
    if (section != "beds")
        throw std::runtime_error("expected beds");
    plan.beds.resize(count);
    for (ba::Bed &bed : plan.beds)
        in >> bed.id >> bed.room >> bed.indexInRoom;

    in >> section >> count;
    if (section != "stays")
        throw std::runtime_error("expected stays");
    plan.stays.resize(count);
    for (ba::Stay &stay : plan.stays)
    {
        std::string gender, prefMaxCap;
        std::size_t equipCount = 0;
        in >> stay.id >> stay.patientName >> gender >> stay.patientAge
            >> prefMaxCap >> stay.arrivalDay >> stay.departureDay
            >> stay.specialty >> equipCount;
        stay.patientGender = parseGender(gender);
        parseOptionalInt(prefMaxCap, stay.patientPreferredMaximumRoomCapacity);
        stay.patientRequiredEquipment.resize(equipCount);
        for (std::string &equipment : stay.patientRequiredEquipment)
            in >> equipment;
        in >> equipCount;
        stay.patientPreferredEquipment.resize(equipCount);
        for (std::string &equipment : stay.patientPreferredEquipment)
            in >> equipment;
    }
    if (!in)
        throw std::runtime_error("truncated instance file " + path);
    return plan;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(
            stderr, "usage: %s <instance.batxt> [seconds] [seed]\n", argv[0]);
        return 2;
    }
    const std::string path = argv[1];
    const double seconds = argc > 2 ? std::atof(argv[2]) : 10.0;
    const std::uint64_t seed
        = argc > 3 ? static_cast<std::uint64_t>(std::atoll(argv[3])) : 1;

    ba::BedPlan plan = loadPlan(path);

    // seconds <= 0: no solve — score the deterministic fixed assignment
    // stay i -> bed (i % (bedCount + 1)) and exit. Used to crosscheck the
    // dumped instance against the Python domain scorer byte-for-byte.
    if (seconds <= 0.0)
    {
        for (std::size_t s = 0; s < plan.stays.size(); ++s)
        {
            plan.stays[s].bed = static_cast<int>(s) % (plan.bedCount() + 1);
        }
        ba::ScoreCalculator::calculateScore(plan);
        std::printf("CROSSCHECK file=%s hard=%lld medium=%lld soft=%lld\n",
                    path.c_str(),
                    static_cast<long long>(plan.score.hard),
                    static_cast<long long>(plan.score.medium),
                    static_cast<long long>(plan.score.soft));
        return 0;
    }

    std::int64_t lastStep = 0;
    ba::Solver::Config config;
    config.spentLimit = std::chrono::milliseconds(
        static_cast<std::int64_t>(seconds * 1000.0));
    config.randomSeed = seed;
    config.onNewBest = [&lastStep](std::int64_t step, const ba::Score &)
    { lastStep = step; };
    ba::Solver solver(config);
    solver.solve(plan);

    // plan.score is already the authoritative full recompute.
    std::printf("RESULT file=%s seed=%llu seconds=%g hard=%lld medium=%lld "
                "soft=%lld laststep=%lld\n",
                path.c_str(),
                static_cast<unsigned long long>(seed),
                seconds,
                static_cast<long long>(plan.score.hard),
                static_cast<long long>(plan.score.medium),
                static_cast<long long>(plan.score.soft),
                static_cast<long long>(lastStep));
    return 0;
}
