// pybind11 bindings for the box-sizing C++ domain.
// The Python extension links the bs::* implementations and imports the
// generic optimaru engine from core/optimaru; the engine itself is not
// modified here.
#include <pybind11/functional.h>  // std::function <-> Python callable (on_new_best)
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>  // std::vector / std::string / std::optional conversions

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bs/constraints.hpp"
#include "bs/model/domain.hpp"
#include "bs/score/score_calculator.hpp"
#include "bs/solver.hpp"
#include "optimaru/python/constraint_binding.hpp"
#include "optimaru/python/score_binding.hpp"

namespace py = pybind11;
using namespace bs;
namespace opy = optimaru::python;

namespace {

std::string quoted(const std::string& text) { return py::repr(py::str(text)).cast<std::string>(); }

std::string sizeText(std::int64_t length, std::int64_t width, std::int64_t height) {
    return "length=" + std::to_string(length) + ", width=" + std::to_string(width) +
           ", height=" + std::to_string(height);
}

std::string boxRepr(const Box& box) {
    return "Box(id=" + quoted(box.id) + ", " + sizeText(box.length, box.width, box.height) +
           ", max_weight=" + (box.maxWeight ? std::to_string(*box.maxWeight) : "None") +
           ", cost=" + py::repr(py::float_(box.cost)).cast<std::string>() +
           ", required=" + (box.required ? "True" : "False") + ")";
}

std::string parcelRepr(const Parcel& parcel) {
    return "Parcel(id=" + quoted(parcel.id) + ", " +
           sizeText(parcel.length, parcel.width, parcel.height) +
           ", weight=" + std::to_string(parcel.weight) +
           ", quantity=" + std::to_string(parcel.quantity) + ", box=" + std::to_string(parcel.box) +
           ")";
}

}  // namespace

PYBIND11_MODULE(_core, m) {
    m.doc() = "Box sizing — optimaru engine-backed C++ wrapper exposed to Python";

    opy::bind_score<Score>(m);

    py::class_<Box>(m, "Box", py::module_local())
            .def(py::init([](std::string id, std::int64_t length, std::int64_t width,
                             std::int64_t height, std::optional<std::int64_t> maxWeight,
                             double cost, bool required) {
                     Box box;
                     box.id = std::move(id);
                     box.length = length;
                     box.width = width;
                     box.height = height;
                     box.maxWeight = maxWeight;
                     box.cost = cost;
                     box.required = required;
                     return box;
                 }),
                 py::arg("id"), py::arg("length"), py::arg("width"), py::arg("height"),
                 py::arg("max_weight") = py::none(), py::arg("cost") = 0.0,
                 py::arg("required") = false)
            .def_readwrite("id", &Box::id)
            .def_readwrite("length", &Box::length)
            .def_readwrite("width", &Box::width)
            .def_readwrite("height", &Box::height)
            .def_readwrite("max_weight", &Box::maxWeight)
            .def_readwrite("cost", &Box::cost)
            .def_readwrite("required", &Box::required)
            .def("__repr__", &boxRepr);

    py::class_<Parcel>(m, "Parcel", py::module_local())
            .def(py::init([](std::string id, std::int64_t length, std::int64_t width,
                             std::int64_t height, std::int64_t weight, std::int64_t quantity,
                             int box) {
                     Parcel parcel;
                     parcel.id = std::move(id);
                     parcel.length = length;
                     parcel.width = width;
                     parcel.height = height;
                     parcel.weight = weight;
                     parcel.quantity = quantity;
                     parcel.box = box;
                     return parcel;
                 }),
                 py::arg("id"), py::arg("length"), py::arg("width"), py::arg("height"),
                 py::arg("weight") = 0, py::arg("quantity") = 1, py::arg("box") = -1)
            .def_readwrite("id", &Parcel::id)
            .def_readwrite("length", &Parcel::length)
            .def_readwrite("width", &Parcel::width)
            .def_readwrite("height", &Parcel::height)
            .def_readwrite("weight", &Parcel::weight)
            .def_readwrite("quantity", &Parcel::quantity)
            // The decision variable: index into problem.boxes, -1 == unassigned.
            .def_readwrite("box", &Parcel::box)
            .def("is_assigned", &Parcel::isAssigned)
            .def("__repr__", &parcelRepr);

    py::class_<BoxSizingProblem>(m, "BoxSizingProblem", py::module_local())
            .def(py::init([](std::vector<Box> boxes, std::vector<Parcel> parcels,
                             std::optional<int> maxBoxTypes, bool warmStart) {
                     BoxSizingProblem problem;
                     problem.boxes = std::move(boxes);
                     problem.parcels = std::move(parcels);
                     problem.maxBoxTypes = maxBoxTypes;
                     problem.warmStart = warmStart;
                     return problem;
                 }),
                 py::arg("boxes"), py::arg("parcels"), py::arg("max_box_types") = py::none(),
                 py::arg("warm_start") = false)
            // NOTE: element access returns copies; use set_assignment for the
            // parcels' boxes. Replacing a list re-validates on the next use.
            .def_property(
                    "boxes", [](const BoxSizingProblem& problem) { return problem.boxes; },
                    [](BoxSizingProblem& problem, std::vector<Box> boxes) {
                        problem.boxes = std::move(boxes);
                        problem.prepared = false;
                    })
            .def_property(
                    "parcels", [](const BoxSizingProblem& problem) { return problem.parcels; },
                    [](BoxSizingProblem& problem, std::vector<Parcel> parcels) {
                        problem.parcels = std::move(parcels);
                        problem.prepared = false;
                    })
            .def_readwrite("max_box_types", &BoxSizingProblem::maxBoxTypes)
            .def_readwrite("warm_start", &BoxSizingProblem::warmStart)
            // No weight fields: weights come only from constraint selections;
            // the defaults are the components' kDefaultWeight, reported by
            // constraint_info().
            .def_readonly("score", &BoxSizingProblem::score)
            .def("box_count", &BoxSizingProblem::boxCount)
            .def("parcel_count", &BoxSizingProblem::parcelCount)
            // Summary getters.
            .def("box_types_used", &BoxSizingProblem::boxTypesUsed)
            .def("void_volume", &BoxSizingProblem::voidVolume)
            .def("fill_ratio", &BoxSizingProblem::fillRatio)
            .def("parcels_per_box", &BoxSizingProblem::parcelsPerBox)
            .def("assignment", &BoxSizingProblem::assignment)
            .def(
                    "set_assignment",
                    [](BoxSizingProblem& problem, const std::vector<int>& boxes) {
                        if (boxes.size() != problem.parcels.size())
                            throw py::value_error("assignment length must equal the parcel count");
                        for (std::size_t p = 0; p < boxes.size(); ++p)
                            problem.parcels[p].box = boxes[p];
                    },
                    py::arg("assignment"));

    m.def(
            "calculate_score",
            [](BoxSizingProblem& problem, const py::object& constraints) {
                if (constraints.is_none()) return ScoreCalculator::score(problem);
                ScoreCalculator::prepare(problem);
                DecomposedCalculator calculator;
                opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                        .into(calculator);
                calculator.rebuild(problem);
                problem.score = calculator.score();
                return problem.score;
            },
            py::arg("problem"), py::arg("constraints") = py::none(),
            "Full recompute of the score (also stores problem.score).");

    optimaru::python::bind_solver_config(
            py::class_<Solver::Config>(m, "SolverConfig", py::module_local()))
            .def(py::init<>())
            .def_readwrite("change_move_weight", &Solver::Config::change_weight)
            .def_readwrite("swap_move_weight", &Solver::Config::swap_weight)
            // Gate on how often a step may draw one of the moves on the set
            // of sizes; inside it they and the move mix take turns evenly.
            // 1 = unrestricted.
            .def_readwrite("custom_move_probability", &Solver::Config::custom_move_probability)
            // The moves on the set of sizes (see cpp/bs/search/moves/).
            .def_readwrite("close_box_moves", &Solver::Config::close_box_moves)
            .def_readwrite("open_box_moves", &Solver::Config::open_box_moves)
            .def_readwrite("swap_box_moves", &Solver::Config::swap_box_moves)
            // Late acceptance that starts over from just below its best when
            // it stalls (see cpp/bs/accept/reheating_late_acceptance.hpp).
            .def_readwrite("reheat", &Solver::Config::reheat);

    py::class_<Solver>(m, "Solver", py::module_local())
            .def(py::init<>())
            .def(py::init<Solver::Config>(), py::arg("config"))
            // Solves in place: sets parcel.box and problem.score. The GIL is
            // released so parallel multi-start workers can fire on_new_best.
            .def("solve", py::overload_cast<BoxSizingProblem&>(&Solver::solve), py::arg("problem"),
                 py::call_guard<py::gil_scoped_release>())
            // Same solve, scored by a caller-chosen constraint set (see
            // optimaru.box_sizing.constraints). The picks replace the defaults;
            // None keeps them. A name may be picked several times: every pick
            // is its own instance with its own knobs.
            .def(
                    "solve_with_constraints",
                    [](Solver& solver, BoxSizingProblem& problem, const py::object& constraints) {
                        ScoreCalculator::prepare(problem);
                        DecomposedCalculator calculator;
                        opy::apply_constraints([] { return defaultConstraints(); }, constraints)
                                .into(calculator);
                        py::gil_scoped_release release;
                        solver.solve(problem, calculator);
                    },
                    py::arg("problem"), py::arg("constraints") = py::none());

    // constraint_costs(), list_constraints() and constraint_info(), off the
    // default assembly.
    opy::bind_constraints<BoxSizingProblem>(
            m, "problem", [] { return defaultConstraints(); }, &ScoreCalculator::prepare);
}
