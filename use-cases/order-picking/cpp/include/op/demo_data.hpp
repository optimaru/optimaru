#ifndef OP_DEMO_DATA_HPP
#define OP_DEMO_DATA_HPP

#include <cstdint>

#include "op/domain.hpp"

namespace op
{

// Reproduction of org.acme.orderpicking.bootstrap.DemoDataGenerator: the same
// 37-product catalog (volumes in cm3, product-family shelving placement), the
// same defaults (5 trolleys x 4 buckets x 60*40*20 cm3, 8 orders, start
// location (A,1) LEFT row 0) and a bit-exact java.util.Random(37) stream, so
// the generated instance is the very dataset the Java quickstart solves at
// startup.
//
// Throws std::invalid_argument when bucketCapacity is smaller than the
// largest product (DemoDataGenerator.validateBucketCapacity: the problem
// assumes every product fits in one bucket).
OrderPickingSolution buildDemoData(int trolleysCount = 5,
                                   int bucketCount = 4,
                                   int bucketCapacity = 60 * 40 * 20,
                                   int ordersCount = 8,
                                   std::int64_t randomSeed = 37);

// The largest product volume in the demo catalog
// (DemoDataGenerator.getMaxProductSize).
int demoMaxProductSize();

}  // namespace op

#endif  // OP_DEMO_DATA_HPP
