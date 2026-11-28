#include "cl/model/demo_data.hpp"

#include <array>
#include <stdexcept>
#include <string>

#include "optimaru/utils/random.hpp"

namespace cl {

namespace {

struct BoxKind {
    const char* name;
    int length;  // mm
    int width;
    int height;
    std::int64_t weight;  // g
    Rotation rotations;
    std::int64_t maxLoadOnTop;  // g; -1 = no limit
};

constexpr std::array<BoxKind, 6> kCatalog = {{
        {"crate", 600, 400, 300, 12000, Rotation::Upright, -1},
        {"carton-l", 500, 400, 400, 9000, Rotation::All, -1},
        {"carton-m", 400, 300, 300, 6000, Rotation::All, -1},
        {"carton-s", 300, 200, 200, 2500, Rotation::All, -1},
        {"tray", 600, 400, 150, 5000, Rotation::Upright, -1},
        {"fragile", 400, 300, 250, 3000, Rotation::Upright, 5000},
}};

}  // namespace

ContainerLoadingProblem generateDemoData(int itemCount, int containerCount, std::int64_t seed) {
    if (itemCount < 0) throw std::invalid_argument("item_count must be >= 0");
    if (containerCount < 0) throw std::invalid_argument("container_count must be >= 0");
    // Seeded, platform-independent draws (see optimaru/utils/random.hpp).
    optimaru::PortableRandom random(static_cast<std::uint64_t>(seed));
    ContainerLoadingProblem problem;
    for (int c = 0; c < containerCount; ++c) {
        Container pallet;
        pallet.id = "pallet-" + std::to_string(c + 1);
        pallet.length = 1200;
        pallet.width = 800;
        pallet.height = 1500;
        pallet.maxWeight = 600000;
        problem.containers.push_back(pallet);
    }
    for (int i = 0; i < itemCount; ++i) {
        const BoxKind& kind = kCatalog[static_cast<std::size_t>(
                random.next_int(static_cast<int>(kCatalog.size())))];
        Item item;
        item.id = std::string(kind.name) + "-" + std::to_string(i + 1);
        item.length = kind.length;
        item.width = kind.width;
        item.height = kind.height;
        item.weight = kind.weight;
        item.rotations = kind.rotations;
        if (kind.maxLoadOnTop >= 0) item.maxLoadOnTop = kind.maxLoadOnTop;
        item.group = "store-" + std::to_string(i % 3 + 1);
        problem.items.push_back(item);
    }
    return problem;
}

}  // namespace cl
