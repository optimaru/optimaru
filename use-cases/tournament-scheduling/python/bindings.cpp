// pybind11 bindings for the tournament-scheduling C++ domain wrappers.
// The Python extension links the existing ts::* implementations and imports
// the generic optimaru engine from ../../../include; the engine itself
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
#include "ts/constraints.hpp"
#include "ts/domain.hpp"
#include "ts/score_calculator.hpp"
#include "ts/solver.hpp"

namespace py = pybind11;
using namespace ts;
namespace tfpy = optimaru::python;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Tournament scheduling — optimaru engine-backed C++ wrapper "
              "exposed to Python";

    m.attr("MEDIUM_SCALE") = kMediumScale;

    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::feasible)
        .def("__str__", &Score::str)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.str() + ">"; });

    py::class_<Team>(m, "Team", py::module_local())
        .def(py::init(
                 [](std::string name)
                 {
                     Team t;
                     t.name = std::move(name);
                     return t;
                 }),
             py::arg("name"))
        .def_readwrite("name", &Team::name);

    py::class_<Day>(m, "Day", py::module_local())
        .def(py::init(
                 [](int dateIndex)
                 {
                     Day d;
                     d.dateIndex = dateIndex;
                     return d;
                 }),
             py::arg("date_index"))
        .def_readwrite("date_index", &Day::dateIndex);

    py::class_<UnavailabilityPenalty>(m, "UnavailabilityPenalty", py::module_local())
        .def(py::init(
                 [](int team, int day)
                 {
                     UnavailabilityPenalty p;
                     p.team = team;
                     p.day = day;
                     return p;
                 }),
             py::arg("team"),
             py::arg("day"))
        .def_readwrite("team", &UnavailabilityPenalty::team)
        .def_readwrite("day", &UnavailabilityPenalty::day);

    py::class_<TeamAssignment>(m, "TeamAssignment", py::module_local())
        .def(py::init(
                 [](std::int64_t id,
                    int day,
                    int indexInDay,
                    int team,
                    bool pinned)
                 {
                     TeamAssignment a;
                     a.id = id;
                     a.day = day;
                     a.indexInDay = indexInDay;
                     a.team = team;
                     a.pinned = pinned;
                     return a;
                 }),
             py::arg("id"),
             py::arg("day"),
             py::arg("index_in_day") = 0,
             py::arg("team") = -1,
             py::arg("pinned") = false)
        .def_readwrite("id", &TeamAssignment::id)
        .def_readwrite("day", &TeamAssignment::day)
        .def_readwrite("index_in_day", &TeamAssignment::indexInDay)
        .def_readwrite("team", &TeamAssignment::team)
        .def_readwrite("pinned", &TeamAssignment::pinned);

    py::class_<TournamentSchedule>(m, "TournamentSchedule", py::module_local())
        .def(py::init(
                 [](std::vector<Team> teams,
                    std::vector<Day> days,
                    std::vector<UnavailabilityPenalty> unavailabilityPenalties,
                    std::vector<TeamAssignment> teamAssignments)
                 {
                     TournamentSchedule s;
                     s.teams = std::move(teams);
                     s.days = std::move(days);
                     s.unavailabilityPenalties
                         = std::move(unavailabilityPenalties);
                     s.teamAssignments = std::move(teamAssignments);
                     return s;
                 }),
             py::arg("teams"),
             py::arg("days"),
             py::arg("unavailability_penalties")
             = std::vector<UnavailabilityPenalty>{},
             py::arg("team_assignments") = std::vector<TeamAssignment>{})
        .def_readwrite("teams", &TournamentSchedule::teams)
        .def_readwrite("days", &TournamentSchedule::days)
        .def_readwrite("unavailability_penalties",
                       &TournamentSchedule::unavailabilityPenalties)
        // NOTE: element access returns copies; use set_assignment for bulk
        // team writes.
        .def_readwrite("team_assignments", &TournamentSchedule::teamAssignments)
        .def_readonly("score", &TournamentSchedule::score)
        .def("team_count", &TournamentSchedule::teamCount)
        .def("day_count", &TournamentSchedule::dayCount)
        // Convenience: the current team per assignment (the solution).
        .def("assignment",
             [](const TournamentSchedule &s)
             {
                 std::vector<int> a;
                 a.reserve(s.teamAssignments.size());
                 for (const TeamAssignment &assignment : s.teamAssignments)
                     a.push_back(assignment.team);
                 return a;
             })
        // Bulk write of the planning variable (team_assignments access
        // returns copies, so per-assignment writes would be lost).
        .def(
            "set_assignment",
            [](TournamentSchedule &s, const std::vector<int> &a)
            {
                if (a.size() != s.teamAssignments.size())
                    throw py::value_error(
                        "assignment length must equal the team assignment "
                        "count");
                for (std::size_t i = 0; i < a.size(); ++i)
                    s.teamAssignments[i].team = a[i];
            },
            py::arg("assignment"));

    m.def("calculate_score",
          &ScoreCalculator::score,
          py::arg("schedule"),
          "Full recompute of the HardSoftScore (also stores schedule.score).");
    m.def("unfairness",
          &ScoreCalculator::unfairness,
          py::arg("loads"),
          "Load-balance unfairness of the given loads (6 s.f., "
          "HALF_EVEN).");

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
        // 0 = auto: the solver scales the history with the instance size
        // (clamp(8 * assignments, 400, 3200)).
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
        .def_readwrite("change_move_weight", &Solver::Config::change_weight)
        .def_readwrite("swap_move_weight", &Solver::Config::swap_weight)
        .def_readwrite("descent_weight", &Solver::Config::descent_weight)
        .def_readwrite("ruin_recreate_as_move_weight",
                       &Solver::Config::ruinRecreateAsMoveWeight)
        .def_readwrite("ruin_recreate_as_move_max_ruin",
                       &Solver::Config::ruinRecreateAsMoveMaxRuin)
        .def_readwrite("custom_move_probability",
                       &Solver::Config::custom_move_probability)
        .def_readwrite("value_recommender", &Solver::Config::value_hint)
        // Loaded-team-count macro moves: the search's only way across the
        // medium level's team-count barrier (see ts/solver.hpp).
        .def_readwrite("team_count_moves", &Solver::Config::teamCountMoves)
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
        // Solves in place: sets team_assignments[*].team and schedule.score.
        // The GIL is released so parallel multi-start workers can fire
        // on_new_best.
        .def("solve",
             py::overload_cast<TournamentSchedule &>(&Solver::solve),
             py::arg("schedule"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // tournament_scheduling.constraints). The picks replace the defaults;
        // None keeps them. A name may be picked several times: every pick is
        // its own instance (a fresh copy of the default component) with its
        // own knobs.
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               TournamentSchedule &schedule,
               const py::object &constraints)
            {
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
    // tournament_scheduling.constraints.score() turns into a score. Scoring
    // once is enough: re-pricing any assembly is pure Python arithmetic on
    // these. The scoped team_unavailability_cost lists the teams it evaluates
    // (those with an unavailability fact) as "units". None = the default
    // assembly; a pick list scores exactly those picks (apply_constraints),
    // each entry carrying its pick's knobs, the k-th pick of a name keyed
    // "<name>#<k>".
    m.def(
        "constraint_costs",
        [](TournamentSchedule &schedule, const py::object &constraints)
        {
            DecomposedCalculator calculator;
            tfpy::apply_constraints([] { return defaultConstraints(); },
                                   constraints)
                .into(calculator);
            return tfpy::constraint_costs(calculator, schedule);
        },
        py::arg("schedule"),
        py::arg("constraints") = py::none(),
        "Raw per-unit cost of every constraint, before weight/exponent/offset/level "
        "(each entry also carries the domain's medium_fold).");

    // The constraint names this domain scores, in default-assembly order —
    // the keys of constraint_costs().
    m.def(
        "list_constraints",
        [] { return tfpy::constraint_names(defaultConstraints()); },
        "Constraint names available to tournament_scheduling.constraints.");

    // The constraint catalog, read off the same default assembly:
    // constraint_info() -> [ConstraintInfo(name, default_level,
    // default_weight, level_flippable, reward, default_enabled), ...] in
    // list_constraints() order. The defaults live in the C++ components only
    // (assignment_count_imbalance_cost: medium, weight 1 in medium units).
    tfpy::bind_constraint_info(m, [] { return defaultConstraints(); });
}
