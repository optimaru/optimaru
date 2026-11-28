#ifndef CL_DEMO_DATA_HPP
#define CL_DEMO_DATA_HPP

#include <cstdint>

#include "cl/model/domain.hpp"

namespace cl {

// The demo: boxes of a small catalog onto pallets. Each pallet is 1200 x 800
// mm with 1500 mm of loading height and carries 600 kg; the boxes are six
// kinds of crate and carton (mm and g), one kind fragile (it carries 5 kg),
// drawn from a seeded std::mt19937_64, so the instance is the same on every
// platform. The boxes go to three stores in turn (the groups "store-1" to
// "store-3").
//
// Throws std::invalid_argument for a negative count.
ContainerLoadingProblem generateDemoData(int itemCount = 40, int containerCount = 4,
                                         std::int64_t seed = 37);

}  // namespace cl

#endif  // CL_DEMO_DATA_HPP
