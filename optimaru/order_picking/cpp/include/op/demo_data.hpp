#ifndef OP_DEMO_DATA_HPP
#define OP_DEMO_DATA_HPP

#include <cstdint>

#include "op/domain.hpp"

namespace op {

// The demo warehouse: a
// 37-product catalog (volumes in cm3, product-family shelving placement), the
// defaults of 5 trolleys x 4 buckets x 60*40*20 cm3, 8 orders, start
// location (A,1) LEFT row 0. Orders are drawn from a seeded std::mt19937_64
// (seed 37), so the instance is the same on every platform.
//
// Throws std::invalid_argument when bucketCapacity is smaller than the
// largest product (the problem
// assumes every product fits in one bucket).
OrderPickingSolution buildDemoData(int trolleysCount = 5, int bucketCount = 4,
                                   int bucketCapacity = 60 * 40 * 20, int ordersCount = 8,
                                   std::int64_t seed = 37);

// The largest product volume in the demo catalog.
int demoMaxProductSize();

}  // namespace op

#endif  // OP_DEMO_DATA_HPP
