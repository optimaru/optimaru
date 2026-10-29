// pybind11 bindings for the meeting-scheduling C++ domain wrappers.
// The Python extension links the existing ms::* implementations and imports
// the generic optimaru engine from ../../../include; the engine itself
// is not modified here.
#include <pybind11/functional.h>  // std::function <-> Python callable
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::string conversions

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ms/constraints.hpp"
#include "ms/demo_data.hpp"
#include "ms/domain.hpp"
#include "ms/score_calculator.hpp"
#include "ms/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace ms;
namespace tfpy = optimaru::python;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Meeting scheduling — optimaru engine-backed C++ wrapper "
              "exposed to Python";

    m.attr("MEDIUM_SCALE") = kMediumScale;
    m.attr("GRAIN_LENGTH_IN_MINUTES") = kGrainLengthInMinutes;

    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::isFeasible)
        .def("__str__", &Score::toString)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.toString() + ">"; });

    py::class_<Person>(m, "Person", py::module_local())
        .def(py::init(
                 [](std::string id, std::string fullName)
                 {
                     Person p;
                     p.id = std::move(id);
                     p.fullName = std::move(fullName);
                     return p;
                 }),
             py::arg("id"),
             py::arg("full_name") = "")
        .def_readwrite("id", &Person::id)
        .def_readwrite("full_name", &Person::fullName);

    py::class_<TimeGrain>(m, "TimeGrain", py::module_local())
        .def(py::init(
                 [](std::string id,
                    int grainIndex,
                    int dayOfYear,
                    int startingMinuteOfDay)
                 {
                     TimeGrain g;
                     g.id = std::move(id);
                     g.grainIndex = grainIndex;
                     g.dayOfYear = dayOfYear;
                     g.startingMinuteOfDay = startingMinuteOfDay;
                     return g;
                 }),
             py::arg("id"),
             py::arg("grain_index"),
             py::arg("day_of_year"),
             py::arg("starting_minute_of_day"))
        .def_readwrite("id", &TimeGrain::id)
        .def_readwrite("grain_index", &TimeGrain::grainIndex)
        .def_readwrite("day_of_year", &TimeGrain::dayOfYear)
        .def_readwrite("starting_minute_of_day",
                       &TimeGrain::startingMinuteOfDay);

    py::class_<Room>(m, "Room", py::module_local())
        .def(py::init(
                 [](std::string id, std::string name, int capacity)
                 {
                     Room r;
                     r.id = std::move(id);
                     r.name = std::move(name);
                     r.capacity = capacity;
                     return r;
                 }),
             py::arg("id"),
             py::arg("name") = "",
             py::arg("capacity") = 0)
        .def_readwrite("id", &Room::id)
        .def_readwrite("name", &Room::name)
        .def_readwrite("capacity", &Room::capacity);

    py::class_<Meeting>(m, "Meeting", py::module_local())
        .def(py::init(
                 [](std::string id,
                    std::string topic,
                    int durationInGrains,
                    std::vector<int> requiredAttendees,
                    std::vector<int> preferredAttendees,
                    bool entireGroupMeeting)
                 {
                     Meeting meeting;
                     meeting.id = std::move(id);
                     meeting.topic = std::move(topic);
                     meeting.durationInGrains = durationInGrains;
                     meeting.requiredAttendees = std::move(requiredAttendees);
                     meeting.preferredAttendees = std::move(preferredAttendees);
                     meeting.entireGroupMeeting = entireGroupMeeting;
                     return meeting;
                 }),
             py::arg("id"),
             py::arg("topic") = "",
             py::arg("duration_in_grains") = 1,
             py::arg("required_attendees") = std::vector<int>{},
             py::arg("preferred_attendees") = std::vector<int>{},
             py::arg("entire_group_meeting") = false)
        .def_readwrite("id", &Meeting::id)
        .def_readwrite("topic", &Meeting::topic)
        .def_readwrite("duration_in_grains", &Meeting::durationInGrains)
        // Person indices; one entry == one attendance fact.
        .def_readwrite("required_attendees", &Meeting::requiredAttendees)
        .def_readwrite("preferred_attendees", &Meeting::preferredAttendees)
        .def_readwrite("entire_group_meeting", &Meeting::entireGroupMeeting)
        .def("required_capacity", &Meeting::requiredCapacity);

    py::class_<MeetingAssignment>(m, "MeetingAssignment", py::module_local())
        .def(py::init(
                 [](std::string id, int meeting, int grainRoom, bool pinned)
                 {
                     MeetingAssignment assignment;
                     assignment.id = std::move(id);
                     assignment.meeting = meeting;
                     assignment.grainRoom = grainRoom;
                     assignment.pinned = pinned;
                     return assignment;
                 }),
             py::arg("id"),
             py::arg("meeting"),
             py::arg("grain_room") = -1,
             py::arg("pinned") = false)
        .def_readwrite("id", &MeetingAssignment::id)
        .def_readwrite("meeting", &MeetingAssignment::meeting)
        // Composite planning variable: time_grain_pos * room_count +
        // room_index; -1 == unassigned.
        .def_readwrite("grain_room", &MeetingAssignment::grainRoom)
        .def_readwrite("pinned", &MeetingAssignment::pinned);

    py::class_<MeetingSchedule>(m, "MeetingSchedule", py::module_local())
        .def(py::init(
                 [](std::vector<Person> people,
                    std::vector<TimeGrain> timeGrains,
                    std::vector<Room> rooms,
                    std::vector<Meeting> meetings,
                    std::vector<MeetingAssignment> meetingAssignments)
                 {
                     MeetingSchedule s;
                     s.people = std::move(people);
                     s.timeGrains = std::move(timeGrains);
                     s.rooms = std::move(rooms);
                     s.meetings = std::move(meetings);
                     s.meetingAssignments = std::move(meetingAssignments);
                     return s;
                 }),
             py::arg("people"),
             py::arg("time_grains"),
             py::arg("rooms"),
             py::arg("meetings"),
             py::arg("meeting_assignments"))
        .def_readwrite("people", &MeetingSchedule::people)
        .def_readwrite("time_grains", &MeetingSchedule::timeGrains)
        .def_readwrite("rooms", &MeetingSchedule::rooms)
        .def_readwrite("meetings", &MeetingSchedule::meetings)
        // NOTE: element access returns copies; use set_assignment for bulk
        // planning-variable writes.
        .def_readwrite("meeting_assignments",
                       &MeetingSchedule::meetingAssignments)
        .def_readonly("score", &MeetingSchedule::score)
        .def_readonly("prepared", &MeetingSchedule::prepared)
        // composite grainRoom encoding helpers (see ms/domain.hpp)
        .def("time_grain_count", &MeetingSchedule::timeGrainCount)
        .def("room_count", &MeetingSchedule::roomCount)
        .def("grain_room_count", &MeetingSchedule::grainRoomCount)
        .def("grain_room_of",
             &MeetingSchedule::grainRoomOf,
             py::arg("time_grain_pos"),
             py::arg("room_index"))
        .def("time_grain_pos_of",
             &MeetingSchedule::timeGrainPosOf,
             py::arg("grain_room"))
        .def("room_index_of",
             &MeetingSchedule::roomIndexOf,
             py::arg("grain_room"))
        // Convenience: the current grainRoom per assignment (the solution).
        .def("assignment",
             [](const MeetingSchedule &s)
             {
                 std::vector<int> a;
                 a.reserve(s.meetingAssignments.size());
                 for (const MeetingAssignment &assignment :
                      s.meetingAssignments)
                     a.push_back(assignment.grainRoom);
                 return a;
             })
        // Bulk write of the planning variable (assignment access via
        // .meeting_assignments returns copies, so per-element writes would
        // be lost).
        .def(
            "set_assignment",
            [](MeetingSchedule &s, const std::vector<int> &a)
            {
                if (a.size() != s.meetingAssignments.size())
                    throw py::value_error(
                        "assignment length must equal the assignment count");
                for (std::size_t i = 0; i < a.size(); ++i)
                    s.meetingAssignments[i].grainRoom = a[i];
            },
            py::arg("assignment"));

    py::class_<Breakdown>(m, "Breakdown", py::module_local())
        .def_readonly("room_conflict_grains", &Breakdown::roomConflictGrains)
        .def_readonly("overtime_sum", &Breakdown::overtimeSum)
        .def_readonly("required_conflict_grains",
                      &Breakdown::requiredConflictGrains)
        .def_readonly("room_capacity_shortage",
                      &Breakdown::roomCapacityShortage)
        .def_readonly("start_end_day_violations",
                      &Breakdown::startEndDayViolations)
        .def_readonly("required_preferred_conflict_grains",
                      &Breakdown::requiredPreferredConflictGrains)
        .def_readonly("preferred_conflict_grains",
                      &Breakdown::preferredConflictGrains)
        .def_readonly("last_grain_index_sum", &Breakdown::lastGrainIndexSum)
        .def_readonly("consecutive_meeting_pairs",
                      &Breakdown::consecutiveMeetingPairs)
        .def_readonly("overlapping_meeting_grains",
                      &Breakdown::overlappingMeetingGrains)
        .def_readonly("larger_room_capacity_sum",
                      &Breakdown::largerRoomCapacitySum)
        .def_readonly("room_stability_violations",
                      &Breakdown::roomStabilityViolations)
        .def("hard_total", &Breakdown::hardTotal)
        .def("medium_total", &Breakdown::mediumTotal)
        .def("soft_total", &Breakdown::softTotal);

    m.def("calculate_score",
          &ScoreCalculator::calculateScore,
          py::arg("schedule"),
          "Full recompute of the HardSoftScore (also stores schedule.score).");
    m.def("compute_breakdown",
          &ScoreCalculator::computeBreakdown,
          py::arg("schedule"),
          "Full per-constraint recompute (raw violation counts per Java "
          "constraint).");
    m.def("demo_data",
          &buildDemoData,
          "The Java quickstart's DemoDataGenerator, reproduced draw-for-draw "
          "with java.util.Random(0) (see ms/demo_data.hpp).");

    py::class_<Solver::Config>(m, "SolverConfig", py::module_local())
        .def(py::init<>())
        .def_property(
            "spent_limit_seconds",
            [](const Solver::Config &c)
            { return std::chrono::duration<double>(c.spentLimit).count(); },
            [](Solver::Config &c, double s)
            {
                c.spentLimit
                    = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::duration<double>(s));
            })
        .def_property(
            "unimproved_spent_limit_seconds",
            [](const Solver::Config &c)
            {
                return std::chrono::duration<double>(c.unimprovedSpentLimit)
                    .count();
            },
            [](Solver::Config &c, double s)
            {
                if (s < 0.0)
                    throw py::value_error(
                        "unimproved_spent_limit_seconds must be non-negative");
                c.unimprovedSpentLimit
                    = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::duration<double>(s));
            })
        .def_readwrite("step_count_limit", &Solver::Config::stepCountLimit)
        .def_readwrite("unimproved_step_count_limit",
                       &Solver::Config::unimprovedStepCountLimit)
        .def_readwrite("random_seed", &Solver::Config::randomSeed)
        .def_property(
            "parallel_solver_count",
            [](const Solver::Config &c) { return c.parallelSolverCount; },
            [](Solver::Config &c, int count)
            {
                if (count <= 0)
                    throw py::value_error(
                        "parallel_solver_count must be positive");
                c.parallelSolverCount = count;
            })
        // 0 = auto: clamp(128 * assignment count, 3200, 6400).
        .def_property(
            "late_acceptance_size",
            [](const Solver::Config &c) { return c.lateAcceptanceSize; },
            [](Solver::Config &c, std::int64_t size)
            {
                if (size < 0)
                    throw py::value_error(
                        "late_acceptance_size must be >= 0 (0 = auto)");
                c.lateAcceptanceSize = static_cast<std::size_t>(size);
            })
        // FULL_ASSERT re-verifies the incremental score against a full
        // recompute after every move (debugging aid; much slower).
        .def_property(
            "full_assert",
            [](const Solver::Config &c)
            {
                return c.environmentMode
                       == optimaru::EnvironmentMode::FULL_ASSERT;
            },
            [](Solver::Config &c, bool on)
            {
                c.environmentMode = on ? optimaru::EnvironmentMode::FULL_ASSERT
                                       : optimaru::EnvironmentMode::NON_ASSERT;
            })
        .def_readwrite("ruin_recreate", &Solver::Config::ruinRecreate)
        .def_readwrite("ruin_recreate_unimproved_step_trigger",
                       &Solver::Config::ruinRecreateUnimprovedStepTrigger)
        .def_readwrite("ruin_recreate_min_ruin_fraction",
                       &Solver::Config::ruinRecreateMinRuinFraction)
        .def_readwrite("ruin_recreate_max_ruin_fraction",
                       &Solver::Config::ruinRecreateMaxRuinFraction)
        .def_readwrite("ruin_recreate_max_ruin_count",
                       &Solver::Config::ruinRecreateMaxRuinCount)
        .def_readwrite("ruin_recreate_operator_decay",
                       &Solver::Config::ruinRecreateOperatorDecay)
        .def_property(
            "ruin_recreate_rewards",
            [](const Solver::Config &c) { return c.ruinRecreateRewards; },
            [](Solver::Config &c, std::vector<double> rewards)
            {
                if (rewards.size() != 4)
                    throw py::value_error(
                        "ruin_recreate_rewards must contain exactly 4 values");
                c.ruinRecreateRewards = std::move(rewards);
            })
        .def_readwrite("ruin_recreate_polish_steps",
                       &Solver::Config::ruinRecreatePolishSteps)
        // Destroy-and-repair as an ordinary acceptor-judged move: probability
        // per step (0 disables; default 2e-4, tuned on the quality bench).
        .def_readwrite("ruin_recreate_as_move_weight",
                       &Solver::Config::ruinRecreateAsMoveWeight)
        .def_readwrite("ruin_recreate_as_move_max_ruin",
                       &Solver::Config::ruinRecreateAsMoveMaxRuin)
        .def_readwrite("change_move_weight", &Solver::Config::changeMoveWeight)
        .def_readwrite("swap_move_weight", &Solver::Config::swapMoveWeight)
        // Guided value selection: probability that a change move draws a
        // starting grain that keeps the whole meeting inside one day
        // (0 disables the hook).
        .def_property(
            "day_fit_value_probability",
            [](const Solver::Config &c) { return c.dayFitValueProbability; },
            [](Solver::Config &c, double probability)
            {
                if (probability < 0.0 || probability > 1.0)
                    throw py::value_error(
                        "day_fit_value_probability must be in [0, 1]");
                c.dayFitValueProbability = probability;
            })
        // Early termination once bestScore >= (hard, soft), e.g. (0, -1e18)
        // stops at the first feasible solution.
        .def_property(
            "best_score_limit",
            [](const Solver::Config &c)
            {
                return py::make_tuple(c.bestScoreLimit.hard,
                                      c.bestScoreLimit.soft);
            },
            [](Solver::Config &c, std::pair<std::int64_t, double> hs)
            { c.bestScoreLimit = Score{hs.first, hs.second}; })
        // Callback fired on every new best solution: on_new_best(step, score).
        // The pybind11 wrapper re-acquires the GIL, so it is safe from the
        // engine's multi-start worker threads too.
        .def_readwrite("on_new_best", &Solver::Config::onNewBest);

    py::class_<Solver>(m, "Solver", py::module_local())
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        // Solves in place: sets meeting_assignments[*].grain_room and
        // schedule.score. The GIL is released so parallel multi-start
        // workers can fire on_new_best.
        .def("solve",
             py::overload_cast<MeetingSchedule &>(&Solver::solve),
             py::arg("schedule"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // meeting_scheduling.constraints). The picks replace the defaults; None
        // keeps them.
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               MeetingSchedule &schedule,
               const py::object &constraints)
            {
                ScoreCalculator::prepare(schedule);
                DecomposedCalculator calculator;
                tfpy::applyConstraints(defaultConstraints(), constraints)
                    .into(calculator);
                py::gil_scoped_release release;
                solver.solve(schedule, calculator);
            },
            py::arg("schedule"),
            py::arg("constraints") = py::none());

    // The raw per-unit costs of the default assembly on `schedule` — the input
    // meeting_scheduling.constraints.score() turns into a score. Scoring once
    // is enough: re-pricing any assembly is pure Python arithmetic on these.
    m.def(
        "constraint_costs",
        [](MeetingSchedule &schedule)
        {
            ScoreCalculator::prepare(schedule);
            DecomposedCalculator calculator;
            defaultConstraints().into(calculator);
            return tfpy::constraintCosts(calculator, schedule);
        },
        py::arg("schedule"),
        "Raw per-unit cost of every constraint, before weight/exponent/level.");

    // The constraint names this domain scores, in default-assembly order —
    // the same strings a score breakdown reports.
    m.def(
        "list_constraints",
        [] { return tfpy::constraintNames(defaultConstraints()); },
        "Constraint names available to meeting_scheduling.constraints.");
}
