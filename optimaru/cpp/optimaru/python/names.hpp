// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_PYTHON_NAMES_HPP
#define OPTIMARU_PYTHON_NAMES_HPP

// The Python-facing spelling of a constraint name, kept free of pybind11 so
// the engine tests can check it (constraint_binding.hpp includes it).

#include <cctype>
#include <map>
#include <stdexcept>
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
inline std::string python_name(const std::string& constraint_name) {
    std::string out;
    for (const char c : constraint_name) {
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
// instances' Python names in objective order: the Python name itself, and
// "<name>#<k>" for the k-th instance (k >= 2) of a name picked more than
// once (apply_constraints, employee-scheduling's rule factory). The Python
// base (_constraint.py instance_keys) derives the same keys from a selection
// list, and the CP-SAT references (cpsat-common resolve()) number repeated
// picks the same way, so they agree as long as they see the same list in the
// same order. A name that occurs once keeps its plain key. A name holding
// '#' would make the keys ambiguous ("a#2" is also the second "a"), so it is
// refused with std::invalid_argument (ValueError in Python), in the words of
// the Python base (_constraint.py Constraint.__init_subclass__);
// ConstraintAssembly refuses it where a component is added.
inline std::vector<std::string> instance_keys(const std::vector<std::string>& python_names) {
    std::map<std::string, int> seen;
    std::vector<std::string> keys;
    keys.reserve(python_names.size());
    for (const std::string& name : python_names) {
        if (name.find('#') != std::string::npos) {
            throw std::invalid_argument("constraint '" + name +
                                        "': a name must not contain '#', which constraint_costs() "
                                        "keys repeated picks with ('<name>#<k>')");
        }
        const int count = ++seen[name];
        keys.push_back(count == 1 ? name : name + "#" + std::to_string(count));
    }
    return keys;
}

}  // namespace python
}  // namespace optimaru

#endif  // OPTIMARU_PYTHON_NAMES_HPP
