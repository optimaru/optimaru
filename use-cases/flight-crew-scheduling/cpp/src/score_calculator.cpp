#include "fcs/score_calculator.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace fcs
{

void ScoreCalculator::prepare(FlightCrewSchedule &schedule)
{
    if (schedule.prepared)
        return;

    // Intern skills: employee skill sets first, then required skills (any
    // required skill nobody holds still gets an id and simply never matches).
    std::unordered_map<std::string, int> skillIds;
    const auto internSkill = [&](const std::string &skill)
    {
        auto [it, inserted]
            = skillIds.emplace(skill, static_cast<int>(skillIds.size()));
        return it->second;
    };
    for (const Employee &employee : schedule.employees)
    {
        for (const std::string &skill : employee.skills)
            internSkill(skill);
    }
    for (FlightAssignment &assignment : schedule.flightAssignments)
        assignment.requiredSkillId = internSkill(assignment.requiredSkill);
    schedule.skillCount = static_cast<int>(skillIds.size());

    const std::size_t employeeCount = schedule.employees.size();
    schedule.employeeSkillFlat.assign(employeeCount * schedule.skillCount, 0);
    for (std::size_t e = 0; e < employeeCount; ++e)
    {
        for (const std::string &skill : schedule.employees[e].skills)
        {
            schedule
                .employeeSkillFlat[e * schedule.skillCount + skillIds.at(skill)]
                = 1;
        }
    }

    // Per-flight calendar days + the day window they span.
    const std::size_t flightCount = schedule.flights.size();
    schedule.flightDepDay.resize(flightCount);
    schedule.flightArrDay.resize(flightCount);
    DayIndex minDay = 0;
    DayIndex maxDay = -1;
    for (std::size_t f = 0; f < flightCount; ++f)
    {
        const DayIndex dep = dayOf(schedule.flights[f].departureUTC);
        const DayIndex arr = dayOf(schedule.flights[f].arrivalUTC);
        schedule.flightDepDay[f] = dep;
        schedule.flightArrDay[f] = arr;
        if (maxDay < minDay)
        {
            minDay = dep;
            maxDay = arr;
        }
        else
        {
            minDay = std::min(minDay, dep);
            maxDay = std::max(maxDay, arr);
        }
    }
    schedule.minDay = minDay;
    schedule.dayCount
        = maxDay < minDay ? 0 : static_cast<int>(maxDay - minDay + 1);

    // Flatten unavailability over the flight-day window (days outside it can
    // never intersect a flight).
    schedule.employeeUnavailableFlat.assign(employeeCount * schedule.dayCount,
                                            0);
    for (std::size_t e = 0; e < employeeCount; ++e)
    {
        for (const DayIndex day : schedule.employees[e].unavailableDays)
        {
            if (day < schedule.minDay
                || day >= schedule.minDay + schedule.dayCount)
                continue;
            schedule.employeeUnavailableFlat[e * schedule.dayCount
                                             + static_cast<std::size_t>(
                                                 day - schedule.minDay)] = 1;
        }
    }

    schedule.prepared = true;
}

bool ScoreCalculator::missingRequiredSkill(const FlightCrewSchedule &schedule,
                                           int assignmentIndex,
                                           int employeeIndex)
{
    return !schedule.employeeHasSkill(
        employeeIndex,
        schedule.flightAssignments[assignmentIndex].requiredSkillId);
}

bool ScoreCalculator::unavailableEmployee(const FlightCrewSchedule &schedule,
                                          int assignmentIndex,
                                          int employeeIndex)
{
    const int flight = schedule.flightAssignments[assignmentIndex].flight;
    const DayIndex first = schedule.flightDepDay[flight];
    const DayIndex last = schedule.flightArrDay[flight];
    for (DayIndex day = first; day <= last; ++day)
    {
        if (schedule.employeeUnavailableOn(employeeIndex, day))
            return true;
    }
    return false;
}

void ScoreCalculator::sortByDeparture(const FlightCrewSchedule &schedule,
                                      std::vector<int> &assignmentIndices)
{
    std::sort(assignmentIndices.begin(),
              assignmentIndices.end(),
              [&schedule](int a, int b)
              {
                  const Minutes da = schedule.departureOf(a);
                  const Minutes db = schedule.departureOf(b);
                  if (da != db)
                      return da < db;
                  return a < b;
              });
}

ItineraryStats
ScoreCalculator::itineraryStats(const FlightCrewSchedule &schedule,
                                int employeeIndex,
                                const std::vector<int> &sorted)
{
    ItineraryStats stats;
    const int n = static_cast<int>(sorted.size());
    if (n == 0)
        return stats;

    // hard 2: flight conflict — overlapping pairs. Sorted by departure, so
    // the inner loop stops at the first non-overlapping departure.
    for (int i = 0; i < n; ++i)
    {
        const Minutes arrI = schedule.arrivalOf(sorted[i]);
        const Minutes depI = schedule.departureOf(sorted[i]);
        for (int j = i + 1; j < n; ++j)
        {
            const Minutes depJ = schedule.departureOf(sorted[j]);
            if (depJ >= arrI)
                break;  // departures only grow from here
            if (depI < schedule.arrivalOf(sorted[j]))
                ++stats.conflictPairs;
        }
    }

    // Group by equal departure time: [groupStart, groupEnd).
    const int home = schedule.employees[employeeIndex].homeAirport;
    int groupStart = 0;
    int prevGroupStart = -1;  // previous group's bounds
    int prevGroupEnd = -1;
    while (groupStart < n)
    {
        const Minutes dep = schedule.departureOf(sorted[groupStart]);
        int groupEnd = groupStart + 1;
        while (groupEnd < n && schedule.departureOf(sorted[groupEnd]) == dep)
            ++groupEnd;

        // hard 3: transfer — a pair (a, b) is departure-consecutive iff a is
        // ALONE on its departure time and b is in the very next group (any
        // other same-time-as-a assignment `c` satisfies dep(a) <= dep(c) <
        // dep(b), so the Java ifNotExists kills the pair).
        if (prevGroupStart >= 0 && prevGroupEnd - prevGroupStart == 1)
        {
            const int a = sorted[prevGroupStart];
            const int arrAirport = schedule.arrivalAirportOf(a);
            for (int k = groupStart; k < groupEnd; ++k)
            {
                if (schedule.departureAirportOf(sorted[k]) != arrAirport)
                    ++stats.transferMismatches;
            }
        }

        // soft 5: every member of the FIRST departure-time group counts.
        if (groupStart == 0)
        {
            for (int k = groupStart; k < groupEnd; ++k)
            {
                if (schedule.departureAirportOf(sorted[k]) != home)
                    ++stats.firstNotHome;
            }
        }
        // soft 6: every member of the LAST departure-time group counts.
        if (groupEnd == n)
        {
            for (int k = groupStart; k < groupEnd; ++k)
            {
                if (schedule.arrivalAirportOf(sorted[k]) != home)
                    ++stats.lastNotHome;
            }
        }

        prevGroupStart = groupStart;
        prevGroupEnd = groupEnd;
        groupStart = groupEnd;
    }
    return stats;
}

Score ScoreCalculator::calculateScore(FlightCrewSchedule &schedule)
{
    prepare(schedule);

    std::int64_t missingSkill = 0;
    std::int64_t unavailable = 0;
    std::vector<std::vector<int>> byEmployee(schedule.employees.size());
    for (int i = 0; i < static_cast<int>(schedule.flightAssignments.size());
         ++i)
    {
        const int employee = schedule.flightAssignments[i].employee;
        if (employee < 0)
            continue;  // unassigned contributes nothing
        if (missingRequiredSkill(schedule, i, employee))
            ++missingSkill;
        if (unavailableEmployee(schedule, i, employee))
            ++unavailable;
        byEmployee[employee].push_back(i);
    }

    std::int64_t conflictPairs = 0;
    std::int64_t transfers = 0;
    std::int64_t firstNotHome = 0;
    std::int64_t lastNotHome = 0;
    for (int e = 0; e < static_cast<int>(byEmployee.size()); ++e)
    {
        if (byEmployee[e].empty())
            continue;
        sortByDeparture(schedule, byEmployee[e]);
        const ItineraryStats stats = itineraryStats(schedule, e, byEmployee[e]);
        conflictPairs += stats.conflictPairs;
        transfers += stats.transferMismatches;
        firstNotHome += stats.firstNotHome;
        lastNotHome += stats.lastNotHome;
    }

    Score score;
    score.hard = -(REQUIRED_SKILL_WEIGHT * missingSkill
                   + FLIGHT_CONFLICT_WEIGHT * conflictPairs
                   + TRANSFER_WEIGHT * transfers
                   + UNAVAILABILITY_WEIGHT * unavailable);
    score.soft = -(FIRST_NOT_HOME_WEIGHT * static_cast<double>(firstNotHome)
                   + LAST_NOT_HOME_WEIGHT * static_cast<double>(lastNotHome));
    if (score.soft == 0.0)
        score.soft = 0.0;  // normalize -0.0
    schedule.score = score;
    return score;
}

}  // namespace fcs
