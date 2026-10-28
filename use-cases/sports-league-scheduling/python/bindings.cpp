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
    m.doc() = "Sports league scheduling — Timefold engine-backed C++ wrapper "
              "exposed to Python";

    py::class_<Score>(m, "Score")
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::isFeasible)
        .def("__str__", &Score::toString)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.toString() + ">"; });

    py::class_<Team>(m, "Team")
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

    py::class_<Round>(m, "Round")
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

    py::class_<Match>(m, "Match")
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

    py::class_<LeagueSchedule>(m, "LeagueSchedule")
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

    py::class_<Breakdown>(m, "Breakdown")
        .def_readonly("matches_on_same_day", &Breakdown::matchesOnSameDay)
        .def_readonly("consecutive_home_matches",
                      &Breakdown::consecutiveHomeMatches)
        .def_readonly("consecutive_away_matches",
                      &Breakdown::consecutiveAwayMatches)
        .def_readonly("repeat_match_on_next_day",
                      &Breakdown::repeatMatchOnNextDay)
        .def_readonly("start_to_away_hop", &Breakdown::startToAwayHop)
        .def_readonly("home_to_away_hop", &Breakdown::homeToAwayHop)
        .def_readonly("away_to_away_hop", &Breakdown::awayToAwayHop)
        .def_readonly("away_to_home_hop", &Breakdown::awayToHomeHop)
        .def_readonly("away_to_end_hop", &Breakdown::awayToEndHop)
        .def_readonly("classic_match_count", &Breakdown::classicMatchCount)
        .def("hard_total", &Breakdown::hardTotal)
        .def("soft_total", &Breakdown::softTotal);

    m.def("calculate_score",
          &ScoreCalculator::calculateScore,
          py::arg("schedule"),
          "Full recompute of the HardSoftScore (also stores schedule.score).");
    m.def("compute_breakdown",
          &ScoreCalculator::computeBreakdown,
          py::arg("schedule"),
          "Full per-constraint recompute (the ten quickstart constraints).");

    py::class_<Solver::Config>(m, "SolverConfig")
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
        // 0 = auto: the solver scales the history with the instance size
        // (clamp(32 * matches, 400, 3200)).
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
        .def_readwrite("change_move_weight", &Solver::Config::changeMoveWeight)
        .def_readwrite("swap_move_weight", &Solver::Config::swapMoveWeight)
        // Nearby selection over matches sharing a team (0 = auto: 4 * teams;
        // negative disables it) and how often a swap partner is drawn from it.
        .def_readwrite("nearby_size", &Solver::Config::nearbySize)
        .def_readwrite("nearby_probability", &Solver::Config::nearbyProbability)
        // Tournament size of the travel-guided entity selection (1 = uniform).
        .def_readwrite("costliest_match_sample",
                       &Solver::Config::costliestMatchSample)
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

    py::class_<Solver>(m, "Solver")
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        // Solves in place: sets match.round and schedule.score. The GIL is
        // released so parallel multi-start workers can fire on_new_best.
        .def("solve",
             py::overload_cast<LeagueSchedule &>(&Solver::solve),
             py::arg("schedule"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // sports_league.constraints). The picks replace the defaults; None
        // keeps them.
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               LeagueSchedule &schedule,
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
    // sports_league.constraints.score() turns into a score. Scoring once is
    // enough: re-pricing any assembly is pure Python arithmetic on these.
    m.def(
        "constraint_costs",
        [](LeagueSchedule &schedule)
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
        "Constraint names available to sports_league.constraints.");
}
