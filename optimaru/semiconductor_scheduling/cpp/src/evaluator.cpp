#include "fab/evaluator.hpp"

#include <algorithm>

#include "fab/constraints.hpp"

namespace fab {

namespace {

template <typename T>
void markOnce(std::vector<char>& flag, std::vector<T>& list, int index) {
    if (!flag[static_cast<std::size_t>(index)]) {
        flag[static_cast<std::size_t>(index)] = 1;
        list.push_back(index);
    }
}

}  // namespace

void ScheduleEvaluator::reset(const SemiconductorSchedule& schedule) {
    schedule_ = &schedule;
    dataMode_ = !schedule.searching;
    totals_.fill(0);
    if (dataMode_) {
        dataTotals_ = rawCostsOf(schedule, schedule.assignments);
        return;
    }
    const std::size_t n = schedule.rows.size();
    const std::size_t machines = schedule.machines.size();
    const std::size_t resources = schedule.auxiliaryResources.size();

    seedTotals_.fill(0);
    if (schedule.seedComplete) seedTotals_ = rawCostsOf(schedule, schedule.seedAssignments);

    decoder_.bind(schedule);
    valuesToState(schedule, option_, rank_);
    decodedValue_.resize(n);
    matchesSeed_.assign(n, 0);
    seedMismatches_ = 0;
    for (std::size_t r = 0; r < n; ++r) {
        decodedValue_[r] = schedule.rows[r].value;
        const bool matches = schedule.seedComplete && r < schedule.seedValues.size() &&
                             schedule.rows[r].value == schedule.seedValues[r];
        matchesSeed_[r] = matches ? 1 : 0;
        if (!matches) ++seedMismatches_;
    }
    pending_.clear();
    isPending_.assign(n, 0);
    decodeFrom_ = 0;  // decoded lazily: a complete seed never is
    decodedSteps_ = 0;

    // Nothing placed yet: every unit is dirty against all-zero caches.
    place_.assign(n, ScorePlacement{});
    placed_.assign(n, 0);
    machineRows_.assign(machines, {});
    positionOnMachine_.assign(n, -1);
    machinesOfGroup_.assign(schedule.groupNames.size(), {});
    for (std::size_t m = 0; m < machines; ++m)
        machinesOfGroup_[static_cast<std::size_t>(schedule.machines[m].groupIndex)].push_back(
                static_cast<int>(m));

    rowEligibility_.assign(n, 0);
    rowPreference_.assign(n, 0);
    lot_.assign(schedule.lots.size(), LotCache{});
    machine_.assign(machines, MachineCache{});
    resourceCapacity_.assign(resources, 0);
    resourceUnavailability_.assign(resources, 0);
    pin_.assign(schedule.pinnedAssignments.size(), 0);
    group_.assign(schedule.groupNames.size(), 0);

    dirtyRow_.assign(n, 0);
    dirtyLot_.assign(schedule.lots.size(), 0);
    dirtyMachine_.assign(machines, 0);
    dirtyResource_.assign(resources, 0);
    dirtyPin_.assign(schedule.pinnedAssignments.size(), 0);
    dirtyGroup_.assign(schedule.groupNames.size(), 0);
    dirtyRows_.clear();
    dirtyLots_.clear();
    dirtyMachines_.clear();
    dirtyResources_.clear();
    dirtyPins_.clear();
    dirtyGroups_.clear();
    for (std::size_t r = 0; r < n; ++r) markOnce(dirtyRow_, dirtyRows_, static_cast<int>(r));
    for (std::size_t l = 0; l < schedule.lots.size(); ++l)
        markOnce(dirtyLot_, dirtyLots_, static_cast<int>(l));
    for (std::size_t m = 0; m < machines; ++m)
        markOnce(dirtyMachine_, dirtyMachines_, static_cast<int>(m));
    for (std::size_t r = 0; r < resources; ++r)
        markOnce(dirtyResource_, dirtyResources_, static_cast<int>(r));
    for (std::size_t p = 0; p < schedule.pinnedAssignments.size(); ++p)
        markOnce(dirtyPin_, dirtyPins_, static_cast<int>(p));
    for (std::size_t g = 0; g < schedule.groupNames.size(); ++g)
        markOnce(dirtyGroup_, dirtyGroups_, static_cast<int>(g));
    dirtyMakespan_ = true;

    touchedMark_.assign(n, 0);
}

void ScheduleEvaluator::markChanged(int row) {
    if (dataMode_) return;
    markOnce(isPending_, pending_, row);
}

void ScheduleEvaluator::markAllChanged() {
    if (dataMode_ || schedule_ == nullptr) return;
    for (int r = 0; r < schedule_->rowCount(); ++r) markChanged(r);
}

void ScheduleEvaluator::applyPending() {
    const SemiconductorSchedule& schedule = *schedule_;
    for (int row : pending_) {
        isPending_[static_cast<std::size_t>(row)] = 0;
        const int value = schedule.rows[static_cast<std::size_t>(row)].value;
        const bool matches = schedule.seedComplete &&
                             value == schedule.seedValues[static_cast<std::size_t>(row)];
        if (matches != static_cast<bool>(matchesSeed_[static_cast<std::size_t>(row)])) {
            seedMismatches_ += matches ? -1 : 1;
            matchesSeed_[static_cast<std::size_t>(row)] = matches ? 1 : 0;
        }
        if (value != decodedValue_[static_cast<std::size_t>(row)]) {
            // Steps before this row's readyStep (in the last decode) never
            // read it, so they stand.
            decodeFrom_ = std::min(decodeFrom_, decoder_.readyStep(row));
            decodedValue_[static_cast<std::size_t>(row)] = value;
            option_[static_cast<std::size_t>(row)] = schedule.optionIndexOf(row, value);
            rank_[static_cast<std::size_t>(row)] = value < 0 ? 0 : schedule.rankOf(value);
        }
    }
    pending_.clear();
}

const RawTotals& ScheduleEvaluator::totals() {
    if (dataMode_) return dataTotals_;
    applyPending();
    if (schedule_->seedComplete && seedMismatches_ == 0) return seedTotals_;
    if (decodeFrom_ != Decoder::kNever) {
        redecode(decodeFrom_);
        decodeFrom_ = Decoder::kNever;
    }
    recost();
    return totals_;
}

void ScheduleEvaluator::redecode(int fromStep) {
    const int step = std::min(fromStep, decoder_.stepCount());
    const int first = decoder_.outputStartOf(step);
    touched_.clear();
    const std::vector<int>& output = decoder_.output();
    for (std::size_t i = static_cast<std::size_t>(first); i < output.size(); ++i)
        markOnce(touchedMark_, touched_, output[i]);
    decoder_.truncate(step);
    decoder_.run(option_.data(), rank_.data());
    decodedSteps_ += decoder_.stepCount() - step;
    for (std::size_t i = static_cast<std::size_t>(first); i < decoder_.output().size(); ++i)
        markOnce(touchedMark_, touched_, decoder_.output()[i]);
    for (int row : touched_) {
        touchedMark_[static_cast<std::size_t>(row)] = 0;
        updatePlacement(row);
    }
}

void ScheduleEvaluator::updatePlacement(int row) {
    const std::size_t r = static_cast<std::size_t>(row);
    const Decoder::Placement& d = decoder_.placement(row);
    const bool nowPlaced = d.step >= 0;
    const bool wasPlaced = placed_[r] != 0;
    ScorePlacement& p = place_[r];
    if (nowPlaced == wasPlaced && (!nowPlaced || (p.machine == d.machine && p.start == d.start &&
                                                  p.end == d.end && p.seq == d.seq)))
        return;
    if (wasPlaced) {
        std::vector<int>& list = machineRows_[static_cast<std::size_t>(p.machine)];
        const int position = positionOnMachine_[r];
        const int last = list.back();
        list[static_cast<std::size_t>(position)] = last;
        positionOnMachine_[static_cast<std::size_t>(last)] = position;
        list.pop_back();
        positionOnMachine_[r] = -1;
        markMachineDirty(p.machine);
    }
    if (nowPlaced) {
        p.row = row;
        p.machine = d.machine;
        p.start = d.start;
        p.end = d.end;
        p.seq = d.seq;
        // A decoded batch is the items of one machine with equal (family,
        // start, end), which is the partition the batch ids give.
        p.batchKey = {true, d.start, d.end};
        std::vector<int>& list = machineRows_[static_cast<std::size_t>(p.machine)];
        positionOnMachine_[r] = static_cast<int>(list.size());
        list.push_back(row);
        markMachineDirty(p.machine);
    }
    placed_[r] = nowPlaced ? 1 : 0;
    markRowDirty(row);
}

void ScheduleEvaluator::markMachineDirty(int machine) {
    markOnce(dirtyMachine_, dirtyMachines_, machine);
    markOnce(dirtyGroup_, dirtyGroups_,
             schedule_->machines[static_cast<std::size_t>(machine)].groupIndex);
}

void ScheduleEvaluator::markRowDirty(int row) {
    const SemiconductorSchedule& schedule = *schedule_;
    markOnce(dirtyRow_, dirtyRows_, row);
    markOnce(dirtyLot_, dirtyLots_, schedule.rows[static_cast<std::size_t>(row)].lot);
    for (int resource : schedule.operationOfRow(row).requiredResources)
        markOnce(dirtyResource_, dirtyResources_, resource);
    const int pin = schedule.pinOfRow[static_cast<std::size_t>(row)];
    if (pin >= 0) markOnce(dirtyPin_, dirtyPins_, pin);
    dirtyMakespan_ = true;
}

void ScheduleEvaluator::recost() {
    const SemiconductorSchedule& schedule = *schedule_;
    auto move = [this](Term term, std::int64_t& cell, std::int64_t value) {
        std::int64_t& total = totals_[termIndex(term)];
        total = checked::add(checked::sub(total, cell), value);
        cell = value;
    };
    auto at = [this](int row) -> const ScorePlacement* {
        return placed_[static_cast<std::size_t>(row)] ? &place_[static_cast<std::size_t>(row)]
                                                      : nullptr;
    };

    for (int row : dirtyRows_) {
        dirtyRow_[static_cast<std::size_t>(row)] = 0;
        const ScorePlacement* p = at(row);
        move(Term::MachineEligibilityCost, rowEligibility_[static_cast<std::size_t>(row)],
             MachineEligibilityCost::rowCost(schedule, row, p));
        move(Term::MachinePreferenceCost, rowPreference_[static_cast<std::size_t>(row)],
             MachinePreferenceCost::rowCost(schedule, row, p));
    }
    dirtyRows_.clear();

    std::vector<const ScorePlacement*> route;
    for (int lot : dirtyLots_) {
        dirtyLot_[static_cast<std::size_t>(lot)] = 0;
        const Lot& l = schedule.lots[static_cast<std::size_t>(lot)];
        route.resize(l.operations.size());
        for (std::size_t i = 0; i < l.operations.size(); ++i)
            route[i] = at(l.firstRow + static_cast<int>(i));
        LotCache& c = lot_[static_cast<std::size_t>(lot)];
        move(Term::LotRoutePrecedenceCost, c.precedence,
             LotRoutePrecedenceCost::lotCost(schedule, lot, route.data(), nullptr));
        move(Term::MaxQueueTimeCost, c.maxQueue,
             MaxQueueTimeCost::lotCost(schedule, lot, route.data(), nullptr));
        move(Term::MinQueueTimeCost, c.minQueue,
             MinQueueTimeCost::lotCost(schedule, lot, route.data(), nullptr));
        move(Term::MissingTransportTimeCost, c.transport,
             MissingTransportTimeCost::lotCost(schedule, lot, route.data(), nullptr));
        move(Term::MaxCycleTimeCost, c.maxCycle,
             MaxCycleTimeCost::lotCost(schedule, lot, route.data(), nullptr));
        move(Term::WeightedTardinessCost, c.tardiness,
             WeightedTardinessCost::lotCost(schedule, lot, route.data(), nullptr));
        move(Term::CycleTimeCost, c.cycle,
             CycleTimeCost::lotCost(schedule, lot, route.data(), nullptr));
        const ScorePlacement* last = route.back();
        c.completed = last != nullptr;
        c.completion = last != nullptr ? last->end : 0;
    }
    dirtyLots_.clear();

    if (dirtyMakespan_) {
        std::vector<Time> completions;
        for (const LotCache& c : lot_) {
            if (c.completed) completions.push_back(c.completion);
        }
        std::int64_t makespan = MakespanCost::scheduleCost(completions, nullptr);
        totals_[termIndex(Term::MakespanCost)] = makespan;
        dirtyMakespan_ = false;
    }

    for (int resource : dirtyResources_) {
        dirtyResource_[static_cast<std::size_t>(resource)] = 0;
        scratch_.clear();
        for (int row : schedule.rowsUsingResource[static_cast<std::size_t>(resource)]) {
            if (const ScorePlacement* p = at(row)) scratch_.push_back(p);
        }
        move(Term::AuxiliaryResourceCapacityCost,
             resourceCapacity_[static_cast<std::size_t>(resource)],
             AuxiliaryResourceCapacityCost::resourceCost(schedule, resource, scratch_, nullptr));
        move(Term::AuxiliaryResourceUnavailabilityCost,
             resourceUnavailability_[static_cast<std::size_t>(resource)],
             AuxiliaryResourceUnavailabilityCost::resourceCost(schedule, resource, scratch_,
                                                               nullptr));
    }
    dirtyResources_.clear();

    for (int pin : dirtyPins_) {
        dirtyPin_[static_cast<std::size_t>(pin)] = 0;
        const Assignment& expected = schedule.pinnedAssignments[static_cast<std::size_t>(pin)];
        move(Term::PinnedAssignmentsCost, pin_[static_cast<std::size_t>(pin)],
             PinnedAssignmentsCost::pinCost(schedule, pin,
                                            at(schedule.rowOf(expected.lot, expected.operation))));
    }
    dirtyPins_.clear();

    for (int machine : dirtyMachines_) {
        dirtyMachine_[static_cast<std::size_t>(machine)] = 0;
        scratch_.clear();
        for (int row : machineRows_[static_cast<std::size_t>(machine)])
            scratch_.push_back(&place_[static_cast<std::size_t>(row)]);
        terms::sortMachineRows(schedule, scratch_);
        MachineCache& c = machine_[static_cast<std::size_t>(machine)];
        move(Term::MachineCapacityCost, c.capacity,
             MachineCapacityCost::machineCost(schedule, machine, scratch_, nullptr));
        move(Term::BatchCapacityCost, c.batchCapacity,
             BatchCapacityCost::machineCost(schedule, machine, scratch_, nullptr));
        move(Term::BatchFamilyConflictCost, c.familyConflict,
             BatchFamilyConflictCost::machineCost(schedule, machine, scratch_, nullptr));
        move(Term::MissingSetupTimeCost, c.setup,
             MissingSetupTimeCost::machineCost(schedule, machine, scratch_, nullptr));
        move(Term::MachineUnavailabilityCost, c.unavailability,
             MachineUnavailabilityCost::machineCost(schedule, machine, scratch_, nullptr));
        move(Term::MinBatchSizeCost, c.minBatch,
             MinBatchSizeCost::machineCost(schedule, machine, scratch_, nullptr));
        c.load = MachineLoadImbalanceCost::machineLoad(scratch_);
    }
    dirtyMachines_.clear();

    std::vector<std::int64_t> loads;
    for (int group : dirtyGroups_) {
        dirtyGroup_[static_cast<std::size_t>(group)] = 0;
        loads.clear();
        for (int machine : machinesOfGroup_[static_cast<std::size_t>(group)])
            loads.push_back(machine_[static_cast<std::size_t>(machine)].load);
        move(Term::MachineLoadImbalanceCost, group_[static_cast<std::size_t>(group)],
             MachineLoadImbalanceCost::groupCost(loads, nullptr));
    }
    dirtyGroups_.clear();
}

}  // namespace fab
