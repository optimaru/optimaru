#include "fp/score_calculator.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace fp
{

namespace
{

// The queried cleanup duration; throws like Java's
// Product.getCleanupDuration when the map has no entry for the pair.
Minutes
cleanupOrThrow(const PackagingSchedule &s, int previousProduct, int product)
{
    const Minutes cleanup = s.cleanup(previousProduct, product);
    if (cleanup == kMissingCleanup)
    {
        throw std::invalid_argument(
            "Cleanup duration previousProduct ("
            + s.products[previousProduct].name + ") to toProduct ("
            + s.products[product].name + ") is missing.");
    }
    return cleanup;
}

}  // namespace

void ScoreCalculator::prepare(PackagingSchedule &schedule)
{
    if (schedule.prepared)
        return;

    const std::size_t productCount = schedule.products.size();
    if (schedule.cleanupMinutes.size() != productCount * productCount)
    {
        throw std::invalid_argument(
            "cleanupMinutes must be a products x products matrix ("
            + std::to_string(productCount * productCount) + " entries, got "
            + std::to_string(schedule.cleanupMinutes.size()) + ")");
    }
    for (const Minutes cleanup : schedule.cleanupMinutes)
    {
        if (cleanup < 0 && cleanup != kMissingCleanup)
            throw std::invalid_argument("cleanup durations must be >= 0");
    }
    for (const Job &job : schedule.jobs)
    {
        if (job.durationMinutes < 0)
            throw std::invalid_argument("job " + job.id
                                        + ": duration must be >= 0");
        if (job.productIndex < 0
            || job.productIndex >= static_cast<int>(productCount))
            throw std::invalid_argument("job " + job.id
                                        + ": bad product index");
    }
    for (const Line &line : schedule.lines)
    {
        if (line.operatorIndex >= static_cast<int>(schedule.operators.size()))
            throw std::invalid_argument("line " + line.id
                                        + ": bad operator index");
    }

    // Every job may appear in at most one list (a line or the pool).
    const int jobCount = static_cast<int>(schedule.jobs.size());
    std::vector<char> seen(jobCount, 0);
    for (int l = 0; l < schedule.listCountWithPool(); ++l)
    {
        for (const int job : schedule.rawList(l))
        {
            if (job < 0 || job >= jobCount)
                throw std::invalid_argument("job index out of range in list");
            if (seen[job])
                throw std::invalid_argument("job " + schedule.jobs[job].id
                                            + " appears in more than one list");
            seen[job] = 1;
        }
    }

    // A pinned job with no list stays unassigned: park it in the pool so the
    // construction heuristic cannot pick it up.
    for (int j = 0; j < jobCount; ++j)
    {
        if (schedule.jobs[j].pinned && !seen[j])
            schedule.unassignedJobs.push_back(j);
    }
    // Pool order carries no meaning: normalize pinned jobs to the front so
    // the pool gets the same pinned-prefix shape as the lines.
    std::stable_partition(schedule.unassignedJobs.begin(),
                          schedule.unassignedJobs.end(),
                          [&](int j) { return schedule.jobs[j].pinned; });

    // Pinned jobs must form a prefix of their line: the engine's list moves
    // insert/remove at arbitrary positions, so a pinned job after an
    // unpinned one could not keep its exact slot. (The quickstart pins jobs
    // already started — always a prefix.)
    schedule.pinnedPrefixLen.assign(schedule.listCountWithPool(), 0);
    for (int l = 0; l < schedule.listCountWithPool(); ++l)
    {
        const std::vector<int> &list = schedule.rawList(l);
        std::size_t prefix = 0;
        while (prefix < list.size() && schedule.jobs[list[prefix]].pinned)
            ++prefix;
        for (std::size_t p = prefix; p < list.size(); ++p)
        {
            if (schedule.jobs[list[p]].pinned)
                throw std::invalid_argument(
                    "pinned job " + schedule.jobs[list[p]].id
                    + " must be part of the pinned prefix of its line");
        }
        schedule.pinnedPrefixLen[l] = static_cast<int>(prefix);
    }

    schedule.engineToJob.clear();
    schedule.jobToEngine.assign(jobCount, -1);
    for (int j = 0; j < jobCount; ++j)
    {
        if (schedule.jobs[j].pinned)
            continue;
        schedule.jobToEngine[j] = static_cast<int>(schedule.engineToJob.size());
        schedule.engineToJob.push_back(j);
    }

    schedule.prepared = true;
}

void ScoreCalculator::updateShadowVariables(PackagingSchedule &schedule)
{
    for (Job &job : schedule.jobs)
    {
        job.lineIndex = -1;
        job.startCleaningDateTime = kNoTime;
        job.startProductionDateTime = kNoTime;
        job.endDateTime = kNoTime;
    }
    for (int l = 0; l < schedule.lineCount(); ++l)
    {
        const Line &line = schedule.lines[l];
        int previousJob = -1;
        for (const int j : line.jobs)
        {
            Job &job = schedule.jobs[j];
            job.lineIndex = l;
            if (previousJob < 0)
            {
                // Java: the first job starts cleaning AND production at the
                // line start (zero-length cleaning window).
                job.startCleaningDateTime = line.startDateTime;
                job.startProductionDateTime = line.startDateTime;
            }
            else
            {
                const Job &previous = schedule.jobs[previousJob];
                job.startCleaningDateTime = previous.endDateTime;
                job.startProductionDateTime
                    = job.startCleaningDateTime
                      + cleanupOrThrow(
                          schedule, previous.productIndex, job.productIndex);
            }
            job.endDateTime = job.startProductionDateTime + job.durationMinutes;
            previousJob = j;
        }
    }
}

ConstraintBreakdown ScoreCalculator::analyze(PackagingSchedule &schedule)
{
    prepare(schedule);
    updateShadowVariables(schedule);
    ConstraintBreakdown breakdown;

    // maxEndDateTime (hard) / idealEndDateTime (medium) /
    // maximizeJobsAssigned (medium).
    for (const Job &job : schedule.jobs)
    {
        if (job.endDateTime == kNoTime)
        {
            breakdown.maximizeJobsAssigned += job.durationMinutes;
            continue;
        }
        if (job.maxEndTime < job.endDateTime)
            breakdown.maxEndDateTime += job.endDateTime - job.maxEndTime;
        if (job.idealEndTime < job.endDateTime)
            breakdown.idealEndDateTime += job.endDateTime - job.idealEndTime;
    }

    // operatorCleaningConflict (hard): every unique pair of assigned jobs
    // whose lines share an operator (Java's Joiners.equal groups the null
    // operator with itself too) and whose cleaning windows overlap. The
    // deliberately naive O(n^2) pair loop mirrors forEachUniquePair.
    const int jobCount = static_cast<int>(schedule.jobs.size());
    for (int a = 0; a < jobCount; ++a)
    {
        const Job &jobA = schedule.jobs[a];
        if (jobA.lineIndex < 0)
            continue;
        for (int b = a + 1; b < jobCount; ++b)
        {
            const Job &jobB = schedule.jobs[b];
            if (jobB.lineIndex < 0)
                continue;
            if (schedule.lines[jobA.lineIndex].operatorIndex
                != schedule.lines[jobB.lineIndex].operatorIndex)
                continue;
            const Minutes start = std::max(jobA.startCleaningDateTime,
                                           jobB.startCleaningDateTime);
            const Minutes end = std::min(jobA.startProductionDateTime,
                                         jobB.startProductionDateTime);
            if (start < end)
                breakdown.operatorCleaningConflict += end - start;
        }
    }

    // minimizeMakespan (soft): the last job of each non-empty line.
    for (const Line &line : schedule.lines)
    {
        if (line.jobs.empty())
            continue;
        const Minutes makespan
            = schedule.jobs[line.jobs.back()].endDateTime - line.startDateTime;
        breakdown.minimizeMakespan += makespan * makespan;
    }
    return breakdown;
}

Score ScoreCalculator::calculateScore(PackagingSchedule &schedule)
{
    schedule.score = analyze(schedule).toScore();
    return schedule.score;
}

}  // namespace fp
