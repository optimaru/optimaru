#include "mnt/demo_data.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <limits>
#include <utility>
#include <vector>
#include <random>
#include "optimaru/portable_random.hpp"

namespace mnt
{

namespace
{

// Seeded, platform-independent draws (see optimaru/portable_random.hpp).
using DemoRandom = optimaru::PortableRandom;

const char *const kJobAreaNames[] = {"Downtown",
                                     "Uptown",
                                     "Park",
                                     "Airport",
                                     "Bay",
                                     "Hill",
                                     "Forest",
                                     "Station",
                                     "Hospital",
                                     "Harbor",
                                     "Market",
                                     "Fort",
                                     "Beach",
                                     "Garden",
                                     "River",
                                     "Springs",
                                     "Tower",
                                     "Mountain"};

const char *const kJobTargetNames[] = {"Street",
                                       "Bridge",
                                       "Tunnel",
                                       "Highway",
                                       "Boulevard",
                                       "Avenue",
                                       "Square",
                                       "Plaza"};
constexpr int kJobTargetNameCount
    = static_cast<int>(sizeof(kJobTargetNames) / sizeof(kJobTargetNames[0]));

}  // namespace

std::int64_t todayEpochDay()
{
    return static_cast<std::int64_t>(std::time(nullptr)) / 86400;
}

std::int64_t nextMonday(std::int64_t epochDay)
{
    const int offset = (7 - weekdayOf(epochDay)) % 7;
    return epochDay + (offset == 0 ? 7 : offset);
}

std::string formatDate(std::int64_t epochDay)
{
    // Howard Hinnant's civil_from_days.
    std::int64_t z = epochDay + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    char buf[16];
    std::snprintf(buf,
                  sizeof(buf),
                  "%04lld-%02u-%02u",
                  static_cast<long long>(y + (m <= 2 ? 1 : 0)),
                  m,
                  d);
    return buf;
}

MaintenanceSchedule generateDemoData(DemoData demoData, std::int64_t fromDate)
{
    MaintenanceSchedule schedule;

    schedule.crews.push_back({"1", "Alpha crew"});
    schedule.crews.push_back({"2", "Beta crew"});
    schedule.crews.push_back({"3", "Gamma crew"});
    if (demoData == DemoData::LARGE)
    {
        schedule.crews.push_back({"4", "Delta crew"});
        schedule.crews.push_back({"5", "Epsilon crew"});
    }

    const int weekListSize = (demoData == DemoData::LARGE) ? 16 : 8;
    schedule.fromDate = fromDate;
    schedule.toDate = fromDate + weekListSize * 7;

    const int workdayTotal = weekListSize * 5;
    const int crewCount = schedule.crewCount();
    const int jobListSize = weekListSize * crewCount * 3 / 5;
    const int jobAreaTargetLimit = std::min(kJobTargetNameCount, crewCount * 2);

    DemoRandom random(17);
    for (int i = 0; i < jobListSize; ++i)
    {
        const std::string jobArea = kJobAreaNames[i / jobAreaTargetLimit];
        const std::string jobTarget = kJobTargetNames[i % jobAreaTargetLimit];
        // 1 day to 2 workweeks (1 workweek on average).
        const int durationInDays = 1 + random.next_int(10);
        const int minMaxBetweenWorkdays
            = durationInDays + 5  // at least 5 days of flexibility
              + random.next_int(workdayTotal - (durationInDays + 5));
        const int minWorkdayOffset
            = random.next_int(workdayTotal - minMaxBetweenWorkdays + 1);
        const int minIdealEndBetweenWorkdays
            = minMaxBetweenWorkdays - 1 - random.next_int(4);
        const std::int64_t minStartDate
            = plusBusinessDays(fromDate, minWorkdayOffset);
        const std::int64_t maxEndDate
            = plusBusinessDays(minStartDate, minMaxBetweenWorkdays);
        const std::int64_t idealEndDate
            = plusBusinessDays(minStartDate, minIdealEndBetweenWorkdays);

        Job job;
        job.id = std::to_string(i);
        job.name = jobArea + " " + jobTarget;
        job.durationInDays = durationInDays;
        job.minStartDate = minStartDate;
        job.maxEndDate = maxEndDate;
        job.idealEndDate = idealEndDate;
        job.tags.push_back(jobArea);
        if (random.next_double() < 0.1)
            job.tags.push_back("Subway");
        schedule.jobs.push_back(std::move(job));
    }
    return schedule;
}

}  // namespace mnt
