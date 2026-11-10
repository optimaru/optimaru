#ifndef TA_DOMAIN_HPP
#define TA_DOMAIN_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "optimaru/model/solution_traits.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/score/levels.hpp"

namespace ta {

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
// Times are minutes counted as plain 64-bit integers (Task::startTime /
// TaskType::baseDuration).
using Minutes = std::int64_t;

constexpr Minutes kNullTime = -1;  // startTime of an unassigned task

// ---------------------------------------------------------------------------
// Score  (four score levels: [1]hard/[3]soft)
// ---------------------------------------------------------------------------
// This use case scores four levels, 1 hard and 3 soft:
//   hard[0]  requiredSkillCost          (missing skill count)
//   soft[0]  unassignedTasksCost        (1 per unassigned task)
//   soft[1]  squaredMakespanCost        (employee endTime^2 each)
//   soft[2]  {critical|major|minor}PriorityTaskEndTimeCost
//            (endTime x 4 / x 2 / x 1)
// The generic engine (optimaru/cpp/optimaru/) only knows Score(int64 hard,
// double soft); its constraints score on three levels (hard, medium, soft).
// Mapping rule (level priority preserved):
//
//   hard[0] -> Level::Hard, soft[0] -> Level::Medium, soft[1] and soft[2]
//   -> Level::Soft, folded as
//   engine.hard = hard * kMissingSkillUnit + soft0   (kMediumFold:
//                 MediumFold::below_hard(kMissingSkillUnit = 10^9))
//   engine.soft = soft1 * kMakespanUnit + soft2      (kMakespanUnit = 10^7)
//
// The medium level folds BELOW hard: one hard unit (a missing skill) weighs
// 10^9 medium units (unassigned tasks). The two soft levels share the
// engine's one soft level, and soft[1] stays above soft[2] because
// squaredMakespanCost's default weight IS kMakespanUnit (the components
// declare their defaults; the fold carries no weight). At the default weights
// the decomposed calculator (MediumFoldOf below) and this adapter produce the
// same engine score bit for bit; an explicit weight stays in units of its
// level (a medium weight in tasks, a hard one in missing skills).
//
// All four levels penalize whole units (skill counts / task counts / squared
// minutes / weighted minutes), so every component is a non-positive integer:
//  - engine.hard is exact int64 arithmetic; hard dominates soft0 as long as
//    |soft0| < 10^9, i.e. fewer than 10^9 tasks.
//  - engine.soft is exact inside the double's 2^53 integer range as long as
//    |soft1| < 9 * 10^8 (sum of squared employee end-minutes) and soft1
//    dominates soft2 as long as |soft2| < 10^7 (weighted end-minute sum).
//    That covers instances whose horizon is tens of thousands of minutes.
// One semantic wrinkle: the engine's feasible() (engine.hard >= 0) now
// also requires soft0 == 0. Because the engine's sequence local search keeps
// every task assigned once construction finished, final solutions always
// have soft0 == 0, so feasibility coincides with hard == 0.
constexpr std::int64_t kMissingSkillUnit = 1000000000LL;
constexpr std::int64_t kMakespanUnit = 10000000LL;
constexpr optimaru::MediumFold kMediumFold = optimaru::MediumFold::below_hard(kMissingSkillUnit);

struct Score {
    std::int64_t hard = 0;   // hard[0]: missing skills
    std::int64_t soft0 = 0;  // soft[0]: unassigned tasks (the medium level)
    std::int64_t soft1 = 0;  // soft[1]: sum of employee endTime^2
    std::int64_t soft2 = 0;  // soft[2]: priority-weighted task end times

    bool feasible() const { return hard >= 0; }

    bool operator<(const Score& o) const {
        if (hard != o.hard) return hard < o.hard;
        if (soft0 != o.soft0) return soft0 < o.soft0;
        if (soft1 != o.soft1) return soft1 < o.soft1;
        return soft2 < o.soft2;
    }
    bool operator>(const Score& o) const { return o < *this; }
    bool operator<=(const Score& o) const { return !(o < *this); }
    bool operator>=(const Score& o) const { return !(*this < o); }
    bool operator==(const Score& o) const {
        return hard == o.hard && soft0 == o.soft0 && soft1 == o.soft1 && soft2 == o.soft2;
    }

    // Printed as: [0]hard/[0/-3725/-77]soft
    std::string str() const {
        return "[" + std::to_string(hard) + "]hard/[" + std::to_string(soft0) + "/" +
               std::to_string(soft1) + "/" + std::to_string(soft2) + "]soft";
    }
};

// hard -> hard, soft0 -> medium (below hard), soft1 * kMakespanUnit + soft2
// -> soft, through the domain's medium fold.
inline optimaru::Score toEngineScore(const Score& s) {
    return kMediumFold.to_engine(s.hard, static_cast<double>(s.soft0),
                                 static_cast<double>(s.soft1) * static_cast<double>(kMakespanUnit) +
                                         static_cast<double>(s.soft2));
}

// A best-score LIMIT (Solver::Config::target_score) in engine terms.
// kNoLevelFloor (INT64_MIN) on soft1 or soft2 means "no floor on the soft
// levels": the engine's soft limit becomes -inf, so only hard and soft0 (the
// levels folded into the engine's hard level, compared exactly) count and no
// soft score, however low, can miss the stop. stop_at_first_feasible passes
// (0, 0, kNoLevelFloor, kNoLevelFloor): every skill met and every task
// assigned. Any other limit folds as a score.
constexpr std::int64_t kNoLevelFloor = std::numeric_limits<std::int64_t>::min();

inline optimaru::Score toEngineScoreLimit(const Score& limit) {
    if (limit.soft1 == kNoLevelFloor || limit.soft2 == kNoLevelFloor)
        return {toEngineScore({limit.hard, limit.soft0, 0, 0}).hard,
                -std::numeric_limits<double>::infinity()};
    return toEngineScore(limit);
}

// Inverse of toEngineScore. Every constraint penalizes (all levels <= 0 and
// integral), so truncating integer division decodes the levels exactly.
inline Score fromEngineScore(const optimaru::Score& e) {
    const std::int64_t combinedSoft = optimaru::round_score_level(e.soft, "soft");
    return {e.hard / kMissingSkillUnit, e.hard % kMissingSkillUnit, combinedSoft / kMakespanUnit,
            combinedSoft % kMakespanUnit};
}

// ---------------------------------------------------------------------------
// Enums  (Affinity / Priority)
// ---------------------------------------------------------------------------
// Affinity of an employee for a customer: better affinity means the task
// takes fewer minutes (baseDuration x multiplier).
enum class Affinity {
    NONE,    // multiplier 4
    LOW,     // multiplier 3
    MEDIUM,  // multiplier 2
    HIGH     // multiplier 1
};

inline int durationMultiplier(Affinity affinity) {
    switch (affinity) {
        case Affinity::HIGH:
            return 1;
        case Affinity::MEDIUM:
            return 2;
        case Affinity::LOW:
            return 3;
        case Affinity::NONE:
        default:
            return 4;
    }
}

enum class Priority { MINOR, MAJOR, CRITICAL };

// ---------------------------------------------------------------------------
// Customer  (problem fact)
// ---------------------------------------------------------------------------
struct Customer {
    std::string id;
    std::string name;
};

// ---------------------------------------------------------------------------
// TaskType  (problem fact)
// ---------------------------------------------------------------------------
struct TaskType {
    std::string code;
    std::string title;
    Minutes baseDuration = 0;  // minutes
    std::vector<std::string> requiredSkills;
};

// ---------------------------------------------------------------------------
// Task  (the list element)
// ---------------------------------------------------------------------------
// The input fields are immutable; the derived fields (employee, startTime)
// are computed from the employees' task lists:
//   startTime = kNullTime                     if unassigned
//             = minStartTime                  if first in the list
//             = max(previous.endTime, minStartTime) otherwise
//   endTime   = startTime + baseDuration * affinityMultiplier
struct Task {
    std::string id;
    int taskType = -1;  // index into TaskAssigningSolution::taskTypes
    int indexInTaskType = 0;
    int customer = -1;  // index into TaskAssigningSolution::customers
    Minutes minStartTime = 0;
    Priority priority = Priority::MINOR;

    // --- derived (recomputed by ScoreCalculator::updateShadowVariables) ---
    int employee = -1;  // -1 == unassigned
    Minutes startTime = kNullTime;
};

// ---------------------------------------------------------------------------
// Employee  (holds the decision variable)
// ---------------------------------------------------------------------------
// tasks is the decision variable: the ordered task indices this employee
// performs back to back. A task in no list is unassigned.
struct Employee {
    std::string id;
    std::string fullName;
    std::vector<std::string> skills;
    // Affinity per customer index; missing entries mean Affinity::NONE.
    std::vector<Affinity> affinityByCustomer;
    std::vector<int> tasks;  // ordered task indices — the decision variable

    Affinity affinity(int customerIndex) const {
        if (customerIndex < 0 || customerIndex >= static_cast<int>(affinityByCustomer.size()))
            return Affinity::NONE;
        return affinityByCustomer[customerIndex];
    }
};

// ---------------------------------------------------------------------------
// TaskAssigningSolution  (the problem and its solution)
// ---------------------------------------------------------------------------
struct TaskAssigningSolution {
    std::vector<TaskType> taskTypes;
    std::vector<Customer> customers;
    std::vector<Task> tasks;
    std::vector<Employee> employees;

    Score score;

    // Duration of `task` when done by `employeeIndex`:
    // baseDuration x the affinity multiplier; unassigned uses NONE (x 4).
    Minutes taskDuration(const Task& task, int employeeIndex) const {
        const Affinity affinity = employeeIndex < 0
                                          ? Affinity::NONE
                                          : employees[employeeIndex].affinity(task.customer);
        return taskTypes[task.taskType].baseDuration * durationMultiplier(affinity);
    }

    // Missing skill count: required skills of the task's type that the
    // employee does not have.
    int missingSkillCount(const Task& task, int employeeIndex) const {
        if (employeeIndex < 0) return 0;
        const Employee& employee = employees[employeeIndex];
        int count = 0;
        for (const std::string& skill : taskTypes[task.taskType].requiredSkills) {
            bool found = false;
            for (const std::string& owned : employee.skills) {
                if (owned == skill) {
                    found = true;
                    break;
                }
            }
            if (!found) ++count;
        }
        return count;
    }
};

}  // namespace ta

// ---------------------------------------------------------------------------
// SolutionTraits specialization  (describes the solution's shape to the
// engine): the lists are the employees' ordered task index sequences, the
// elements are the tasks. A task missing from every list is unassigned; the
// engine's construction heuristic assigns every task and local search keeps
// them assigned. No task is pinned, so no is_pinned hook is declared.
// ---------------------------------------------------------------------------
template <>
struct optimaru::SolutionTraits<ta::TaskAssigningSolution> {
    static constexpr optimaru::Shape shape = optimaru::Shape::Sequence;
    static int list_count(const ta::TaskAssigningSolution& s) {
        return static_cast<int>(s.employees.size());
    }
    static int element_count(const ta::TaskAssigningSolution& s) {
        return static_cast<int>(s.tasks.size());
    }
    static int list_size(const ta::TaskAssigningSolution& s, int listIndex) {
        return static_cast<int>(s.employees[listIndex].tasks.size());
    }
    static int element_at(const ta::TaskAssigningSolution& s, int listIndex, int position) {
        return s.employees[listIndex].tasks[position];
    }
    static void insert_element(ta::TaskAssigningSolution& s, int listIndex, int position,
                               int element) {
        std::vector<int>& tasks = s.employees[listIndex].tasks;
        tasks.insert(tasks.begin() + position, element);
    }
    static int remove_element(ta::TaskAssigningSolution& s, int listIndex, int position) {
        std::vector<int>& tasks = s.employees[listIndex].tasks;
        const int element = tasks[position];
        tasks.erase(tasks.begin() + position);
        return element;
    }
    static void reverse_segment(ta::TaskAssigningSolution& s, int listIndex, int first, int last) {
        std::vector<int>& tasks = s.employees[listIndex].tasks;
        std::reverse(tasks.begin() + first, tasks.begin() + last + 1);
    }
};

// The domain's medium fold for the decomposed calculator (see the Score
// adapter above): unassignedTasksCost (Level::Medium) folds below hard.
template <>
struct optimaru::MediumFoldOf<ta::TaskAssigningSolution> {
    static constexpr optimaru::MediumFold value = ta::kMediumFold;
};

#endif  // TA_DOMAIN_HPP
