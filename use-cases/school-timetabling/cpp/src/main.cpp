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
    {"국어", 5},
    {"수학", 5},
    {"영어", 4},
    {"과학", 4},
    {"사회", 4},
    {"체육", 3},
    {"음악", 3},
    {"미술", 3},
    {"도덕", 2},
    {"역사", 2},
};

const std::vector<std::string> GROUPS
    = {"1-1", "1-2", "2-1", "2-2", "3-1", "3-2"};

// Subject-teacher map: the core subjects are split between two teachers
// (classes 1-1/1-2/2-1 vs 2-2/3-1/3-2); the rest teach every class.
std::string teacherFor(const std::string &subject, const std::string &group)
{
    const bool firstHalf = (group == "1-1" || group == "1-2" || group == "2-1");
    if (subject == "국어")
        return firstHalf ? "김국어" : "이국어";
    if (subject == "수학")
        return firstHalf ? "박수학" : "최수학";
    if (subject == "영어")
        return firstHalf ? "정영어" : "강영어";
    if (subject == "과학")
        return firstHalf ? "조과학" : "윤과학";
    if (subject == "사회")
        return "장사회";
    if (subject == "체육")
        return "임체육";
    if (subject == "음악")
        return "한음악";
    if (subject == "미술")
        return "오미술";
    return "서인문";  // 도덕 + 역사
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
        t.rooms.push_back({group + "교실", "일반"});
    t.rooms.push_back({"과학실", "과학실"});  // room index 6
    t.rooms.push_back({"체육관", "체육관"});  // room index 7
    t.rooms.push_back({"음악실", "음악실"});  // room index 8

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
    t.teacherUnavailable["김국어"]
        = {slotOf(0, 1), slotOf(0, 2)};  // Mon 1-2교시
    t.teacherUnavailable["임체육"]
        = {slotOf(4, 6), slotOf(4, 7)};               // Fri 6-7교시
    t.teacherUnavailable["장사회"] = {slotOf(2, 1)};  // Wed 1교시

    // Pin 1-1's first PE lesson to Wednesday period 5 in the gym: the solver
    // must schedule everything else around it.
    for (Lesson &lesson : t.lessons)
    {
        if (lesson.id == "1-1-체육-1")
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

const char *DAY_NAMES[DAYS] = {"월", "화", "수", "목", "금"};

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
        std::printf("  [%s]      월    화    수    목    금\n",
                    GROUPS[g].c_str());
        for (int p = 1; p <= PERIODS_PER_DAY; ++p)
        {
            std::printf("    %d교시", p);
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

    std::printf("  [1-1 상세: 교시  과목  교사  교실]\n");
    for (int d = 0; d < DAYS; ++d)
    {
        for (int p = 1; p <= PERIODS_PER_DAY; ++p)
        {
            for (int i : cell[0][slotOf(d, p)])
            {
                const Lesson &lesson = t.lessons[i];
                std::printf(
                    "    %s %d교시  %s  %s  %s%s\n",
                    DAY_NAMES[d],
                    p,
                    lesson.subject.c_str(),
                    lesson.teacher.c_str(),
                    t.rooms[t.roomIndexOf(lesson.slotRoom)].name.c_str(),
                    lesson.pinned ? "  (고정)" : "");
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
    std::printf("  교사 주당 시수:");
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
    std::printf("== 데모 학교 시간표 (%d일 x %d교시, 학급 %zu개, 수업 %zu개, "
                "교실 %zu개, %d초) ==\n",
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
    std::printf("  고정 수업 유지: %s\n", pinnedKept ? "OK" : "FAIL");

    if (table.score.hard == 0 && pinnedKept)
    {
        std::printf("  hard 0 도달: OK\n");
        return 0;
    }
    std::printf("  hard 0 도달 실패 (hard=%lld)\n",
                static_cast<long long>(table.score.hard));
    return 1;
}
