// Demo: reproduces the Java quickstart's DemoDataGenerator instance (5
// lines, 100 jobs, java.util.Random(37) replica), solves it for N seconds
// (default 10) and prints the per-line schedule. Exits non-zero unless the
// solution is feasible (0hard).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "fp/demo_data.hpp"
#include "fp/domain.hpp"
#include "fp/score_calculator.hpp"
#include "fp/solver.hpp"

using namespace fp;

namespace
{

std::string formatTime(Minutes minute)
{
    // Times are relative to the demo's Monday 00:00 == minute 0.
    char buffer[32];
    std::snprintf(buffer,
                  sizeof(buffer),
                  "d%02lld %02lld:%02lld",
                  static_cast<long long>(minute / (24 * 60)),
                  static_cast<long long>(minute / 60 % 24),
                  static_cast<long long>(minute % 60));
    return buffer;
}

void printSchedule(const PackagingSchedule &schedule)
{
    for (int l = 0; l < schedule.lineCount(); ++l)
    {
        const Line &line = schedule.lines[l];
        const std::string operatorId
            = line.operatorIndex >= 0
                  ? schedule.operators[line.operatorIndex].id
                  : "(none)";
        std::printf("  [%s | %s] %zu jobs\n",
                    line.name.c_str(),
                    operatorId.c_str(),
                    line.jobs.size());
        for (const int j : line.jobs)
        {
            const Job &job = schedule.jobs[j];
            std::printf("    #%-3s %-42s clean %s  run %s -> %s  (ideal %s, "
                        "max %s)\n",
                        job.id.c_str(),
                        job.name.c_str(),
                        formatTime(job.startCleaningDateTime).c_str(),
                        formatTime(job.startProductionDateTime).c_str(),
                        formatTime(job.endDateTime).c_str(),
                        formatTime(job.idealEndTime).c_str(),
                        formatTime(job.maxEndTime).c_str());
        }
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char **argv)
{
    int seconds = 10;
    if (argc > 1)
        seconds = std::atoi(argv[1]);

    PackagingSchedule schedule = generateDemoData();
    std::printf("== 식품 포장 스케줄링 데모 (line %d, operator %zu, product "
                "%zu, job %zu, %d초) ==\n",
                schedule.lineCount(),
                schedule.operators.size(),
                schedule.products.size(),
                schedule.jobs.size(),
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

    int assigned = 0;
    for (const Job &job : schedule.jobs)
    {
        if (job.lineIndex >= 0)
            ++assigned;
    }
    std::printf("  배정된 작업: %d / %zu\n", assigned, schedule.jobs.size());

    if (schedule.score.hard == 0)
    {
        std::printf("  hard 0 도달: OK\n");
        return 0;
    }
    std::printf("  hard 0 도달 실패 (hard=%lld)\n",
                static_cast<long long>(schedule.score.hard));
    return 1;
}
