#ifndef MNT_DEMO_DATA_HPP
#define MNT_DEMO_DATA_HPP

#include <cstdint>
#include <string>

#include "mnt/domain.hpp"

namespace mnt {

// The demo instances. Durations,
// windows and tags are drawn from a seeded std::mt19937_64 (seed 17), so a
// size gives the same instance on every platform.
//   SMALL: 3 crews, 8 weeks, 14 jobs   LARGE: 5 crews, 16 weeks, 48 jobs
enum class DemoData { SMALL, LARGE };

// `fromDate` is the calendar start: nextMonday(todayEpochDay()) for a
// schedule starting next week, or any fixed Monday for reproducible tests.
MaintenanceSchedule generateDemoData(DemoData demoData, std::int64_t fromDate);

// Today's date (UTC) as an epoch day.
std::int64_t todayEpochDay();

// The next Monday STRICTLY after `epochDay`.
std::int64_t nextMonday(std::int64_t epochDay);

// ISO yyyy-mm-dd for printing epoch days.
std::string formatDate(std::int64_t epochDay);

}  // namespace mnt

#endif  // MNT_DEMO_DATA_HPP
