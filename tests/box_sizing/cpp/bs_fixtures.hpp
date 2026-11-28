// What bs_test.cpp and bs_plugin_test.cpp share: the builders of the small
// hand-made problems and of the random instances.
#ifndef BS_TEST_FIXTURES_HPP
#define BS_TEST_FIXTURES_HPP

#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <string>

#include "bs/model/domain.hpp"

namespace bs_fixtures {

inline void addBox(bs::BoxSizingProblem& problem, std::int64_t length, std::int64_t width,
                   std::int64_t height, std::optional<std::int64_t> maxWeight = std::nullopt,
                   double cost = 0.0, bool required = false) {
    bs::Box box;
    box.id = "B" + std::to_string(problem.boxes.size());
    box.length = length;
    box.width = width;
    box.height = height;
    box.maxWeight = maxWeight;
    box.cost = cost;
    box.required = required;
    problem.boxes.push_back(box);
    problem.prepared = false;
}

inline void addParcel(bs::BoxSizingProblem& problem, std::int64_t length, std::int64_t width,
                      std::int64_t height, int box = -1, std::int64_t quantity = 1,
                      std::int64_t weight = 0) {
    bs::Parcel parcel;
    parcel.id = "P" + std::to_string(problem.parcels.size());
    parcel.length = length;
    parcel.width = width;
    parcel.height = height;
    parcel.weight = weight;
    parcel.quantity = quantity;
    parcel.box = box;
    problem.parcels.push_back(parcel);
    problem.prepared = false;
}

// A random instance: `boxCount` candidate sizes of which the last takes every
// parcel, `parcelCount` parcel sizes with quantities 1..20, and at most
// `maxBoxTypes` sizes (0 = no limit). `costStep` > 0 gives every box a cost of
// a multiple of it. Raw draws of the generator only, so the instance is the
// same with every standard library.
inline bs::BoxSizingProblem randomInstance(int boxCount, int parcelCount, int maxBoxTypes,
                                           std::uint64_t seed, double costStep = 0.0) {
    std::mt19937_64 rng(seed);
    const auto draw = [&rng](std::int64_t low, std::int64_t high) {
        return low + static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(high - low + 1));
    };
    bs::BoxSizingProblem problem;
    for (int b = 0; b + 1 < boxCount; ++b) {
        addBox(problem, 10 * draw(10, 60), 10 * draw(8, 45), 10 * draw(5, 40), std::nullopt,
               costStep * static_cast<double>(draw(1, 40)));
    }
    addBox(problem, 600, 450, 400, std::nullopt, costStep * 40.0);
    for (int p = 0; p < parcelCount; ++p) {
        addParcel(problem, 10 * draw(5, 60), 10 * draw(4, 45), 10 * draw(2, 40), -1, draw(1, 20),
                  100 * draw(1, 200));
    }
    if (maxBoxTypes > 0) problem.maxBoxTypes = maxBoxTypes;
    return problem;
}

// Shipping prices on a random instance: every box bills by its size at
// `divisor` (5,000 in mm and g: 1 kg per 5 litres), and the parcels ship to
// three zones, the first priced by the table without a zone. The breaks run
// to 30 kg, above the dimensional weight of the largest box (600 x 450 x 400:
// 21.6 kg) and the heaviest parcel (20 kg), and the farther zone pays more
// per break.
inline void addRates(bs::BoxSizingProblem& problem, std::int64_t divisor = 5000) {
    for (bs::Box& box : problem.boxes) box.dimDivisor = divisor;
    const std::int64_t limits[] = {1000, 2000, 5000, 10000, 20000, 30000};
    for (int zone = 0; zone < 3; ++zone) {
        bs::RateTable table;
        if (zone > 0) table.zone = "Z" + std::to_string(zone);
        for (int i = 0; i < 6; ++i) {
            table.breaks.push_back({limits[i], (3.0 + 1.5 * i) * (1.0 + 0.5 * zone)});
        }
        problem.rateTables.push_back(table);
    }
    for (std::size_t p = 0; p < problem.parcels.size(); ++p) {
        if (p % 3 != 0) problem.parcels[p].zone = "Z" + std::to_string(p % 3);
    }
    problem.prepared = false;
}

inline bool sameScore(const bs::Score& a, const bs::Score& b) {
    return a.hard == b.hard && std::fabs(a.soft - b.soft) < 1e-9;
}

}  // namespace bs_fixtures

#endif  // BS_TEST_FIXTURES_HPP
