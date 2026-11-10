#include "fab/score_calculator.hpp"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "fab/constraints.hpp"
#include "fab/decoder.hpp"

namespace fab {

namespace {

// The default soft weights, read off the components.
constexpr double kPreferenceWeight = MachinePreferenceCost::kDefaultWeight;
constexpr double kMinBatchWeight = MinBatchSizeCost::kDefaultWeight;
constexpr double kLoadWeight = MachineLoadImbalanceCost::kDefaultWeight;
constexpr double kTardinessWeight = WeightedTardinessCost::kDefaultWeight;
constexpr double kCycleWeight = CycleTimeCost::kDefaultWeight;
constexpr double kMakespanWeight = MakespanCost::kDefaultWeight;
// The 14 hard terms all weigh 1.
static_assert(MachineEligibilityCost::kDefaultWeight == 1.0 &&
                      LotRoutePrecedenceCost::kDefaultWeight == 1.0 &&
                      MachineCapacityCost::kDefaultWeight == 1.0 &&
                      BatchCapacityCost::kDefaultWeight == 1.0 &&
                      BatchFamilyConflictCost::kDefaultWeight == 1.0 &&
                      MissingSetupTimeCost::kDefaultWeight == 1.0 &&
                      MachineUnavailabilityCost::kDefaultWeight == 1.0 &&
                      MaxQueueTimeCost::kDefaultWeight == 1.0 &&
                      MinQueueTimeCost::kDefaultWeight == 1.0 &&
                      MissingTransportTimeCost::kDefaultWeight == 1.0 &&
                      AuxiliaryResourceCapacityCost::kDefaultWeight == 1.0 &&
                      AuxiliaryResourceUnavailabilityCost::kDefaultWeight == 1.0 &&
                      PinnedAssignmentsCost::kDefaultWeight == 1.0 &&
                      MaxCycleTimeCost::kDefaultWeight == 1.0,
              "the monolithic calculator assumes hard default weights of 1");

[[noreturn]] void invalid(const std::string& message) { throw std::invalid_argument(message); }

void checkWindows(const std::vector<TimeWindow>& windows, const std::string& owner) {
    for (const TimeWindow& window : windows) {
        if (window.start < 0 || window.end <= window.start)
            invalid("time window of " + owner + " must have 0 <= start < end");
    }
}

std::string keyText(const SemiconductorSchedule& schedule, int lot, int operation) {
    return "('" + schedule.lots[static_cast<std::size_t>(lot)].id + "', '" +
           schedule.lots[static_cast<std::size_t>(lot)]
                   .operations[static_cast<std::size_t>(operation)]
                   .id +
           "')";
}

void checkReference(const SemiconductorSchedule& schedule, const Assignment& a, const char* what) {
    if (a.lot < 0 || a.lot >= static_cast<int>(schedule.lots.size()) || a.operation < 0 ||
        a.operation >= static_cast<int>(
                               schedule.lots[static_cast<std::size_t>(a.lot)].operations.size())) {
        invalid(std::string(what) + " references unknown operation");
    }
    if (a.machine < 0 || a.machine >= static_cast<int>(schedule.machines.size()))
        invalid(std::string(what) + " references unknown machine");
    if (a.start < 0 || a.end <= a.start)
        invalid(std::string(what) + " end must be after start (start >= 0)");
}

}  // namespace

void ScoreCalculator::validateAssignments(const SemiconductorSchedule& schedule,
                                          const std::vector<Assignment>& assignments) {
    std::unordered_set<int> assigned;
    for (const Assignment& a : assignments) {
        checkReference(schedule, a, "assignment");
        const int row = schedule.rowOf(a.lot, a.operation);
        if (!assigned.insert(row).second)
            invalid("operation is assigned more than once: " +
                    keyText(schedule, a.lot, a.operation));
    }
}

void ScoreCalculator::prepare(SemiconductorSchedule& schedule) {
    if (schedule.prepared) return;

    // Machines and groups.
    std::unordered_map<std::string, int> groupIndex;
    std::unordered_set<std::string> machineIds;
    schedule.groupNames.clear();
    for (Machine& machine : schedule.machines) {
        if (!machineIds.insert(machine.id).second) invalid("machine ids must be unique");
        if (machine.batchCapacity < 1)
            invalid("batch_capacity of machine '" + machine.id + "' must be >= 1");
        if (machine.minimumBatchSize < 1 || machine.minimumBatchSize > machine.batchCapacity)
            invalid("minimum_batch_size must be >= 1 and must not exceed batch_capacity");
        checkWindows(machine.unavailable, "machine '" + machine.id + "'");
        auto [it, inserted] =
                groupIndex.try_emplace(machine.group, static_cast<int>(schedule.groupNames.size()));
        if (inserted) schedule.groupNames.push_back(machine.group);
        machine.groupIndex = it->second;
        machine.unavailableByStart = machine.unavailable;
        std::stable_sort(
                machine.unavailableByStart.begin(), machine.unavailableByStart.end(),
                [](const TimeWindow& a, const TimeWindow& b) { return a.start < b.start; });
    }

    // Resources.
    std::unordered_set<std::string> resourceIds;
    for (const AuxiliaryResource& resource : schedule.auxiliaryResources) {
        if (!resourceIds.insert(resource.id).second)
            invalid("auxiliary resource ids must be unique");
        if (resource.capacity < 1)
            invalid("capacity of auxiliary resource '" + resource.id + "' must be >= 1");
        checkWindows(resource.unavailable, "auxiliary resource '" + resource.id + "'");
    }

    // Lots, operations, families, rows.
    std::unordered_map<std::string, int> familyIndex;
    std::unordered_set<std::string> lotIds;
    std::map<std::pair<int, int>, Time> batchDuration;  // (machine, family)
    schedule.familyNames.clear();
    schedule.maxOptionCount = 1;
    int row = 0;
    for (Lot& lot : schedule.lots) {
        if (!lotIds.insert(lot.id).second) invalid("lot ids must be unique");
        if (lot.operations.empty()) invalid("lot '" + lot.id + "' needs at least one operation");
        if (lot.priority < 1) invalid("priority of lot '" + lot.id + "' must be >= 1");
        lot.firstRow = row;
        std::unordered_set<std::string> operationIds;
        for (Operation& operation : lot.operations) {
            if (!operationIds.insert(operation.id).second)
                invalid("operation ids must be unique inside lot '" + lot.id + "'");
            if (operation.eligibleMachines.empty())
                invalid("operation '" + operation.id + "' of lot '" + lot.id +
                        "' needs an eligible machine");
            auto [fit, finserted] = familyIndex.try_emplace(
                    operation.family, static_cast<int>(schedule.familyNames.size()));
            if (finserted) schedule.familyNames.push_back(operation.family);
            operation.familyIndex = fit->second;
            for (int resource : operation.requiredResources) {
                if (resource < 0 ||
                    resource >= static_cast<int>(schedule.auxiliaryResources.size()))
                    invalid("unknown required resource");
            }
            operation.lastOptionOfMachine.clear();
            for (std::size_t i = 0; i < operation.eligibleMachines.size(); ++i) {
                const MachineOption& option = operation.eligibleMachines[i];
                if (option.machine < 0 ||
                    option.machine >= static_cast<int>(schedule.machines.size()))
                    invalid("unknown eligible machine");
                if (option.duration <= 0) invalid("machine option duration must be > 0");
                if (schedule.machines[static_cast<std::size_t>(option.machine)].isBatch()) {
                    auto [dit, dinserted] = batchDuration.try_emplace(
                            {option.machine, operation.familyIndex}, option.duration);
                    if (!dinserted && dit->second != option.duration)
                        invalid("operations in the same machine/family batch must have equal "
                                "duration");
                }
                auto last = std::find_if(
                        operation.lastOptionOfMachine.begin(), operation.lastOptionOfMachine.end(),
                        [&](const auto& entry) { return entry.first == option.machine; });
                if (last == operation.lastOptionOfMachine.end())
                    operation.lastOptionOfMachine.emplace_back(option.machine, static_cast<int>(i));
                else
                    last->second = static_cast<int>(i);
            }
            schedule.maxOptionCount = std::max(schedule.maxOptionCount,
                                               static_cast<int>(operation.eligibleMachines.size()));
            ++row;
        }
    }
    std::vector<int> byId(schedule.lots.size());
    std::iota(byId.begin(), byId.end(), 0);
    std::sort(byId.begin(), byId.end(), [&](int a, int b) {
        return schedule.lots[static_cast<std::size_t>(a)].id <
               schedule.lots[static_cast<std::size_t>(b)].id;
    });
    for (std::size_t i = 0; i < byId.size(); ++i)
        schedule.lots[static_cast<std::size_t>(byId[i])].idOrder = static_cast<int>(i);

    if (static_cast<int>(schedule.rows.size()) != row) {
        schedule.rows.clear();
        for (int l = 0; l < static_cast<int>(schedule.lots.size()); ++l) {
            for (int o = 0;
                 o < static_cast<int>(schedule.lots[static_cast<std::size_t>(l)].operations.size());
                 ++o)
                schedule.rows.push_back({l, o, -1});
        }
    }

    // Setup and transport tables: the last entry wins; entries naming a group
    // or family nothing uses can never be read.
    schedule.setupByKey.clear();
    const std::size_t families = schedule.familyNames.size();
    for (const SetupTime& setup : schedule.setupTimes) {
        const auto g = groupIndex.find(setup.machineGroup);
        const auto from = familyIndex.find(setup.fromFamily);
        const auto to = familyIndex.find(setup.toFamily);
        if (g == groupIndex.end() || from == familyIndex.end() || to == familyIndex.end()) continue;
        const std::size_t key = (static_cast<std::size_t>(g->second) * families +
                                 static_cast<std::size_t>(from->second)) *
                                        families +
                                static_cast<std::size_t>(to->second);
        schedule.setupByKey[key] = setup.duration;
    }
    const std::size_t groups = schedule.groupNames.size();
    schedule.transportByGroup.assign(groups * groups, 0);
    for (const TransportTime& transport : schedule.transportTimes) {
        const auto from = groupIndex.find(transport.fromGroup);
        const auto to = groupIndex.find(transport.toGroup);
        if (from == groupIndex.end() || to == groupIndex.end()) continue;
        schedule.transportByGroup[static_cast<std::size_t>(from->second) * groups +
                                  static_cast<std::size_t>(to->second)] = transport.duration;
    }

    // Pins.
    schedule.pinOfRow.assign(static_cast<std::size_t>(row), -1);
    for (std::size_t p = 0; p < schedule.pinnedAssignments.size(); ++p) {
        const Assignment& pin = schedule.pinnedAssignments[p];
        checkReference(schedule, pin, "pinned assignment");
        const int pinnedRow = schedule.rowOf(pin.lot, pin.operation);
        const MachineOption* option = schedule.operationOfRow(pinnedRow).lastOptionOn(pin.machine);
        if (option == nullptr || pin.end - pin.start != option->duration)
            invalid("pinned assignments must use an eligible machine and duration");
        if (schedule.pinOfRow[static_cast<std::size_t>(pinnedRow)] >= 0)
            invalid("an operation may only be pinned once");
        schedule.pinOfRow[static_cast<std::size_t>(pinnedRow)] = static_cast<int>(p);
    }

    schedule.rowsUsingResource.assign(schedule.auxiliaryResources.size(), {});
    for (int r = 0; r < row; ++r) {
        for (int resource : schedule.operationOfRow(r).requiredResources)
            schedule.rowsUsingResource[static_cast<std::size_t>(resource)].push_back(r);
    }

    validateAssignments(schedule, schedule.assignments);
    validateAssignments(schedule, schedule.seedAssignments);
    schedule.prepared = true;
}

RawTotals rawCostsOf(const SemiconductorSchedule& schedule,
                     const std::vector<Assignment>& assignments, RawItems* items) {
    auto list = [items](Term term) -> std::vector<std::int64_t>* {
        return items == nullptr ? nullptr : &(*items)[termIndex(term)];
    };
    RawTotals total{};
    auto add = [&total](Term term, std::int64_t value) {
        total[termIndex(term)] = checked::add(total[termIndex(term)], value);
    };

    const int n = schedule.rowCount();
    std::vector<ScorePlacement> placements(assignments.size());
    std::vector<const ScorePlacement*> ofRow(static_cast<std::size_t>(n), nullptr);
    std::vector<PlacementList> byMachine(schedule.machines.size());
    std::vector<PlacementList> byResource(schedule.auxiliaryResources.size());
    std::unordered_map<std::string, std::int64_t> batchIds;
    for (std::size_t i = 0; i < assignments.size(); ++i) {
        const Assignment& a = assignments[i];
        ScorePlacement& p = placements[i];
        p.row = schedule.rowOf(a.lot, a.operation);
        p.machine = a.machine;
        p.start = a.start;
        p.end = a.end;
        p.seq = static_cast<std::int64_t>(i);
        if (a.batchId && !a.batchId->empty()) {
            auto [it, inserted] =
                    batchIds.try_emplace(*a.batchId, static_cast<std::int64_t>(batchIds.size()));
            p.batchKey = {true, it->second, 0};
        } else {
            p.batchKey = {false, a.start, 0};
        }
    }
    for (const ScorePlacement& p : placements) {
        ofRow[static_cast<std::size_t>(p.row)] = &p;
        byMachine[static_cast<std::size_t>(p.machine)].push_back(&p);
        for (int resource : schedule.operationOfRow(p.row).requiredResources)
            byResource[static_cast<std::size_t>(resource)].push_back(&p);
    }

    for (int row = 0; row < n; ++row) {
        const ScorePlacement* p = ofRow[static_cast<std::size_t>(row)];
        add(Term::MachineEligibilityCost,
            terms::emit(list(Term::MachineEligibilityCost),
                        MachineEligibilityCost::rowCost(schedule, row, p)));
        add(Term::MachinePreferenceCost,
            terms::emit(list(Term::MachinePreferenceCost),
                        MachinePreferenceCost::rowCost(schedule, row, p)));
    }

    std::vector<Time> completions;
    for (int lot = 0; lot < static_cast<int>(schedule.lots.size()); ++lot) {
        const Lot& l = schedule.lots[static_cast<std::size_t>(lot)];
        const ScorePlacement* const* route = ofRow.data() + l.firstRow;
        add(Term::LotRoutePrecedenceCost,
            LotRoutePrecedenceCost::lotCost(schedule, lot, route,
                                            list(Term::LotRoutePrecedenceCost)));
        add(Term::MaxQueueTimeCost,
            MaxQueueTimeCost::lotCost(schedule, lot, route, list(Term::MaxQueueTimeCost)));
        add(Term::MinQueueTimeCost,
            MinQueueTimeCost::lotCost(schedule, lot, route, list(Term::MinQueueTimeCost)));
        add(Term::MissingTransportTimeCost,
            MissingTransportTimeCost::lotCost(schedule, lot, route,
                                              list(Term::MissingTransportTimeCost)));
        add(Term::WeightedTardinessCost,
            WeightedTardinessCost::lotCost(schedule, lot, route,
                                           list(Term::WeightedTardinessCost)));
        add(Term::CycleTimeCost,
            CycleTimeCost::lotCost(schedule, lot, route, list(Term::CycleTimeCost)));
        if (const ScorePlacement* last = route[l.operations.size() - 1])
            completions.push_back(last->end);
    }
    for (int lot = 0; lot < static_cast<int>(schedule.lots.size()); ++lot) {
        const ScorePlacement* const* route =
                ofRow.data() + schedule.lots[static_cast<std::size_t>(lot)].firstRow;
        add(Term::MaxCycleTimeCost,
            MaxCycleTimeCost::lotCost(schedule, lot, route, list(Term::MaxCycleTimeCost)));
    }

    for (int resource = 0; resource < static_cast<int>(schedule.auxiliaryResources.size());
         ++resource) {
        add(Term::AuxiliaryResourceCapacityCost,
            AuxiliaryResourceCapacityCost::resourceCost(
                    schedule, resource, byResource[static_cast<std::size_t>(resource)],
                    list(Term::AuxiliaryResourceCapacityCost)));
    }
    for (int resource = 0; resource < static_cast<int>(schedule.auxiliaryResources.size());
         ++resource) {
        add(Term::AuxiliaryResourceUnavailabilityCost,
            AuxiliaryResourceUnavailabilityCost::resourceCost(
                    schedule, resource, byResource[static_cast<std::size_t>(resource)],
                    list(Term::AuxiliaryResourceUnavailabilityCost)));
    }
    for (int pin = 0; pin < static_cast<int>(schedule.pinnedAssignments.size()); ++pin) {
        const Assignment& expected = schedule.pinnedAssignments[static_cast<std::size_t>(pin)];
        const ScorePlacement* actual =
                ofRow[static_cast<std::size_t>(schedule.rowOf(expected.lot, expected.operation))];
        add(Term::PinnedAssignmentsCost,
            terms::emit(list(Term::PinnedAssignmentsCost),
                        PinnedAssignmentsCost::pinCost(schedule, pin, actual)));
    }

    std::vector<std::int64_t> load(schedule.machines.size(), 0);
    for (int machine = 0; machine < static_cast<int>(schedule.machines.size()); ++machine) {
        PlacementList& rows = byMachine[static_cast<std::size_t>(machine)];
        terms::sortMachineRows(schedule, rows);
        add(Term::MachineCapacityCost,
            MachineCapacityCost::machineCost(schedule, machine, rows,
                                             list(Term::MachineCapacityCost)));
        add(Term::BatchCapacityCost,
            BatchCapacityCost::machineCost(schedule, machine, rows, list(Term::BatchCapacityCost)));
        add(Term::BatchFamilyConflictCost,
            BatchFamilyConflictCost::machineCost(schedule, machine, rows,
                                                 list(Term::BatchFamilyConflictCost)));
        add(Term::MinBatchSizeCost,
            MinBatchSizeCost::machineCost(schedule, machine, rows, list(Term::MinBatchSizeCost)));
        add(Term::MachineUnavailabilityCost,
            MachineUnavailabilityCost::machineCost(schedule, machine, rows,
                                                   list(Term::MachineUnavailabilityCost)));
        add(Term::MissingSetupTimeCost,
            MissingSetupTimeCost::machineCost(schedule, machine, rows,
                                              list(Term::MissingSetupTimeCost)));
        load[static_cast<std::size_t>(machine)] = MachineLoadImbalanceCost::machineLoad(rows);
    }
    for (int group = 0; group < static_cast<int>(schedule.groupNames.size()); ++group) {
        std::vector<std::int64_t> loads;
        for (std::size_t m = 0; m < schedule.machines.size(); ++m) {
            if (schedule.machines[m].groupIndex == group) loads.push_back(load[m]);
        }
        add(Term::MachineLoadImbalanceCost,
            MachineLoadImbalanceCost::groupCost(loads, list(Term::MachineLoadImbalanceCost)));
    }
    add(Term::MakespanCost, MakespanCost::scheduleCost(completions, list(Term::MakespanCost)));
    return total;
}

RawTotals ScoreCalculator::rawCosts(const SemiconductorSchedule& schedule, RawItems* items) {
    return rawCostsOf(schedule, schedule.assignments, items);
}

RawTotals ScoreCalculator::searchStateRawCosts(const SemiconductorSchedule& schedule) {
    if (schedule.seedComplete && schedule.seedValues.size() == schedule.rows.size()) {
        bool seed = true;
        for (std::size_t r = 0; r < schedule.rows.size() && seed; ++r)
            seed = schedule.rows[r].value == schedule.seedValues[r];
        if (seed) return rawCostsOf(schedule, schedule.seedAssignments);
    }
    return rawCostsOf(schedule, decodeValues(schedule));
}

Score ScoreCalculator::scoreOf(const RawTotals& raw) {
    std::int64_t hard = 0;
    for (int t = 0; t < kHardTermCount; ++t)
        hard = checked::add(hard, raw[static_cast<std::size_t>(t)]);
    // Soft in double, in list order, the way the plugin assembly sums its
    // globals (weight * total): no int64 product to overflow, and the same
    // score bit for bit.
    double soft = 0.0;
    for (const auto& [weight, term] : {std::pair{kPreferenceWeight, Term::MachinePreferenceCost},
                                       std::pair{kMinBatchWeight, Term::MinBatchSizeCost},
                                       std::pair{kLoadWeight, Term::MachineLoadImbalanceCost},
                                       std::pair{kTardinessWeight, Term::WeightedTardinessCost},
                                       std::pair{kCycleWeight, Term::CycleTimeCost},
                                       std::pair{kMakespanWeight, Term::MakespanCost}})
        soft += weight * static_cast<double>(raw[termIndex(term)]);
    return {-hard, soft == 0.0 ? 0.0 : -soft};
}

void ScoreCalculator::storeTotals(SemiconductorSchedule& schedule, const RawTotals& raw) {
    schedule.makespan = raw[termIndex(Term::MakespanCost)];
    schedule.totalWeightedTardiness = raw[termIndex(Term::WeightedTardinessCost)];
    schedule.totalCycleTime = raw[termIndex(Term::CycleTimeCost)];
}

Score ScoreCalculator::score(SemiconductorSchedule& schedule) {
    prepare(schedule);
    const RawTotals raw = rawCosts(schedule);
    schedule.score = scoreOf(raw);
    schedule.scored = true;
    storeTotals(schedule, raw);
    return schedule.score;
}

Score ScoreCalculator::searchStateScore(const SemiconductorSchedule& schedule) {
    return scoreOf(searchStateRawCosts(schedule));
}

}  // namespace fab
