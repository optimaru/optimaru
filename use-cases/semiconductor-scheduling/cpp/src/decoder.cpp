#include "fab/decoder.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace fab
{

namespace
{

constexpr int kMaxPasses = 10000;

inline Time overlap(Time startA, Time endA, Time startB, Time endB)
{
    return std::max<Time>(0, checked::sub(std::min(endA, endB), std::max(startA, startB)));
}

}  // namespace

void Decoder::bind(const SemiconductorSchedule &schedule)
{
    schedule_ = &schedule;
    machineItems_.assign(schedule.machines.size(), {});
    resourceItems_.assign(schedule.auxiliaryResources.size(), {});
    pinEntry_.assign(schedule.pinnedAssignments.size(), -1);
    for (std::size_t p = 0; p < schedule.pinnedAssignments.size(); ++p)
    {
        const Assignment &pin = schedule.pinnedAssignments[p];
        const int row = schedule.rowOf(pin.lot, pin.operation);
        const Operation &operation = schedule.operationOfRow(row);
        auto &items = machineItems_[static_cast<std::size_t>(pin.machine)];
        items.push_back({pin.start, pin.end, operation.familyIndex, -1, row});
        pinEntry_[p] = static_cast<int>(items.size()) - 1;
        for (int resource : operation.requiredResources)
            resourceItems_[static_cast<std::size_t>(resource)].push_back({pin.start, pin.end, -1});
    }
    const std::size_t n = schedule.rows.size();
    placement_.assign(n, Placement{});
    readyStep_.assign(n, kNever);
    for (const Lot &lot : schedule.lots)
    {
        if (!lot.operations.empty())
            readyStep_[static_cast<std::size_t>(lot.firstRow)] = 0;
    }
    nextOperation_.assign(schedule.lots.size(), 0);
    steps_.clear();
    output_.clear();
}

void Decoder::truncate(int step)
{
    if (step >= stepCount())
        return;
    const SemiconductorSchedule &schedule = *schedule_;
    const std::size_t first = static_cast<std::size_t>(steps_[static_cast<std::size_t>(step)]);
    for (std::size_t i = first; i < output_.size(); ++i)
    {
        const int row = output_[i];
        const OperationRow &r = schedule.rows[static_cast<std::size_t>(row)];
        placement_[static_cast<std::size_t>(row)] = Placement{};
        const Lot &lot = schedule.lots[static_cast<std::size_t>(r.lot)];
        if (r.operation + 1 < static_cast<int>(lot.operations.size()))
            readyStep_[static_cast<std::size_t>(row + 1)] = kNever;
        int &next = nextOperation_[static_cast<std::size_t>(r.lot)];
        next = std::min(next, r.operation);
    }
    output_.resize(first);
    steps_.resize(static_cast<std::size_t>(step));
    for (auto &items : machineItems_)
    {
        while (!items.empty() && items.back().stamp >= step)
            items.pop_back();
    }
    for (auto &items : resourceItems_)
    {
        while (!items.empty() && items.back().stamp >= step)
            items.pop_back();
    }
}

// (rank, -lot priority, lot id): the 0.1.x selection key.
bool Decoder::before(int a, int b, const std::int64_t *rank) const
{
    const std::int64_t rankA = rank[a];
    const std::int64_t rankB = rank[b];
    if (rankA != rankB)
        return rankA < rankB;
    const Lot &lotA = schedule_->lotOfRow(a);
    const Lot &lotB = schedule_->lotOfRow(b);
    if (lotA.priority != lotB.priority)
        return lotA.priority > lotB.priority;
    if (lotA.idOrder != lotB.idOrder)
        return lotA.idOrder < lotB.idOrder;
    return a < b;
}

void Decoder::placeRow(int row, int machine, Time start, Time end, bool pinned, int entry, int step)
{
    const SemiconductorSchedule &schedule = *schedule_;
    const OperationRow &r = schedule.rows[static_cast<std::size_t>(row)];
    Placement &p = placement_[static_cast<std::size_t>(row)];
    p.step = step;
    p.machine = machine;
    p.start = start;
    p.end = end;
    p.seq = static_cast<std::int64_t>(output_.size());
    p.pinned = pinned;
    p.entry = entry;
    output_.push_back(row);
    nextOperation_[static_cast<std::size_t>(r.lot)] = r.operation + 1;
    const Lot &lot = schedule.lots[static_cast<std::size_t>(r.lot)];
    if (r.operation + 1 < static_cast<int>(lot.operations.size()))
        readyStep_[static_cast<std::size_t>(row + 1)] = step + 1;
}

void Decoder::run(const int *option, const std::int64_t *rank)
{
    const SemiconductorSchedule &schedule = *schedule_;
    for (;;)
    {
        ready_.clear();
        for (std::size_t l = 0; l < schedule.lots.size(); ++l)
        {
            const Lot &lot = schedule.lots[l];
            const int position = nextOperation_[l];
            if (position >= static_cast<int>(lot.operations.size()))
                continue;
            const int row = lot.firstRow + position;
            if (option[row] >= 0)
                ready_.push_back(row);
        }
        if (ready_.empty())
            return;
        int selected = ready_.front();
        for (int row : ready_)
        {
            if (before(row, selected, rank))
                selected = row;
        }
        const int step = stepCount();
        steps_.push_back(static_cast<int>(output_.size()));

        const int pin = schedule.pinOfRow[static_cast<std::size_t>(selected)];
        if (pin >= 0)
        {
            const Assignment &a = schedule.pinnedAssignments[static_cast<std::size_t>(pin)];
            placeRow(selected, a.machine, a.start, a.end, true,
                     pinEntry_[static_cast<std::size_t>(pin)], step);
            continue;
        }
        const Operation &operation = schedule.operationOfRow(selected);
        const MachineOption &chosen
            = operation.eligibleMachines[static_cast<std::size_t>(option[selected])];
        const int m = chosen.machine;
        const Machine &machine = schedule.machines[static_cast<std::size_t>(m)];

        batch_.clear();
        batch_.push_back(selected);
        if (machine.isBatch())
        {
            std::stable_sort(ready_.begin(), ready_.end(),
                             [&](int a, int b) { return before(a, b, rank); });
            for (int candidate : ready_)
            {
                if (candidate == selected)
                    continue;
                const Operation &other = schedule.operationOfRow(candidate);
                const MachineOption &otherOption
                    = other.eligibleMachines[static_cast<std::size_t>(option[candidate])];
                if (otherOption.machine == m && other.familyIndex == operation.familyIndex
                    && otherOption.duration == chosen.duration)
                {
                    batch_.push_back(candidate);
                }
                if (static_cast<int>(batch_.size()) >= machine.batchCapacity)
                    break;
            }
        }

        Time ready = 0;
        bool first = true;
        for (int row : batch_)
        {
            const OperationRow &r = schedule.rows[static_cast<std::size_t>(row)];
            const Lot &lot = schedule.lots[static_cast<std::size_t>(r.lot)];
            Time time;
            if (r.operation == 0)
            {
                time = lot.releaseTime;
            }
            else
            {
                const Placement &previous = placement_[static_cast<std::size_t>(row - 1)];
                const Machine &previousMachine
                    = schedule.machines[static_cast<std::size_t>(previous.machine)];
                const Time wait = std::max(
                    schedule.transportTime(previousMachine.groupIndex, machine.groupIndex),
                    lot.operations[static_cast<std::size_t>(r.operation - 1)].minQueueTime);
                time = checked::add(previous.end, wait);
            }
            ready = first ? time : std::max(ready, time);
            first = false;
        }
        const Time start = earliestStart(m, operation, ready, chosen.duration);
        const Time end = checked::add(start, chosen.duration);
        auto &items = machineItems_[static_cast<std::size_t>(m)];
        for (int row : batch_)
        {
            items.push_back({start, end, operation.familyIndex, step, row});
            const int entry = static_cast<int>(items.size()) - 1;
            for (int resource : schedule.operationOfRow(row).requiredResources)
                resourceItems_[static_cast<std::size_t>(resource)].push_back({start, end, step});
            placeRow(row, m, start, end, false, entry, step);
        }
    }
}

Time Decoder::earliestStart(int m, const Operation &operation, Time ready, Time duration) const
{
    const SemiconductorSchedule &schedule = *schedule_;
    const Machine &machine = schedule.machines[static_cast<std::size_t>(m)];
    const auto &items = machineItems_[static_cast<std::size_t>(m)];
    const int family = operation.familyIndex;
    Time start = std::max<Time>(0, ready);
    for (int pass = 0; pass < kMaxPasses; ++pass)
    {
        // Machine downtime: jump past the first window (by start) in the way,
        // until none is.
        for (bool moved = true; moved;)
        {
            moved = false;
            for (const TimeWindow &window : machine.unavailableByStart)
            {
                if (overlap(start, checked::add(start, duration), window.start, window.end) > 0)
                {
                    start = window.end;
                    moved = true;
                    break;
                }
            }
        }
        const Time end = checked::add(start, duration);

        // The selected operation's resources: downtime, then capacity.
        bool conflict = false;
        Time conflictEnd = 0;
        for (int r : operation.requiredResources)
        {
            const AuxiliaryResource &resource
                = schedule.auxiliaryResources[static_cast<std::size_t>(r)];
            bool down = false;
            Time downEnd = 0;
            for (const TimeWindow &window : resource.unavailable)
            {
                if (overlap(start, end, window.start, window.end) > 0)
                {
                    downEnd = down ? std::max(downEnd, window.end) : window.end;
                    down = true;
                }
            }
            int users = 0;
            Time firstFree = 0;
            for (const ResourceItem &item : resourceItems_[static_cast<std::size_t>(r)])
            {
                if (overlap(start, end, item.start, item.end) > 0)
                {
                    firstFree = users == 0 ? item.end : std::min(firstFree, item.end);
                    ++users;
                }
            }
            if (down)
            {
                conflictEnd = std::max(conflict ? conflictEnd : 0, downEnd);
                conflict = true;
            }
            if (users >= resource.capacity)
            {
                conflictEnd = std::max(conflict ? conflictEnd : 0, firstFree);
                conflict = true;
            }
        }
        if (conflict)
        {
            start = conflictEnd;
            continue;
        }

        // The machine's own items.
        bool overlapping = false;
        Time overlapEnd = 0;
        const MachineItem *incompatible = nullptr;
        for (const MachineItem &item : items)
        {
            if (overlap(start, end, item.start, item.end) <= 0)
                continue;
            overlapEnd = overlapping ? std::max(overlapEnd, item.end) : item.end;
            overlapping = true;
            if (item.family != family && (incompatible == nullptr || item.end > incompatible->end))
                incompatible = &item;
        }
        if (!machine.isBatch() && overlapping)
        {
            start = overlapEnd;
            continue;
        }
        if (machine.isBatch())
        {
            if (incompatible != nullptr)
            {
                start = checked::add(incompatible->end,
                                     schedule.setupTime(machine.groupIndex, incompatible->family, family));
                continue;
            }
            if (overlapping)
            {
                start = overlapEnd;
                continue;
            }
        }
        // The setup from the latest-ending earlier item (the first such).
        const MachineItem *previous = nullptr;
        for (const MachineItem &item : items)
        {
            if (item.end <= start && (previous == nullptr || item.end > previous->end))
                previous = &item;
        }
        if (previous != nullptr
            && !(machine.isBatch() && previous->family == family && previous->end == start))
        {
            const Time required = checked::add(
                previous->end, schedule.setupTime(machine.groupIndex, previous->family, family));
            if (start < required)
            {
                start = required;
                continue;
            }
        }
        return start;
    }
    throw std::runtime_error("failed to place operation inside the scheduling horizon");
}

std::vector<Assignment> Decoder::assignments() const
{
    const SemiconductorSchedule &schedule = *schedule_;
    std::vector<Assignment> out;
    out.reserve(output_.size());
    for (int row : output_)
    {
        const OperationRow &r = schedule.rows[static_cast<std::size_t>(row)];
        const Placement &p = placement_[static_cast<std::size_t>(row)];
        Assignment a;
        a.lot = r.lot;
        a.operation = r.operation;
        a.machine = p.machine;
        a.start = p.start;
        a.end = p.end;
        if (p.pinned)
        {
            const int pin = schedule.pinOfRow[static_cast<std::size_t>(row)];
            a.batchId = schedule.pinnedAssignments[static_cast<std::size_t>(pin)].batchId;
        }
        out.push_back(std::move(a));
    }

    // Batch ids: per batch machine (machine order), its items stably sorted
    // by (start, end), grouped by (family, start, end) in first-appearance
    // order, numbered by one counter over all machines. Pin items count.
    std::vector<std::vector<int>> groupOfItem(schedule.machines.size());
    std::vector<std::vector<int>> idOfGroup(schedule.machines.size());
    int counter = 0;
    for (std::size_t m = 0; m < schedule.machines.size(); ++m)
    {
        if (!schedule.machines[m].isBatch())
            continue;
        const auto &items = machineItems_[m];
        std::vector<int> order(items.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b)
                         {
                             const MachineItem &x = items[static_cast<std::size_t>(a)];
                             const MachineItem &y = items[static_cast<std::size_t>(b)];
                             return std::tie(x.start, x.end) < std::tie(y.start, y.end);
                         });
        std::map<std::tuple<int, Time, Time>, int> groups;
        groupOfItem[m].assign(items.size(), -1);
        for (int index : order)
        {
            const MachineItem &item = items[static_cast<std::size_t>(index)];
            auto [it, inserted]
                = groups.try_emplace({item.family, item.start, item.end}, static_cast<int>(groups.size()));
            if (inserted)
                idOfGroup[m].push_back(++counter);
            groupOfItem[m][static_cast<std::size_t>(index)] = it->second;
        }
    }
    for (std::size_t i = 0; i < output_.size(); ++i)
    {
        const Placement &p = placement_[static_cast<std::size_t>(output_[i])];
        const std::size_t m = static_cast<std::size_t>(p.machine);
        if (!schedule.machines[m].isBatch())
            continue;
        const int group = groupOfItem[m][static_cast<std::size_t>(p.entry)];
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "B%03d", idOfGroup[m][static_cast<std::size_t>(group)]);
        out[i].batchId = std::string(buffer);
    }
    return out;
}

std::vector<int> defaultChoices(const SemiconductorSchedule &schedule)
{
    std::vector<int> choices(schedule.rows.size(), 0);
    for (int row = 0; row < schedule.rowCount(); ++row)
    {
        const auto &options = schedule.operationOfRow(row).eligibleMachines;
        int best = 0;
        for (int i = 1; i < static_cast<int>(options.size()); ++i)
        {
            const MachineOption &a = options[static_cast<std::size_t>(i)];
            const MachineOption &b = options[static_cast<std::size_t>(best)];
            if (std::tie(a.preferenceCost, a.duration) < std::tie(b.preferenceCost, b.duration))
                best = i;
        }
        choices[static_cast<std::size_t>(row)] = best;
    }
    return choices;
}

void seedState(const SemiconductorSchedule &schedule,
               const std::vector<int> &choices,
               std::vector<int> &selected,
               std::vector<int> &ranks)
{
    const int n = schedule.rowCount();
    std::vector<int> seeded(static_cast<std::size_t>(n), -1);
    for (std::size_t i = 0; i < schedule.assignments.size(); ++i)
    {
        const Assignment &a = schedule.assignments[i];
        seeded[static_cast<std::size_t>(schedule.rowOf(a.lot, a.operation))] = static_cast<int>(i);
    }
    selected = choices;
    for (int row = 0; row < n; ++row)
    {
        const int index = seeded[static_cast<std::size_t>(row)];
        if (index < 0)
            continue;
        const Assignment &a = schedule.assignments[static_cast<std::size_t>(index)];
        const auto &options = schedule.operationOfRow(row).eligibleMachines;
        for (int i = 0; i < static_cast<int>(options.size()); ++i)
        {
            const MachineOption &option = options[static_cast<std::size_t>(i)];
            if (option.machine == a.machine && option.duration == a.end - a.start)
                selected[static_cast<std::size_t>(row)] = i;  // the last match wins
        }
    }
    std::vector<int> order(static_cast<std::size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    auto key = [&](int row)
    {
        const int index = seeded[static_cast<std::size_t>(row)];
        const bool unseeded = index < 0;
        const Time start
            = unseeded ? 0 : schedule.assignments[static_cast<std::size_t>(index)].start;
        return std::make_tuple(unseeded, start, row);
    };
    std::sort(order.begin(), order.end(), [&](int a, int b) { return key(a) < key(b); });
    ranks.assign(static_cast<std::size_t>(n), 0);
    for (int rank = 0; rank < n; ++rank)
        ranks[static_cast<std::size_t>(order[static_cast<std::size_t>(rank)])] = rank;
}

std::vector<Assignment> decode(const SemiconductorSchedule &schedule,
                               const std::vector<int> &option,
                               const std::vector<std::int64_t> &rank)
{
    if (option.size() != schedule.rows.size() || rank.size() != schedule.rows.size())
        throw std::invalid_argument("decode needs one option and one rank per operation row");
    Decoder decoder;
    decoder.bind(schedule);
    decoder.run(option.data(), rank.data());
    return decoder.assignments();
}

void valuesToState(const SemiconductorSchedule &schedule,
                   std::vector<int> &option,
                   std::vector<std::int64_t> &rank)
{
    const std::size_t n = schedule.rows.size();
    option.resize(n);
    rank.resize(n);
    for (std::size_t r = 0; r < n; ++r)
    {
        const int value = schedule.rows[r].value;
        option[r] = schedule.optionIndexOf(static_cast<int>(r), value);
        rank[r] = value < 0 ? 0 : schedule.rankOf(value);
    }
}

std::vector<Assignment> decodeValues(const SemiconductorSchedule &schedule)
{
    std::vector<int> option;
    std::vector<std::int64_t> rank;
    valuesToState(schedule, option, rank);
    return decode(schedule, option, rank);
}

}  // namespace fab
