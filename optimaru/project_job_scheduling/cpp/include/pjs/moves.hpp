#ifndef PJS_MOVES_HPP
#define PJS_MOVES_HPP

#include <algorithm>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "optimaru/search/operators.hpp"
#include "pjs/domain.hpp"

namespace pjs {

// Rebuilds the whole schedule from an order of its allocations: the serial
// schedule generation scheme of resource-constrained project scheduling.
//
// The built-in moves change one delay (or one mode) at a time. A delay counts
// from the end of the predecessors, so one change shifts every successor, and
// a schedule in which each allocation already starts as early as it can has
// almost no single change that keeps the resources within capacity and ends
// earlier: the search stalls a few percent above what the instance allows.
//
// This move works on the order instead. It reads the start dates of the
// working schedule, changes the order they give, and places the allocations
// again in that order, each as early as its predecessors and the renewable
// resources allow. The changes of order, one per move:
//   - one allocation moves next to another, or the two trade places: mostly
//     a few places apart (kReach), now and then anywhere (kFarOdds);
//   - a few neighbours in the order trade places at random (kShuffleOdds);
//   - a group moves by a number of days: an allocation with everything
//     before it, or after it, in the precedence graph, or a whole project
//     (kGroupOdds, kMaxShift);
//   - one allocation switches to another of its execution modes, half the
//     time together with a second one of its project;
//   - none: the order is the one of the schedule pushed to the right
//     (kJustifyOdds, justifyRight), the forward-backward pass.
// The result is written back as delays. A schedule within capacity, placed
// again in its own order, ends nowhere later; so what the acceptor judges is
// the change of order alone, and a rebuilt schedule never exceeds a renewable
// capacity unless a delay hits the end of the delay range.
//
// With several projects, the ones that end after their critical path are
// first in the order, and half the moves pick their allocation there: the
// delay of a project is scored above the makespan, and a project that is not
// late loses nothing by waiting for one that is.
//
// Pinned allocations keep their value: their delay still counts from the end
// of their predecessors, and they take their resources whatever is left.
// Non-renewable resources depend on the modes only and are left to the
// score. The move does nothing while an allocation is unassigned (inside a
// ruin).
//
// Cost per move: a few passes over the allocations (the dates, a sort, the
// placement, which scans the days of the profile from each allocation's
// earliest start), then one assignment per allocation whose value changed.
// Solver::Config::custom_move_probability caps how often a step draws it.
class SerialScheduleMove final : public optimaru::MoveOperator<ProjectJobSchedule> {
public:
    // How far apart in the order the two allocations of a move mostly are.
    static constexpr int kReach = 8;
    // One move in this many takes the order of the schedule pushed to the
    // right (justifyRight) and changes nothing else.
    static constexpr int kJustifyOdds = 8;
    // Of the changes of order: one in this many moves a group, by up to
    // this many days; one in this many of the rest shuffles a few
    // neighbours; one in this many of what is left reaches anywhere.
    static constexpr int kGroupOdds = 4;
    static constexpr int kMaxShift = 64;
    static constexpr int kShuffleOdds = 6;
    static constexpr int kFarOdds = 6;
    // Draws to find a second allocation of the project that has several
    // modes, and to find an allocation of a late project.
    static constexpr int kSecondModeTries = 16;
    static constexpr int kLateTries = 8;
    // What the allocations of a late project are ahead by in the order:
    // more than twice any start date.
    static constexpr std::int64_t kUrgent = std::int64_t{1} << 40;

    const char* name() const override { return "serialSchedule"; }

    bool apply(optimaru::OperatorContext<ProjectJobSchedule>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        ProjectJobSchedule& schedule = ctx.editor.solution();
        const int count = schedule.allocationCount();
        if (count == 0) return false;
        using Traits = optimaru::SolutionTraits<ProjectJobSchedule>;

        // The working schedule: values, dates, and the order of its starts.
        value_.resize(count);
        start_.resize(count);
        end_.resize(count);
        movable_.clear();
        for (int a : schedule.topologicalOrder) {
            const Allocation& allocation = schedule.allocations[a];
            if (allocation.modeDelay < 0) return false;  // inside a ruin
            int done = 0;
            for (int p : allocation.predecessorAllocations) done = std::max(done, end_[p]);
            value_[a] = allocation.modeDelay;
            start_[a] = done + schedule.delayOfValue(allocation.modeDelay);
            end_[a] = start_[a] + schedule.executionModeOf(a, allocation.modeDelay).duration;
            if (!Traits::is_pinned(schedule, a)) movable_.push_back(a);
        }
        if (movable_.size() < 2) return false;
        if (position_.size() != static_cast<std::size_t>(count)) {
            position_.resize(count);
            for (int i = 0; i < count; ++i) position_[schedule.topologicalOrder[i]] = i;
        }
        key_.resize(count);
        for (int a = 0; a < count; ++a) key_[a] = 2 * static_cast<std::int64_t>(start_[a]);
        // The projects that end after their critical path go first.
        bool anyLate = false;
        if (schedule.projects.size() > 1) {
            late_project_.assign(schedule.projects.size(), 0);
            for (int a = 0; a < count; ++a) {
                if (schedule.jobTypeOf(a) != JobType::SINK) continue;
                if (end_[a] > schedule.projectCriticalPathEndDateOf(a)) {
                    late_project_[schedule.jobs[schedule.allocations[a].job].project] = 1;
                    anyLate = true;
                }
            }
            for (int a = 0; a < count; ++a) {
                if (late_project_[schedule.jobs[schedule.allocations[a].job].project])
                    key_[a] -= kUrgent;
            }
        }
        // Ties go by the topological position, which keeps a zero-duration
        // predecessor ahead of its successor.
        std::sort(movable_.begin(), movable_.end(), [this](int a, int b) {
            return key_[a] != key_[b] ? key_[a] < key_[b] : position_[a] < position_[b];
        });

        // The change of order. Half the moves are about a late project when
        // there is one: its delay is what the score looks at first.
        const int size = static_cast<int>(movable_.size());
        int at = static_cast<int>(rng() % static_cast<std::uint64_t>(size));
        if (anyLate && (rng() & 1u) != 0) {
            for (int attempt = 0; attempt < kLateTries; ++attempt) {
                if (late_project_[schedule.jobs[schedule.allocations[movable_[at]].job].project])
                    break;
                at = static_cast<int>(rng() % static_cast<std::uint64_t>(size));
            }
        }
        const int moved = movable_[at];
        const int modeCount = schedule.executionModeCountOf(moved);
        if (rng() % kJustifyOdds == 0) {
            justifyRight(schedule);
        } else if (modeCount > 1 && rng() % 3 == 0) {
            switchMode(schedule, moved, rng);
            // Half the time a second allocation of the project switches as
            // well: where a non-renewable resource is at its capacity, one
            // mode can take more of it only when another gives some back.
            if ((rng() & 1u) != 0) {
                const int project = schedule.jobs[schedule.allocations[moved].job].project;
                for (int attempt = 0; attempt < kSecondModeTries; ++attempt) {
                    const int other = movable_[rng() % static_cast<std::uint64_t>(size)];
                    if (other == moved || schedule.executionModeCountOf(other) < 2) continue;
                    if (schedule.jobs[schedule.allocations[other].job].project != project) continue;
                    switchMode(schedule, other, rng);
                    break;
                }
            }
        } else if (rng() % kGroupOdds == 0) {
            // A group moves together in the order, by a number of days: the
            // allocation with everything before it (or after it) in the
            // precedence graph, or its whole project.
            std::int64_t shift = 2;
            while (shift < 2 * kMaxShift && (rng() & 1u) != 0) shift *= 2;
            const bool earlier = (rng() & 1u) != 0;
            if (earlier) shift = -shift;
            if (schedule.projects.size() > 1 && (rng() & 1u) != 0) {
                const int project = schedule.jobs[schedule.allocations[moved].job].project;
                for (int a = 0; a < count; ++a) {
                    if (schedule.jobs[schedule.allocations[a].job].project == project)
                        key_[a] += shift;
                }
            } else {
                seen_.assign(count, 0);
                stack_.assign(1, moved);
                seen_[moved] = 1;
                while (!stack_.empty()) {
                    const int a = stack_.back();
                    stack_.pop_back();
                    key_[a] += shift;
                    const std::vector<int>& next =
                            earlier ? schedule.allocations[a].predecessorAllocations
                                    : schedule.successorAllocations[a];
                    for (int b : next) {
                        if (!seen_[b]) {
                            seen_[b] = 1;
                            stack_.push_back(b);
                        }
                    }
                }
            }
        } else if (rng() % kShuffleOdds == 0) {
            // A few neighbours in the order trade their places at random.
            const int span = std::min(size - at, 3 + static_cast<int>(rng() % 3));
            for (int i = span - 1; i > 0; --i) {
                const int j = static_cast<int>(rng() % static_cast<std::uint64_t>(i + 1));
                std::swap(key_[movable_[at + i]], key_[movable_[at + j]]);
            }
        } else {
            int other_at;
            if (rng() % kFarOdds == 0) {
                other_at = static_cast<int>(rng() % static_cast<std::uint64_t>(size));
                if (other_at == at) return false;
            } else {
                int distance = 1;
                while (distance < kReach && (rng() & 1u) != 0) ++distance;
                other_at = (rng() & 1u) != 0 ? at + distance : at - distance;
                if (other_at < 0 || other_at >= size) return false;
            }
            const int other = movable_[other_at];
            if (rng() % 3 == 0) {
                std::swap(key_[moved], key_[other]);
            } else {
                // Next to the other one, on its far side.
                key_[moved] = key_[other] + (other_at > at ? 1 : -1);
            }
        }

        // The placement: among the allocations whose predecessors are all
        // placed, the one first in the order goes next.
        pending_.resize(count);
        ready_.clear();
        for (int a = 0; a < count; ++a) {
            pending_[a] = static_cast<int>(schedule.allocations[a].predecessorAllocations.size());
            if (pending_[a] == 0) ready_.push_back(a);
        }
        const auto later = [this](int a, int b) {
            return key_[a] != key_[b] ? key_[a] > key_[b] : position_[a] > position_[b];
        };
        std::make_heap(ready_.begin(), ready_.end(), later);
        const int resourceCount = static_cast<int>(schedule.resources.size());
        if (static_cast<int>(usage_.size()) != resourceCount) usage_.assign(resourceCount, {});
        for (std::vector<int>& days : usage_) std::fill(days.begin(), days.end(), 0);
        const int lastDelay = schedule.delayRangeSize - 1;
        planned_.resize(count);
        while (!ready_.empty()) {
            std::pop_heap(ready_.begin(), ready_.end(), later);
            const int a = ready_.back();
            ready_.pop_back();
            const Allocation& allocation = schedule.allocations[a];
            int done = 0;
            for (int p : allocation.predecessorAllocations) done = std::max(done, end_[p]);
            const ExecutionMode& mode = schedule.executionModeOf(a, value_[a]);
            int delay = schedule.delayOfValue(value_[a]);
            if (!Traits::is_pinned(schedule, a)) {
                delay = earliestDelay(schedule, mode, done, lastDelay);
            }
            start_[a] = done + delay;
            end_[a] = start_[a] + mode.duration;
            occupy(schedule, mode, start_[a], end_[a]);
            planned_[a] = schedule.valueOf(delay, schedule.modeSlotOfValue(value_[a]));
            for (int successor : schedule.successorAllocations[a]) {
                if (--pending_[successor] == 0) {
                    ready_.push_back(successor);
                    std::push_heap(ready_.begin(), ready_.end(), later);
                }
            }
        }

        changed_.clear();
        ctx.editor.begin_batch();
        for (int a : schedule.topologicalOrder) {
            const int before = schedule.allocations[a].modeDelay;
            if (before == planned_[a]) continue;
            changed_.push_back({a, before});
            ctx.editor.assign(a, planned_[a]);
            entities.push_back(a);
        }
        ctx.editor.end_batch();
        return !changed_.empty();
    }

    void undo_last(optimaru::OperatorContext<ProjectJobSchedule>& ctx) override {
        ctx.editor.begin_batch();
        for (auto it = changed_.rbegin(); it != changed_.rend(); ++it) {
            ctx.editor.assign(it->first, it->second);
        }
        ctx.editor.end_batch();
    }

private:
    // Another execution mode of the allocation, at random.
    void switchMode(const ProjectJobSchedule& schedule, int allocation, std::mt19937_64& rng) {
        const int modeCount = schedule.executionModeCountOf(allocation);
        const int current = schedule.executionModeIndexOfValue(allocation, value_[allocation]);
        const int next = (current + 1 +
                          static_cast<int>(rng() % static_cast<std::uint64_t>(modeCount - 1))) %
                         modeCount;
        value_[allocation] = schedule.valueOf(schedule.delayOfValue(value_[allocation]), next);
    }

    // The order of the schedule pushed to the right: from the last end back,
    // each allocation that is not pinned goes as late as its successors and
    // the renewable resources allow, never earlier than it is. Placing the
    // allocations again in the order of those starts is the second half of a
    // forward-backward pass: the schedule ends no later, and often earlier,
    // since what was started early for no reason now waits behind what the
    // end depends on.
    void justifyRight(const ProjectJobSchedule& schedule) {
        using Traits = optimaru::SolutionTraits<ProjectJobSchedule>;
        const int count = schedule.allocationCount();
        byEnd_.resize(count);
        for (int a = 0; a < count; ++a) byEnd_[a] = a;
        std::sort(byEnd_.begin(), byEnd_.end(), [this](int a, int b) {
            return end_[a] != end_[b] ? end_[a] > end_[b] : position_[a] > position_[b];
        });
        const int resourceCount = static_cast<int>(schedule.resources.size());
        if (static_cast<int>(usage_.size()) != resourceCount) usage_.assign(resourceCount, {});
        for (std::vector<int>& days : usage_) std::fill(days.begin(), days.end(), 0);
        late_.resize(count);
        for (int a : byEnd_) {
            const ExecutionMode& mode = schedule.executionModeOf(a, value_[a]);
            int start = start_[a];
            if (!Traits::is_pinned(schedule, a) && !schedule.successorAllocations[a].empty()) {
                int limit = late_[schedule.successorAllocations[a].front()];
                for (int successor : schedule.successorAllocations[a]) {
                    limit = std::min(limit, late_[successor]);
                }
                start = latestStart(schedule, mode, start_[a], limit - mode.duration);
            }
            late_[a] = start;
            occupy(schedule, mode, start, start + mode.duration);
            // In the order of the pushed starts, a late project still first.
            key_[a] += 2 * static_cast<std::int64_t>(start - start_[a]);
        }
    }

    // The latest start in [earliest, latest] at which the mode fits the
    // renewable profile for its whole duration; `earliest` when none does.
    int latestStart(const ProjectJobSchedule& schedule, const ExecutionMode& mode, int earliest,
                    int latest) const {
        if (mode.duration == 0) return std::max(earliest, latest);
        int start = latest;
        while (start > earliest) {
            int blocked = -1;  // the first day of the span that is full
            for (const ResourceRequirement& requirement : mode.resourceRequirements) {
                const Resource& resource = schedule.resources[requirement.resource];
                const int need = needOf(mode, requirement.resource);
                if (!resource.renewable || need > resource.capacity) continue;
                const std::vector<int>& days = usage_[requirement.resource];
                const int room = resource.capacity - need;
                const int end = std::min(start + mode.duration, static_cast<int>(days.size()));
                for (int day = start; day < end && (blocked < 0 || day < blocked); ++day) {
                    if (days[day] > room) {
                        blocked = day;
                        break;
                    }
                }
            }
            if (blocked < 0) return start;
            start = blocked - mode.duration;
        }
        return earliest;
    }

    // The smallest delay, up to lastDelay, at which the mode fits the
    // renewable profile for its whole duration from `done` on; lastDelay
    // when none does. A requirement above its capacity fits nowhere and is
    // not looked at: the score counts it wherever the allocation goes.
    int earliestDelay(const ProjectJobSchedule& schedule, const ExecutionMode& mode, int done,
                      int lastDelay) const {
        if (mode.duration == 0) return 0;
        int delay = 0;
        while (delay < lastDelay) {
            const int start = done + delay;
            int blocked = -1;  // the last day of the span that is full
            for (const ResourceRequirement& requirement : mode.resourceRequirements) {
                const Resource& resource = schedule.resources[requirement.resource];
                const int need = needOf(mode, requirement.resource);
                if (!resource.renewable || need > resource.capacity) continue;
                const std::vector<int>& days = usage_[requirement.resource];
                const int room = resource.capacity - need;
                const int last = std::min(start + mode.duration, static_cast<int>(days.size())) - 1;
                for (int day = last; day >= start && day > blocked; --day) {
                    if (days[day] > room) {
                        blocked = day;
                        break;
                    }
                }
            }
            if (blocked < 0) return delay;
            delay = blocked + 1 - done;
        }
        return lastDelay;
    }

    // What the mode takes of the resource: it may name it in several
    // requirements, and the score adds them.
    static int needOf(const ExecutionMode& mode, int resource) {
        int need = 0;
        for (const ResourceRequirement& requirement : mode.resourceRequirements) {
            if (requirement.resource == resource) need += requirement.requirement;
        }
        return need;
    }

    void occupy(const ProjectJobSchedule& schedule, const ExecutionMode& mode, int start, int end) {
        if (end <= start) return;
        for (const ResourceRequirement& requirement : mode.resourceRequirements) {
            if (!schedule.resources[requirement.resource].renewable) continue;
            std::vector<int>& days = usage_[requirement.resource];
            if (static_cast<int>(days.size()) < end) days.resize(end, 0);
            for (int day = start; day < end; ++day) days[day] += requirement.requirement;
        }
    }

    // Scratch, reused across moves (no per-move allocation).
    std::vector<int> value_;               // per allocation: its value, with the changed mode
    std::vector<int> start_;               // per allocation: start, first as read, then as placed
    std::vector<int> end_;                 // per allocation: end, likewise
    std::vector<std::int64_t> key_;        // per allocation: its place in the order
    std::vector<int> position_;            // per allocation: its topological position
    std::vector<int> movable_;             // the allocations that are not pinned, by start
    std::vector<int> pending_;             // per allocation: predecessors not placed yet
    std::vector<int> ready_;               // heap of the allocations that can be placed
    std::vector<std::vector<int>> usage_;  // [resource][day], renewable resources
    std::vector<int> planned_;             // per allocation: its value after the move
    std::vector<int> byEnd_;               // the allocations, the last end first
    std::vector<char> seen_;               // per allocation: in the group that moves
    std::vector<char> late_project_;       // per project: ends after its critical path
    std::vector<int> stack_;               // the walk over the precedence graph
    std::vector<int> late_;                // per allocation: its start pushed to the right
    std::vector<std::pair<int, int>> changed_;  // (allocation, value before), for the undo
};

}  // namespace pjs

#endif  // PJS_MOVES_HPP
