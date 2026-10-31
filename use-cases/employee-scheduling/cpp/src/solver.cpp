#include "es/solver.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "es/incremental_calculator.hpp"
#include "es/score_calculator.hpp"
#include "optimaru/api/score/director/score_director.hpp"
#include "optimaru/api/solver/solver.hpp"

namespace es {

namespace {

using Clock = std::chrono::steady_clock;

// Diagnostic: when ES_PHASE_TIMING is set, log wall time spent in each solve
// phase to stderr. Off by default (no output, negligible cost).
inline bool phaseTimingEnabled() {
    static const bool on = std::getenv("ES_PHASE_TIMING") != nullptr;
    return on;
}
inline void logPhase(const char* name, Clock::time_point start) {
    if (!phaseTimingEnabled()) return;
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::fprintf(stderr, "[phase-timing] %-24s %8.1f ms\n", name, ms);
}

// Split the total budget: local search gets spentLimit*(1-fraction), the rest
// is left for the greedy hard-repair (bounded by the solve-wide deadline).
inline std::chrono::milliseconds localSearchLimit(const Solver::Config& config) {
    const double fraction = std::clamp(config.greedyRepairBudgetFraction, 0.0, 0.9);
    const auto ms = static_cast<std::int64_t>(
            std::chrono::duration<double, std::milli>(config.spentLimit).count() * (1.0 - fraction));
    return std::chrono::milliseconds(ms);
}

using PenalizedGetter = std::function<const std::vector<int>&()>;
using ShiftsOfGetter = std::function<const std::vector<int>&(int)>;

// A pinned shift keeps its assignment fixed — moves must never pick one.
inline bool pinnedShift(const EmployeeSchedule& schedule, int i) {
    return i >= 0 && schedule.shifts[i].pinned;
}

// The engine's moves pass over a pinned shift only while another one is movable,
// so with every shift pinned there must be no search at all.
inline bool allShiftsPinned(const EmployeeSchedule& schedule) {
    return std::all_of(schedule.shifts.begin(), schedule.shifts.end(),
                       [](const Shift& shift) { return shift.pinned; });
}

int randomShiftFromHotEmployee(const EmployeeSchedule& schedule, const PenalizedGetter& penalized,
                               const ShiftsOfGetter& shiftsOf, std::mt19937_64& rng,
                               double probability) {
    if (schedule.shifts.empty()) return -1;
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const std::vector<int>& hot = penalized();
    if (!hot.empty() && unit(rng) < probability) {
        const std::vector<int>& list = shiftsOf(hot[rng() % hot.size()]);
        if (!list.empty()) {
            const int s = list[rng() % list.size()];
            if (!pinnedShift(schedule, s)) return s;
        }
    }
    const int n = static_cast<int>(schedule.shifts.size());
    for (int t = 0; t < 8; ++t) {
        const int s = static_cast<int>(rng() % static_cast<std::uint64_t>(n));
        if (!pinnedShift(schedule, s)) return s;
    }
    for (int s = 0; s < n; ++s)
        if (!pinnedShift(schedule, s)) return s;
    return -1;  // every shift is pinned
}

void addCandidate(std::vector<int>& candidates, int employee, int oldEmployee, int limit) {
    if (employee == oldEmployee || employee < 0 || static_cast<int>(candidates.size()) >= limit) {
        return;
    }
    if (std::find(candidates.begin(), candidates.end(), employee) == candidates.end()) {
        candidates.push_back(employee);
    }
}

int bitCount(std::uint64_t x) {
    int count = 0;
    while (x != 0) {
        x &= x - 1;
        ++count;
    }
    return count;
}

class SmartChangeMove final : public optimaru::MoveOperator<EmployeeSchedule> {
public:
    SmartChangeMove(std::vector<std::vector<int>> eligible, int candidateCount,
                    PenalizedGetter penalized, ShiftsOfGetter shiftsOf)
        : eligible_(std::move(eligible)),
          candidateCount_(std::max(1, candidateCount)),
          penalized_(std::move(penalized)),
          shiftsOf_(std::move(shiftsOf)) {}

    const char* name() const override { return "employeeSmartChange"; }

    bool apply(optimaru::OperatorContext<EmployeeSchedule>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        EmployeeSchedule& schedule = ctx.director.workingSolution();
        const int employeeCount = static_cast<int>(schedule.employees.size());
        if (schedule.shifts.empty() || employeeCount < 2) return false;

        const int shift = randomShiftFromHotEmployee(schedule, penalized_, shiftsOf_, rng, 0.75);
        if (shift < 0) return false;
        const int oldEmployee = schedule.shifts[shift].employeeIndex;
        const int limit = std::min(candidateCount_, employeeCount - (oldEmployee >= 0 ? 1 : 0));
        if (limit <= 0) return false;

        candidates_.clear();
        if (shift < static_cast<int>(eligible_.size()) && !eligible_[shift].empty()) {
            const std::vector<int>& pool = eligible_[shift];
            if (static_cast<int>(pool.size()) <= limit) {
                for (int employee : pool) addCandidate(candidates_, employee, oldEmployee, limit);
            } else {
                const int maxAttempts = std::max(12, limit * 6);
                for (int attempt = 0; attempt < maxAttempts &&
                                      static_cast<int>(candidates_.size()) < limit;
                     ++attempt) {
                    addCandidate(candidates_, pool[rng() % pool.size()], oldEmployee, limit);
                }
            }
        }
        for (int attempt = 0;
             static_cast<int>(candidates_.size()) < limit && attempt < employeeCount * 2;
             ++attempt) {
            addCandidate(candidates_,
                         static_cast<int>(rng() % static_cast<std::uint64_t>(employeeCount)),
                         oldEmployee, limit);
        }
        if (candidates_.empty()) return false;

        bool initialized = false;
        Score bestScore{std::numeric_limits<std::int64_t>::min(),
                        -std::numeric_limits<double>::infinity()};
        int bestEmployee = candidates_.front();
        for (int employee : candidates_) {
            ctx.director.changeVariable(shift, employee);
            Score score = ctx.director.calculateScore();
            ctx.director.changeVariable(shift, oldEmployee);
            if (!initialized || score > bestScore) {
                initialized = true;
                bestScore = score;
                bestEmployee = employee;
            }
        }

        lastShift_ = shift;
        lastOldEmployee_ = oldEmployee;
        ctx.director.changeVariable(shift, bestEmployee);
        entities.push_back(shift);
        return true;
    }

    void undoLast(optimaru::OperatorContext<EmployeeSchedule>& ctx) override {
        ctx.director.changeVariable(lastShift_, lastOldEmployee_);
    }

private:
    std::vector<std::vector<int>> eligible_;
    int candidateCount_;
    PenalizedGetter penalized_;
    ShiftsOfGetter shiftsOf_;
    std::vector<int> candidates_;
    int lastShift_ = -1;
    int lastOldEmployee_ = -1;
};

class EmployeeWindowSwapMove final : public optimaru::MoveOperator<EmployeeSchedule> {
public:
    EmployeeWindowSwapMove(const EmployeeSchedule& schedule, int maxDays,
                           PenalizedGetter penalized, ShiftsOfGetter shiftsOf)
        : maxDays_(std::max(1, maxDays)),
          penalized_(std::move(penalized)),
          shiftsOf_(std::move(shiftsOf)) {
        int horizon = 0;
        for (const Shift& shift : schedule.shifts) horizon = std::max(horizon, shift.startDay + 1);
        shiftsByDay_.resize(horizon);
        for (int i = 0; i < static_cast<int>(schedule.shifts.size()); ++i) {
            const int day = schedule.shifts[i].startDay;
            if (day >= 0) shiftsByDay_[day].push_back(i);
        }
    }

    const char* name() const override { return "employeeWindowSwap"; }

    bool apply(optimaru::OperatorContext<EmployeeSchedule>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        EmployeeSchedule& schedule = ctx.director.workingSolution();
        const int employeeCount = static_cast<int>(schedule.employees.size());
        const int shiftCount = static_cast<int>(schedule.shifts.size());
        if (shiftCount == 0 || employeeCount < 2) return false;

        lastChanged_.clear();
        for (int attempt = 0; attempt < 16; ++attempt) {
            const int seedShift =
                    randomShiftFromHotEmployee(schedule, penalized_, shiftsOf_, rng, 0.60);
            if (seedShift < 0) return false;
            const int employeeA = schedule.shifts[seedShift].employeeIndex;
            if (employeeA < 0) continue;
            int employeeB =
                    static_cast<int>(rng() % static_cast<std::uint64_t>(employeeCount - 1));
            if (employeeB >= employeeA) ++employeeB;

            const int windowLength =
                    1 + static_cast<int>(rng() % static_cast<std::uint64_t>(maxDays_));
            const int seedDay = schedule.shifts[seedShift].startDay;
            const int startDay =
                    seedDay - static_cast<int>(rng() % static_cast<std::uint64_t>(windowLength));
            const int endDay = startDay + windowLength;

            const int firstDay = std::max(0, startDay);
            const int lastDay = std::min(endDay, static_cast<int>(shiftsByDay_.size()));
            for (int day = firstDay; day < lastDay; ++day) {
                for (int shift : shiftsByDay_[day]) {
                    const Shift& s = schedule.shifts[shift];
                    if (s.pinned) continue;  // never reassign a pinned shift
                    const int employee = s.employeeIndex;
                    if (employee == employeeA || employee == employeeB) {
                        lastChanged_.push_back({shift, employee});
                    }
                }
            }
            std::sort(lastChanged_.begin(), lastChanged_.end());
            if (lastChanged_.size() >= 2) {
                ctx.director.beginVariableChanges();
                for (auto [shift, employee] : lastChanged_) {
                    ctx.director.changeVariable(shift,
                                                employee == employeeA ? employeeB : employeeA);
                    entities.push_back(shift);
                }
                ctx.director.endVariableChanges();
                return true;
            }
            lastChanged_.clear();
        }
        return false;
    }

    void undoLast(optimaru::OperatorContext<EmployeeSchedule>& ctx) override {
        ctx.director.beginVariableChanges();
        for (auto it = lastChanged_.rbegin(); it != lastChanged_.rend(); ++it) {
            ctx.director.changeVariable(it->first, it->second);
        }
        ctx.director.endVariableChanges();
    }

private:
    int maxDays_;
    PenalizedGetter penalized_;
    ShiftsOfGetter shiftsOf_;
    std::vector<std::vector<int>> shiftsByDay_;
    std::vector<std::pair<int, int>> lastChanged_;
};

// Sets a shift to unassigned (employeeIndex = -1). The generic change/swap
// moves only ever pick a real employee (the value range is the employee list),
// so without this the search can never empty a slot. Late Acceptance keeps an
// unassignment only if it improves the score.
//
// Targeted selection: among a random sample of assigned shifts (whose coverage
// group still has >= 2 assigned, so emptying keeps at least one), prefer the
// most worthwhile to empty — first any shift falling on the assignee's
// undesired day (removing a preference penalty), then the most over-staffed
// coverage group (day + location). Beats blind random unassignment: it aims
// straight at the slots whose emptying tends to improve the score.
class UnassignMove final : public optimaru::MoveOperator<EmployeeSchedule> {
public:
    explicit UnassignMove(const EmployeeSchedule& schedule) {
        std::map<std::pair<int, std::string>, int> gidx;
        groupOf_.assign(schedule.shifts.size(), -1);
        for (std::size_t i = 0; i < schedule.shifts.size(); ++i) {
            std::pair<int, std::string> key(schedule.shifts[i].startDay,
                                            schedule.shifts[i].location);
            auto it = gidx.find(key);
            int g;
            if (it == gidx.end()) {
                g = static_cast<int>(groups_.size());
                gidx.emplace(key, g);
                groups_.emplace_back();
            } else {
                g = it->second;
            }
            groupOf_[i] = g;
            groups_[g].push_back(static_cast<int>(i));
        }
    }

    const char* name() const override { return "employeeUnassign"; }

    bool apply(optimaru::OperatorContext<EmployeeSchedule>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        EmployeeSchedule& schedule = ctx.director.workingSolution();
        const int n = static_cast<int>(schedule.shifts.size());
        if (n == 0) return false;
        int best = -1;
        long bestWorth = -1;
        for (int attempt = 0; attempt < 24; ++attempt) {
            const int shift = static_cast<int>(rng() % static_cast<std::uint64_t>(n));
            if (schedule.shifts[shift].pinned) continue;  // never unassign a pinned shift
            const int emp = schedule.shifts[shift].employeeIndex;
            if (emp < 0) continue;
            const std::vector<int>& group = groups_[groupOf_[shift]];
            int occ = 0;
            for (int g : group)
                if (schedule.shifts[g].employeeIndex >= 0) ++occ;
            if (occ < 2) continue;  // keep at least one assigned in the group
            const bool undesired =
                    schedule.employees[emp].undesiredDates.count(schedule.shifts[shift].startDay) >
                    0;
            const long worth = (undesired ? 1000 : 0) + occ;
            if (worth > bestWorth) {
                bestWorth = worth;
                best = shift;
            }
        }
        if (best < 0) return false;
        lastShift_ = best;
        lastOldEmployee_ = schedule.shifts[best].employeeIndex;
        ctx.director.changeVariable(best, -1);
        entities.push_back(best);
        return true;
    }

    void undoLast(optimaru::OperatorContext<EmployeeSchedule>& ctx) override {
        ctx.director.changeVariable(lastShift_, lastOldEmployee_);
    }

private:
    std::vector<std::vector<int>> groups_;  // coverage group -> shift indices
    std::vector<int> groupOf_;              // shift -> group
    int lastShift_ = -1;
    int lastOldEmployee_ = -1;
};

class EmployeeHotRuinOperator final : public optimaru::RuinOperator<EmployeeSchedule> {
public:
    EmployeeHotRuinOperator(int maxDays, PenalizedGetter penalized, ShiftsOfGetter shiftsOf)
        : maxDays_(std::max(1, maxDays)),
          penalized_(std::move(penalized)),
          shiftsOf_(std::move(shiftsOf)) {}

    const char* name() const override { return "employeeHotRuin"; }

    std::vector<int> selectRuinSet(optimaru::OperatorContext<EmployeeSchedule>& ctx,
                                   int ruinCount, std::mt19937_64& rng) override {
        const EmployeeSchedule& schedule = ctx.director.workingSolution();
        const int shiftCount = static_cast<int>(schedule.shifts.size());
        const int employeeCount = static_cast<int>(schedule.employees.size());
        if (shiftCount == 0 || ruinCount <= 0) return {};

        std::vector<int> ruined;
        ruined.reserve(std::min(ruinCount, shiftCount));
        std::vector<char> used(shiftCount, 0);
        auto add = [&](int shift) {
            if (shift < 0 || shift >= shiftCount || used[shift] ||
                static_cast<int>(ruined.size()) >= ruinCount) {
                return;
            }
            used[shift] = 1;
            ruined.push_back(shift);
        };

        int employee = -1;
        const std::vector<int>& hot = penalized_();
        if (!hot.empty()) {
            employee = hot[rng() % hot.size()];
        } else if (employeeCount > 0) {
            for (int attempt = 0; attempt < employeeCount * 2; ++attempt) {
                int candidate =
                        static_cast<int>(rng() % static_cast<std::uint64_t>(employeeCount));
                if (!shiftsOf_(candidate).empty()) {
                    employee = candidate;
                    break;
                }
            }
        }

        if (employee >= 0) {
            const std::vector<int>& assigned = shiftsOf_(employee);
            if (!assigned.empty()) {
                const int seedShift = assigned[rng() % assigned.size()];
                const int windowLength =
                        1 + static_cast<int>(rng() % static_cast<std::uint64_t>(maxDays_));
                const int seedDay = schedule.shifts[seedShift].startDay;
                const int startDay = seedDay - static_cast<int>(
                                                       rng() %
                                                       static_cast<std::uint64_t>(windowLength));
                const int endDay = startDay + windowLength;

                for (int shift : assigned) {
                    const int day = schedule.shifts[shift].startDay;
                    if (day >= startDay && day < endDay) add(shift);
                }
                for (int shift = 0; shift < shiftCount; ++shift) {
                    const int day = schedule.shifts[shift].startDay;
                    if (day >= startDay && day < endDay) add(shift);
                }
                for (int shift : assigned) add(shift);
            }
        }

        while (static_cast<int>(ruined.size()) < ruinCount &&
               static_cast<int>(ruined.size()) < shiftCount) {
            add(static_cast<int>(rng() % static_cast<std::uint64_t>(shiftCount)));
        }
        return ruined;
    }

private:
    int maxDays_;
    PenalizedGetter penalized_;
    ShiftsOfGetter shiftsOf_;
};

std::vector<std::vector<int>> buildEligibleEmployees(const EmployeeSchedule& schedule) {
    const int shiftCount = static_cast<int>(schedule.shifts.size());
    const int employeeCount = static_cast<int>(schedule.employees.size());
    std::vector<std::vector<int>> eligible(shiftCount);
    for (int i = 0; i < shiftCount; ++i) {
        const Shift& sh = schedule.shifts[i];
        for (int e = 0; e < employeeCount; ++e) {
            const Employee& emp = schedule.employees[e];
            if ((sh.requiredSkillBit == 0 || (emp.skillMask & sh.requiredSkillBit) != 0) &&
                ScoreCalculator::overlapWithDates(sh, emp.unavailableDates) == 0) {
                eligible[i].push_back(e);
            }
        }
    }
    return eligible;
}

void seedDayAwareInitialRoster(const Solver::Config& config, EmployeeSchedule& schedule) {
    if (!config.guidedMoves || !config.employeeGreedyConstruction || schedule.shifts.empty() ||
        schedule.employees.empty()) {
        return;
    }
    bool hasUnassigned = false;
    int horizon = 0;
    for (const Shift& shift : schedule.shifts) {
        if (shift.employeeIndex < 0) hasUnassigned = true;
        horizon = std::max(horizon, shift.startDay + 1);
    }
    if (!hasUnassigned || horizon <= 0) return;

    const int employeeCount = static_cast<int>(schedule.employees.size());
    const std::vector<std::vector<int>> eligible = buildEligibleEmployees(schedule);
    std::vector<std::vector<int>> shiftsByDay(horizon);
    for (int s = 0; s < static_cast<int>(schedule.shifts.size()); ++s) {
        const int day = schedule.shifts[s].startDay;
        if (day >= 0 && day < horizon) shiftsByDay[day].push_back(s);
    }

    std::vector<int> consecutive(employeeCount, 0);
    std::vector<int> load(employeeCount, 0);
    std::vector<char> workedToday(employeeCount, 0);
    for (int day = 0; day < horizon; ++day) {
        std::fill(workedToday.begin(), workedToday.end(), 0);
        for (int shift : shiftsByDay[day]) {
            const int employee = schedule.shifts[shift].employeeIndex;
            if (employee >= 0 && employee < employeeCount) {
                workedToday[employee] = 1;
                ++load[employee];
            }
        }

        std::vector<int> unassigned;
        for (int shift : shiftsByDay[day]) {
            if (schedule.shifts[shift].employeeIndex < 0) unassigned.push_back(shift);
        }
        std::sort(unassigned.begin(), unassigned.end(), [&](int a, int b) {
            const std::size_t ea = eligible[a].size();
            const std::size_t eb = eligible[b].size();
            if (ea != eb) return ea < eb;  // scarce skills first
            return a < b;
        });

        for (int shift : unassigned) {
            int bestEmployee = -1;
            bool bestWouldExceed = true;
            bool bestUndesired = true;
            int bestRun = std::numeric_limits<int>::max();
            int bestFlexibility = std::numeric_limits<int>::max();
            int bestLoad = std::numeric_limits<int>::max();
            for (int employee : eligible[shift]) {
                if (employee < 0 || employee >= employeeCount || workedToday[employee]) continue;
                const bool wouldExceed = consecutive[employee] >= config.employeeRunRepairMaxDays;
                // Prefer NOT starting a worker on a day they would rather have
                // off, so the initial roster does not create preference
                // violations the search then has to undo. Ranked just under the
                // consecutive-cap guard, above the load-balancing tie-breaks.
                const bool undesiredToday =
                        schedule.employees[employee].undesiredDates.count(day) > 0;
                const int run = consecutive[employee];
                const int flexibility = bitCount(schedule.employees[employee].skillMask);
                const int employeeLoad = load[employee];
                const bool better =
                        bestEmployee < 0 || wouldExceed < bestWouldExceed ||
                        (wouldExceed == bestWouldExceed &&
                         (undesiredToday < bestUndesired ||
                          (undesiredToday == bestUndesired &&
                           (run < bestRun ||
                            (run == bestRun &&
                             (flexibility < bestFlexibility ||
                              (flexibility == bestFlexibility && employeeLoad < bestLoad)))))));
                if (better) {
                    bestEmployee = employee;
                    bestWouldExceed = wouldExceed;
                    bestUndesired = undesiredToday;
                    bestRun = run;
                    bestFlexibility = flexibility;
                    bestLoad = employeeLoad;
                }
            }
            if (bestEmployee >= 0) {
                schedule.shifts[shift].employeeIndex = bestEmployee;
                workedToday[bestEmployee] = 1;
                ++load[bestEmployee];
            }
        }

        for (int employee = 0; employee < employeeCount; ++employee) {
            consecutive[employee] = workedToday[employee] ? consecutive[employee] + 1 : 0;
        }
    }
}

struct RelocationRepair {
    bool found = false;
    int shift = -1;
    int oldEmployee = -1;
    int newEmployee = -1;
    int shift2 = -1;
    int oldEmployee2 = -1;
    int newEmployee2 = -1;
    Score score{std::numeric_limits<std::int64_t>::min(),
                -std::numeric_limits<double>::infinity()};
};

struct EmployeeDayIndex {
    int employeeCount = 0;
    int horizon = 0;
    std::vector<int> dayLoad;
    std::vector<int> singleShift;

    std::size_t offset(int employee, int day) const {
        return static_cast<std::size_t>(employee) * horizon + day;
    }

    int load(int employee, int day) const {
        return dayLoad[offset(employee, day)];
    }

    int singleShiftOnDay(int employee, int day) const {
        const std::size_t index = offset(employee, day);
        return dayLoad[index] == 1 ? singleShift[index] : -1;
    }
};

EmployeeDayIndex buildEmployeeDayIndex(const EmployeeSchedule& schedule) {
    EmployeeDayIndex index;
    index.employeeCount = static_cast<int>(schedule.employees.size());
    for (const Shift& shift : schedule.shifts) {
        index.horizon = std::max(index.horizon, shift.startDay + 1);
    }
    if (index.employeeCount <= 0 || index.horizon <= 0) return index;

    const std::size_t size = static_cast<std::size_t>(index.employeeCount) * index.horizon;
    index.dayLoad.assign(size, 0);
    index.singleShift.assign(size, -1);
    for (int s = 0; s < static_cast<int>(schedule.shifts.size()); ++s) {
        const int employee = schedule.shifts[s].employeeIndex;
        const int day = schedule.shifts[s].startDay;
        if (employee < 0 || employee >= index.employeeCount || day < 0 || day >= index.horizon) {
            continue;
        }
        const std::size_t pos = index.offset(employee, day);
        if (index.dayLoad[pos] == 0) {
            index.singleShift[pos] = s;
        } else {
            index.singleShift[pos] = -1;
        }
        ++index.dayLoad[pos];
    }
    return index;
}

void applyRepair(optimaru::ScoreDirector<EmployeeSchedule>& director,
                 const RelocationRepair& repair) {
    if (repair.shift2 >= 0) director.beginVariableChanges();
    director.changeVariable(repair.shift, repair.newEmployee);
    if (repair.shift2 >= 0) director.changeVariable(repair.shift2, repair.newEmployee2);
    if (repair.shift2 >= 0) director.endVariableChanges();
}

// Every repair scan scores each candidate, so on a large roster one scan can
// outlast the budget. A scan polls this after each evaluation and, once it
// reports expiry, stops with the best repair found so far.
class RepairDeadline {
public:
    explicit RepairDeadline(Clock::time_point deadline) : deadline_(deadline) {}

    bool expired() {
        return ++evaluations_ % kEvaluationsPerCheck == 0 && Clock::now() >= deadline_;
    }

private:
    static constexpr std::uint32_t kEvaluationsPerCheck = 64;
    Clock::time_point deadline_;
    std::uint32_t evaluations_ = 0;
};

RelocationRepair bestDayRepair(optimaru::ScoreDirector<EmployeeSchedule>& director,
                               const std::vector<std::vector<int>>& eligible,
                               RepairDeadline& deadline) {
    EmployeeSchedule& schedule = director.workingSolution();
    const int employeeCount = static_cast<int>(schedule.employees.size());
    const int shiftCount = static_cast<int>(schedule.shifts.size());
    RelocationRepair best;
    const EmployeeDayIndex dayIndex = buildEmployeeDayIndex(schedule);
    const int horizon = dayIndex.horizon;
    if (horizon <= 0) return best;

    for (int shift = 0; shift < shiftCount; ++shift) {
        if (schedule.shifts[shift].pinned) continue;  // never relocate a pinned shift
        const int oldEmployee = schedule.shifts[shift].employeeIndex;
        const int day = schedule.shifts[shift].startDay;
        if (oldEmployee < 0 || day < 0 || day >= horizon) continue;
        if (dayIndex.load(oldEmployee, day) <= 1) continue;
        for (int target : eligible[shift]) {
            if (target == oldEmployee || target < 0 || target >= employeeCount) continue;
            if (dayIndex.load(target, day) != 0) continue;
            director.changeVariable(shift, target);
            Score score = director.calculateScore();
            director.changeVariable(shift, oldEmployee);
            if (!best.found || score > best.score) {
                best.found = true;
                best.shift = shift;
                best.oldEmployee = oldEmployee;
                best.newEmployee = target;
                best.score = score;
            }
            if (deadline.expired()) return best;
        }
    }
    return best;
}

RelocationRepair bestRunRepair(optimaru::ScoreDirector<EmployeeSchedule>& director,
                               const std::vector<std::vector<int>>& eligible, int maxDays,
                               RepairDeadline& deadline) {
    EmployeeSchedule& schedule = director.workingSolution();
    const int employeeCount = static_cast<int>(schedule.employees.size());
    RelocationRepair best;
    const EmployeeDayIndex dayIndex = buildEmployeeDayIndex(schedule);
    const int horizon = dayIndex.horizon;
    if (horizon <= 0) return best;

    for (int employee = 0; employee < employeeCount; ++employee) {
        int first = -1;
        for (int day = 0; day <= horizon; ++day) {
            const bool worked = day < horizon && dayIndex.load(employee, day) > 0;
            if (worked && first < 0) {
                first = day;
            } else if ((!worked || day == horizon) && first >= 0) {
                const int last = day - 1;
                if (last - first + 1 > maxDays) {
                    for (int d = first; d <= last; ++d) {
                        const int shiftOnDay = dayIndex.singleShiftOnDay(employee, d);
                        if (shiftOnDay < 0 || schedule.shifts[shiftOnDay].pinned) continue;
                        for (int target : eligible[shiftOnDay]) {
                            if (target == employee || target < 0 || target >= employeeCount) continue;
                            if (dayIndex.load(target, d) != 0) continue;
                            director.changeVariable(shiftOnDay, target);
                            Score score = director.calculateScore();
                            director.changeVariable(shiftOnDay, employee);
                            if (!best.found || score > best.score) {
                                best.found = true;
                                best.shift = shiftOnDay;
                                best.oldEmployee = employee;
                                best.newEmployee = target;
                                best.score = score;
                            }
                            if (deadline.expired()) return best;
                        }
                    }
                }
                first = -1;
            }
        }
    }
    return best;
}

RelocationRepair bestRunSwapRepair(optimaru::ScoreDirector<EmployeeSchedule>& director,
                                   const std::vector<std::vector<int>>& eligible, int maxDays,
                                   RepairDeadline& deadline) {
    EmployeeSchedule& schedule = director.workingSolution();
    const int employeeCount = static_cast<int>(schedule.employees.size());
    RelocationRepair best;
    const EmployeeDayIndex dayIndex = buildEmployeeDayIndex(schedule);
    const int horizon = dayIndex.horizon;
    if (horizon <= 0) return best;
    auto eligibleFor = [&](int shift, int employee) {
        const std::vector<int>& pool = eligible[shift];
        return std::find(pool.begin(), pool.end(), employee) != pool.end();
    };

    for (int employee = 0; employee < employeeCount; ++employee) {
        int first = -1;
        for (int day = 0; day <= horizon; ++day) {
            const bool worked = day < horizon && dayIndex.load(employee, day) > 0;
            if (worked && first < 0) {
                first = day;
            } else if ((!worked || day == horizon) && first >= 0) {
                const int last = day - 1;
                if (last - first + 1 > maxDays) {
                    for (int d = first; d <= last; ++d) {
                        const int shiftA = dayIndex.singleShiftOnDay(employee, d);
                        if (shiftA < 0 || schedule.shifts[shiftA].pinned) continue;
                        for (int target : eligible[shiftA]) {
                            if (target == employee || target < 0 || target >= employeeCount) {
                                continue;
                            }
                            if (dayIndex.load(target, d) != 0) continue;
                            for (int d2 = 0; d2 < horizon; ++d2) {
                                if (d2 == d) continue;
                                if (dayIndex.load(employee, d2) != 0) continue;
                                const int shiftB = dayIndex.singleShiftOnDay(target, d2);
                                if (shiftB < 0 || schedule.shifts[shiftB].pinned ||
                                    !eligibleFor(shiftB, employee))
                                    continue;
                                director.beginVariableChanges();
                                director.changeVariable(shiftA, target);
                                director.changeVariable(shiftB, employee);
                                director.endVariableChanges();
                                Score score = director.calculateScore();
                                director.beginVariableChanges();
                                director.changeVariable(shiftB, target);
                                director.changeVariable(shiftA, employee);
                                director.endVariableChanges();
                                if (!best.found || score > best.score) {
                                    best.found = true;
                                    best.shift = shiftA;
                                    best.oldEmployee = employee;
                                    best.newEmployee = target;
                                    best.shift2 = shiftB;
                                    best.oldEmployee2 = target;
                                    best.newEmployee2 = employee;
                                    best.score = score;
                                }
                                if (deadline.expired()) return best;
                            }
                        }
                    }
                }
                first = -1;
            }
        }
    }
    return best;
}

template <typename Calculator>
void greedyHardRepair(const Solver::Config& config, EmployeeSchedule& schedule,
                      Calculator& calculator, Clock::time_point deadline) {
    if (!config.guidedMoves || schedule.shifts.empty() || schedule.employees.empty()) return;
    // Respect the solve-wide wall-clock budget. This final repair used to run
    // to convergence with no time check, so on a many-constraint instance where
    // local search never reaches feasibility it could overshoot the limit many
    // times over (a 3s budget ran 34s; a 20s budget ran 184s). The deadline is
    // checked between repair scans and inside them (see RepairDeadline).
    if (Clock::now() >= deadline) return;
    optimaru::ScoreDirector<EmployeeSchedule> director(schedule, calculator);
    Score current = director.calculateScore();
    if (current.hard >= 0) return;

    const std::vector<std::vector<int>> eligible = buildEligibleEmployees(schedule);
    RepairDeadline scanDeadline(deadline);
    const int maxSteps = static_cast<int>(schedule.shifts.size()) * 2;
    int steps = 0;
    for (int cycle = 0;
         cycle < 4 && steps < maxSteps && current.hard < 0 && Clock::now() < deadline; ++cycle) {
        bool changed = false;
        if (config.employeeDayRepairMoves) {
            while (steps < maxSteps && Clock::now() < deadline) {
                RelocationRepair day = bestDayRepair(director, eligible, scanDeadline);
                if (!day.found || !(day.score >= current)) break;
                applyRepair(director, day);
                current = day.score;
                changed = true;
                ++steps;
            }
            if (current.hard >= 0) break;
        }
        if (config.employeeRunRepairMoves) {
            while (steps < maxSteps && current.hard < 0 && Clock::now() < deadline) {
                RelocationRepair run = bestRunSwapRepair(
                        director, eligible, config.employeeRunRepairMaxDays, scanDeadline);
                RelocationRepair relocate = bestRunRepair(
                        director, eligible, config.employeeRunRepairMaxDays, scanDeadline);
                if (relocate.found && (!run.found || relocate.score > run.score)) {
                    run = relocate;
                }
                if (!run.found || !(run.score >= current)) break;
                applyRepair(director, run);
                current = run.score;
                changed = true;
                ++steps;
            }
        }
        if (!changed) break;
    }
}

// Engine setup shared by both solve() overloads. `penalized` / `shiftsOf`
// abstract over where the violation-guidance state lives: the monolithic
// calculator's penalizedEmployees()/employeeShifts() or the decomposed
// calculator's penalizedUnits()/membersPerUnit().
template <typename Calculator, typename PenalizedFn, typename ShiftsOfFn>
void runEngine(const Solver::Config& config, EmployeeSchedule& schedule,
               Calculator& calculator, PenalizedFn penalized, ShiftsOfFn shiftsOf,
               std::chrono::milliseconds localSearchLimit) {
    const int shiftCount = static_cast<int>(schedule.shifts.size());

    optimaru::SolverConfig engineConfig;
    engineConfig.termination.spentLimit = localSearchLimit;
    engineConfig.termination.bestScoreLimit = config.bestScoreLimit;
    engineConfig.termination.unimprovedSpentLimit = config.unimprovedSpentLimit;
    engineConfig.termination.stepCountLimit = config.stepCountLimit;
    engineConfig.termination.unimprovedStepCountLimit = config.unimprovedStepCountLimit;
    if (config.lateAcceptanceSize == 0) {
        throw std::invalid_argument("lateAcceptanceSize must be positive");
    }
    engineConfig.localSearch.acceptor.lateAcceptanceSize = config.lateAcceptanceSize;
    engineConfig.localSearch.moveSelector.changeMoveWeight = config.changeMoveWeight;
    engineConfig.localSearch.moveSelector.swapMoveWeight = config.swapMoveWeight;
    engineConfig.localSearch.moveSelector.relocateMoveWeight = config.relocateMoveWeight;
    engineConfig.localSearch.moveSelector.listSwapMoveWeight = config.listSwapMoveWeight;
    engineConfig.localSearch.moveSelector.twoOptMoveWeight = config.twoOptMoveWeight;
    engineConfig.localSearch.moveSelector.orOptMoveWeight = config.orOptMoveWeight;
    engineConfig.localSearch.moveSelector.tailSwapMoveWeight = config.tailSwapMoveWeight;
    auto& lns = engineConfig.localSearch.ruinRecreate;
    lns.enabled = config.ruinRecreate;
    lns.unimprovedStepTrigger = config.ruinRecreateUnimprovedStepTrigger;
    lns.minRuinFraction = config.ruinRecreateMinRuinFraction;
    lns.maxRuinFraction = config.ruinRecreateMaxRuinFraction;
    lns.maxRuinCount = config.ruinRecreateMaxRuinCount;
    lns.operatorDecay = config.ruinRecreateOperatorDecay;
    if (config.ruinRecreateRewards.size() != 4) {
        throw std::invalid_argument("ruinRecreateRewards must contain exactly 4 values");
    }
    lns.rewards = config.ruinRecreateRewards;
    lns.polishSteps = config.ruinRecreatePolishSteps;
    lns.asMoveWeight = config.ruinRecreateAsMoveWeight;
    lns.asMoveMaxRuin = config.ruinRecreateAsMoveMaxRuin;
    engineConfig.localSearch.descentWeight = config.descentWeight;
    engineConfig.localSearch.descentMaxSweeps = config.descentMaxSweeps;
    engineConfig.localSearch.customMoveProbability = config.customMoveProbability;
    engineConfig.randomSeed = config.randomSeed;
    engineConfig.onNewBest = config.onNewBest;

    // Guided move selection: the domain heuristics plugged into the engine's
    // recommender hooks (returning -1 falls back to uniform-random selection).
    std::vector<std::vector<int>> eligible;
    if (config.guidedMoves) {
        // Axis A precompute: for each shift, the employees that can work it
        // without a hard violation (required skill AND available on the days).
        eligible = buildEligibleEmployees(schedule);

        // Axis B: 70% of the time pick a shift belonging to an employee that
        // currently has a hard violation (the "hot spots"). Once feasible there
        // are no penalized employees, so this naturally falls back to uniform
        // exploration for soft optimisation.
        engineConfig.localSearch.entityRecommender =
                [penalized, shiftsOf](std::mt19937_64& rng) -> int {
            const std::vector<int>& hot = penalized();
            if (hot.empty()) return -1;
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            if (unit(rng) >= 0.70) return -1;
            const std::vector<int>& list = shiftsOf(hot[rng() % hot.size()]);
            if (list.empty()) return -1;
            return list[rng() % list.size()];
        };

        // Axis A: 85% from the skill-capable/available pool, 15% fully random
        // (to keep diversity and allow escaping local optima).
        engineConfig.localSearch.valueRecommender = [&eligible](std::mt19937_64& rng, int s) -> int {
            const std::vector<int>& pool = eligible[s];
            if (pool.empty()) return -1;
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            if (unit(rng) >= 0.85) return -1;
            return pool[rng() % pool.size()];
        };
    }

    // Temporal nearby lists: bias swap partners / descent pairs / related-ruin
    // clusters toward shifts starting close in time — the window where the
    // pairwise hard constraints (overlap, one-per-day, min-rest) interact.
    // The meter reads an immutable start-time table, so it is safe across
    // multi-start threads.
    if (config.nearbyShifts && shiftCount >= 2) {
        auto starts = std::make_shared<std::vector<Minutes>>();
        starts->reserve(shiftCount);
        for (const Shift& sh : schedule.shifts) starts->push_back(sh.start);
        // Neighbor-list size: the densest 24h start-time window of the
        // instance (every shift a pair penalty could bind to is inside it).
        std::vector<Minutes> sorted(starts->begin(), starts->end());
        std::sort(sorted.begin(), sorted.end());
        int maxWindow = 1;
        std::size_t lo = 0;
        for (std::size_t hi = 0; hi < sorted.size(); ++hi) {
            while (sorted[hi] - sorted[lo] > MINUTES_PER_DAY) ++lo;
            maxWindow = std::max(maxWindow, static_cast<int>(hi - lo + 1));
        }
        engineConfig.localSearch.nearby.distanceMeter = [starts](int a, int b) {
            const Minutes d = (*starts)[a] - (*starts)[b];
            return static_cast<double>(d < 0 ? -d : d);
        };
        engineConfig.localSearch.nearby.size = std::min(shiftCount - 1, maxWindow);
        engineConfig.localSearch.nearby.probability = config.nearbyShiftsProbability;
    }

    optimaru::Solver<EmployeeSchedule> engine(std::move(engineConfig));
    PenalizedGetter penalizedRef = penalized;
    ShiftsOfGetter shiftsOfRef = shiftsOf;
    if (config.guidedMoves && config.smartChangeMoves) {
        engine.addMoveOperator(std::make_unique<SmartChangeMove>(
                eligible, config.smartChangeCandidateCount, penalizedRef, shiftsOfRef));
    }
    if (config.guidedMoves && config.employeeWindowMoves) {
        engine.addMoveOperator(std::make_unique<EmployeeWindowSwapMove>(
                schedule, config.employeeWindowMaxDays, penalizedRef, shiftsOfRef));
    }
    if (config.guidedMoves && config.employeeRuinMoves) {
        engine.addRuinOperator(std::make_unique<EmployeeHotRuinOperator>(
                config.employeeWindowMaxDays, penalizedRef, shiftsOfRef));
    }
    if (config.employeeUnassignMoves) {
        engine.addMoveOperator(std::make_unique<UnassignMove>(schedule));
    }
    engine.solve(schedule, calculator);
}

}  // namespace

void Solver::solve(EmployeeSchedule& schedule) {
    const auto solveStart = Clock::now();
    const auto deadline = solveStart + config_.spentLimit;
    if (schedule.employees.empty() || allShiftsPinned(schedule)) {
        schedule.score = ScoreCalculator::calculateScore(schedule);
        return;
    }

    ScoreCalculator::prepare(schedule);  // intern skills / precompute day indices
    seedDayAwareInitialRoster(config_, schedule);

    const int workerCount = std::max(1, config_.parallelSolverCount);
    if (workerCount > 1) {
        std::vector<EmployeeSchedule> candidates(workerCount, schedule);
        std::vector<std::exception_ptr> failures(workerCount);
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (int i = 0; i < workerCount; ++i) {
            workers.emplace_back([this, i, &candidates, &failures] {
                try {
                    Config workerConfig = config_;
                    workerConfig.parallelSolverCount = 1;
                    workerConfig.randomSeed += static_cast<std::uint64_t>(i);
                    if (i != 0) workerConfig.onNewBest = nullptr;
                    Solver worker(std::move(workerConfig));
                    worker.solve(candidates[i]);
                } catch (...) {
                    failures[i] = std::current_exception();
                }
            });
        }
        for (std::thread& worker : workers) worker.join();
        for (const std::exception_ptr& failure : failures) {
            if (failure) std::rethrow_exception(failure);
        }
        int best = 0;
        for (int i = 1; i < workerCount; ++i) {
            if (candidates[i].score > candidates[best].score) best = i;
        }
        schedule = std::move(candidates[best]);
        return;
    }

    EmployeeSchedulingIncrementalScoreCalculator calculator;
    runEngine(config_, schedule, calculator,
              [&calculator]() -> const std::vector<int>& {
                  return calculator.penalizedEmployees();
              },
              [&calculator](int e) -> const std::vector<int>& {
                  return calculator.employeeShifts()[e];
              },
              localSearchLimit(config_));
    greedyHardRepair(config_, schedule, calculator, deadline);

    schedule.score = ScoreCalculator::calculateScore(schedule);
}

void Solver::solve(EmployeeSchedule& schedule,
                   optimaru::DecomposedScoreCalculator<EmployeeSchedule>& calculator) {
    const auto solveStart = Clock::now();
    const auto deadline = solveStart + config_.spentLimit;
    ScoreCalculator::prepare(schedule);
    const auto tSeed = Clock::now();
    seedDayAwareInitialRoster(config_, schedule);
    logPhase("seed", tSeed);

    if (!schedule.employees.empty() && !allShiftsPinned(schedule)) {
        // DecomposedScoreCalculator compiles compatible delta constraints into
        // structural groups at reset and leaves all others on the generic path.
        const auto tEngine = Clock::now();
        runEngine(config_, schedule, calculator,
                  [&calculator]() -> const std::vector<int>& {
                      return calculator.penalizedUnits();
                  },
                  [&calculator](int e) -> const std::vector<int>& {
                      return calculator.membersPerUnit()[e];
                  },
                  localSearchLimit(config_));
        logPhase("engine(CH+LS)", tEngine);
        const auto tRepair = Clock::now();
        greedyHardRepair(config_, schedule, calculator, deadline);
        logPhase("greedyHardRepair", tRepair);
    }

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraintTotals() explains exactly this score.
    calculator.resetWorkingSolution(schedule);
    schedule.score = calculator.calculateScore();
}

}  // namespace es
