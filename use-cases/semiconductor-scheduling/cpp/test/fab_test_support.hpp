#ifndef FAB_TEST_SUPPORT_HPP
#define FAB_TEST_SUPPORT_HPP

// Shared by fab_test.cpp and fab_plugin_test.cpp: check(), a fixture
// builder addressing everything by id, and seeded random instances.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "fab/domain.hpp"
#include "fab/score_calculator.hpp"
#include "fab/score_terms.hpp"

namespace fabtest
{

using namespace fab;

inline int failures = 0;

inline void check(bool condition, const std::string &what)
{
    if (!condition)
    {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

inline int finish(const char *suite)
{
    if (failures == 0)
        std::printf("%s: all checks passed\n", suite);
    else
        std::printf("%s: %d check(s) FAILED\n", suite, failures);
    return failures == 0 ? 0 : 1;
}

struct Option
{
    std::string machine;
    Time duration;
    std::int64_t preference = 0;
};

// Builds a SemiconductorSchedule by ids, like the Python models do.
struct Builder
{
    SemiconductorSchedule s;
    std::map<std::string, int> machines;
    std::map<std::string, int> resources;
    std::map<std::string, int> lots;

    Builder &machine(const std::string &id, const std::string &group, int capacity = 1, int minimum = 1,
                     std::vector<TimeWindow> unavailable = {})
    {
        Machine m;
        m.id = id;
        m.group = group;
        m.batchCapacity = capacity;
        m.minimumBatchSize = minimum;
        m.unavailable = std::move(unavailable);
        machines[id] = static_cast<int>(s.machines.size());
        s.machines.push_back(std::move(m));
        return *this;
    }
    Builder &resource(const std::string &id, int capacity = 1, std::vector<TimeWindow> unavailable = {})
    {
        AuxiliaryResource r;
        r.id = id;
        r.capacity = capacity;
        r.unavailable = std::move(unavailable);
        resources[id] = static_cast<int>(s.auxiliaryResources.size());
        s.auxiliaryResources.push_back(std::move(r));
        return *this;
    }
    Builder &lot(const std::string &id, Time release, Time due, std::int64_t priority = 1,
                 std::optional<Time> maxCycle = std::nullopt)
    {
        Lot l;
        l.id = id;
        l.product = "P";
        l.releaseTime = release;
        l.dueTime = due;
        l.priority = priority;
        l.maxCycleTime = maxCycle;
        lots[id] = static_cast<int>(s.lots.size());
        s.lots.push_back(std::move(l));
        return *this;
    }
    // Appends a route step to the LAST lot added.
    Builder &op(const std::string &id, const std::string &family, std::vector<Option> options,
                std::vector<std::string> required = {}, Time minQueue = 0,
                std::optional<Time> maxQueue = std::nullopt)
    {
        Operation o;
        o.id = id;
        o.family = family;
        for (const Option &option : options)
            o.eligibleMachines.push_back({machines.at(option.machine), option.duration, option.preference});
        for (const std::string &r : required)
            o.requiredResources.push_back(resources.at(r));
        o.minQueueTime = minQueue;
        o.maxQueueTime = maxQueue;
        s.lots.back().operations.push_back(std::move(o));
        return *this;
    }
    Builder &setup(const std::string &group, const std::string &from, const std::string &to, Time duration)
    {
        s.setupTimes.push_back({group, from, to, duration});
        return *this;
    }
    Builder &transport(const std::string &from, const std::string &to, Time duration)
    {
        s.transportTimes.push_back({from, to, duration});
        return *this;
    }
    Assignment make(const std::string &lot, const std::string &operation, const std::string &machine, Time start,
                    Time end, std::optional<std::string> batch = std::nullopt) const
    {
        const int l = lots.at(lot);
        int o = 0;
        while (s.lots[static_cast<std::size_t>(l)].operations[static_cast<std::size_t>(o)].id != operation)
            ++o;
        return {l, o, machines.at(machine), start, end, std::move(batch)};
    }
    Builder &assign(const std::string &lot, const std::string &operation, const std::string &machine, Time start,
                    Time end, std::optional<std::string> batch = std::nullopt)
    {
        s.assignments.push_back(make(lot, operation, machine, start, end, std::move(batch)));
        return *this;
    }
    Builder &pin(const std::string &lot, const std::string &operation, const std::string &machine, Time start,
                 Time end)
    {
        s.pinnedAssignments.push_back(make(lot, operation, machine, start, end));
        return *this;
    }
    SemiconductorSchedule build()
    {
        SemiconductorSchedule out = s;
        ScoreCalculator::prepare(out);
        return out;
    }
};

inline std::int64_t raw(const RawTotals &totals, Term term) { return totals[termIndex(term)]; }

// A seeded random instance with a random (not decoded) assignment list:
// re-entrant routes, repeated machine options, batch tools, duplicate setup
// and transport entries, downtime, duplicated required resources, pins.
inline SemiconductorSchedule randomInstance(std::uint64_t seed)
{
    std::mt19937_64 rng(seed);
    auto pick = [&rng](int low, int high)
    { return low + static_cast<int>(rng() % static_cast<std::uint64_t>(high - low + 1)); };
    const char *families[] = {"A", "B", "C"};
    const char *groups[] = {"G1", "G2", "G3"};
    Builder b;
    auto windows = [&](int count)
    {
        std::vector<TimeWindow> out;
        for (int i = 0; i < count; ++i)
        {
            const Time start = pick(0, 40);
            out.push_back({start, start + pick(1, 6)});
        }
        return out;
    };
    const int machineCount = pick(2, 5);
    for (int m = 0; m < machineCount; ++m)
    {
        const int capacity = pick(0, 3) <= 1 ? 1 : pick(2, 3);
        b.machine("M" + std::to_string(m), groups[pick(0, 2)], capacity, pick(1, capacity), windows(pick(0, 3) / 2));
    }
    const int resourceCount = pick(0, 2);
    for (int r = 0; r < resourceCount; ++r)
        b.resource("R" + std::to_string(r), pick(1, 2), windows(pick(0, 2)));
    std::map<std::pair<int, std::string>, Time> batchDuration;
    const int lotCount = pick(1, 5);
    for (int l = 0; l < lotCount; ++l)
    {
        std::optional<Time> maxCycle;
        if (pick(0, 2) == 0)
            maxCycle = pick(5, 30);
        b.lot("L" + std::to_string(pick(0, 99)) + "-" + std::to_string(l), pick(0, 5), pick(5, 40), pick(1, 3),
              maxCycle);
        const int opCount = pick(1, 4);
        for (int o = 0; o < opCount; ++o)
        {
            const std::string family = families[pick(0, 2)];
            std::vector<Option> options;
            const int optionCount = pick(0, 3) <= 1 ? 1 : pick(2, 3);
            for (int k = 0; k < optionCount; ++k)
            {
                const int m = pick(0, machineCount - 1);
                Time duration = pick(1, 6);
                if (b.s.machines[static_cast<std::size_t>(m)].batchCapacity > 1)
                    duration = batchDuration.try_emplace({m, family}, duration).first->second;
                options.push_back({"M" + std::to_string(m), duration, pick(0, 3) == 0 ? 3 : 0});
            }
            std::vector<std::string> required;
            if (resourceCount > 0)
            {
                const int count = pick(0, 3) / 2;
                for (int k = 0; k < count; ++k)
                    required.push_back("R" + std::to_string(pick(0, resourceCount - 1)));
            }
            std::optional<Time> maxQueue;
            if (pick(0, 2) == 0)
                maxQueue = pick(0, 5);
            b.op("O" + std::to_string(o), family, options, required, pick(0, 3) == 0 ? pick(1, 3) : 0, maxQueue);
        }
    }
    for (int k = pick(0, 6); k > 0; --k)
        b.setup(groups[pick(0, 2)], families[pick(0, 2)], families[pick(0, 2)], pick(0, 4));
    for (int k = pick(0, 5); k > 0; --k)
        b.transport(groups[pick(0, 2)], groups[pick(0, 2)], pick(0, 4));
    // Pins: on the last option of a machine, with its duration.
    std::vector<std::pair<int, int>> keys;
    for (int l = 0; l < lotCount; ++l)
    {
        for (int o = 0; o < static_cast<int>(b.s.lots[static_cast<std::size_t>(l)].operations.size()); ++o)
            keys.emplace_back(l, o);
    }
    std::shuffle(keys.begin(), keys.end(), rng);
    const int pins = std::min<int>(static_cast<int>(keys.size()), pick(0, 3) / 2);
    for (int p = 0; p < pins; ++p)
    {
        const auto [l, o] = keys[static_cast<std::size_t>(p)];
        const Operation &operation = b.s.lots[static_cast<std::size_t>(l)].operations[static_cast<std::size_t>(o)];
        const MachineOption &any = operation.eligibleMachines[static_cast<std::size_t>(
            pick(0, static_cast<int>(operation.eligibleMachines.size()) - 1))];
        const MachineOption *last = &any;
        for (const MachineOption &option : operation.eligibleMachines)
        {
            if (option.machine == any.machine)
                last = &option;
        }
        const Time start = pick(0, 20);
        b.s.pinnedAssignments.push_back({l, o, last->machine, start, start + last->duration, std::nullopt});
    }
    // A random assignment list (data mode).
    const char *ids[] = {"", "RUN1", "RUN2", "B001", "B002"};
    for (const auto &[l, o] : keys)
    {
        if (pick(0, 6) == 0)
            continue;
        const Operation &operation = b.s.lots[static_cast<std::size_t>(l)].operations[static_cast<std::size_t>(o)];
        int machine;
        Time duration;
        if (pick(0, 4) != 0)
        {
            const MachineOption &option = operation.eligibleMachines[static_cast<std::size_t>(
                pick(0, static_cast<int>(operation.eligibleMachines.size()) - 1))];
            machine = option.machine;
            duration = option.duration;
        }
        else
        {
            machine = pick(0, machineCount - 1);
            duration = pick(1, 6);
        }
        const Time start = pick(0, 30);
        std::optional<std::string> batch;
        if (pick(0, 2) != 0)
            batch = ids[pick(0, 4)];
        b.s.assignments.push_back({l, o, machine, start, start + duration, batch});
    }
    return b.build();
}

// A random planning value per row: (rank, slot) with ties, occasionally -1.
inline int randomValue(const SemiconductorSchedule &s, std::mt19937_64 &rng, bool allowUnassigned)
{
    if (allowUnassigned && rng() % 25 == 0)
        return -1;
    const int rank = static_cast<int>(rng() % static_cast<std::uint64_t>(s.rowCount()));
    const int slot = static_cast<int>(rng() % static_cast<std::uint64_t>(s.maxOptionCount));
    return s.valueOf(rank, slot);
}

}  // namespace fabtest

#endif  // FAB_TEST_SUPPORT_HPP
