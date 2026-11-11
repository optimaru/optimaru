// pybind11 bindings for the tournament-scheduling C++ domain wrappers.
// The Python extension links the existing ts::* implementations and imports
// the generic optimaru engine from optimaru/cpp/optimaru; the engine itself
// is not modified here.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::string conversions

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "optimaru/python/constraint_binding.hpp"
#include "optimaru/python/score_binding.hpp"
#include "ts/constraints.hpp"
#include "ts/domain.hpp"
#include "ts/score_calculator.hpp"
#include "ts/solver.hpp"

namespace py = pybind11;
using namespace ts;
namespace opy = optimaru::python;

PYBIND11_MODULE(_core, m) {
    m.doc() =
            "Tournament scheduling — optimaru engine-backed C++ wrapper "
            "exposed to Python";

    m.attr("MEDIUM_SCALE") = kMediumScale;

    opy::bind_score<Score>(m);

    py::class_<Team>(m, "Team", py::module_local())
            .def(py::init([](std::string name) {
                     Team t;
                     t.name = std::move(name);
                     return t;
                 }),
                 py::arg("name"))
            .def_readwrite("name", &Team::name);

    py::class_<Day>(m, "Day", py::module_local())
            .def(py::init([](int dateIndex) {
                     Day d;
                     d.dateIndex = dateIndex;
                     return d;
                 }),
                 py::arg("date_index"))
            .def_readwrite("date_index", &Day::dateIndex);

    py::class_<UnavailabilityPenalty>(m, "UnavailabilityPenalty", py::module_local())
            .def(py::init([](int team, int day) {
                     UnavailabilityPenalty p;
                     p.team = team;
                     p.day = day;
                     return p;
                 }),
                 py::arg("team"), py::arg("day"))
            .def_readwrite("team", &UnavailabilityPenalty::team)
            .def_readwrite("day", &UnavailabilityPenalty::day);

    py::class_<TeamAssignment>(m, "TeamAssignment", py::module_local())
            .def(py::init([](std::int64_t id, int day, int indexInDay, int team, bool pinned) {
                     TeamAssignment a;
                     a.id = id;
                     a.day = day;
                     a.indexInDay = indexInDay;
                     a.team = team;
                     a.pinned = pinned;
                     return a;
                 }),
                 py::arg("id"), py::arg("day"), py::arg("index_in_day") = 0, py::arg("team") = -1,
                 py::arg("pinned") = false)
            .def_readwrite("id", &TeamAssignment::id)
            .def_readwrite("day", &TeamAssignment::day)
            .def_readwrite("index_in_day", &TeamAssignment::indexInDay)
            .def_readwrite("team", &TeamAssignment::team)
            .def_readwrite("pinned", &TeamAssignment::pinned);

    py::class_<TournamentSchedule>(m, "TournamentSchedule", py::module_local())
            .def(py::init([](std::vector<Team> teams, std::vector<Day> days,
                             std::vector<UnavailabilityPenalty> unavailabilityPenalties,
                             std::vector<TeamAssignment> teamAssignments) {
                     TournamentSchedule s;
                     s.teams = std::move(teams);
                     s.days = std::move(days);
                     s.unavailabilityPenalties = std::move(unavailabilityPenalties);
                     s.teamAssignments = std::move(teamAssignments);
                     return s;
                 }),
                 py::arg("teams"), py::arg("days"),
                 py::arg("unavailability_penalties") = std::vector<UnavailabilityPenalty>{},
                 py::arg("team_assignments") = std::vector<TeamAssignment>{})
            .def_readwrite("teams", &TournamentSchedule::teams)
            .def_readwrite("days", &TournamentSchedule::days)
            .def_readwrite("unavailability_penalties", &TournamentSchedule::unavailabilityPenalties)
            // NOTE: element access returns copies; use set_assignment for bulk
            // team writes.
            .def_readwrite("team_assignments", &TournamentSchedule::teamAssignments)
            .def_readonly("score", &TournamentSchedule::score)
            .def("team_count", &TournamentSchedule::teamCount)
            .def("day_count", &TournamentSchedule::dayCount)
            // Convenience: the current team per assignment (the solution).
            .def("assignment",
                 [](const TournamentSchedule& s) {
                     std::vector<int> a;
                     a.reserve(s.teamAssignments.size());
                     for (const TeamAssignment& assignment : s.teamAssignments)
                         a.push_back(assignment.team);
                     return a;
                 })
            // Bulk write of the decision variable (team_assignments access
            // returns copies, so per-assignment writes would be lost).
            .def(
                    "set_assignment",
                    [](TournamentSchedule& s, const std::vector<int>& a) {
                        if (a.size() != s.teamAssignments.size())
                            throw py::value_error(
                                    "assignment length must equal the team assignment "
                                    "count");
                        for (std::size_t i = 0; i < a.size(); ++i) s.teamAssignments[i].team = a[i];
                    },
                    py::arg("assignment"));

    m.def("calculate_score", &ScoreCalculator::score, py::arg("schedule"),
          "Full recompute of the score (also stores schedule.score).");
    m.def("unfairness", &ScoreCalculator::unfairness, py::arg("loads"),
          "Load-balance unfairness of the given loads (6 s.f., "
          "HALF_EVEN).");

    optimaru::python::bind_solver_config(
            py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
            .def(py::init<>())
            .def_readwrite("change_move_weight", &Solver::Config::change_weight)
            .def_readwrite("swap_move_weight", &Solver::Config::swap_weight)
            .def_readwrite("descent_weight", &Solver::Config::descent_weight)
            .def_readwrite("custom_move_probability", &Solver::Config::custom_move_probability)
            .def_readwrite("value_recommender", &Solver::Config::value_hint)
            // Loaded-team-count macro moves: the search's only way across the
            // medium level's team-count barrier (see ts/solver.hpp).
            .def_readwrite("team_count_moves", &Solver::Config::team_count_moves);

    py::class_<Solver>(m, "Solver", py::module_local())
            .def(py::init<>())
            .def(py::init<Solver::Config>(), py::arg("config"))
            // Solves in place: sets team_assignments[*].team and schedule.score.
            // The GIL is released so parallel multi-start workers can fire
            // on_new_best.
            .def("solve", py::overload_cast<TournamentSchedule&>(&Solver::solve),
                 py::arg("schedule"), py::call_guard<py::gil_scoped_release>())
            // Same solve, scored by a caller-chosen constraint set (see
            // optimaru.tournament_scheduling.constraints). The picks replace the defaults;
            // None keeps them. A name may be picked several times: every pick is
            // its own instance (a fresh copy of the default component) with its
            // own knobs.
            .def(
                    "solve_with_constraints",
                    [](Solver& solver, TournamentSchedule& schedule,
                       const py::object& constraints) {
                        DecomposedCalculator calculator;
                        opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                                .into(calculator);
                        py::gil_scoped_release release;
                        solver.solve(schedule, calculator);
                    },
                    py::arg("schedule"), py::arg("constraints") = py::none());

    // constraint_costs(), list_constraints() and constraint_info(), off the
    // default assembly. In constraint_costs() the scoped team_unavailability_cost
    // lists the teams it evaluates (those with an unavailability fact) as "units".
    opy::bind_constraints<TournamentSchedule>(m, "schedule", [] { return defaultConstraints(); });
}
