#ifndef OPTIMARU_PYTHON_NAMES_HPP
#define OPTIMARU_PYTHON_NAMES_HPP

// The Python-facing spelling of a constraint name, kept free of pybind11 so
// the engine tests can check it (constraint_binding.hpp includes it).

#include <cctype>
#include <string>

namespace optimaru {
namespace python {

// The C++ components carry their registry name: the struct name with a
// lower-case first letter ("facilityCapacityCost"). The Python packages are
// snake_case, so derive one from the other MECHANICALLY rather than
// hand-writing an alias table that would drift the first time a component is
// added or renamed:
//
//   facilityCapacityCost -> facility_capacity_cost
//   vehicleCapacityCost  -> vehicle_capacity_cost
//
// Spaces and dashes (the pre-0.2.0 registry spelling, "facility setup cost")
// map to underscores too.
//
// One shape it does not handle: a run of capitals is broken up letter by
// letter ("HTTPTag" -> "h_t_t_p_tag"), because the rule cannot tell an
// acronym from adjacent words without a dictionary. Spell an acronym in the
// C++ name the way it should read in Python ("httpTagCost").
inline std::string pythonName(const std::string& constraintName) {
    std::string out;
    for (const char c : constraintName) {
        if (c == ' ' || c == '-') {
            if (!out.empty() && out.back() != '_') out += '_';
        } else if (std::isupper(static_cast<unsigned char>(c))) {
            if (!out.empty() && out.back() != '_') out += '_';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace python
}  // namespace optimaru

#endif  // OPTIMARU_PYTHON_NAMES_HPP
