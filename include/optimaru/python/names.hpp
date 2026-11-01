#ifndef OPTIMARU_PYTHON_NAMES_HPP
#define OPTIMARU_PYTHON_NAMES_HPP

// The Python-facing spelling of a constraint name, kept free of pybind11 so
// the engine tests can check it (constraint_binding.hpp includes it).

#include <cctype>
#include <map>
#include <string>
#include <vector>

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
// One shape it does not handle: a run of capitals is broken up letter by
// letter ("HTTPTag" -> "h_t_t_p_tag"), because the rule cannot tell an
// acronym from adjacent words without a dictionary. Spell an acronym in the
// C++ name the way it should read in Python ("httpTagCost").
inline std::string pythonName(const std::string& constraintName) {
    std::string out;
    for (const char c : constraintName) {
        if (std::isupper(static_cast<unsigned char>(c))) {
            if (!out.empty() && out.back() != '_') out += '_';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    return out;
}

// The key of each constraint instance in `constraint_costs()`, given the
// instances' Python names in calculator order: the Python name itself, and
// "<name>#<k>" for the k-th instance (k >= 2) of a name picked more than
// once (applyConstraints, employee-scheduling's rule factory). The Python
// base (_constraint.py instance_keys) derives the same keys from a selection
// list, and the CP-SAT references (cpsat-common resolve()) number repeated
// picks the same way, so they agree as long as they see the same list in the
// same order. A name that occurs once keeps its plain key.
inline std::vector<std::string> instanceKeys(const std::vector<std::string>& pythonNames) {
    std::map<std::string, int> seen;
    std::vector<std::string> keys;
    keys.reserve(pythonNames.size());
    for (const std::string& name : pythonNames) {
        const int count = ++seen[name];
        keys.push_back(count == 1 ? name : name + "#" + std::to_string(count));
    }
    return keys;
}

}  // namespace python
}  // namespace optimaru

#endif  // OPTIMARU_PYTHON_NAMES_HPP
