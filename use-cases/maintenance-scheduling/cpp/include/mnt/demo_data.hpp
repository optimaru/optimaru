#ifndef MNT_DEMO_DATA_HPP
#define MNT_DEMO_DATA_HPP

#include <cstdint>
#include <string>

#include "mnt/domain.hpp"

namespace mnt
{

// Faithful port of the quickstart's DemoDataGenerator, including a replica
// of java.util.Random(17) so the generated instances match the Java demo
// bit for bit (same durations, windows and tags per job).
//   SMALL: 3 crews, 8 weeks, 14 jobs   LARGE: 5 crews, 16 weeks, 48 jobs
enum class DemoData
{
    SMALL,
    LARGE
};

// `fromDate` is the calendar start (Java uses the next Monday after today;
// pass nextMonday(todayEpochDay()) for the same behavior, or any fixed
// Monday for reproducible tests).
MaintenanceSchedule generateDemoData(DemoData demoData, std::int64_t fromDate);

// Today's date (UTC) as an epoch day.
std::int64_t todayEpochDay();

// The next Monday STRICTLY after `epochDay` (Java's
// TemporalAdjusters.next(DayOfWeek.MONDAY)).
std::int64_t nextMonday(std::int64_t epochDay);

// ISO yyyy-mm-dd for printing epoch days.
std::string formatDate(std::int64_t epochDay);

}  // namespace mnt

#endif  // MNT_DEMO_DATA_HPP
