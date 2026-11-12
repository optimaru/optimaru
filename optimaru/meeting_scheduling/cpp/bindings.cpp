// pybind11 bindings for the meeting-scheduling C++ domain wrappers.
// The Python extension links the existing ms::* implementations and imports
// the generic optimaru engine from optimaru/cpp/optimaru; the engine itself
// is not modified here.
#include <pybind11/functional.h>  // std::function <-> Python callable
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::string conversions

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "ms/constraints.hpp"
#include "ms/demo_data.hpp"
#include "ms/domain.hpp"
#include "ms/score_calculator.hpp"
#include "ms/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"
#include "optimaru/python/score_binding.hpp"

namespace py = pybind11;
using namespace ms;
namespace opy = optimaru::python;

PYBIND11_MODULE(_core, m) {
    m.doc() =
            "Meeting scheduling — optimaru engine-backed C++ wrapper "
            "exposed to Python";

    m.attr("MEDIUM_SCALE") = kMediumScale;
    m.attr("GRAIN_LENGTH_IN_MINUTES") = kGrainLengthInMinutes;

    opy::bind_score<Score>(m);

    py::class_<Person>(m, "Person", py::module_local())
            .def(py::init([](std::string id, std::string fullName) {
                     Person p;
                     p.id = std::move(id);
                     p.fullName = std::move(fullName);
                     return p;
                 }),
                 py::arg("id"), py::arg("full_name") = "")
            .def_readwrite("id", &Person::id)
            .def_readwrite("full_name", &Person::fullName);

    py::class_<TimeGrain>(m, "TimeGrain", py::module_local())
            .def(py::init([](std::string id, int grainIndex, int dayOfYear,
                             int startingMinuteOfDay) {
                     TimeGrain g;
                     g.id = std::move(id);
                     g.grainIndex = grainIndex;
                     g.dayOfYear = dayOfYear;
                     g.startingMinuteOfDay = startingMinuteOfDay;
                     return g;
                 }),
                 py::arg("id"), py::arg("grain_index"), py::arg("day_of_year"),
                 py::arg("starting_minute_of_day"))
            .def_readwrite("id", &TimeGrain::id)
            .def_readwrite("grain_index", &TimeGrain::grainIndex)
            .def_readwrite("day_of_year", &TimeGrain::dayOfYear)
            .def_readwrite("starting_minute_of_day", &TimeGrain::startingMinuteOfDay);

    py::class_<Room>(m, "Room", py::module_local())
            .def(py::init([](std::string id, std::string name, int capacity) {
                     Room r;
                     r.id = std::move(id);
                     r.name = std::move(name);
                     r.capacity = capacity;
                     return r;
                 }),
                 py::arg("id"), py::arg("name") = "", py::arg("capacity") = 0)
            .def_readwrite("id", &Room::id)
            .def_readwrite("name", &Room::name)
            .def_readwrite("capacity", &Room::capacity);

    py::class_<Meeting>(m, "Meeting", py::module_local())
            .def(py::init([](std::string id, std::string topic, int durationInGrains,
                             std::vector<int> requiredAttendees,
                             std::vector<int> preferredAttendees, bool entireGroupMeeting) {
                     Meeting meeting;
                     meeting.id = std::move(id);
                     meeting.topic = std::move(topic);
                     meeting.durationInGrains = durationInGrains;
                     meeting.requiredAttendees = std::move(requiredAttendees);
                     meeting.preferredAttendees = std::move(preferredAttendees);
                     meeting.entireGroupMeeting = entireGroupMeeting;
                     return meeting;
                 }),
                 py::arg("id"), py::arg("topic") = "", py::arg("duration_in_grains") = 1,
                 py::arg("required_attendees") = std::vector<int>{},
                 py::arg("preferred_attendees") = std::vector<int>{},
                 py::arg("entire_group_meeting") = false)
            .def_readwrite("id", &Meeting::id)
            .def_readwrite("topic", &Meeting::topic)
            .def_readwrite("duration_in_grains", &Meeting::durationInGrains)
            // Person indices; one entry == one attendance.
            .def_readwrite("required_attendees", &Meeting::requiredAttendees)
            .def_readwrite("preferred_attendees", &Meeting::preferredAttendees)
            .def_readwrite("entire_group_meeting", &Meeting::entireGroupMeeting)
            .def("required_capacity", &Meeting::requiredCapacity);

    py::class_<MeetingAssignment>(m, "MeetingAssignment", py::module_local())
            .def(py::init([](std::string id, int meeting, int grainRoom, bool pinned) {
                     MeetingAssignment assignment;
                     assignment.id = std::move(id);
                     assignment.meeting = meeting;
                     assignment.grainRoom = grainRoom;
                     assignment.pinned = pinned;
                     return assignment;
                 }),
                 py::arg("id"), py::arg("meeting"), py::arg("grain_room") = -1,
                 py::arg("pinned") = false)
            .def_readwrite("id", &MeetingAssignment::id)
            .def_readwrite("meeting", &MeetingAssignment::meeting)
            // Composite decision variable: time_grain_pos * room_count +
            // room_index; -1 == unassigned.
            .def_readwrite("grain_room", &MeetingAssignment::grainRoom)
            .def_readwrite("pinned", &MeetingAssignment::pinned);

    py::class_<MeetingSchedule>(m, "MeetingSchedule", py::module_local())
            .def(py::init([](std::vector<Person> people, std::vector<TimeGrain> timeGrains,
                             std::vector<Room> rooms, std::vector<Meeting> meetings,
                             std::vector<MeetingAssignment> meetingAssignments) {
                     MeetingSchedule s;
                     s.people = std::move(people);
                     s.timeGrains = std::move(timeGrains);
                     s.rooms = std::move(rooms);
                     s.meetings = std::move(meetings);
                     s.meetingAssignments = std::move(meetingAssignments);
                     return s;
                 }),
                 py::arg("people"), py::arg("time_grains"), py::arg("rooms"), py::arg("meetings"),
                 py::arg("meeting_assignments"))
            .def_readwrite("people", &MeetingSchedule::people)
            .def_readwrite("time_grains", &MeetingSchedule::timeGrains)
            .def_readwrite("rooms", &MeetingSchedule::rooms)
            .def_readwrite("meetings", &MeetingSchedule::meetings)
            // NOTE: element access returns copies; use set_assignment for bulk
            // grain_room writes.
            .def_readwrite("meeting_assignments", &MeetingSchedule::meetingAssignments)
            .def_readonly("score", &MeetingSchedule::score)
            .def_readonly("prepared", &MeetingSchedule::prepared)
            // composite grainRoom encoding helpers (see ms/domain.hpp)
            .def("time_grain_count", &MeetingSchedule::timeGrainCount)
            .def("room_count", &MeetingSchedule::roomCount)
            .def("grain_room_count", &MeetingSchedule::grainRoomCount)
            .def("grain_room_of", &MeetingSchedule::grainRoomOf, py::arg("time_grain_pos"),
                 py::arg("room_index"))
            .def("time_grain_pos_of", &MeetingSchedule::timeGrainPosOf, py::arg("grain_room"))
            .def("room_index_of", &MeetingSchedule::roomIndexOf, py::arg("grain_room"))
            // Convenience: the current grainRoom per assignment (the solution).
            .def("assignment",
                 [](const MeetingSchedule& s) {
                     std::vector<int> a;
                     a.reserve(s.meetingAssignments.size());
                     for (const MeetingAssignment& assignment : s.meetingAssignments)
                         a.push_back(assignment.grainRoom);
                     return a;
                 })
            // Bulk write of the decision variable (assignment access via
            // .meeting_assignments returns copies, so per-element writes would
            // be lost).
            .def(
                    "set_assignment",
                    [](MeetingSchedule& s, const std::vector<int>& a) {
                        if (a.size() != s.meetingAssignments.size())
                            throw py::value_error(
                                    "assignment length must equal the assignment count");
                        for (std::size_t i = 0; i < a.size(); ++i)
                            s.meetingAssignments[i].grainRoom = a[i];
                    },
                    py::arg("assignment"));

    m.def("calculate_score", &ScoreCalculator::score, py::arg("schedule"),
          "Full recompute of the score (also stores schedule.score).");
    m.def("generate_demo_data", &buildDemoData,
          "The demo instance (deterministic; see ms/demo_data.hpp).");
    m.attr("demo_data") = m.attr("generate_demo_data");  // the name before 0.5.1

    optimaru::python::bind_solver_config(
            py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
            .def(py::init<>())
            .def_readwrite("change_move_weight", &Solver::Config::change_weight)
            .def_readwrite("swap_move_weight", &Solver::Config::swap_weight)
            // Guided value selection: probability that a change move draws a
            // starting grain that keeps the whole meeting inside one day
            // (0 disables the hook).
            .def_property(
                    "day_fit_value_probability",
                    [](const Solver::Config& c) { return c.day_fit_value_probability; },
                    [](Solver::Config& c, double probability) {
                        if (probability < 0.0 || probability > 1.0)
                            throw py::value_error("day_fit_value_probability must be in [0, 1]");
                        c.day_fit_value_probability = probability;
                    });

    py::class_<Solver>(m, "Solver", py::module_local())
            .def(py::init<>())
            .def(py::init<Solver::Config>(), py::arg("config"))
            // Solves in place: sets meeting_assignments[*].grain_room and
            // schedule.score. The GIL is released so parallel multi-start
            // workers can fire on_new_best.
            .def("solve", py::overload_cast<MeetingSchedule&>(&Solver::solve), py::arg("schedule"),
                 py::call_guard<py::gil_scoped_release>())
            // Same solve, scored by a caller-chosen constraint set (see
            // optimaru.meeting_scheduling.constraints). The picks replace the defaults; None
            // keeps them. A name may be picked several times: every pick is its
            // own instance (a fresh copy of the default component) with its own
            // knobs.
            .def(
                    "solve_with_constraints",
                    [](Solver& solver, MeetingSchedule& schedule, const py::object& constraints) {
                        ScoreCalculator::prepare(schedule);
                        DecomposedCalculator calculator;
                        opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                                .into(calculator);
                        py::gil_scoped_release release;
                        solver.solve(schedule, calculator);
                    },
                    py::arg("schedule"), py::arg("constraints") = py::none());

    // constraint_costs(), list_constraints() and constraint_info(), off the
    // default assembly.
    opy::bind_constraints<MeetingSchedule>(
            m, "schedule", [] { return defaultConstraints(); }, &ScoreCalculator::prepare);
}
