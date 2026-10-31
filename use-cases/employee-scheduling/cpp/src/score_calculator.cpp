#include "es/score_calculator.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace es {

void ScoreCalculator::prepare(EmployeeSchedule& schedule) {
    const int employeeCount = static_cast<int>(schedule.employees.size());
    std::unordered_map<std::string, int> skillId;
    for (Shift& sh : schedule.shifts) {
        // The rules index per-day tables by these day numbers without a check.
        if (sh.start < 0 || sh.end <= sh.start || dayOf(sh.end) > MAX_DAY) {
            throw std::invalid_argument("shift '" + sh.id + "': need 0 <= start < end up to day " +
                                        std::to_string(MAX_DAY) + ", got start=" +
                                        std::to_string(sh.start) +
                                        ", end=" + std::to_string(sh.end));
        }
        // Every calculator indexes the employee list by this without a check.
        if (sh.employeeIndex < -1 || sh.employeeIndex >= employeeCount) {
            throw std::invalid_argument("shift '" + sh.id + "': employee_index " +
                                        std::to_string(sh.employeeIndex) + " is out of range -1.." +
                                        std::to_string(employeeCount - 1));
        }
        // Empty requiredSkill == no skill requirement: bit 0 (matches everyone,
        // never a miss). Don't intern "" as a phantom skill nobody holds.
        sh.requiredSkillBit = 0;
        if (!sh.requiredSkill.empty()) {
            const int id = skillId.emplace(sh.requiredSkill, static_cast<int>(skillId.size()))
                                   .first->second;
            if (id >= 64) {
                throw std::invalid_argument("shifts require more than 64 distinct skills");
            }
            sh.requiredSkillBit = 1ULL << id;
        }
        sh.startDay = static_cast<int>(dayOf(sh.start));
        sh.endDay = static_cast<int>(dayOf(sh.end));
    }
    // Only required skills get a bit: one no shift asks for cannot change a score.
    for (Employee& e : schedule.employees) {
        std::uint64_t mask = 0;
        for (const std::string& sk : e.skills) {
            auto it = skillId.find(sk);
            if (it != skillId.end()) mask |= (1ULL << it->second);
        }
        e.skillMask = mask;
    }
}

Minutes ScoreCalculator::minutesOnDay(const Shift& shift, DayIndex day) {
    Minutes dayStart = day * MINUTES_PER_DAY;
    // Whole minutes are counted up to 23:59, so the usable window ends one minute
    // short of midnight when a shift reaches or crosses the day boundary.
    Minutes lo = std::max(shift.start, dayStart);
    Minutes hi = std::min(shift.end, dayStart + MINUTES_PER_DAY);
    if (hi >= dayStart + MINUTES_PER_DAY) {
        hi = dayStart + MINUTES_PER_DAY - 1;
    }
    return std::max<Minutes>(0, hi - lo);
}

Minutes ScoreCalculator::overlapWithDates(const Shift& shift, const std::set<DayIndex>& dates) {
    if (dates.empty()) return 0;
    Minutes total = 0;
    if (dates.count(shift.startDay)) total += minutesOnDay(shift, shift.startDay);
    if (shift.endDay != shift.startDay && dates.count(shift.endDay))
        total += minutesOnDay(shift, shift.endDay);
    return total;
}

std::int64_t ScoreCalculator::shiftHardPenalty(const Employee& emp, const Shift& shift) {
    std::int64_t p = 0;
    if (shift.requiredSkillBit != 0 && (emp.skillMask & shift.requiredSkillBit) == 0)
        p += 1;  // Missing required skill (bit 0 == no requirement)
    p += overlapWithDates(shift, emp.unavailableDates);         // Unavailable employee
    return p;
}

std::int64_t ScoreCalculator::shiftSoftDelta(const Employee& emp, const Shift& shift) {
    return overlapWithDates(shift, emp.desiredDates)         // Desired day (reward)
           - overlapWithDates(shift, emp.undesiredDates);    // Undesired day (penalty)
}

std::int64_t ScoreCalculator::pairHardPenalty(const Shift& a, const Shift& b) {
    std::int64_t p = 0;
    // Overlapping shift (overlap minutes).
    Minutes overlap = std::min(a.end, b.end) - std::max(a.start, b.start);
    if (overlap > 0) p += overlap;
    // Max one shift per day (precomputed day index avoids a division per pair).
    if (a.startDay == b.startDay) p += 1;
    // At least 10 hours between two shifts (a zero gap counts).
    const Shift& early = (a.start <= b.start) ? a : b;
    const Shift& late = (a.start <= b.start) ? b : a;
    if (early.end <= late.start) {
        Minutes gap = late.start - early.end;
        if (gap < MIN_REST_MINUTES) p += (MIN_REST_MINUTES - gap);
    }
    return p;
}

double ScoreCalculator::unfairnessFromAggregates(long sumOfLoads, long sumOfSquares, int itemCount) {
    if (itemCount == 0) return 0.0;
    // ssd = sum(load^2) - sum(load)^2 / n  (identical to the n==1 special case).
    double ssd = static_cast<double>(sumOfSquares) -
                 (static_cast<double>(sumOfLoads) * sumOfLoads) / static_cast<double>(itemCount);
    if (ssd < 0) ssd = 0;
    double root = std::sqrt(ssd);
    if (root == 0.0) return 0.0;
    // Round to 6 significant figures, HALF_EVEN.
    // Avoid log10(), pow() and process-global floating-point environment
    // mutations in the per-move hot path.  The loop computes the same decimal
    // magnitude (ceil(log10(root))) using only a handful of comparisons.
    int digitsBeforePoint = 0;
    double magnitude = 1.0;
    if (root > 1.0) {
        while (magnitude < root) {
            magnitude *= 10.0;
            ++digitsBeforePoint;
        }
    } else {
        while (magnitude / 10.0 >= root) {
            magnitude /= 10.0;
            --digitsBeforePoint;
        }
    }
    double factor = 1.0;
    const int exponent = 6 - digitsBeforePoint;
    if (exponent >= 0) {
        for (int i = 0; i < exponent; ++i) factor *= 10.0;
    } else {
        for (int i = 0; i > exponent; --i) factor /= 10.0;
    }
    const double scaled = root * factor;
    const double lower = std::floor(scaled);
    const double fraction = scaled - lower;
    const double rounded = fraction > 0.5 ||
                                   (fraction == 0.5 && std::fmod(lower, 2.0) != 0.0)
                           ? lower + 1.0
                           : lower;
    return rounded / factor;
}

double ScoreCalculator::unfairness(const std::vector<long>& loads) {
    long sum = 0, sumSq = 0;
    for (long v : loads) {
        sum += v;
        sumSq += v * v;
    }
    return unfairnessFromAggregates(sum, sumSq, static_cast<int>(loads.size()));
}

Score ScoreCalculator::calculateScore(EmployeeSchedule& schedule) {
    prepare(schedule);
    Score score;
    const auto& shifts = schedule.shifts;
    const auto& employees = schedule.employees;

    std::vector<std::vector<int>> byEmployee(employees.size());
    for (int i = 0; i < static_cast<int>(shifts.size()); ++i) {
        int e = shifts[i].employeeIndex;
        if (e >= 0) byEmployee[e].push_back(i);
    }

    // Per-shift constraints.
    for (const Shift& shift : shifts) {
        if (shift.employeeIndex < 0) continue;
        const Employee& emp = employees[shift.employeeIndex];
        score.hard -= shiftHardPenalty(emp, shift);
        score.soft += shiftSoftDelta(emp, shift);
    }

    // Pairwise constraints (same employee).
    for (const std::vector<int>& group : byEmployee) {
        for (std::size_t a = 0; a < group.size(); ++a) {
            for (std::size_t b = a + 1; b < group.size(); ++b) {
                score.hard -= pairHardPenalty(shifts[group[a]], shifts[group[b]]);
            }
        }
    }

    // Soft: balance employee shift assignments (all employees, including zero).
    if (!employees.empty()) {
        long sum = 0, sumSq = 0;
        for (const auto& g : byEmployee) {
            long c = static_cast<long>(g.size());
            sum += c;
            sumSq += c * c;
        }
        score.soft -= unfairnessFromAggregates(sum, sumSq, static_cast<int>(employees.size()));
    }

    schedule.score = score;
    return score;
}

}  // namespace es
