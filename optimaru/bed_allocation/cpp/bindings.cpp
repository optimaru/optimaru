// pybind11 bindings for the bed-allocation C++ domain wrappers.
// The Python extension links the existing ba::* implementations and imports
// the generic optimaru engine from ../../../include; the engine itself
// is not modified here. One domain is exposed: the quickstart port
// (ba::BedPlan, BASIC bed variable with a "no bed" sentinel for
// allowsUnassigned semantics), as the module optimaru.bed_allocation._core.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::map / std::optional / std::string

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ba/constraints.hpp"
#include "ba/domain.hpp"
#include "ba/score_calculator.hpp"
#include "ba/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"

namespace py = pybind11;
using namespace ba;
namespace tfpy = optimaru::python;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "Bed allocation (hospital bed scheduling) — optimaru "
              "engine-backed C++ wrapper exposed to Python";

    py::class_<Score>(m, "Score", py::module_local())
        .def_readonly("hard", &Score::hard)
        .def_readonly("medium", &Score::medium)
        .def_readonly("soft", &Score::soft)
        .def("is_feasible", &Score::feasible)
        .def("__str__", &Score::str)
        .def("__repr__",
             [](const Score &s) { return "<Score " + s.str() + ">"; });

    py::enum_<Gender>(m, "Gender", py::module_local())
        .value("MALE", Gender::MALE)
        .value("FEMALE", Gender::FEMALE);

    py::enum_<GenderLimitation>(m, "GenderLimitation", py::module_local())
        .value("ANY_GENDER", GenderLimitation::ANY_GENDER)
        .value("MALE_ONLY", GenderLimitation::MALE_ONLY)
        .value("FEMALE_ONLY", GenderLimitation::FEMALE_ONLY)
        .value("SAME_GENDER", GenderLimitation::SAME_GENDER);

    py::class_<Department>(m, "Department", py::module_local())
        .def(py::init(
                 [](std::string id,
                    std::string name,
                    std::map<std::string, int> specialtyToPriority,
                    std::optional<int> minimumAge,
                    std::optional<int> maximumAge)
                 {
                     Department d;
                     d.id = std::move(id);
                     d.name = std::move(name);
                     d.specialtyToPriority = std::move(specialtyToPriority);
                     d.minimumAge = minimumAge;
                     d.maximumAge = maximumAge;
                     return d;
                 }),
             py::arg("id"),
             py::arg("name"),
             py::arg("specialty_to_priority") = std::map<std::string, int>{},
             py::arg("minimum_age") = std::nullopt,
             py::arg("maximum_age") = std::nullopt)
        .def_readwrite("id", &Department::id)
        .def_readwrite("name", &Department::name)
        .def_readwrite("specialty_to_priority",
                       &Department::specialtyToPriority)
        .def_readwrite("minimum_age", &Department::minimumAge)
        .def_readwrite("maximum_age", &Department::maximumAge);

    py::class_<Room>(m, "Room", py::module_local())
        .def(py::init(
                 [](std::string id,
                    std::string name,
                    int department,
                    int capacity,
                    GenderLimitation genderLimitation,
                    std::vector<std::string> equipment)
                 {
                     Room r;
                     r.id = std::move(id);
                     r.name = std::move(name);
                     r.department = department;
                     r.capacity = capacity;
                     r.genderLimitation = genderLimitation;
                     r.equipment = std::move(equipment);
                     return r;
                 }),
             py::arg("id"),
             py::arg("name"),
             py::arg("department") = 0,
             py::arg("capacity") = 1,
             py::arg("gender_limitation") = GenderLimitation::ANY_GENDER,
             py::arg("equipment") = std::vector<std::string>{})
        .def_readwrite("id", &Room::id)
        .def_readwrite("name", &Room::name)
        // Index into BedPlan.departments.
        .def_readwrite("department", &Room::department)
        .def_readwrite("capacity", &Room::capacity)
        .def_readwrite("gender_limitation", &Room::genderLimitation)
        .def_readwrite("equipment", &Room::equipment);

    py::class_<Bed>(m, "Bed", py::module_local())
        .def(py::init(
                 [](std::string id, int room, int indexInRoom)
                 {
                     Bed b;
                     b.id = std::move(id);
                     b.room = room;
                     b.indexInRoom = indexInRoom;
                     return b;
                 }),
             py::arg("id"),
             py::arg("room"),
             py::arg("index_in_room") = 0)
        .def_readwrite("id", &Bed::id)
        // Index into BedPlan.rooms.
        .def_readwrite("room", &Bed::room)
        .def_readwrite("index_in_room", &Bed::indexInRoom);

    py::class_<Stay>(m, "Stay", py::module_local())
        .def(py::init(
                 [](std::string id,
                    std::string patientName,
                    Gender patientGender,
                    int patientAge,
                    int arrivalDay,
                    int departureDay,
                    std::string specialty,
                    std::optional<int> patientPreferredMaximumRoomCapacity,
                    std::vector<std::string> patientRequiredEquipment,
                    std::vector<std::string> patientPreferredEquipment,
                    int bed)
                 {
                     Stay s;
                     s.id = std::move(id);
                     s.patientName = std::move(patientName);
                     s.patientGender = patientGender;
                     s.patientAge = patientAge;
                     s.arrivalDay = arrivalDay;
                     s.departureDay = departureDay;
                     s.specialty = std::move(specialty);
                     s.patientPreferredMaximumRoomCapacity
                         = patientPreferredMaximumRoomCapacity;
                     s.patientRequiredEquipment
                         = std::move(patientRequiredEquipment);
                     s.patientPreferredEquipment
                         = std::move(patientPreferredEquipment);
                     s.bed = bed;
                     return s;
                 }),
             py::arg("id"),
             py::arg("patient_name"),
             py::arg("patient_gender"),
             py::arg("patient_age"),
             py::arg("arrival_day"),
             py::arg("departure_day"),
             py::arg("specialty") = "",
             py::arg("patient_preferred_maximum_room_capacity") = std::nullopt,
             py::arg("patient_required_equipment") = std::vector<std::string>{},
             py::arg("patient_preferred_equipment")
             = std::vector<std::string>{},
             py::arg("bed") = -1)
        .def_readwrite("id", &Stay::id)
        .def_readwrite("patient_name", &Stay::patientName)
        .def_readwrite("patient_gender", &Stay::patientGender)
        .def_readwrite("patient_age", &Stay::patientAge)
        .def_readwrite("patient_preferred_maximum_room_capacity",
                       &Stay::patientPreferredMaximumRoomCapacity)
        .def_readwrite("patient_required_equipment",
                       &Stay::patientRequiredEquipment)
        .def_readwrite("patient_preferred_equipment",
                       &Stay::patientPreferredEquipment)
        .def_readwrite("arrival_day", &Stay::arrivalDay)
        .def_readwrite("departure_day", &Stay::departureDay)
        .def_readwrite("specialty", &Stay::specialty)
        // The planning variable: -1 uninitialized, [0, bed_count) a real
        // bed, bed_count (== BedPlan.no_bed_value()) the "no bed" sentinel.
        .def_readwrite("bed", &Stay::bed)
        .def("night_count", &Stay::nightCount)
        .def("same_night_count", &Stay::sameNightCount, py::arg("other"));

    py::class_<BedPlan>(m, "BedPlan", py::module_local())
        .def(py::init(
                 [](std::vector<Department> departments,
                    std::vector<Room> rooms,
                    std::vector<Bed> beds,
                    std::vector<Stay> stays)
                 {
                     BedPlan plan;
                     plan.departments = std::move(departments);
                     plan.rooms = std::move(rooms);
                     plan.beds = std::move(beds);
                     plan.stays = std::move(stays);
                     return plan;
                 }),
             py::arg("departments"),
             py::arg("rooms"),
             py::arg("beds"),
             py::arg("stays"))
        .def_readonly("departments", &BedPlan::departments)
        .def_readonly("rooms", &BedPlan::rooms)
        .def_readonly("beds", &BedPlan::beds)
        // NOTE: element access returns copies; use set_assignment for bulk
        // bed writes.
        .def_readonly("stays", &BedPlan::stays)
        .def_readonly("score", &BedPlan::score)
        .def("bed_count", &BedPlan::bedCount)
        .def("no_bed_value",
             &BedPlan::noBedValue,
             "The \"no bed\" sentinel planning value (== bed_count()); the "
             "quickstart's null bed, penalized per night on the medium "
             "level (unassigned_stays_cost).")
        // Convenience: the current bed value per stay (the solution).
        .def("assignment",
             [](const BedPlan &plan)
             {
                 std::vector<int> assignment;
                 assignment.reserve(plan.stays.size());
                 for (const Stay &stay : plan.stays)
                     assignment.push_back(stay.bed);
                 return assignment;
             })
        // Bulk write of the planning variable (stay access via .stays
        // returns copies, so per-stay writes would be lost).
        .def(
            "set_assignment",
            [](BedPlan &plan, const std::vector<int> &assignment)
            {
                if (assignment.size() != plan.stays.size())
                    throw py::value_error(
                        "assignment length must equal the stay count");
                for (std::size_t i = 0; i < assignment.size(); ++i)
                    plan.stays[i].bed = assignment[i];
            },
            py::arg("assignment"));

    m.def("calculate_score",
          &ScoreCalculator::score,
          py::arg("plan"),
          "Full recompute of the HardMediumSoft score (also stores "
          "plan.score).");

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
        // 0 = auto: clamp(8 * stay count, 8000, 12000).
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
        // Nearby selection: two stays are close when their date windows are.
        // 0 disables it.
        .def_readwrite("nearby_probability", &Solver::Config::nearbyProbability)
        .def_readwrite("nearby_size", &Solver::Config::nearbySize)
        // Early termination once bestScore >= (hard, medium, soft), e.g.
        // (0, -2**63, -2**63) stops at the first feasible solution: INT64_MIN
        // on medium or soft is "no floor" (toEngineScoreLimit, ba/domain.hpp).
        .def_property(
            "best_score_limit",
            [](const Solver::Config &c)
            {
                return py::make_tuple(c.target_score.hard,
                                      c.target_score.medium,
                                      c.target_score.soft);
            },
            [](Solver::Config &c,
               std::tuple<std::int64_t, std::int64_t, std::int64_t> hms)
            {
                c.target_score = Score{
                    std::get<0>(hms), std::get<1>(hms), std::get<2>(hms)};
            })
        // Callback fired on every new best solution: on_new_best(step, score).
        // The pybind11 wrapper re-acquires the GIL, so it is safe from the
        // engine's multi-start worker threads too.
        .def_readwrite("on_new_best", &Solver::Config::on_new_best);

    py::class_<Solver>(m, "Solver", py::module_local())
        .def(py::init<>())
        .def(py::init<Solver::Config>(), py::arg("config"))
        // Solves in place: sets stay.bed and plan.score. The GIL is released
        // so parallel multi-start workers can fire on_new_best.
        .def("solve",
             py::overload_cast<BedPlan &>(&Solver::solve),
             py::arg("plan"),
             py::call_guard<py::gil_scoped_release>())
        // Same solve, scored by a caller-chosen constraint set (see
        // optimaru.bed_allocation.constraints). The picks replace the defaults;
        // None keeps them. A name may be picked several times: every pick is
        // its own instance (a fresh copy of the default component) with its
        // own knobs.
        .def(
            "solve_with_constraints",
            [](Solver &solver, BedPlan &plan, const py::object &constraints)
            {
                ScoreCalculator::prepare(plan);
                DecomposedCalculator calculator;
                tfpy::apply_constraints([] { return defaultConstraints(); },
                                       constraints)
                    .into(calculator);
                py::gil_scoped_release release;
                solver.solve(plan, calculator);
            },
            py::arg("plan"),
            py::arg("constraints") = py::none());

    // The raw per-unit costs of an assembly on `plan` — the input
    // optimaru.bed_allocation.constraints.score() turns into a score. Scoring once is
    // enough: re-pricing any assembly is pure Python arithmetic on these.
    // Units are the planning values (beds in order, then the "no bed"
    // sentinel); a global constraint has one entry. "units" lists what a unit
    // constraint evaluates: the real beds for the per-bed rules, the sentinel
    // for unassigned_stays_cost. Every entry carries the domain's medium fold (into_soft,
    // 10^9), which score() applies. None = the default assembly; a pick list
    // scores exactly those picks (apply_constraints), each entry carrying its
    // pick's knobs, the k-th pick of a name keyed "<name>#<k>".
    m.def(
        "constraint_costs",
        [](BedPlan &plan, const py::object &constraints)
        {
            ScoreCalculator::prepare(plan);
            DecomposedCalculator calculator;
            tfpy::apply_constraints([] { return defaultConstraints(); },
                                   constraints)
                .into(calculator);
            return tfpy::constraint_costs(calculator, plan);
        },
        py::arg("plan"),
        py::arg("constraints") = py::none(),
        "Raw per-unit cost of every constraint, before weight/exponent/offset/level.");

    // The constraint names this domain scores, in default-assembly order
    // (unit constraints first, then the global mixed_gender_room_cost) — the
    // same strings a score breakdown reports.
    m.def(
        "list_constraints",
        [] { return tfpy::constraint_names(defaultConstraints()); },
        "Constraint names available to optimaru.bed_allocation.constraints.");

    // The constraint catalog, read off the default assembly: the components'
    // kDefaultWeight / kDefaultLevel are the only place a default lives.
    tfpy::bind_constraint_info(m, [] { return defaultConstraints(); });
}
