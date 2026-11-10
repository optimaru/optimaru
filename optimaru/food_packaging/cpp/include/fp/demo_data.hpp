#ifndef FP_DEMO_DATA_HPP
#define FP_DEMO_DATA_HPP

#include "fp/domain.hpp"

namespace fp
{

// The demo plant, modelled on the quickstart's demo data (5 lines, 100
// jobs). Products, durations and time windows are drawn from a seeded
// std::mt19937_64 (seed 37), so the instance is the same on every platform.
//
// Only relative times matter (nothing reads absolute dates), so the
// calendar starts on a Monday at minute 0: minStartTime = day * 1440,
// idealEndTime = day * 1440 + 960 (16:00), workCalendar = [0, 14 days).
PackagingSchedule generateDemoData(int lineCount = 5, int jobCount = 100);

}  // namespace fp

#endif  // FP_DEMO_DATA_HPP
