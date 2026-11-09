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
        .def("is_feasible", &Score::feasible)
        .def("__str__", &Score::str)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.str() + ">"; });

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

    m.def("calculate_score",
          &ScoreCalculator::score,
          py::arg("schedule"),
          "Full recompute of the HardSoftScore (also stores schedule.score).");
    m.def("demo_data",
          &buildDemoData,
          "The demo instance (deterministic; see ms/demo_data.hpp).");

    optimaru::python::bind_config_copy(py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
        .def(py::init<>())
        .def_property(
            "spent_limit_seconds",
            [](const Solver::Config &c)
            { return std::chrono::duration<double>(c.time_limit).count(); },
            [](Solver::Config &c, double s)
            { c.time_limit = optimaru::python::seconds_to_millis(s, "spent_limit_seconds"); })
        .def_property(
            "unimproved_spent_limit_seconds",
            [](const Solver::Config &c)
            {
                return std::chrono::duration<double>(c.stall_time_limit)
                    .count();
            },
            [](Solver::Config &c, double s)
            { c.stall_time_limit = optimaru::python::seconds_to_millis(s, "unimproved_spent_limit_seconds"); })
        .def_readwrite("step_count_limit", &Solver::Config::step_limit)
        .def_readwrite("unimproved_step_count_limit",
                       &Solver::Config::stall_step_limit)
        .def_readwrite("random_seed", &Solver::Config::seed)
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
            [](const Solver::Config &c) { return c.history_length; },
            [](Solver::Config &c, std::int64_t size)
            {
                if (size < 0)
                    throw py::value_error(
                        "late_acceptance_size must be >= 0 (0 = auto)");
                c.history_length = static_cast<std::size_t>(size);
            })
        // ScoreChecks::Full re-verifies the incremental score against a full
        // recompute after every move (debugging aid; much slower).
        .def_property(
            "full_assert",
            [](const Solver::Config &c)
            {
                return c.score_checks
                       == optimaru::ScoreChecks::Full;
            },
            [](Solver::Config &c, bool on)
            {
                c.score_checks = on ? optimaru::ScoreChecks::Full
                                       : optimaru::ScoreChecks::Off;
            })
        .def_readwrite("ruin_recreate", &Solver::Config::ruin_recreate)
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
        .def_readwrite("change_move_weight", &Solver::Config::change_weight)
        .def_readwrite("swap_move_weight", &Solver::Config::swap_weight)
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
        // Early termination once bestScore >= (hard, soft), e.g. (0, -inf)
        // stops at the first feasible solution.
        .def_property(
            "best_score_limit",
            [](const Solver::Config &c)
            {
                return py::make_tuple(c.target_score.hard,
                                      c.target_score.soft);
            },
            [](Solver::Config &c, std::pair<std::int64_t, double> hs)
            { c.target_score = Score{hs.first, hs.second}; })
        // Callback fired on every new best solution: on_new_best(step, score).
        // The pybind11 wrapper re-acquires the GIL, so it is safe from the
        // engine's multi-start worker threads too.
        .def_readwrite("on_new_best", &Solver::Config::on_new_best);

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
        // keeps them. A name may be picked several times: every pick is its
        // own instance (a fresh copy of the default component) with its own
        // knobs.
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               MeetingSchedule &schedule,
               const py::object &constraints)
            {
                ScoreCalculator::prepare(schedule);
                DecomposedCalculator calculator;
                tfpy::apply_constraints([] { return defaultConstraints(); },
                                       constraints)
                    .into(calculator);
                py::gil_scoped_release release;
                solver.solve(schedule, calculator);
            },
            py::arg("schedule"),
            py::arg("constraints") = py::none());

    // The raw per-unit costs of an assembly on `schedule` — the input
    // meeting_scheduling.constraints.score() turns into a score. Scoring once
    // is enough: re-pricing any assembly is pure Python arithmetic on these.
    // None = the default assembly; a pick list scores exactly those picks
    // (apply_constraints), each entry carrying its pick's knobs, the k-th pick
    // of a name keyed "<name>#<k>".
    m.def(
        "constraint_costs",
        [](MeetingSchedule &schedule, const py::object &constraints)
        {
            ScoreCalculator::prepare(schedule);
            DecomposedCalculator calculator;
            tfpy::apply_constraints([] { return defaultConstraints(); },
                                   constraints)
                .into(calculator);
            return tfpy::constraint_costs(calculator, schedule);
        },
        py::arg("schedule"),
        py::arg("constraints") = py::none(),
        "Raw per-unit cost of every constraint, before weight/exponent/offset/"
        "level (each entry also carries the domain's medium_fold).");

    // The constraint names this domain scores, in default-assembly order —
    // the keys of constraint_costs().
    m.def(
        "list_constraints",
        [] { return tfpy::constraint_names(defaultConstraints()); },
        "Constraint names available to meeting_scheduling.constraints.");

    // The constraint catalog, read off the same default assembly:
    // constraint_info() -> [ConstraintInfo(name, default_level,
    // default_weight, level_flippable, reward, default_enabled), ...] in
    // list_constraints() order. The defaults live in the C++ components only
    // (the two attendance-conflict mediums: weight 1 in medium units).
    tfpy::bind_constraint_info(m, [] { return defaultConstraints(); });
}
