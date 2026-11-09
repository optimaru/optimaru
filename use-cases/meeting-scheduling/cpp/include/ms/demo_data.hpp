#ifndef MS_DEMO_DATA_HPP
#define MS_DEMO_DATA_HPP

// The demo instance, modelled on the quickstart's demo data and shared by the
// C++ demo executable and the Python bindings. Durations and attendees are
// drawn from a seeded std::mt19937_64, and the four days are fixed to
// dayOfYear 200..203, so the instance is the same on every run and platform.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <random>

#include "ms/domain.hpp"

namespace ms
{

// The demo generator's random draws: std::mt19937_64 with hand-written
// bounded draws, so a seed gives the same instance with every standard
// library (the std:: distributions are implementation-defined).
class DemoRandom
{
public:
    explicit DemoRandom(std::uint64_t seed)
        : engine_(seed)
    {
    }

    // Uniform in [0, bound), bound > 0.
    int next_int(int bound)
    {
        return static_cast<int>(engine_() % static_cast<std::uint64_t>(bound));
    }

    // Uniform in [0, bound), bound > 0.
    std::int64_t next_long(std::int64_t bound)
    {
        return static_cast<std::int64_t>(engine_()
                                         % static_cast<std::uint64_t>(bound));
    }

    // Uniform in [0, 1).
    double next_double()
    {
        return static_cast<double>(engine_() >> 11) * 0x1.0p-53;
    }

private:
    std::mt19937_64 engine_;
};

namespace demo_detail
{

// Apply `consumer` to `count`
// random elements still matching `filter` (each drawn uniformly among the
// remaining matches).
inline void applyRandomValue(int count,
                             std::vector<Meeting> &meetings,
                             DemoRandom &random,
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
        int skip = size > 0 ? random.next_int(size) : 0;
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

// The demo instance (seed 0): 20
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

    DemoRandom random(0);
    MeetingSchedule schedule;

    // People (two draws per person: first name, then last name).
    // static: lambdas below read it without capturing; MSVC rejects that for a
    // non-static local (C3493) even though it is constexpr.
    static constexpr int kPersonCount = 20;
    for (int i = 0; i < kPersonCount; ++i)
    {
        const std::string first = kFirstNames[random.next_int(27)];
        const std::string last = kLastNames[random.next_int(20)];
        schedule.people.push_back({std::to_string(i), first + " " + last});
    }

    // Time grains: 4 days x (08:00 .. 17:45 step 15) = 160, grainIndex 1..160
    // (consecutive, 1-based).
    static constexpr int kFirstDayOfYear = 200;
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
    static constexpr int kMeetingCount = 24;
    for (int i = 0; i < kMeetingCount; ++i)
    {
        Meeting meeting;
        meeting.id = std::to_string(i);
        meeting.topic = kTopics[i];
        meeting.durationInGrains = 0;  // drawn below
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
            const int person = random.next_int(kPersonCount);
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
            const int person = random.next_int(kPersonCount);
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
