// Project-job-scheduling demo: rebuilds the Java quickstart's
// DemoDataGenerator instance (2 projects, 7 resources — 1 global renewable +
// per project 1 renewable / 2 non-renewable locals —, 24 jobs of which 20
// STANDARD with 3 execution modes each, delays 0..499), solves for N seconds
// and asserts the result is feasible (hard == 0).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "pjs/domain.hpp"
#include "pjs/score_calculator.hpp"
#include "pjs/solver.hpp"

using namespace pjs;

namespace
{

int randInt(std::mt19937_64 &rng, int minInclusive, int maxInclusive)
{
    std::uniform_int_distribution<int> pick(minInclusive, maxInclusive);
    return pick(rng);
}

double unitDraw(std::mt19937_64 &rng)
{
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    return unit(rng);
}

// The Java generator's Pair<Float, T> tables: the index of the first
// cumulative threshold >= a unit draw.
int drawIndex(std::mt19937_64 &rng, const std::vector<double> &cumulative)
{
    const double draw = unitDraw(rng);
    for (std::size_t i = 0; i < cumulative.size(); ++i)
    {
        if (draw <= cumulative[i])
            return static_cast<int>(i);
    }
    return static_cast<int>(cumulative.size()) - 1;
}

ProjectJobSchedule generateDemoData()
{
    std::mt19937_64 rng(0);
    ProjectJobSchedule schedule;

    // Projects (the Java FIRST_PROJECT / SECOND_PROJECT).
    schedule.projects.push_back({"0", 0, 10});
    schedule.projects.push_back({"1", 4, 19});

    // Resources: one global renewable + per project one renewable local and
    // two non-renewable locals.
    schedule.resources.push_back({"0", 16, true, -1});
    schedule.resources.push_back({"1", 13, true, 0});
    schedule.resources.push_back({"2", 44, false, 0});
    schedule.resources.push_back({"3", 39, false, 0});
    schedule.resources.push_back({"4", 24, true, 1});
    schedule.resources.push_back({"5", 66, false, 1});
    schedule.resources.push_back({"6", 56, false, 1});

    // Jobs: 12 per project — SOURCE, 10x STANDARD, SINK.
    const int jobsCountPerProject = 12;
    int modeCounter = 0;
    int requirementCounter = 0;
    std::vector<std::vector<int>> successorJobs;  // global job indices
    for (int p = 0; p < 2; ++p)
    {
        const int base = p * jobsCountPerProject;
        // The project's resource pool the generator draws from:
        // [global, local renewable, local non-renewable x2].
        const std::vector<int> pool = {0, 3 * p + 1, 3 * p + 2, 3 * p + 3};
        for (int i = 0; i < jobsCountPerProject; ++i)
        {
            Job job;
            job.id = std::to_string(base + i);
            job.project = p;
            job.jobType = i == 0                         ? JobType::SOURCE
                          : i == jobsCountPerProject - 1 ? JobType::SINK
                                                         : JobType::STANDARD;
            schedule.jobs.push_back(job);
        }
        // Execution modes, in job order (like the Java forEach).
        for (int i = 0; i < jobsCountPerProject; ++i)
        {
            Job &job = schedule.jobs[base + i];
            if (job.jobType != JobType::STANDARD)
            {
                job.executionModes.push_back(
                    {std::to_string(modeCounter++), 0, {}});
                continue;
            }
            // Three modes; the requirement count is drawn once per job.
            const int requirementsSize = randInt(rng, 1, 3);
            for (int m = 0; m < 3; ++m)
            {
                ExecutionMode mode;
                mode.id = std::to_string(modeCounter++);
                mode.duration = randInt(rng, 1, 5);
                while (static_cast<int>(mode.resourceRequirements.size())
                       < requirementsSize)
                {
                    // 40% global, 20% / 15% / 25% the project's locals.
                    const int resource
                        = pool[drawIndex(rng, {0.4, 0.6, 0.75, 1.0})];
                    const bool duplicate
                        = std::any_of(mode.resourceRequirements.begin(),
                                      mode.resourceRequirements.end(),
                                      [resource](const ResourceRequirement &req)
                                      { return req.resource == resource; });
                    if (!duplicate)
                    {
                        mode.resourceRequirements.push_back(
                            {resource, randInt(rng, 1, 5)});
                        ++requirementCounter;
                    }
                }
                job.executionModes.push_back(mode);
            }
        }
        // Successor jobs: the source feeds the first three STANDARD jobs;
        // every other job feeds 1 (54%) / 2 (27%) / 3 (19%) later jobs;
        // the last STANDARD job feeds the sink, the sink feeds nothing.
        std::vector<std::vector<int>> perProject(jobsCountPerProject);
        perProject[0] = {base + 1, base + 2, base + 3};
        for (int i = 1; i < jobsCountPerProject; ++i)
        {
            int count = drawIndex(rng, {0.54, 0.81, 1.0}) + 1;
            count = std::min(count, jobsCountPerProject - i - 1);
            std::vector<int> successors;
            while (static_cast<int>(successors.size()) < count)
            {
                const int jobIdx
                    = base + randInt(rng, i + 1, jobsCountPerProject - 1);
                if (std::find(successors.begin(), successors.end(), jobIdx)
                    == successors.end())
                {
                    successors.push_back(jobIdx);
                }
            }
            std::sort(successors.begin(), successors.end());
            perProject[i] = successors;
        }
        perProject[jobsCountPerProject - 2] = {base + jobsCountPerProject - 1};
        perProject[jobsCountPerProject - 1] = {};
        successorJobs.insert(
            successorJobs.end(), perProject.begin(), perProject.end());
    }
    (void)requirementCounter;

    // Allocations: one per job; predecessors invert the successor lists;
    // SOURCE/SINK allocations are preassigned (first mode, delay 0) and
    // pinned via their job type.
    const int jobCount = static_cast<int>(schedule.jobs.size());
    schedule.allocations.resize(jobCount);
    for (int a = 0; a < jobCount; ++a)
    {
        schedule.allocations[a].id = std::to_string(a);
        schedule.allocations[a].job = a;
        if (schedule.jobs[a].jobType != JobType::STANDARD)
            schedule.allocations[a].modeDelay = 0;  // (mode 0, delay 0)
    }
    for (int a = 0; a < jobCount; ++a)
    {
        for (int s : successorJobs[a])
            schedule.allocations[s].predecessorAllocations.push_back(a);
    }
    return schedule;
}

void printSummary(const ProjectJobSchedule &schedule)
{
    std::printf("  [프로젝트별 완료]\n");
    for (int a = 0; a < schedule.allocationCount(); ++a)
    {
        if (schedule.jobTypeOf(a) != JobType::SINK)
            continue;
        const Project &project
            = schedule
                  .projects[schedule.jobs[schedule.allocations[a].job].project];
        const int endDate = schedule.allocations[a].endDate;
        const int criticalPathEnd = project.criticalPathEndDate();
        std::printf("    프로젝트 %s: 완료일 %d (critical path 종료 %d, "
                    "지연 %d)\n",
                    project.id.c_str(),
                    endDate,
                    criticalPathEnd,
                    std::max(0, endDate - criticalPathEnd));
    }

    std::printf("  [할당 상세: 처음 12건]\n");
    for (int a = 0; a < schedule.allocationCount() && a < 12; ++a)
    {
        const Allocation &allocation = schedule.allocations[a];
        const Job &job = schedule.jobs[allocation.job];
        const char *type = job.jobType == JobType::SOURCE ? "SOURCE"
                           : job.jobType == JobType::SINK ? "SINK  "
                                                          : "STD   ";
        if (allocation.modeDelay < 0)
        {
            std::printf("    Job %-2s %s  (미배정)\n", job.id.c_str(), type);
            continue;
        }
        std::printf("    Job %-2s %s  mode %d  delay %-3d  day %3d-%3d\n",
                    job.id.c_str(),
                    type,
                    schedule.executionModeIndexOfValue(a, allocation.modeDelay),
                    schedule.delayOfValue(allocation.modeDelay),
                    allocation.startDate,
                    allocation.endDate);
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char **argv)
{
    const int seconds = (argc > 1) ? std::atoi(argv[1]) : 10;

    ProjectJobSchedule schedule = generateDemoData();
    int standardJobs = 0;
    for (const Job &job : schedule.jobs)
    {
        if (job.jobType == JobType::STANDARD)
            ++standardJobs;
    }
    std::printf("== 프로젝트 잡 스케줄링 데모 (프로젝트 %zu, 리소스 %zu, "
                "잡 %zu (STANDARD %d), delay 0..%d, %d초) ==\n",
                schedule.projects.size(),
                schedule.resources.size(),
                schedule.jobs.size(),
                standardJobs,
                schedule.delayRangeSize - 1,
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
    printSummary(schedule);

    if (schedule.score.hard == 0)
    {
        std::printf("  hard 0 도달: OK\n");
        return 0;
    }
    std::printf("  hard 0 도달 실패 (hard=%lld)\n",
                static_cast<long long>(schedule.score.hard));
    return 1;
}
