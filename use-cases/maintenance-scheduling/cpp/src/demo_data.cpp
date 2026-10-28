#include "mnt/demo_data.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <limits>
#include <utility>
#include <vector>

namespace mnt
{

namespace
{

// Bit-exact replica of java.util.Random (48-bit LCG), enough for the demo
// generator: nextInt(bound) and nextDouble().
class JavaRandom
{
public:
    explicit JavaRandom(std::uint64_t seed)
        : seed_((seed ^ kMultiplier) & kMask48)
    {
    }

    std::int32_t nextInt(std::int32_t bound)
    {
        std::int32_t r = next(31);
        const std::int32_t m = bound - 1;
        if ((bound & m) == 0)  // power of two
        {
            r = static_cast<std::int32_t>((static_cast<std::int64_t>(bound) * r)
                                          >> 31);
        }
        else
        {
            // Java: for (int u = r; u - (r = u % bound) + m < 0; u =
            // next(31)); — the int32 overflow test, done here in 64 bits.
            std::int32_t u = r;
            r = u % bound;
            while (static_cast<std::int64_t>(u) - r + m
                   > std::numeric_limits<std::int32_t>::max())
            {
                u = next(31);
                r = u % bound;
            }
        }
        return r;
    }

    double nextDouble()
    {
        const std::int64_t high = static_cast<std::int64_t>(next(26)) << 27;
        return static_cast<double>(high + next(27)) * 0x1.0p-53;
    }

private:
    static constexpr std::uint64_t kMultiplier = 0x5DEECE66DULL;
    static constexpr std::uint64_t kAddend = 0xBULL;
    static constexpr std::uint64_t kMask48 = (1ULL << 48) - 1;

    std::int32_t next(int bits)
    {
        seed_ = (seed_ * kMultiplier + kAddend) & kMask48;
        return static_cast<std::int32_t>(seed_ >> (48 - bits));
    }

    std::uint64_t seed_;
};

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

    JavaRandom random(17);
    for (int i = 0; i < jobListSize; ++i)
    {
        const std::string jobArea = kJobAreaNames[i / jobAreaTargetLimit];
        const std::string jobTarget = kJobTargetNames[i % jobAreaTargetLimit];
        // 1 day to 2 workweeks (1 workweek on average).
        const int durationInDays = 1 + random.nextInt(10);
        const int minMaxBetweenWorkdays
            = durationInDays + 5  // at least 5 days of flexibility
              + random.nextInt(workdayTotal - (durationInDays + 5));
        const int minWorkdayOffset
            = random.nextInt(workdayTotal - minMaxBetweenWorkdays + 1);
        const int minIdealEndBetweenWorkdays
            = minMaxBetweenWorkdays - 1 - random.nextInt(4);
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
        if (random.nextDouble() < 0.1)
            job.tags.push_back("Subway");
        schedule.jobs.push_back(std::move(job));
    }
    return schedule;
}

}  // namespace mnt
