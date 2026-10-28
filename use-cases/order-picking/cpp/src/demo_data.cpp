#include "op/demo_data.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace op
{

namespace
{

// ---------------------------------------------------------------------------
// java.util.Random (48-bit LCG), bit-exact — the demo instance depends on the
// exact stream of new Random(37), so the Java generator's nextInt calls are
// replayed verbatim.
// ---------------------------------------------------------------------------
class JavaRandom
{
public:
    explicit JavaRandom(std::int64_t seed)
        : seed_((static_cast<std::uint64_t>(seed) ^ MULTIPLIER) & MASK)
    {
    }

    // java.util.Random.nextInt(int bound), bound > 0.
    int nextInt(int bound)
    {
        if ((bound & -bound) == bound)  // bound is a power of 2
        {
            return static_cast<int>(
                (static_cast<std::int64_t>(bound) * next(31)) >> 31);
        }
        int bits;
        int val;
        do
        {
            bits = next(31);
            val = bits % bound;
            // Java's `bits - val + (bound - 1) < 0` relies on 32-bit wrap.
        } while (
            static_cast<std::int32_t>(static_cast<std::uint32_t>(bits)
                                      - static_cast<std::uint32_t>(val)
                                      + static_cast<std::uint32_t>(bound - 1))
            < 0);
        return val;
    }

private:
    static constexpr std::uint64_t MULTIPLIER = 0x5DEECE66DULL;
    static constexpr std::uint64_t MASK = (1ULL << 48) - 1;

    int next(int bits)
    {
        seed_ = (seed_ * MULTIPLIER + 0xBULL) & MASK;
        return static_cast<int>(seed_ >> (48 - bits));
    }

    std::uint64_t seed_;
};

// ---------------------------------------------------------------------------
// The DemoDataGenerator catalog: PRODUCTS with their ProductFamily, in
// declaration order (product ids "0".."36" follow that order), and the
// family -> candidate shelvings map (column 0..4 = A..E, row 0..2 = 1..3, in
// the Java list order — the order matters, buildProducts draws an index).
// ---------------------------------------------------------------------------
enum class ProductFamily
{
    GENERAL_FOOD,
    FRESH_FOOD,
    MEET_AND_FISH,
    FROZEN_PRODUCTS,
    FRUITS_AND_VEGETABLES,
    HOUSE_CLEANING,
    DRINKS,
    SNACKS,
    PETS
};

struct CatalogProduct
{
    const char *name;
    int volume;  // cm3
    ProductFamily family;
};

constexpr std::array<CatalogProduct, 37> CATALOG = {{
    {"Kelloggs Cornflakes", 30 * 12 * 35, ProductFamily::GENERAL_FOOD},
    {"Cream Crackers", 23 * 7 * 2, ProductFamily::GENERAL_FOOD},
    {"Tea Bags 240 packet", 2 * 6 * 15, ProductFamily::GENERAL_FOOD},
    {"Tomato Soup Can", 10 * 10 * 10, ProductFamily::GENERAL_FOOD},
    {"Baked Beans in Tomato Sauce", 10 * 10 * 10, ProductFamily::GENERAL_FOOD},
    {"Classic Mint Sauce", 8 * 10 * 8, ProductFamily::GENERAL_FOOD},
    {"Raspberry Conserve", 8 * 10 * 8, ProductFamily::GENERAL_FOOD},
    {"Orange Fine Shred Marmalade", 7 * 8 * 7, ProductFamily::GENERAL_FOOD},
    {"Free Range Eggs 6 Pack", 15 * 10 * 8, ProductFamily::FRESH_FOOD},
    {"Mature Cheddar 400G", 10 * 9 * 5, ProductFamily::FRESH_FOOD},
    {"Butter Packet", 12 * 5 * 5, ProductFamily::FRESH_FOOD},
    {"Iceberg Lettuce Each", 2500, ProductFamily::FRUITS_AND_VEGETABLES},
    {"Carrots 1Kg", 1000, ProductFamily::FRUITS_AND_VEGETABLES},
    {"Organic Fair Trade Bananas 5 Pack",
     1800,
     ProductFamily::FRUITS_AND_VEGETABLES},
    {"Gala Apple Minimum 5 Pack",
     25 * 20 * 10,
     ProductFamily::FRUITS_AND_VEGETABLES},
    {"Orange Bag 3kg", 29 * 20 * 15, ProductFamily::FRUITS_AND_VEGETABLES},
    {"Fairy Non Biological Laundry Liquid 4.55L",
     5000,
     ProductFamily::HOUSE_CLEANING},
    {"Toilet Tissue 8 Roll White", 50 * 20 * 20, ProductFamily::HOUSE_CLEANING},
    {"Kitchen Roll 200 Sheets x 2",
     30 * 30 * 15,
     ProductFamily::HOUSE_CLEANING},
    {"Stainless Steel Cleaner 500Ml", 500, ProductFamily::HOUSE_CLEANING},
    {"Antibacterial Surface Spray", 12 * 4 * 25, ProductFamily::HOUSE_CLEANING},
    {"Beef Lean Steak Mince 500g", 500, ProductFamily::MEET_AND_FISH},
    {"Smoked Salmon 120G", 150, ProductFamily::MEET_AND_FISH},
    {"Steak Burgers 454G", 450, ProductFamily::MEET_AND_FISH},
    {"Pork Cooked Ham 125G", 125, ProductFamily::MEET_AND_FISH},
    {"Chicken Breast Fillets 300G", 300, ProductFamily::MEET_AND_FISH},
    {"6 Milk Bricks Pack", 22 * 16 * 21, ProductFamily::DRINKS},
    {"Milk Brick", 1232, ProductFamily::DRINKS},
    {"Skimmed Milk 2.5L", 2500, ProductFamily::DRINKS},
    {"3L Orange Juice", 3 * 1000, ProductFamily::DRINKS},
    {"Alcohol Free Beer 4 Pack", 30 * 15 * 30, ProductFamily::DRINKS},
    {"Pepsi Regular Bottle", 1000, ProductFamily::DRINKS},
    {"Pepsi Diet 6 x 330ml", 35 * 12 * 12, ProductFamily::DRINKS},
    {"Schweppes Lemonade 2L", 2000, ProductFamily::DRINKS},
    {"Coke Zero 8 x 330ml", 40 * 12 * 12, ProductFamily::DRINKS},
    {"Natural Mineral Water Still 6 X 1.5Ltr", 6 * 1500, ProductFamily::DRINKS},
    {"Cocktail Crisps 6 Pack", 20 * 10 * 10, ProductFamily::SNACKS},
}};

struct Shelf
{
    int column;
    int row;
};

// SHELVINGS_PER_FAMILY, in the Java list order per family.
const std::vector<Shelf> &shelvingsFor(ProductFamily family)
{
    static const std::vector<Shelf> FRUITS = {{0, 0}, {0, 1}};  // (A,1) (A,2)
    static const std::vector<Shelf> FRESH = {{0, 2}};           // (A,3)
    static const std::vector<Shelf> MEET = {{1, 1}, {1, 2}};    // (B,2) (B,3)
    static const std::vector<Shelf> FROZEN = {{1, 1}, {1, 0}};  // (B,2) (B,1)
    static const std::vector<Shelf> DRINKS = {{3, 0}};          // (D,1)
    static const std::vector<Shelf> SNACKS = {{3, 1}};          // (D,2)
    static const std::vector<Shelf> GENERAL
        = {{1, 1}, {2, 2}, {3, 1}, {3, 2}};  // (B,2) (C,3) (D,2) (D,3)
    static const std::vector<Shelf> CLEANING = {{4, 1}, {4, 0}};  // (E,2) (E,1)
    static const std::vector<Shelf> PETS = {{4, 2}};              // (E,3)
    switch (family)
    {
    case ProductFamily::FRUITS_AND_VEGETABLES:
        return FRUITS;
    case ProductFamily::FRESH_FOOD:
        return FRESH;
    case ProductFamily::MEET_AND_FISH:
        return MEET;
    case ProductFamily::FROZEN_PRODUCTS:
        return FROZEN;
    case ProductFamily::DRINKS:
        return DRINKS;
    case ProductFamily::SNACKS:
        return SNACKS;
    case ProductFamily::GENERAL_FOOD:
        return GENERAL;
    case ProductFamily::HOUSE_CLEANING:
        return CLEANING;
    case ProductFamily::PETS:
        return PETS;
    }
    return PETS;  // unreachable
}

struct PlacedProduct
{
    std::string id;
    std::string name;
    int volume;
    WarehouseLocation location;
};

// DemoDataGenerator.buildProducts: place each catalog product on a random
// shelving of its family, random side, random row 1..ROWS_SIZE.
std::vector<PlacedProduct> buildProducts(JavaRandom &random)
{
    std::vector<PlacedProduct> products;
    products.reserve(CATALOG.size());
    for (std::size_t i = 0; i < CATALOG.size(); ++i)
    {
        const CatalogProduct &raw = CATALOG[i];
        const std::vector<Shelf> &shelvings = shelvingsFor(raw.family);
        const int shelvingIndex
            = random.nextInt(static_cast<int>(shelvings.size()));
        const Side side = random.nextInt(2) == 0 ? Side::LEFT : Side::RIGHT;
        const int row = random.nextInt(Warehouse::SHELVING_ROWS_SIZE) + 1;
        WarehouseLocation location;
        location.column = shelvings[shelvingIndex].column;
        location.shelvingRow = shelvings[shelvingIndex].row;
        location.side = side;
        location.row = row;
        products.push_back({std::to_string(i), raw.name, raw.volume, location});
    }
    return products;
}

}  // namespace

int demoMaxProductSize()
{
    int max = 0;
    for (const CatalogProduct &product : CATALOG)
        if (product.volume > max)
            max = product.volume;
    return max;
}

OrderPickingSolution buildDemoData(int trolleysCount,
                                   int bucketCount,
                                   int bucketCapacity,
                                   int ordersCount,
                                   std::int64_t randomSeed)
{
    // DemoDataGenerator.validateBucketCapacity.
    if (bucketCapacity < demoMaxProductSize())
    {
        throw std::invalid_argument(
            "The selected bucketCapacity: " + std::to_string(bucketCapacity)
            + ", is lower than the maximum product size: "
            + std::to_string(demoMaxProductSize())
            + ". However for a matter of simplicity the problem was "
              "simplified on the assumption that products can always fit in "
              "a trolley bucket. Please use a higher value");
    }

    JavaRandom random(randomSeed);
    OrderPickingSolution solution;

    // START_LOCATION: shelving (A,1), LEFT side, row 0.
    WarehouseLocation startLocation;
    startLocation.column = 0;
    startLocation.shelvingRow = 0;
    startLocation.side = Side::LEFT;
    startLocation.row = 0;

    // buildTrolleys: ids "1".."N".
    for (int i = 1; i <= trolleysCount; ++i)
    {
        Trolley trolley;
        trolley.id = std::to_string(i);
        trolley.bucketCount = bucketCount;
        trolley.bucketCapacity = bucketCapacity;
        trolley.location = startLocation;
        solution.trolleys.push_back(trolley);
    }

    // buildOrders: draws products first, then the orders (same Random).
    const std::vector<PlacedProduct> products = buildProducts(random);
    const int productCount = static_cast<int>(products.size());
    for (int orderNumber = 1; orderNumber <= ordersCount; ++orderNumber)
    {
        const int orderItemsSize = 1 + random.nextInt(productCount - 1);
        const int orderIndex = static_cast<int>(solution.orders.size());
        solution.orders.push_back({std::to_string(orderNumber)});

        // Draw orderItemsSize products, skipping repeats (Java dedupes by
        // product id but keeps drawing) — then one PickTask per kept item
        // (buildPickTasks: ids "<order>-0", "<order>-1", ...).
        std::vector<char> taken(productCount, 0);
        int pickIdx = 0;
        for (int i = 0; i < orderItemsSize; ++i)
        {
            const int productItemIndex = random.nextInt(productCount);
            if (taken[productItemIndex])
                continue;
            taken[productItemIndex] = 1;
            const PlacedProduct &product = products[productItemIndex];
            PickTask task;
            task.id = solution.orders[orderIndex].id + "-"
                      + std::to_string(pickIdx++);
            task.orderIndex = orderIndex;
            task.productName = product.name;
            task.volume = product.volume;
            task.location = product.location;
            solution.pickTasks.push_back(task);
        }
    }
    return solution;
}

}  // namespace op
