// Bin-packing demo: the web demo instance (20 items, capacity 100, four
// colours and three conflict pairs). The weights sum to 746, so at least
// ceil(746 / 100) = 8 bins are needed. Solves for N seconds (default 2) and
// asserts a feasible (0hard) result.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bp/domain.hpp"
#include "bp/solver.hpp"

using namespace bp;

namespace {

BinPackingProblem buildDemoProblem() {
    struct Row {
        const char* id;
        std::int64_t weight;
        const char* color;
    };
    const Row rows[] = {
            {"I01", 48, "red"},    {"I02", 30, "blue"},  {"I03", 19, "green"},
            {"I04", 36, "yellow"}, {"I05", 36, nullptr}, {"I06", 27, nullptr},
            {"I07", 42, "red"},    {"I08", 42, "blue"},  {"I09", 42, "green"},
            {"I10", 42, "yellow"}, {"I11", 42, nullptr}, {"I12", 42, nullptr},
            {"I13", 24, nullptr},  {"I14", 24, nullptr}, {"I15", 51, "red"},
            {"I16", 49, "blue"},   {"I17", 55, nullptr}, {"I18", 45, nullptr},
            {"I19", 33, nullptr},  {"I20", 17, nullptr},
    };
    BinPackingProblem problem;
    problem.binCapacity = 100;
    for (const Row& row : rows) {
        Item item;
        item.id = row.id;
        item.weight = row.weight;
        if (row.color != nullptr) item.color = std::string(row.color);
        problem.items.push_back(item);
    }
    problem.conflicts = {{"I01", "I16"}, {"I07", "I18"}, {"I10", "I19"}};
    return problem;
}

}  // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 2;
    BinPackingProblem problem = buildDemoProblem();

    Solver::Config config;
    config.time_limit = std::chrono::seconds(seconds);
    config.seed = 42;
    Solver solver(config);
    solver.solve(problem);

    std::printf("score %s, %d bins\n", problem.score.str().c_str(), problem.binsUsed());
    const std::vector<std::int64_t> loads = problem.binLoads();
    for (std::size_t bin = 0; bin < loads.size(); ++bin) {
        std::printf("  bin %zu (load %lld):", bin, static_cast<long long>(loads[bin]));
        for (const Item& item : problem.items) {
            if (item.bin == static_cast<int>(bin)) std::printf(" %s", item.id.c_str());
        }
        std::printf("\n");
    }
    if (!problem.score.feasible()) {
        std::fprintf(stderr, "infeasible result\n");
        return 1;
    }
    return 0;
}
