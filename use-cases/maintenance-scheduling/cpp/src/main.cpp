// Demo: reproduces the Java quickstart's DemoDataGenerator instance (SMALL
// by default, LARGE with `--large`), solves it for N seconds (default 10)
// and prints the per-crew schedule. Exits non-zero unless the solution is
// feasible (0hard).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mnt/demo_data.hpp"
#include "mnt/domain.hpp"
#include "mnt/score_calculator.hpp"
#include "mnt/solver.hpp"

using namespace mnt;

namespace
{

void printSchedule(const MaintenanceSchedule &schedule)
{
    for (int c = 0; c < schedule.crewCount(); ++c)
    {
        std::printf("  [%s]\n", schedule.crews[c].name.c_str());
        // Jobs of this crew ordered by start date.
        std::vector<int> jobIndexes;
        for (int j = 0; j < static_cast<int>(schedule.jobs.size()); ++j)
        {
            const Job &job = schedule.jobs[j];
            if (job.crewDate >= 0 && schedule.crewIndexOf(job.crewDate) == c)
                jobIndexes.push_back(j);
        }
        for (std::size_t a = 0; a < jobIndexes.size(); ++a)
        {
            for (std::size_t b = a + 1; b < jobIndexes.size(); ++b)
            {
                if (schedule.dateIndexOf(schedule.jobs[jobIndexes[b]].crewDate)
                    < schedule.dateIndexOf(
                        schedule.jobs[jobIndexes[a]].crewDate))
                {
                    std::swap(jobIndexes[a], jobIndexes[b]);
                }
            }
        }
        for (int j : jobIndexes)
        {
            const Job &job = schedule.jobs[j];
            const int dateIndex = schedule.dateIndexOf(job.crewDate);
            const std::int64_t start = schedule.startDates[dateIndex];
            const std::int64_t end
                = schedule.endDateFor(job.durationInDays, dateIndex);
            std::string tags;
            for (const std::string &tag : job.tags)
            {
                tags += tags.empty() ? "" : ",";
                tags += tag;
            }
            std::printf("    %-18s %2d d  %s -> %s  (ideal %s, max %s) [%s]\n",
                        job.name.c_str(),
                        job.durationInDays,
                        formatDate(start).c_str(),
                        formatDate(end).c_str(),
                        formatDate(job.idealEndDate).c_str(),
                        formatDate(job.maxEndDate).c_str(),
                        tags.c_str());
        }
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char **argv)
{
    int seconds = 10;
    DemoData size = DemoData::SMALL;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--large") == 0)
            size = DemoData::LARGE;
        else
            seconds = std::atoi(argv[i]);
    }

    const std::int64_t fromDate = nextMonday(todayEpochDay());
    MaintenanceSchedule schedule = generateDemoData(size, fromDate);
    std::printf("== maintenance scheduling demo (%s: %d crews, %zu jobs, %s ~ %s, "
                "%d s) ==\n",
                size == DemoData::LARGE ? "LARGE" : "SMALL",
                schedule.crewCount(),
                schedule.jobs.size(),
                formatDate(schedule.fromDate).c_str(),
                formatDate(schedule.toDate).c_str(),
                seconds);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(seconds);
    config.randomSeed = 42;
    config.onNewBest = [](std::int64_t step, const Score &score)
    {
        std::printf("    new best @step %-8lld %s\n",
                    static_cast<long long>(step),
                    score.toString().c_str());
    };
    Solver solver(config);
    solver.solve(schedule);

    std::printf("\n  final score: %s  (feasible=%s)\n\n",
                schedule.score.toString().c_str(),
                schedule.score.isFeasible() ? "true" : "false");
    printSchedule(schedule);

    bool allAssigned = true;
    for (const Job &job : schedule.jobs)
    {
        if (job.crewDate < 0)
            allAssigned = false;
    }
    std::printf("  all jobs assigned: %s\n", allAssigned ? "OK" : "FAIL");

    if (schedule.score.hard == 0 && allAssigned)
    {
        std::printf("  reached hard 0: OK\n");
        return 0;
    }
    std::printf("  failed to reach hard 0 (hard=%lld)\n",
                static_cast<long long>(schedule.score.hard));
    return 1;
}
