// Box-sizing demo: 12 candidate box sizes, 36 parcel sizes (630 shipments) and
// room on the shelf for 4 sizes. Solves for N seconds (default 2), prints the
// sizes to stock and asserts a feasible (0hard) result.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "bs/model/domain.hpp"
#include "bs/solver.hpp"

using namespace bs;

namespace {

// Sizes in mm, weights in g.
BoxSizingProblem buildDemoProblem() {
    struct Size {
        const char* id;
        std::int64_t length, width, height;
    };
    const Size sizes[] = {
            {"B01", 160, 120, 80},  {"B02", 200, 150, 100}, {"B03", 220, 190, 90},
            {"B04", 250, 200, 150}, {"B05", 270, 180, 150}, {"B06", 300, 220, 120},
            {"B07", 340, 250, 210}, {"B08", 350, 250, 150}, {"B09", 410, 310, 280},
            {"B10", 450, 350, 250}, {"B11", 480, 380, 340}, {"B12", 520, 400, 400},
    };
    BoxSizingProblem problem;
    for (const Size& size : sizes) {
        Box box;
        box.id = size.id;
        box.length = size.length;
        box.width = size.width;
        box.height = size.height;
        box.maxWeight = 30000;
        problem.boxes.push_back(box);
    }
    // 36 parcel sizes from a small linear congruential generator: the same
    // instance on every platform.
    std::uint64_t state = 20261128;
    const auto draw = [&state](std::int64_t low, std::int64_t high) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return low + static_cast<std::int64_t>((state >> 33) %
                                               static_cast<std::uint64_t>(high - low + 1));
    };
    for (int i = 0; i < 36; ++i) {
        Parcel parcel;
        parcel.id = "P" + std::string(i < 9 ? "0" : "") + std::to_string(i + 1);
        parcel.length = 10 * draw(8, 48);
        parcel.width = 10 * draw(6, 36);
        parcel.height = 10 * draw(3, 30);
        parcel.weight = 100 * draw(2, 120);
        parcel.quantity = draw(1, 34);
        problem.parcels.push_back(parcel);
    }
    problem.maxBoxTypes = 4;
    return problem;
}

}  // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 2;
    BoxSizingProblem problem = buildDemoProblem();
    std::printf("== box sizing demo (%d candidates, %d parcel sizes, at most %d sizes, %d s) ==\n",
                problem.boxCount(), problem.parcelCount(), *problem.maxBoxTypes, seconds);

    Solver::Config config;
    config.time_limit = std::chrono::seconds(seconds);
    config.seed = 42;
    Solver solver(config);
    solver.solve(problem);

    std::printf("score %s, %d sizes in use, fill ratio %.3f\n", problem.score.str().c_str(),
                problem.boxTypesUsed(), problem.fillRatio());
    const std::vector<std::int64_t> shipments = problem.parcelsPerBox();
    for (int b = 0; b < problem.boxCount(); ++b) {
        if (shipments[b] == 0) continue;
        const Box& box = problem.boxes[b];
        std::printf("  %s %lld x %lld x %lld: %lld shipments\n", box.id.c_str(),
                    static_cast<long long>(box.length), static_cast<long long>(box.width),
                    static_cast<long long>(box.height), static_cast<long long>(shipments[b]));
    }
    if (!problem.score.feasible()) {
        std::fprintf(stderr, "infeasible result\n");
        return 1;
    }
    return 0;
}
