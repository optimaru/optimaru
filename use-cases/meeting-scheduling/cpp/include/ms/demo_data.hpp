#ifndef MS_DEMO_DATA_HPP
#define MS_DEMO_DATA_HPP

// Reproduction of the Java quickstart's DemoDataGenerator, shared by the C++
// demo executable and the Python bindings. The generator mirrors the Java
// code draw-for-draw with a java.util.Random(0)-compatible LCG, so the
// resulting instance (durations, attendee draws) matches the Java demo
// exactly. Only the calendar differs: Java derives dayOfYear from "tomorrow";
// here the four days are fixed to dayOfYear 200..203 for reproducibility.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ms/domain.hpp"

namespace ms
{

// java.util.Random: the 48-bit LCG, next(31) and nextInt(bound).
class JavaRandom
{
public:
    explicit JavaRandom(std::uint64_t seed)
        : seed_((seed ^ kMultiplier) & kMask)
    {
    }

    int nextInt(int bound)
    {
        int r = next31();
        const int m = bound - 1;
        if ((bound & m) == 0)  // power of two
        {
            return static_cast<int>((static_cast<std::int64_t>(bound) * r)
                                    >> 31);
        }
        // Rejection loop; the Java int-overflow test done in 64 bits.
        int u = r;
        r = u % bound;
        while (static_cast<std::int64_t>(u) - r + m > 2147483647LL)
        {
            u = next31();
            r = u % bound;
        }
        return r;
    }

private:
    static constexpr std::uint64_t kMultiplier = 0x5DEECE66DULL;
    static constexpr std::uint64_t kAddend = 0xBULL;
    static constexpr std::uint64_t kMask = (1ULL << 48) - 1;

    int next31()
    {
        seed_ = (seed_ * kMultiplier + kAddend) & kMask;
        return static_cast<int>(seed_ >> 17);
    }

    std::uint64_t seed_;
};

namespace demo_detail
{

// Java DemoDataGenerator::applyRandomValue: apply `consumer` to `count`
// random elements still matching `filter` (each drawn uniformly among the
// remaining matches).
inline void applyRandomValue(int count,
                             std::vector<Meeting> &meetings,
                             JavaRandom &random,
                             const std::function<bool(const Meeting &)> &filter,
                             const std::function<void(Meeting &)> &consumer)
{
    int size = 0;
    for (const Meeting &meeting : meetings)
    {
        if (filter(meeting))
            ++size;
    }
    for (int i = 0; i < count; ++i)
    {
        int skip = size > 0 ? random.nextInt(size) : 0;
        for (Meeting &meeting : meetings)
        {
            if (!filter(meeting))
                continue;
            if (skip-- == 0)
            {
                consumer(meeting);
                break;
            }
        }
        --size;
        if (size < 0)
            break;
    }
}

inline bool contains(const std::vector<int> &values, int value)
{
    for (int v : values)
    {
        if (v == value)
            return true;
    }
    return false;
}

}  // namespace demo_detail

// Java DemoDataGenerator::generateDemoData with java.util.Random(0): 20
// people, 4 days x 40 grains (08:00-17:45, grainIndex 1..160), 3 rooms
// (30/20/16 seats), 24 meetings (10x8 + 7x12 + 7x16 grains) with the same
// required/preferred attendee draws, one MeetingAssignment per meeting.
inline MeetingSchedule buildDemoData()
{
    static const char *kFirstNames[] = {
        "Amy",  "Beth", "Carl", "Dan",  "Elsa", "Flo",  "Gus",  "Hugo", "Ivy",
        "Jay",  "Jeri", "Hope", "Avis", "Lino", "Lyle", "Nick", "Dino", "Otha",
        "Gwen", "Jose", "Dena", "Jana", "Dave", "Russ", "Josh", "Dana", "Katy"};
    static const char *kLastNames[]
        = {"Cole", "Fox",   "Green", "Jones", "King", "Li",   "Poe",
           "Rye",  "Smith", "Watt",  "Howe",  "Lowe", "Wise", "Clay",
           "Carr", "Hood",  "Long",  "Horn",  "Haas", "Meza"};
    static const char *kTopics[] = {
        "Strategize B2B",
        "Fast track e-business",
        "Cross sell virtualization",
        "Profitize multitasking",
        "Transform one stop shop",
        "Engage braindumps",
        "Downsize data mining",
        "Ramp up policies",
        "On board synergies",
        "Reinvigorate user experience",
        "Strategize e-business",
        "Fast track virtualization",
        "Cross sell multitasking",
        "Profitize one stop shop",
        "Transform braindumps",
        "Engage data mining",
        "Downsize policies",
        "Ramp up synergies",
        "On board user experience",
        "Reinvigorate B2B",
        "Strategize virtualization",
        "Fast track multitasking",
        "Cross sell one stop shop",
        "Reinvigorate multitasking",
    };

    JavaRandom random(0);
    MeetingSchedule schedule;

    // People (two draws per person: first name, then last name).
    constexpr int kPersonCount = 20;
    for (int i = 0; i < kPersonCount; ++i)
    {
        const std::string first = kFirstNames[random.nextInt(27)];
        const std::string last = kLastNames[random.nextInt(20)];
        schedule.people.push_back({std::to_string(i), first + " " + last});
    }

    // Time grains: 4 days x (08:00 .. 17:45 step 15) = 160, grainIndex 1..160
    // like the Java generator's pre-incremented counter.
    constexpr int kFirstDayOfYear = 200;
    int count = 0;
    for (int day = 0; day < 4; ++day)
    {
        for (int minute = 8 * 60; minute <= 17 * 60 + 45; minute += 15)
        {
            ++count;
            schedule.timeGrains.push_back(
                {std::to_string(count), count, kFirstDayOfYear + day, minute});
        }
    }

    schedule.rooms.push_back({"R1", "Room 1", 30});
    schedule.rooms.push_back({"R2", "Room 2", 20});
    schedule.rooms.push_back({"R3", "Room 3", 16});

    // Meetings.
    constexpr int kMeetingCount = 24;
    for (int i = 0; i < kMeetingCount; ++i)
    {
        Meeting meeting;
        meeting.id = std::to_string(i);
        meeting.topic = kTopics[i];
        meeting.durationInGrains = 0;  // drawn below, like Java
        schedule.meetings.push_back(meeting);
    }

    // Durations: thirds of 8 / 12 / 16 grains, leftovers 8.
    const std::pair<float, int> durationShares[]
        = {{0.33F, 8}, {0.33F, 12}, {0.33F, 16}};
    for (const auto &[share, grains] : durationShares)
    {
        const int duration = grains;
        demo_detail::applyRandomValue(
            static_cast<int>(share * static_cast<float>(kMeetingCount)),
            schedule.meetings,
            random,
            [](const Meeting &m) { return m.durationInGrains == 0; },
            [duration](Meeting &m) { m.durationInGrains = duration; });
    }
    for (Meeting &meeting : schedule.meetings)
    {
        if (meeting.durationInGrains == 0)
            meeting.durationInGrains = 8;
    }

    // Required attendees: draw distinct people until the target size.
    const auto requiredConsumer = [&random](Meeting &meeting, int size)
    {
        do
        {
            const int person = random.nextInt(kPersonCount);
            if (!demo_detail::contains(meeting.requiredAttendees, person))
                meeting.requiredAttendees.push_back(person);
        } while (static_cast<int>(meeting.requiredAttendees.size()) < size);
    };
    const std::pair<float, int> requiredShares[] = {{0.36F, 2},
                                                    {0.08F, 3},
                                                    {0.02F, 4},
                                                    {0.08F, 5},
                                                    {0.1F, 6},
                                                    {0.05F, 7},
                                                    {0.05F, 8},
                                                    {0.05F, 10}};
    for (const auto &[share, size] : requiredShares)
    {
        const int target = size;
        demo_detail::applyRandomValue(
            static_cast<int>(share * static_cast<float>(kMeetingCount)),
            schedule.meetings,
            random,
            [](const Meeting &m) { return m.requiredAttendees.empty(); },
            [&](Meeting &m) { requiredConsumer(m, target); });
    }

    // Preferred attendees: distinct people not already required.
    const auto preferredConsumer = [&random](Meeting &meeting, int size)
    {
        do
        {
            const int person = random.nextInt(kPersonCount);
            if (!demo_detail::contains(meeting.preferredAttendees, person)
                && !demo_detail::contains(meeting.requiredAttendees, person))
            {
                meeting.preferredAttendees.push_back(person);
            }
        } while (static_cast<int>(meeting.preferredAttendees.size()) < size);
    };
    const std::pair<float, int> preferredShares[] = {{0.06F, 1},
                                                     {0.2F, 2},
                                                     {0.18F, 3},
                                                     {0.06F, 4},
                                                     {0.04F, 5},
                                                     {0.04F, 6},
                                                     {0.04F, 7},
                                                     {0.04F, 8},
                                                     {0.08F, 9},
                                                     {0.04F, 10}};
    for (const auto &[share, size] : preferredShares)
    {
        const int target = size;
        demo_detail::applyRandomValue(
            static_cast<int>(share * static_cast<float>(kMeetingCount)),
            schedule.meetings,
            random,
            [](const Meeting &m) { return m.preferredAttendees.empty(); },
            [&](Meeting &m) { preferredConsumer(m, target); });
    }

    // Meeting assignments, 1:1 with meetings.
    for (int i = 0; i < kMeetingCount; ++i)
    {
        MeetingAssignment assignment;
        assignment.id = std::to_string(i);
        assignment.meeting = i;
        schedule.meetingAssignments.push_back(assignment);
    }
    return schedule;
}

}  // namespace ms

#endif  // MS_DEMO_DATA_HPP
