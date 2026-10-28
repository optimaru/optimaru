#ifndef FP_DEMO_DATA_HPP
#define FP_DEMO_DATA_HPP

#include "fp/domain.hpp"

namespace fp
{

// Faithful port of the quickstart's DemoDataGenerator (default config:
// demo-data.line-count=5, demo-data.job-count=100), including a bit-exact
// replica of java.util.Random(37) — same products, cleaning matrix,
// durations and time windows as the Java demo.
//
// Java anchors the calendar at the next-or-same Monday of "today" at
// midnight; only relative times matter (nothing reads absolute dates), so
// here that Monday 00:00 is simply minute 0: minStartTime = day * 1440,
// idealEndTime = day * 1440 + 960 (16:00), workCalendar = [0, 14 days).
PackagingSchedule generateDemoData(int lineCount = 5, int jobCount = 100);

}  // namespace fp

#endif  // FP_DEMO_DATA_HPP
