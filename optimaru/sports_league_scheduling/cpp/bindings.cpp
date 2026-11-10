// pybind11 bindings for the sports-league-scheduling C++ domain wrappers.
// The Python extension links the existing sls::* implementations and imports
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

#include "sls/constraints.hpp"
#include "sls/domain.hpp"
#include "sls/score_calculator.hpp"
#include "sls/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace sls;
namespace tfpy = optimaru::python;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Sports league scheduling — optimaru engine-backed C++ wrapper "
              "exposed to Python";

    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::feasible)
        .def("__str__", &Score::str)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.str() + ">"; });

    py::class_<Team>(m, "Team", py::module_local())
        .def(py::init(
                 [](std::string id, std::string name)
                 {
                     Team t;
                     t.id = std::move(id);
                     t.name = std::move(name);
                     return t;
                 }),
             py::arg("id"),
             py::arg("name") = "")
        .def_readwrite("id", &Team::id)
        .def_readwrite("name", &Team::name);

    py::class_<Round>(m, "Round", py::module_local())
        .def(py::init(
                 [](int index, bool weekendOrHoliday)
                 {
                     Round r;
                     r.index = index;
                     r.weekendOrHoliday = weekendOrHoliday;
                     return r;
                 }),
             py::arg("index"),
             py::arg("weekend_or_holiday") = false)
        .def_readwrite("index", &Round::index)
        .def_readwrite("weekend_or_holiday", &Round::weekendOrHoliday);

    py::class_<Match>(m, "Match", py::module_local())
        .def(py::init(
                 [](std::string id,
                    int homeTeam,
                    int awayTeam,
                    bool classicMatch,
                    int round)
                 {
                     Match match;
                     match.id = std::move(id);
                     match.homeTeam = homeTeam;
                     match.awayTeam = awayTeam;
                     match.classicMatch = classicMatch;
                     match.round = round;
                     return match;
                 }),
             py::arg("id"),
             py::arg("home_team"),
             py::arg("away_team"),
             py::arg("classic_match") = false,
             py::arg("round") = -1)
        .def_readwrite("id", &Match::id)
        .def_readwrite("home_team", &Match::homeTeam)
        .def_readwrite("away_team", &Match::awayTeam)
        .def_readwrite("classic_match", &Match::classicMatch)
        .def_readwrite("round", &Match::round);

    py::class_<LeagueSchedule>(m, "LeagueSchedule", py::module_local())
        .def(py::init(
                 [](std::vector<Team> teams,
                    std::vector<Round> rounds,
                    std::vector<Match> matches,
                    std::vector<std::vector<int>> distanceToTeam)
                 {
                     LeagueSchedule s;
                     s.teams = std::move(teams);
                     s.rounds = std::move(rounds);
                     s.matches = std::move(matches);
                     s.distanceToTeam = std::move(distanceToTeam);
                     return s;
                 }),
             py::arg("teams"),
             py::arg("rounds"),
             py::arg("matches"),
             py::arg("distance_to_team") = std::vector<std::vector<int>>{})
        // NOTE: element access on these list attributes returns copies (the
        // pybind11 std::vector value conversion); use set_assignment for
        // bulk round writes.
        .def_readwrite("teams", &LeagueSchedule::teams)
        .def_readwrite("rounds", &LeagueSchedule::rounds)
        .def_readwrite("matches", &LeagueSchedule::matches)
        .def_readwrite("distance_to_team", &LeagueSchedule::distanceToTeam)
        .def_readonly("score", &LeagueSchedule::score)
        .def("team_count", &LeagueSchedule::teamCount)
        .def("round_count", &LeagueSchedule::roundCount)
        .def("match_count", &LeagueSchedule::matchCount)
        // Convenience: the current round (value) per match (the solution).
        .def("assignment",
             [](const LeagueSchedule &s)
             {
                 std::vector<int> a;
                 a.reserve(s.matches.size());
                 for (const Match &match : s.matches)
                     a.push_back(match.round);
                 return a;
             })
        // Bulk write of the planning variable (match access via .matches
        // returns copies, so per-match writes would be lost).
        .def(
            "set_assignment",
            [](LeagueSchedule &s, const std::vector<int> &a)
            {
                if (a.size() != s.matches.size())
                    throw py::value_error(
                        "assignment length must equal the match count");
                for (std::size_t i = 0; i < a.size(); ++i)
                    s.matches[i].round = a[i];
            },
            py::arg("assignment"));

    m.def("calculate_score",
          &ScoreCalculator::score,
          py::arg("schedule"),
          "Full recompute of the score (also stores schedule.score).");
    // constraint_costs() is the only per-constraint surface.

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
        // (clamp(32 * matches, 400, 3200)).
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
        // Nearby selection over matches sharing a team (0 = auto: 4 * teams;
        // negative disables it) and how often a swap partner is drawn from it.
        .def_readwrite("nearby_size", &Solver::Config::nearbySize)
        .def_readwrite("nearby_probability", &Solver::Config::nearbyProbability)
        // Tournament size of the travel-guided entity selection (1 = uniform).
        .def_readwrite("costliest_match_sample",
                       &Solver::Config::costliestMatchSample)
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
        // Solves in place: sets match.round and schedule.score. The GIL is
        // released so parallel multi-start workers can fire on_new_best.
        .def("solve",
             py::overload_cast<LeagueSchedule &>(&Solver::solve),
             py::arg("schedule"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // optimaru.sports_league_scheduling.constraints). The picks replace the defaults; None
        // keeps them. A name may be picked several times: every pick is its
        // own instance (a fresh copy of the default component) with its own
        // knobs.
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               LeagueSchedule &schedule,
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
    // optimaru.sports_league_scheduling.constraints.score() turns into a score. Scoring once is
    // enough: re-pricing any assembly is pure Python arithmetic on these.
    // None = the default assembly; a pick list scores exactly those picks
    // (apply_constraints), each entry carrying its pick's knobs, the k-th pick
    // of a name keyed "<name>#<k>".
    m.def(
        "constraint_costs",
        [](LeagueSchedule &schedule, const py::object &constraints)
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
        "Raw per-unit cost of every constraint, before "
        "weight/exponent/offset/level.");

    // The constraint names this domain scores, in default-assembly order
    // (unit constraints first, then the globals) — the keys of
    // constraint_costs().
    m.def(
        "list_constraints",
        [] { return tfpy::constraint_names(defaultConstraints()); },
        "Constraint names available to optimaru.sports_league_scheduling.constraints.");

    // The constraint catalog, read off the same default assembly:
    // constraint_info() -> [ConstraintInfo(name, default_level,
    // default_weight, level_flippable, reward, default_enabled), ...] in
    // list_constraints() order. The defaults live in the C++ components only.
    tfpy::bind_constraint_info(m, [] { return defaultConstraints(); });
}
