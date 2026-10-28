#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "st/domain.hpp"
#include "st/score_calculator.hpp"
#include "st/solver.hpp"

using namespace st;

namespace
{

constexpr int DAYS = 5;
constexpr int PERIODS_PER_DAY = 7;

int slotOf(int day, int period) { return day * PERIODS_PER_DAY + (period - 1); }

// -------------------------------------------------------------------------
// A demo middle school in code, quickstart-demo sized: 5 days x 7 periods,
// 6 classes (1-1..3-2), 13 teachers, 10 subjects, 6 homerooms + 3 special
// rooms. Every class has exactly 35 weekly hours, so a feasible timetable
// packs each class grid perfectly.
// -------------------------------------------------------------------------

struct SubjectHours
{
    const char *subject;
    int hours;
};

// Weekly hours per class; sums to 35 == DAYS * PERIODS_PER_DAY.
const std::vector<SubjectHours> CURRICULUM = {
    {"Korean", 5},
    {"Math", 5},
    {"English", 4},
    {"Science", 4},
    {"Social", 4},
    {"PE", 3},
    {"Music", 3},
    {"Art", 3},
    {"Ethics", 2},
    {"History", 2},
};

const std::vector<std::string> GROUPS
    = {"1-1", "1-2", "2-1", "2-2", "3-1", "3-2"};

// Subject-teacher map: the core subjects are split between two teachers
// (classes 1-1/1-2/2-1 vs 2-2/3-1/3-2); the rest teach every class.
std::string teacherFor(const std::string &subject, const std::string &group)
{
    const bool firstHalf = (group == "1-1" || group == "1-2" || group == "2-1");
    if (subject == "Korean")
        return firstHalf ? "Kim (Korean)" : "Lee (Korean)";
    if (subject == "Math")
        return firstHalf ? "Park (Math)" : "Choi (Math)";
    if (subject == "English")
        return firstHalf ? "Jung (English)" : "Kang (English)";
    if (subject == "Science")
        return firstHalf ? "Cho (Science)" : "Yoon (Science)";
    if (subject == "Social")
        return "Jang (Social)";
    if (subject == "PE")
        return "Lim (PE)";
    if (subject == "Music")
        return "Han (Music)";
    if (subject == "Art")
        return "Oh (Art)";
    return "Seo (Humanities)";  // Ethics + History
}

Timetable buildDemoSchool()
{
    Timetable t;
    for (int d = 0; d < DAYS; ++d)
    {
        for (int p = 1; p <= PERIODS_PER_DAY; ++p)
            t.timeslots.push_back({d, p});
    }
    for (const std::string &group : GROUPS)
        t.rooms.push_back({group + " room", "regular"});
    t.rooms.push_back({"Science lab", "science"});  // room index 6
    t.rooms.push_back({"Gym", "gym"});              // room index 7
    t.rooms.push_back({"Music room", "music"});     // room index 8

    for (const std::string &group : GROUPS)
    {
        for (const SubjectHours &sh : CURRICULUM)
        {
            for (int h = 1; h <= sh.hours; ++h)
            {
                Lesson lesson;
                lesson.id = group + "-" + sh.subject + "-" + std::to_string(h);
                lesson.subject = sh.subject;
                lesson.teacher = teacherFor(sh.subject, group);
                lesson.studentGroup = group;
                t.lessons.push_back(lesson);
            }
        }
    }

    // A few teacher unavailabilities (hard 4).
    t.teacherUnavailable["Kim (Korean)"]
        = {slotOf(0, 1), slotOf(0, 2)};  // Mon periods 1-2
    t.teacherUnavailable["Lim (PE)"]
        = {slotOf(4, 6), slotOf(4, 7)};                   // Fri periods 6-7
    t.teacherUnavailable["Jang (Social)"] = {slotOf(2, 1)};  // Wed period 1

    // Pin 1-1's first PE lesson to Wednesday period 5 in the gym: the solver
    // must schedule everything else around it.
    for (Lesson &lesson : t.lessons)
    {
        if (lesson.id == "1-1-PE-1")
        {
            lesson.slotRoom = t.slotRoomOf(slotOf(2, 5), /*gym*/ 7);
            lesson.pinned = true;
        }
    }
    return t;
}

// -------------------------------------------------------------------------
// Output: one grid per class (rows = periods, columns = days), a detailed
// room/teacher listing for class 1-1, and the per-teacher load summary.
// -------------------------------------------------------------------------

const char *DAY_NAMES[DAYS] = {"Mon", "Tue", "Wed", "Thu", "Fri"};

void printTimetable(const Timetable &t)
{
    // lessons per (group, timeslot); with hard == 0 each cell has exactly one.
    std::vector<std::vector<std::vector<int>>> cell(
        GROUPS.size(), std::vector<std::vector<int>>(t.timeslotCount()));
    for (int i = 0; i < static_cast<int>(t.lessons.size()); ++i)
    {
        const Lesson &lesson = t.lessons[i];
        if (lesson.slotRoom < 0)
            continue;
        for (std::size_t g = 0; g < GROUPS.size(); ++g)
        {
            if (lesson.studentGroup == GROUPS[g])
            {
                cell[g][t.timeslotIndexOf(lesson.slotRoom)].push_back(i);
            }
        }
    }

    for (std::size_t g = 0; g < GROUPS.size(); ++g)
    {
        std::printf("  [%s]      Mon   Tue   Wed   Thu   Fri\n",
                    GROUPS[g].c_str());
        for (int p = 1; p <= PERIODS_PER_DAY; ++p)
        {
            std::printf("    P%d    ", p);
            for (int d = 0; d < DAYS; ++d)
            {
                const std::vector<int> &lessons = cell[g][slotOf(d, p)];
                if (lessons.empty())
                {
                    std::printf("  ----");
                }
                else
                {
                    // With hard == 0 there is never more than one lesson here.
                    std::printf("  %s",
                                t.lessons[lessons.front()].subject.c_str());
                }
            }
            std::printf("\n");
        }
        std::printf("\n");
    }

    std::printf("  [1-1 detail: period  subject  teacher  room]\n");
    for (int d = 0; d < DAYS; ++d)
    {
        for (int p = 1; p <= PERIODS_PER_DAY; ++p)
        {
            for (int i : cell[0][slotOf(d, p)])
            {
                const Lesson &lesson = t.lessons[i];
                std::printf(
                    "    %s P%d  %s  %s  %s%s\n",
                    DAY_NAMES[d],
                    p,
                    lesson.subject.c_str(),
                    lesson.teacher.c_str(),
                    t.rooms[t.roomIndexOf(lesson.slotRoom)].name.c_str(),
                    lesson.pinned ? "  (pinned)" : "");
            }
        }
    }
    std::printf("\n");
}

void printTeacherLoads(const Timetable &t)
{
    std::vector<std::string> names;
    std::vector<int> loads;
    for (const Lesson &lesson : t.lessons)
    {
        std::size_t i = 0;
        for (; i < names.size(); ++i)
        {
            if (names[i] == lesson.teacher)
                break;
        }
        if (i == names.size())
        {
            names.push_back(lesson.teacher);
            loads.push_back(0);
        }
        if (lesson.slotRoom >= 0)
            ++loads[i];
    }
    std::printf("  Teacher hours per week:");
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        std::printf(" %s=%d", names[i].c_str(), loads[i]);
    }
    std::printf("\n\n");
}

}  // namespace

int main(int argc, char **argv)
{
    const int seconds = (argc > 1) ? std::atoi(argv[1]) : 10;

    Timetable table = buildDemoSchool();
    std::printf("== Demo school timetable (%d days x %d periods, %zu classes, %zu "
                "lessons, %zu rooms, %ds) ==\n",
                DAYS,
                PERIODS_PER_DAY,
                GROUPS.size(),
                table.lessons.size(),
                table.rooms.size(),
                seconds);

    Solver::Config config;
    config.spentLimit = std::chrono::seconds(seconds);
    config.randomSeed = 42;
    config.onNewBest = [](std::int64_t step, const Score &score)
    {
        std::printf("    new best @step %-8lld %s\n",
                    static_cast<long long>(step),
                    score.toString().c_str());
    };
    Solver solver(config);
    solver.solve(table);

    std::printf("\n  final score: %s  (feasible=%s)\n\n",
                table.score.toString().c_str(),
                table.score.isFeasible() ? "true" : "false");
    printTimetable(table);
    printTeacherLoads(table);

    bool pinnedKept = true;
    for (const Lesson &lesson : table.lessons)
    {
        if (lesson.pinned
            && lesson.slotRoom != table.slotRoomOf(slotOf(2, 5), /*gym*/ 7))
        {
            pinnedKept = false;
        }
    }
    std::printf("  Pinned lesson kept: %s\n", pinnedKept ? "OK" : "FAIL");

    if (table.score.hard == 0 && pinnedKept)
    {
        std::printf("  Reached hard 0: OK\n");
        return 0;
    }
    std::printf("  Did not reach hard 0 (hard=%lld)\n",
                static_cast<long long>(table.score.hard));
    return 1;
}
