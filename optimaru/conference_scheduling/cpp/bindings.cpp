// pybind11 bindings for the conference-scheduling C++ domain wrappers.
// The Python extension links the existing cs::* implementations and imports
// the generic optimaru engine from optimaru/cpp/optimaru; the engine itself
// is not modified here.
#include <pybind11/functional.h>  // std::function <-> Python callable
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::string conversions

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "cs/constraints.hpp"
#include "cs/domain.hpp"
#include "cs/score_calculator.hpp"
#include "cs/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace cs;
namespace tfpy = optimaru::python;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Conference scheduling — optimaru engine-backed C++ wrapper "
              "exposed to Python";

    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::feasible)
        .def("__str__", &Score::str)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.str() + ">"; });

    py::class_<Timeslot>(m, "Timeslot", py::module_local())
        .def(py::init(
                 [](std::string id,
                    std::int64_t startMinute,
                    std::int64_t endMinute,
                    std::vector<int> talkTypes,
                    std::vector<std::string> tags)
                 {
                     Timeslot t;
                     t.id = std::move(id);
                     t.startMinute = startMinute;
                     t.endMinute = endMinute;
                     t.talkTypes = std::move(talkTypes);
                     t.tags = std::move(tags);
                     return t;
                 }),
             py::arg("id"),
             py::arg("start_minute"),
             py::arg("end_minute"),
             py::arg("talk_types"),
             py::arg("tags") = std::vector<std::string>{})
        .def_readwrite("id", &Timeslot::id)
        .def_readwrite("start_minute", &Timeslot::startMinute)
        .def_readwrite("end_minute", &Timeslot::endMinute)
        .def_readwrite("talk_types", &Timeslot::talkTypes)
        .def_readwrite("tags", &Timeslot::tags)
        .def("duration_in_minutes", &Timeslot::durationInMinutes);

    py::class_<Room>(m, "Room", py::module_local())
        .def(py::init(
                 [](std::string id,
                    std::string name,
                    int capacity,
                    std::vector<int> talkTypes,
                    std::vector<int> unavailableTimeslots,
                    std::vector<std::string> tags)
                 {
                     Room r;
                     r.id = std::move(id);
                     r.name = std::move(name);
                     r.capacity = capacity;
                     r.talkTypes = std::move(talkTypes);
                     r.unavailableTimeslots = std::move(unavailableTimeslots);
                     r.tags = std::move(tags);
                     return r;
                 }),
             py::arg("id"),
             py::arg("name") = std::string{},
             py::arg("capacity") = 0,
             py::arg("talk_types") = std::vector<int>{},
             py::arg("unavailable_timeslots") = std::vector<int>{},
             py::arg("tags") = std::vector<std::string>{})
        .def_readwrite("id", &Room::id)
        .def_readwrite("name", &Room::name)
        .def_readwrite("capacity", &Room::capacity)
        .def_readwrite("talk_types", &Room::talkTypes)
        .def_readwrite("unavailable_timeslots", &Room::unavailableTimeslots)
        .def_readwrite("tags", &Room::tags);

    py::class_<Speaker>(m, "Speaker", py::module_local())
        .def(py::init(
                 [](std::string id,
                    std::string name,
                    std::vector<int> unavailableTimeslots,
                    std::vector<std::string> requiredTimeslotTags,
                    std::vector<std::string> preferredTimeslotTags,
                    std::vector<std::string> prohibitedTimeslotTags,
                    std::vector<std::string> undesiredTimeslotTags,
                    std::vector<std::string> requiredRoomTags,
                    std::vector<std::string> preferredRoomTags,
                    std::vector<std::string> prohibitedRoomTags,
                    std::vector<std::string> undesiredRoomTags)
                 {
                     Speaker s;
                     s.id = std::move(id);
                     s.name = std::move(name);
                     s.unavailableTimeslots = std::move(unavailableTimeslots);
                     s.requiredTimeslotTags = std::move(requiredTimeslotTags);
                     s.preferredTimeslotTags = std::move(preferredTimeslotTags);
                     s.prohibitedTimeslotTags
                         = std::move(prohibitedTimeslotTags);
                     s.undesiredTimeslotTags = std::move(undesiredTimeslotTags);
                     s.requiredRoomTags = std::move(requiredRoomTags);
                     s.preferredRoomTags = std::move(preferredRoomTags);
                     s.prohibitedRoomTags = std::move(prohibitedRoomTags);
                     s.undesiredRoomTags = std::move(undesiredRoomTags);
                     return s;
                 }),
             py::arg("id"),
             py::arg("name") = std::string{},
             py::arg("unavailable_timeslots") = std::vector<int>{},
             py::arg("required_timeslot_tags") = std::vector<std::string>{},
             py::arg("preferred_timeslot_tags") = std::vector<std::string>{},
             py::arg("prohibited_timeslot_tags") = std::vector<std::string>{},
             py::arg("undesired_timeslot_tags") = std::vector<std::string>{},
             py::arg("required_room_tags") = std::vector<std::string>{},
             py::arg("preferred_room_tags") = std::vector<std::string>{},
             py::arg("prohibited_room_tags") = std::vector<std::string>{},
             py::arg("undesired_room_tags") = std::vector<std::string>{})
        .def_readwrite("id", &Speaker::id)
        .def_readwrite("name", &Speaker::name)
        .def_readwrite("unavailable_timeslots", &Speaker::unavailableTimeslots)
        .def_readwrite("required_timeslot_tags", &Speaker::requiredTimeslotTags)
        .def_readwrite("preferred_timeslot_tags",
                       &Speaker::preferredTimeslotTags)
        .def_readwrite("prohibited_timeslot_tags",
                       &Speaker::prohibitedTimeslotTags)
        .def_readwrite("undesired_timeslot_tags",
                       &Speaker::undesiredTimeslotTags)
        .def_readwrite("required_room_tags", &Speaker::requiredRoomTags)
        .def_readwrite("preferred_room_tags", &Speaker::preferredRoomTags)
        .def_readwrite("prohibited_room_tags", &Speaker::prohibitedRoomTags)
        .def_readwrite("undesired_room_tags", &Speaker::undesiredRoomTags);

    py::class_<Talk>(m, "Talk", py::module_local())
        .def(py::init(
                 [](std::string code,
                    std::string title,
                    int talkType,
                    std::vector<int> speakers,
                    std::vector<std::string> themeTrackTags,
                    std::vector<std::string> sectorTags,
                    std::vector<std::string> audienceTypes,
                    int audienceLevel,
                    std::vector<std::string> contentTags,
                    std::string language,
                    int favoriteCount,
                    int crowdControlRisk,
                    std::vector<int> prerequisiteTalks,
                    std::vector<std::string> mutuallyExclusiveTalksTags,
                    std::vector<std::string> requiredTimeslotTags,
                    std::vector<std::string> preferredTimeslotTags,
                    std::vector<std::string> prohibitedTimeslotTags,
                    std::vector<std::string> undesiredTimeslotTags,
                    std::vector<std::string> requiredRoomTags,
                    std::vector<std::string> preferredRoomTags,
                    std::vector<std::string> prohibitedRoomTags,
                    std::vector<std::string> undesiredRoomTags,
                    int slotRoom,
                    bool pinned)
                 {
                     Talk t;
                     t.code = std::move(code);
                     t.title = std::move(title);
                     t.talkType = talkType;
                     t.speakers = std::move(speakers);
                     t.themeTrackTags = std::move(themeTrackTags);
                     t.sectorTags = std::move(sectorTags);
                     t.audienceTypes = std::move(audienceTypes);
                     t.audienceLevel = audienceLevel;
                     t.contentTags = std::move(contentTags);
                     t.language = std::move(language);
                     t.favoriteCount = favoriteCount;
                     t.crowdControlRisk = crowdControlRisk;
                     t.prerequisiteTalks = std::move(prerequisiteTalks);
                     t.mutuallyExclusiveTalksTags
                         = std::move(mutuallyExclusiveTalksTags);
                     t.requiredTimeslotTags = std::move(requiredTimeslotTags);
                     t.preferredTimeslotTags = std::move(preferredTimeslotTags);
                     t.prohibitedTimeslotTags
                         = std::move(prohibitedTimeslotTags);
                     t.undesiredTimeslotTags = std::move(undesiredTimeslotTags);
                     t.requiredRoomTags = std::move(requiredRoomTags);
                     t.preferredRoomTags = std::move(preferredRoomTags);
                     t.prohibitedRoomTags = std::move(prohibitedRoomTags);
                     t.undesiredRoomTags = std::move(undesiredRoomTags);
                     t.slotRoom = slotRoom;
                     t.pinned = pinned;
                     return t;
                 }),
             py::arg("code"),
             py::arg("title") = std::string{},
             py::arg("talk_type") = 0,
             py::arg("speakers") = std::vector<int>{},
             py::arg("theme_track_tags") = std::vector<std::string>{},
             py::arg("sector_tags") = std::vector<std::string>{},
             py::arg("audience_types") = std::vector<std::string>{},
             py::arg("audience_level") = 0,
             py::arg("content_tags") = std::vector<std::string>{},
             py::arg("language") = std::string{"en"},
             py::arg("favorite_count") = 0,
             py::arg("crowd_control_risk") = 0,
             py::arg("prerequisite_talks") = std::vector<int>{},
             py::arg("mutually_exclusive_talks_tags")
             = std::vector<std::string>{},
             py::arg("required_timeslot_tags") = std::vector<std::string>{},
             py::arg("preferred_timeslot_tags") = std::vector<std::string>{},
             py::arg("prohibited_timeslot_tags") = std::vector<std::string>{},
             py::arg("undesired_timeslot_tags") = std::vector<std::string>{},
             py::arg("required_room_tags") = std::vector<std::string>{},
             py::arg("preferred_room_tags") = std::vector<std::string>{},
             py::arg("prohibited_room_tags") = std::vector<std::string>{},
             py::arg("undesired_room_tags") = std::vector<std::string>{},
             py::arg("slot_room") = -1,
             py::arg("pinned") = false)
        .def_readwrite("code", &Talk::code)
        .def_readwrite("title", &Talk::title)
        .def_readwrite("talk_type", &Talk::talkType)
        .def_readwrite("speakers", &Talk::speakers)
        .def_readwrite("theme_track_tags", &Talk::themeTrackTags)
        .def_readwrite("sector_tags", &Talk::sectorTags)
        .def_readwrite("audience_types", &Talk::audienceTypes)
        .def_readwrite("audience_level", &Talk::audienceLevel)
        .def_readwrite("content_tags", &Talk::contentTags)
        .def_readwrite("language", &Talk::language)
        .def_readwrite("required_timeslot_tags", &Talk::requiredTimeslotTags)
        .def_readwrite("preferred_timeslot_tags", &Talk::preferredTimeslotTags)
        .def_readwrite("prohibited_timeslot_tags",
                       &Talk::prohibitedTimeslotTags)
        .def_readwrite("undesired_timeslot_tags", &Talk::undesiredTimeslotTags)
        .def_readwrite("required_room_tags", &Talk::requiredRoomTags)
        .def_readwrite("preferred_room_tags", &Talk::preferredRoomTags)
        .def_readwrite("prohibited_room_tags", &Talk::prohibitedRoomTags)
        .def_readwrite("undesired_room_tags", &Talk::undesiredRoomTags)
        .def_readwrite("mutually_exclusive_talks_tags",
                       &Talk::mutuallyExclusiveTalksTags)
        .def_readwrite("prerequisite_talks", &Talk::prerequisiteTalks)
        .def_readwrite("favorite_count", &Talk::favoriteCount)
        .def_readwrite("crowd_control_risk", &Talk::crowdControlRisk)
        .def_readwrite("slot_room", &Talk::slotRoom)
        .def_readwrite("pinned", &Talk::pinned);

    py::class_<ConferenceSchedule>(m, "ConferenceSchedule", py::module_local())
        .def(py::init(
                 [](std::string name,
                    std::vector<std::string> talkTypes,
                    std::vector<Timeslot> timeslots,
                    std::vector<Room> rooms,
                    std::vector<Speaker> speakers,
                    std::vector<Talk> talks,
                    int minimumConsecutiveTalksPauseInMinutes)
                 {
                     ConferenceSchedule c;
                     c.name = std::move(name);
                     c.talkTypes = std::move(talkTypes);
                     c.timeslots = std::move(timeslots);
                     c.rooms = std::move(rooms);
                     c.speakers = std::move(speakers);
                     c.talks = std::move(talks);
                     c.minimumConsecutiveTalksPauseInMinutes
                         = minimumConsecutiveTalksPauseInMinutes;
                     return c;
                 }),
             py::arg("name"),
             py::arg("talk_types"),
             py::arg("timeslots"),
             py::arg("rooms"),
             py::arg("speakers"),
             py::arg("talks"),
             py::arg("minimum_consecutive_talks_pause_in_minutes") = 30)
        .def_readwrite("name", &ConferenceSchedule::name)
        .def_readwrite("talk_types", &ConferenceSchedule::talkTypes)
        .def_readwrite("timeslots", &ConferenceSchedule::timeslots)
        .def_readwrite("rooms", &ConferenceSchedule::rooms)
        .def_readwrite("speakers", &ConferenceSchedule::speakers)
        // NOTE: element access returns copies; use set_assignment for bulk
        // slot_room writes.
        .def_readwrite("talks", &ConferenceSchedule::talks)
        .def_readwrite(
            "minimum_consecutive_talks_pause_in_minutes",
            &ConferenceSchedule::minimumConsecutiveTalksPauseInMinutes)
        .def_readonly("score", &ConferenceSchedule::score)
        .def_readonly("prepared", &ConferenceSchedule::prepared)
        // composite slotRoom encoding helpers (see cs/domain.hpp)
        .def("timeslot_count", &ConferenceSchedule::timeslotCount)
        .def("room_count", &ConferenceSchedule::roomCount)
        .def("talk_count", &ConferenceSchedule::talkCount)
        .def("slot_room_count", &ConferenceSchedule::slotRoomCount)
        .def("slot_room_of",
             &ConferenceSchedule::slotRoomOf,
             py::arg("timeslot_index"),
             py::arg("room_index"))
        .def("timeslot_index_of",
             &ConferenceSchedule::timeslotIndexOf,
             py::arg("slot_room"))
        .def("room_index_of",
             &ConferenceSchedule::roomIndexOf,
             py::arg("slot_room"))
        // Convenience: the current slotRoom per talk (the solution).
        .def("assignment",
             [](const ConferenceSchedule &c)
             {
                 std::vector<int> a;
                 a.reserve(c.talks.size());
                 for (const Talk &talk : c.talks)
                     a.push_back(talk.slotRoom);
                 return a;
             })
        // Bulk write of the decision variable (talk access via .talks
        // returns copies, so per-talk writes would be lost).
        .def(
            "set_assignment",
            [](ConferenceSchedule &c, const std::vector<int> &a)
            {
                if (a.size() != c.talks.size())
                    throw py::value_error(
                        "assignment length must equal the talk count");
                for (std::size_t i = 0; i < a.size(); ++i)
                    c.talks[i].slotRoom = a[i];
            },
            py::arg("assignment"));

    // No Python Breakdown class: constraint_costs() is the one
    // per-constraint surface. The C++
    // cs::Breakdown keeps its members, named after the registry names.

    m.def("calculate_score",
          &ScoreCalculator::score,
          py::arg("schedule"),
          "Full recompute of the score (also stores schedule.score).");
    m.def("prepare",
          &ScoreCalculator::prepare,
          py::arg("schedule"),
          "Normalize the instance in place (idempotent; also run by "
          "calculate_score / constraint_costs / Solver.solve).");

    py::class_<Solver::Config> config(m, "SolverConfig", py::module_local());

    optimaru::python::bind_config_copy(config);
    config.def(py::init<>())
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
        // 0 = auto: clamp(8 * talkCount, 800, 3200).
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
        // Consecutive local-search runs sharing the one time limit; each
        // restarts from the previous run's best (see cs/solver.hpp).
        .def_property(
            "restart_segments",
            [](const Solver::Config &c) { return c.restartSegments; },
            [](Solver::Config &c, int segments)
            {
                if (segments <= 0)
                    throw py::value_error("restart_segments must be positive");
                c.restartSegments = segments;
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
        // Destroy-and-repair as an ordinary acceptor-judged move; negative =
        // auto (0.2 / slotRoomCount, the budget-share rule), 0 disables.
        .def_readwrite("ruin_recreate_as_move_weight",
                       &Solver::Config::ruinRecreateAsMoveWeight)
        .def_readwrite("ruin_recreate_as_move_max_ruin",
                       &Solver::Config::ruinRecreateAsMoveMaxRuin)
        .def_readwrite("descent_weight", &Solver::Config::descent_weight)
        // Violation-guided entity selection probability (0 = off; see
        // cs/solver.hpp).
        .def_readwrite("hot_talk_probability",
                       &Solver::Config::hotTalkProbability)
        .def_readwrite("change_move_weight", &Solver::Config::change_weight)
        .def_readwrite("swap_move_weight", &Solver::Config::swap_weight)
        // Bias change moves toward the talk's type-compatible cells (see
        // cs/solver.hpp).
        .def_readwrite("compatible_value_probability",
                       &Solver::Config::compatibleValueProbability)
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
        // Solves in place: sets talk.slot_room and schedule.score. The GIL is
        // released so parallel multi-start workers can fire on_new_best.
        .def("solve",
             py::overload_cast<ConferenceSchedule &>(&Solver::solve),
             py::arg("schedule"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // optimaru.conference_scheduling.constraints). The picks replace the
        // defaults; None keeps them. A name may be picked several times:
        // every pick is its own instance (a fresh copy of the default
        // component) with its own knobs.
        .def(
            "solve_with_constraints",
            [](Solver &solver,
               ConferenceSchedule &schedule,
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

    // The raw costs of an assembly on `schedule` — the input
    // optimaru.conference_scheduling.constraints.score() turns into a score. Scoring
    // once is enough: re-pricing any assembly is pure Python arithmetic on
    // these. None = the default assembly; a pick list scores exactly those
    // picks (apply_constraints), each entry carrying its pick's knobs, the
    // k-th pick of a name keyed "<name>#<k>".
    m.def(
        "constraint_costs",
        [](ConferenceSchedule &schedule, const py::object &constraints)
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
        "Raw cost of every constraint (one global total each), before "
        "weight/exponent/offset/level; the three *_diversity_cost rewards "
        "are <= 0.");

    // The constraint names this domain scores, in default-assembly order —
    // the same strings constraint_costs() reports.
    m.def(
        "list_constraints",
        [] { return tfpy::constraint_names(defaultConstraints()); },
        "Constraint names available to optimaru.conference_scheduling.constraints.");

    // The constraint catalog, read off the same default assembly:
    // constraint_info() -> [ConstraintInfo(name, default_level,
    // default_weight, level_flippable, reward, default_enabled), ...] in
    // list_constraints() order. The defaults live in the C++ components only.
    tfpy::bind_constraint_info(m, [] { return defaultConstraints(); });
}
