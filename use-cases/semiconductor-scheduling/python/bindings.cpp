// pybind11 bindings for the semiconductor-scheduling C++ domain (namespace
// fab). The Python extension links the fab::* implementation in ../cpp and
// imports the generic optimaru engine from ../../../include; the engine
// itself is not modified here.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::optional / std::string conversions

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fab/constraints.hpp"
#include "fab/decoder.hpp"
#include "fab/domain.hpp"
#include "fab/score_calculator.hpp"
#include "fab/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace fab;
namespace tfpy = optimaru::python;

namespace
{

// A (hard, soft) pair from a Score or any 2-sequence, for the comparisons.
bool pairOf(const py::handle &other, std::int64_t &hard, double &soft)
{
    if (py::isinstance<Score>(other))
    {
        const Score &s = other.cast<const Score &>();
        hard = s.hard;
        soft = s.soft;
        return true;
    }
    if (py::isinstance<py::sequence>(other) && !py::isinstance<py::str>(other) && py::len(other) == 2)
    {
        const py::sequence seq = py::reinterpret_borrow<py::sequence>(other);
        try
        {
            hard = seq[0].cast<std::int64_t>();
            soft = seq[1].cast<double>();
            return true;
        }
        catch (const py::cast_error &)
        {
            return false;
        }
    }
    return false;
}

int compare(const Score &s, std::int64_t hard, double soft)
{
    if (s.hard != hard)
        return s.hard < hard ? -1 : 1;
    if (s.soft != soft)
        return s.soft < soft ? -1 : 1;
    return 0;
}

// Python modulo: an option index from any int choice.
int optionOf(const SemiconductorSchedule &schedule, int row, std::int64_t choice)
{
    const std::int64_t count = schedule.optionCountOfRow(row);
    return static_cast<int>(((choice % count) + count) % count);
}

void storeScore(SemiconductorSchedule &schedule, const Score &score)
{
    ScoreCalculator::storeTotals(schedule, ScoreCalculator::rawCosts(schedule));
    schedule.score = score;
    schedule.scored = true;
}

}  // namespace

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Semiconductor fab scheduling — optimaru engine-backed C++ wrapper exposed to Python";

    // (hard, soft), lexicographic. Also a 2-tuple: iterable, indexable and
    // comparable with Scores and (hard, soft) pairs, like a NamedTuple.
    py::class_<Score>(m, "Score", py::module_local())
        .def(py::init([](std::int64_t hard, double soft) { return Score{hard, soft}; }), py::arg("hard") = 0,
             py::arg("soft") = 0.0)
        .def_readonly("hard", &Score::hard)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::feasible)
        .def("__str__", &Score::str)
        .def("__repr__", [](const Score &s) { return "<Score " + s.str() + ">"; })
        .def("__len__", [](const Score &) { return 2; })
        .def("__iter__", [](const Score &s) { return py::iter(py::make_tuple(s.hard, s.soft)); })
        .def("__getitem__",
             [](const Score &s, const py::object &i) { return py::make_tuple(s.hard, s.soft).attr("__getitem__")(i); })
        .def("__hash__", [](const Score &s) { return py::hash(py::make_tuple(s.hard, s.soft)); })
        .def("__eq__",
             [](const Score &s, const py::object &o) -> py::object
             {
                 std::int64_t h;
                 double v;
                 if (!pairOf(o, h, v))
                     return py::reinterpret_borrow<py::object>(Py_NotImplemented);
                 return py::bool_(compare(s, h, v) == 0);
             })
        .def("__lt__",
             [](const Score &s, const py::object &o) -> py::object
             {
                 std::int64_t h;
                 double v;
                 if (!pairOf(o, h, v))
                     return py::reinterpret_borrow<py::object>(Py_NotImplemented);
                 return py::bool_(compare(s, h, v) < 0);
             })
        .def("__le__",
             [](const Score &s, const py::object &o) -> py::object
             {
                 std::int64_t h;
                 double v;
                 if (!pairOf(o, h, v))
                     return py::reinterpret_borrow<py::object>(Py_NotImplemented);
                 return py::bool_(compare(s, h, v) <= 0);
             })
        .def("__gt__",
             [](const Score &s, const py::object &o) -> py::object
             {
                 std::int64_t h;
                 double v;
                 if (!pairOf(o, h, v))
                     return py::reinterpret_borrow<py::object>(Py_NotImplemented);
                 return py::bool_(compare(s, h, v) > 0);
             })
        .def("__ge__",
             [](const Score &s, const py::object &o) -> py::object
             {
                 std::int64_t h;
                 double v;
                 if (!pairOf(o, h, v))
                     return py::reinterpret_borrow<py::object>(Py_NotImplemented);
                 return py::bool_(compare(s, h, v) >= 0);
             });

    py::class_<TimeWindow>(m, "TimeWindow", py::module_local())
        .def(py::init([](Time start, Time end) { return TimeWindow{start, end}; }), py::arg("start"), py::arg("end"))
        .def_readonly("start", &TimeWindow::start)
        .def_readonly("end", &TimeWindow::end);

    py::class_<Machine>(m, "Machine", py::module_local())
        .def(py::init(
                 [](std::string id, std::string group, int batchCapacity, int minimumBatchSize,
                    std::vector<TimeWindow> unavailable)
                 {
                     Machine machine;
                     machine.id = std::move(id);
                     machine.group = std::move(group);
                     machine.batchCapacity = batchCapacity;
                     machine.minimumBatchSize = minimumBatchSize;
                     machine.unavailable = std::move(unavailable);
                     return machine;
                 }),
             py::arg("id"), py::arg("group"), py::arg("batch_capacity") = 1, py::arg("minimum_batch_size") = 1,
             py::arg("unavailable") = std::vector<TimeWindow>{})
        .def_readonly("id", &Machine::id)
        .def_readonly("group", &Machine::group)
        .def_readonly("batch_capacity", &Machine::batchCapacity)
        .def_readonly("minimum_batch_size", &Machine::minimumBatchSize)
        .def_readonly("unavailable", &Machine::unavailable);

    py::class_<AuxiliaryResource>(m, "AuxiliaryResource", py::module_local())
        .def(py::init([](std::string id, int capacity, std::vector<TimeWindow> unavailable)
                      { return AuxiliaryResource{std::move(id), capacity, std::move(unavailable)}; }),
             py::arg("id"), py::arg("capacity") = 1, py::arg("unavailable") = std::vector<TimeWindow>{})
        .def_readonly("id", &AuxiliaryResource::id)
        .def_readonly("capacity", &AuxiliaryResource::capacity)
        .def_readonly("unavailable", &AuxiliaryResource::unavailable);

    py::class_<MachineOption>(m, "MachineOption", py::module_local())
        .def(py::init([](int machine, Time duration, std::int64_t preferenceCost)
                      { return MachineOption{machine, duration, preferenceCost}; }),
             py::arg("machine"), py::arg("duration"), py::arg("preference_cost") = 0)
        .def_readonly("machine", &MachineOption::machine)
        .def_readonly("duration", &MachineOption::duration)
        .def_readonly("preference_cost", &MachineOption::preferenceCost);

    py::class_<Operation>(m, "Operation", py::module_local())
        .def(py::init(
                 [](std::string id, std::string family, std::vector<MachineOption> eligibleMachines,
                    std::vector<int> requiredResources, Time minQueueTime, std::optional<Time> maxQueueTime)
                 {
                     Operation operation;
                     operation.id = std::move(id);
                     operation.family = std::move(family);
                     operation.eligibleMachines = std::move(eligibleMachines);
                     operation.requiredResources = std::move(requiredResources);
                     operation.minQueueTime = minQueueTime;
                     operation.maxQueueTime = maxQueueTime;
                     return operation;
                 }),
             py::arg("id"), py::arg("family"), py::arg("eligible_machines"),
             py::arg("required_resources") = std::vector<int>{}, py::arg("min_queue_time") = 0,
             py::arg("max_queue_time") = py::none())
        .def_readonly("id", &Operation::id)
        .def_readonly("family", &Operation::family)
        .def_readonly("eligible_machines", &Operation::eligibleMachines)
        .def_readonly("required_resources", &Operation::requiredResources)
        .def_readonly("min_queue_time", &Operation::minQueueTime)
        .def_readonly("max_queue_time", &Operation::maxQueueTime);

    py::class_<Lot>(m, "Lot", py::module_local())
        .def(py::init(
                 [](std::string id, std::string product, Time releaseTime, Time dueTime, std::int64_t priority,
                    std::optional<Time> maxCycleTime, std::vector<Operation> operations)
                 {
                     Lot lot;
                     lot.id = std::move(id);
                     lot.product = std::move(product);
                     lot.releaseTime = releaseTime;
                     lot.dueTime = dueTime;
                     lot.priority = priority;
                     lot.maxCycleTime = maxCycleTime;
                     lot.operations = std::move(operations);
                     return lot;
                 }),
             py::arg("id"), py::arg("product"), py::arg("release_time"), py::arg("due_time"),
             py::arg("priority") = 1, py::arg("max_cycle_time") = py::none(), py::arg("operations"))
        .def_readonly("id", &Lot::id)
        .def_readonly("product", &Lot::product)
        .def_readonly("release_time", &Lot::releaseTime)
        .def_readonly("due_time", &Lot::dueTime)
        .def_readonly("priority", &Lot::priority)
        .def_readonly("max_cycle_time", &Lot::maxCycleTime)
        .def_readonly("operations", &Lot::operations);

    py::class_<SetupTime>(m, "SetupTime", py::module_local())
        .def(py::init([](std::string group, std::string from, std::string to, Time duration)
                      { return SetupTime{std::move(group), std::move(from), std::move(to), duration}; }),
             py::arg("machine_group"), py::arg("from_family"), py::arg("to_family"), py::arg("duration"))
        .def_readonly("machine_group", &SetupTime::machineGroup)
        .def_readonly("from_family", &SetupTime::fromFamily)
        .def_readonly("to_family", &SetupTime::toFamily)
        .def_readonly("duration", &SetupTime::duration);

    py::class_<TransportTime>(m, "TransportTime", py::module_local())
        .def(py::init([](std::string from, std::string to, Time duration)
                      { return TransportTime{std::move(from), std::move(to), duration}; }),
             py::arg("from_group"), py::arg("to_group"), py::arg("duration"))
        .def_readonly("from_group", &TransportTime::fromGroup)
        .def_readonly("to_group", &TransportTime::toGroup)
        .def_readonly("duration", &TransportTime::duration);

    py::class_<Assignment>(m, "Assignment", py::module_local())
        .def(py::init([](int lot, int operation, int machine, Time start, Time end, std::optional<std::string> batch)
                      { return Assignment{lot, operation, machine, start, end, std::move(batch)}; }),
             py::arg("lot"), py::arg("operation"), py::arg("machine"), py::arg("start"), py::arg("end"),
             py::arg("batch_id") = py::none())
        .def_readonly("lot", &Assignment::lot)
        .def_readonly("operation", &Assignment::operation)
        .def_readonly("machine", &Assignment::machine)
        .def_readonly("start", &Assignment::start)
        .def_readonly("end", &Assignment::end)
        .def_readonly("batch_id", &Assignment::batchId)
        .def("__repr__",
             [](const Assignment &a)
             {
                 return "<Assignment lot=" + std::to_string(a.lot) + " operation=" + std::to_string(a.operation)
                        + " machine=" + std::to_string(a.machine) + " " + std::to_string(a.start) + "-"
                        + std::to_string(a.end) + (a.batchId ? " batch=" + *a.batchId : std::string()) + ">";
             });

    py::class_<SemiconductorSchedule>(m, "SemiconductorSchedule", py::module_local())
        .def(py::init(
                 [](std::vector<Machine> machines, std::vector<AuxiliaryResource> resources,
                    std::vector<SetupTime> setupTimes, std::vector<TransportTime> transportTimes,
                    std::vector<Lot> lots, std::vector<Assignment> pinned, std::vector<Assignment> assignments,
                    bool warmStart)
                 {
                     SemiconductorSchedule schedule;
                     schedule.machines = std::move(machines);
                     schedule.auxiliaryResources = std::move(resources);
                     schedule.setupTimes = std::move(setupTimes);
                     schedule.transportTimes = std::move(transportTimes);
                     schedule.lots = std::move(lots);
                     schedule.pinnedAssignments = std::move(pinned);
                     schedule.assignments = std::move(assignments);
                     schedule.warmStart = warmStart;
                     ScoreCalculator::prepare(schedule);  // std::invalid_argument -> ValueError
                     return schedule;
                 }),
             py::arg("machines"), py::arg("auxiliary_resources") = std::vector<AuxiliaryResource>{},
             py::arg("setup_times") = std::vector<SetupTime>{},
             py::arg("transport_times") = std::vector<TransportTime>{}, py::arg("lots"),
             py::arg("pinned_assignments") = std::vector<Assignment>{},
             py::arg("assignments") = std::vector<Assignment>{}, py::arg("warm_start") = false)
        // Facts are read-only (copies): the schedule is validated once.
        .def_readonly("machines", &SemiconductorSchedule::machines)
        .def_readonly("auxiliary_resources", &SemiconductorSchedule::auxiliaryResources)
        .def_readonly("setup_times", &SemiconductorSchedule::setupTimes)
        .def_readonly("transport_times", &SemiconductorSchedule::transportTimes)
        .def_readonly("lots", &SemiconductorSchedule::lots)
        .def_readonly("pinned_assignments", &SemiconductorSchedule::pinnedAssignments)
        // A new assignment list is validated and clears the stored score.
        .def_property(
            "assignments", [](const SemiconductorSchedule &s) { return s.assignments; },
            [](SemiconductorSchedule &s, std::vector<Assignment> assignments)
            {
                ScoreCalculator::validateAssignments(s, assignments);
                s.assignments = std::move(assignments);
                s.scored = false;
            })
        .def_readwrite("warm_start", &SemiconductorSchedule::warmStart)
        .def_readonly("makespan", &SemiconductorSchedule::makespan)
        .def_readonly("total_weighted_tardiness", &SemiconductorSchedule::totalWeightedTardiness)
        .def_readonly("total_cycle_time", &SemiconductorSchedule::totalCycleTime)
        .def_readonly("score", &SemiconductorSchedule::score)
        .def_readonly("scored", &SemiconductorSchedule::scored)
        .def("row_count", &SemiconductorSchedule::rowCount)
        .def_readonly("max_option_count", &SemiconductorSchedule::maxOptionCount);

    m.def(
        "calculate_score",
        [](SemiconductorSchedule &schedule, const py::object &constraints)
        {
            ScoreCalculator::prepare(schedule);
            // The monolith knows the default picks only; a list (repeats and
            // offsets included) takes the assembly path.
            if (constraints.is_none())
                return ScoreCalculator::score(schedule);
            DecomposedCalculator calculator;
            tfpy::apply_constraints([] { return defaultConstraints(); }, constraints).into(calculator);
            calculator.rebuild(schedule);
            const Score score = calculator.score();
            storeScore(schedule, score);
            return score;
        },
        py::arg("schedule"), py::arg("constraints") = py::none(),
        "Scores schedule.assignments as given (None = the default constraints, else the picks, a name "
        "picked several times once per pick) and stores score, makespan, total_weighted_tardiness and "
        "total_cycle_time.");

    py::enum_<optimaru::AcceptanceRule>(m, "AcceptorType", py::module_local())
        .value("HILL_CLIMBING", optimaru::AcceptanceRule::HillClimbing)
        .value("LATE_ACCEPTANCE", optimaru::AcceptanceRule::LateAcceptance)
        .value("ENTITY_TABU", optimaru::AcceptanceRule::EntityTabu)
        .value("SIMULATED_ANNEALING", optimaru::AcceptanceRule::SimulatedAnnealing)
        .value("GREAT_DELUGE", optimaru::AcceptanceRule::GreatDeluge)
        .value("STEP_COUNTING_HILL_CLIMBING", optimaru::AcceptanceRule::StepCountingHillClimbing)
        .value("RECORD_TO_RECORD_TRAVEL", optimaru::AcceptanceRule::RecordToRecordTravel);

    optimaru::python::bind_config_copy(py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
        .def(py::init<>())
        .def_property(
            "spent_limit_seconds", [](const Solver::Config &c) { return std::chrono::duration<double>(c.time_limit).count(); },
            [](Solver::Config &c, double s)
            { c.time_limit = optimaru::python::seconds_to_millis(s, "spent_limit_seconds"); })
        .def_property(
            "unimproved_spent_limit_seconds",
            [](const Solver::Config &c) { return std::chrono::duration<double>(c.stall_time_limit).count(); },
            [](Solver::Config &c, double s)
            { c.stall_time_limit = optimaru::python::seconds_to_millis(s, "unimproved_spent_limit_seconds"); })
        .def_readwrite("step_count_limit", &Solver::Config::step_limit)
        .def_readwrite("unimproved_step_count_limit", &Solver::Config::stall_step_limit)
        .def_readwrite("random_seed", &Solver::Config::seed)
        .def_property(
            "parallel_solver_count", [](const Solver::Config &c) { return c.parallelSolverCount; },
            [](Solver::Config &c, int count)
            {
                if (count <= 0)
                    throw py::value_error("parallel_solver_count must be positive");
                c.parallelSolverCount = count;
            })
        .def_property(
            "full_assert",
            [](const Solver::Config &c) { return c.score_checks == optimaru::ScoreChecks::Full; },
            [](Solver::Config &c, bool on)
            { c.score_checks = on ? optimaru::ScoreChecks::Full : optimaru::ScoreChecks::Off; })
        .def_readwrite("acceptor_type", &Solver::Config::rule)
        // 0 = auto: clamp(16 * operation rows, 400, 3200).
        .def_property(
            "late_acceptance_size", [](const Solver::Config &c) { return c.history_length; },
            [](Solver::Config &c, std::int64_t size)
            {
                if (size < 0)
                    throw py::value_error("late_acceptance_size must be >= 0 (0 = auto)");
                c.history_length = static_cast<std::size_t>(size);
            })
        .def_property(
            "simulated_annealing_starting_temperature",
            [](const Solver::Config &c)
            {
                return py::make_tuple(c.start_temperature.hard,
                                      c.start_temperature.soft);
            },
            [](Solver::Config &c, std::pair<std::int64_t, double> hs)
            { c.start_temperature = Score{hs.first, hs.second}; })
        // SIMULATED_ANNEALING with a (0, 0) starting temperature: soft
        // temperature = this fraction of the start's |soft| (hard 0).
        .def_property(
            "simulated_annealing_temperature_fraction",
            [](const Solver::Config &c) { return c.simulatedAnnealingTemperatureFraction; },
            [](Solver::Config &c, double f)
            {
                if (!(f > 0.0))
                    throw py::value_error("simulated_annealing_temperature_fraction must be > 0");
                c.simulatedAnnealingTemperatureFraction = f;
            })
        .def_readwrite("change_move_weight", &Solver::Config::change_weight)
        .def_readwrite("swap_move_weight", &Solver::Config::swap_weight)
        .def_property(
            "machine_move_probability", [](const Solver::Config &c) { return c.machineMoveProbability; },
            [](Solver::Config &c, double p)
            {
                if (!(p >= 0.0 && p <= 1.0))
                    throw py::value_error("machine_move_probability must be in [0, 1]");
                c.machineMoveProbability = p;
            })
        .def_readwrite("priority_swap_moves", &Solver::Config::prioritySwapMoves)
        .def_readwrite("custom_move_probability", &Solver::Config::custom_move_probability)
        .def_readwrite("ruin_recreate", &Solver::Config::ruin_recreate)
        .def_readwrite("ruin_recreate_unimproved_step_trigger", &Solver::Config::ruinRecreateUnimprovedStepTrigger)
        .def_readwrite("ruin_recreate_min_ruin_fraction", &Solver::Config::ruinRecreateMinRuinFraction)
        .def_readwrite("ruin_recreate_max_ruin_fraction", &Solver::Config::ruinRecreateMaxRuinFraction)
        .def_readwrite("ruin_recreate_max_ruin_count", &Solver::Config::ruinRecreateMaxRuinCount)
        .def_readwrite("ruin_recreate_operator_decay", &Solver::Config::ruinRecreateOperatorDecay)
        .def_property(
            "ruin_recreate_rewards", [](const Solver::Config &c) { return c.ruinRecreateRewards; },
            [](Solver::Config &c, std::vector<double> rewards)
            {
                if (rewards.size() != 4)
                    throw py::value_error("ruin_recreate_rewards must contain exactly 4 values");
                c.ruinRecreateRewards = std::move(rewards);
            })
        .def_readwrite("ruin_recreate_polish_steps", &Solver::Config::ruinRecreatePolishSteps)
        // Early termination once bestScore >= (hard, soft); (0, -inf) stops
        // at the first feasible schedule.
        .def_property(
            "best_score_limit",
            [](const Solver::Config &c) { return py::make_tuple(c.target_score.hard, c.target_score.soft); },
            [](Solver::Config &c, std::pair<std::int64_t, double> hs) { c.target_score = Score{hs.first, hs.second}; })
        // on_new_best(step, score) on every new best; the wrapper re-acquires
        // the GIL, so it is safe from the worker threads too.
        .def_readwrite("on_new_best", &Solver::Config::on_new_best);

    py::class_<Solver>(m, "Solver", py::module_local())
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        .def("solve", py::overload_cast<SemiconductorSchedule &>(&Solver::solve), py::arg("schedule"),
             py::call_guard<py::gil_scoped_release>())
        // The same search scored by a caller-chosen constraint set (single
        // start). A name may be picked several times: every pick is its own
        // instance (a fresh copy of the default component, reading the
        // calculator's one shared schedule view) with its own knobs.
        .def(
            "solve_with_constraints",
            [](Solver &solver, SemiconductorSchedule &schedule, const py::object &constraints)
            {
                ScoreCalculator::prepare(schedule);
                DecomposedCalculator calculator;
                tfpy::apply_constraints([] { return defaultConstraints(); }, constraints).into(calculator);
                py::gil_scoped_release release;
                solver.solve(schedule, calculator);
            },
            py::arg("schedule"), py::arg("constraints") = py::none());

    // The raw costs of an assembly on schedule.assignments: one entry per
    // constraint (a global constraint reports its total as one element) —
    // the input semiconductor_scheduling.constraints.score() turns into a
    // score. None = the default assembly; a pick list scores exactly those
    // picks (apply_constraints), each entry carrying its pick's knobs, the
    // k-th pick of a name keyed "<name>#<k>".
    m.def(
        "constraint_costs",
        [](SemiconductorSchedule &schedule, const py::object &constraints)
        {
            ScoreCalculator::prepare(schedule);
            DecomposedCalculator calculator;
            tfpy::apply_constraints([] { return defaultConstraints(); }, constraints).into(calculator);
            return tfpy::constraint_costs(calculator, schedule);
        },
        py::arg("schedule"), py::arg("constraints") = py::none(),
        "Raw cost of every constraint on schedule.assignments, before weight/exponent/offset/level.");

    // The per-item lists (one item per lot, pair, machine, window, ...).
    m.def(
        "constraint_items",
        [](SemiconductorSchedule &schedule)
        {
            ScoreCalculator::prepare(schedule);
            RawItems items;
            ScoreCalculator::rawCosts(schedule, &items);
            const std::vector<std::string> names = tfpy::constraint_names(defaultConstraints());
            py::dict out;
            for (std::size_t t = 0; t < names.size(); ++t)
                out[py::str(names[t])] = py::cast(items[t]);
            return out;
        },
        py::arg("schedule"), "Per-item raw costs of every constraint on schedule.assignments (one list per constraint).");

    m.def("list_constraints", [] { return tfpy::constraint_names(defaultConstraints()); },
          "Constraint names available to semiconductor_scheduling.constraints.");

    tfpy::bind_constraint_info(m, [] { return defaultConstraints(); });

    // --- the decoder, for tests and tools ---------------------------------
    m.def(
        "default_choices",
        [](SemiconductorSchedule &schedule)
        {
            ScoreCalculator::prepare(schedule);
            return defaultChoices(schedule);
        },
        py::arg("schedule"), "Per operation row, the option with the least (preference cost, duration).");
    m.def(
        "seed_state",
        [](SemiconductorSchedule &schedule, const std::vector<int> &choices)
        {
            ScoreCalculator::prepare(schedule);
            if (static_cast<int>(choices.size()) != schedule.rowCount())
                throw py::value_error("seed_state needs one choice per operation row");
            std::vector<int> selected, ranks;
            seedState(schedule, choices, selected, ranks);
            return py::make_tuple(selected, ranks);
        },
        py::arg("schedule"), py::arg("choices"), "schedule.assignments as a (choices, ranks) search state.");
    m.def(
        "decode",
        [](SemiconductorSchedule &schedule, const std::vector<std::int64_t> &choices,
           const std::vector<std::int64_t> &ranks)
        {
            ScoreCalculator::prepare(schedule);
            if (static_cast<int>(choices.size()) != schedule.rowCount()
                || static_cast<int>(ranks.size()) != schedule.rowCount())
                throw py::value_error("decode needs one choice and one rank per operation row");
            std::vector<int> option(choices.size());
            for (int row = 0; row < schedule.rowCount(); ++row)
                option[static_cast<std::size_t>(row)] = optionOf(schedule, row, choices[static_cast<std::size_t>(row)]);
            return decode(schedule, option, ranks);
        },
        py::arg("schedule"), py::arg("choices"), py::arg("ranks"),
        "The serial schedule-generation decoder: choices (used modulo the option count) and ranks per row.");
}
