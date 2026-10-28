#include "fp/demo_data.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace fp
{

namespace
{

// Bit-exact replica of java.util.Random (48-bit LCG): nextInt(bound),
// nextLong() and the Java 17+ RandomGenerator default nextLong(bound), the
// three methods the demo generator draws from.
class JavaRandom
{
public:
    explicit JavaRandom(std::uint64_t seed)
        : seed_((seed ^ kMultiplier) & kMask48)
    {
    }

    std::int32_t nextInt(std::int32_t bound)
    {
        std::int32_t r = next(31);
        const std::int32_t m = bound - 1;
        if ((bound & m) == 0)  // power of two
        {
            r = static_cast<std::int32_t>((static_cast<std::int64_t>(bound) * r)
                                          >> 31);
        }
        else
        {
            // Java: for (int u = r; u - (r = u % bound) + m < 0; u =
            // next(31)); — the int32 overflow test, done here in 64 bits.
            std::int32_t u = r;
            r = u % bound;
            while (static_cast<std::int64_t>(u) - r + m
                   > std::numeric_limits<std::int32_t>::max())
            {
                u = next(31);
                r = u % bound;
            }
        }
        return r;
    }

    std::int64_t nextLong()
    {
        // ((long) next(32) << 32) + next(32) — the second draw is a SIGNED
        // 32-bit value, sign-extended before the addition.
        const std::int64_t high = static_cast<std::int64_t>(next(32)) << 32;
        return high + next(32);
    }

    // RandomSupport.boundedNextLong (the RandomGenerator default method
    // java.util.Random inherits): rejection sampling over nextLong() >>> 1.
    std::int64_t nextLong(std::int64_t bound)
    {
        std::int64_t r = nextLong();
        const std::int64_t m = bound - 1;
        if ((bound & m) == 0)
            return r & m;
        std::int64_t u
            = static_cast<std::int64_t>(static_cast<std::uint64_t>(r) >> 1);
        r = u % bound;
        // Java: u + m - r < 0 with wrapping int64 arithmetic.
        while (static_cast<std::int64_t>(static_cast<std::uint64_t>(u)
                                         + static_cast<std::uint64_t>(m)
                                         - static_cast<std::uint64_t>(r))
               < 0)
        {
            u = static_cast<std::int64_t>(static_cast<std::uint64_t>(nextLong())
                                          >> 1);
            r = u % bound;
        }
        return r;
    }

private:
    static constexpr std::uint64_t kMultiplier = 0x5DEECE66DULL;
    static constexpr std::uint64_t kAddend = 0xBULL;
    static constexpr std::uint64_t kMask48 = (1ULL << 48) - 1;

    std::int32_t next(int bits)
    {
        seed_ = (seed_ * kMultiplier + kAddend) & kMask48;
        return static_cast<std::int32_t>(seed_ >> (48 - bits));
    }

    std::uint64_t seed_;
};

const char *const kIngredients[] = {"Carrots",
                                    "Peas",
                                    "Cabbage",
                                    "Tomato",
                                    "Eggplant",
                                    "Broccoli",
                                    "Spinach",
                                    "Pumpkin",
                                    "Pepper",
                                    "Onions"};
constexpr int kIngredientCount
    = static_cast<int>(sizeof(kIngredients) / sizeof(kIngredients[0]));

const char *const kVariations[] = {"small bag", "medium bag", "large bag"};

struct DemoProduct
{
    std::string id;
    std::string name;
    std::set<std::string> ingredients;
};

}  // namespace

PackagingSchedule generateDemoData(int lineCount, int jobCount)
{
    const int noCleaningMinutes = 10;
    const int cleaningMinutesMinimum = 30;
    const int cleaningMinutesMaximum = 60;
    const int jobDurationMinutesMinimum = 120;
    const int jobDurationMinutesMaximum = 300;
    // Java int arithmetic: (2*10 + 30 + 60) / 4 + (120 + 300) / 2 == 237.
    const int averageCleaningAndJobDurationMinutes
        = (2 * noCleaningMinutes + cleaningMinutesMinimum
           + cleaningMinutesMaximum)
              / 4
          + (jobDurationMinutesMinimum + jobDurationMinutesMaximum) / 2;

    constexpr Minutes kMinutesPerDay = 24 * 60;

    JavaRandom random(37);
    PackagingSchedule solution;
    solution.fromMinute = 0;                  // START_DATE (a Monday)
    solution.toMinute = 14 * kMinutesPerDay;  // START_DATE.plusWeeks(2)

    // --- products (6 per ingredient: 3 single + 3 combinations) ---
    std::vector<DemoProduct> demoProducts;
    long productId = 0;
    for (int i = 0; i < kIngredientCount; ++i)
    {
        const std::string ingredient = kIngredients[i];
        const int r = random.nextInt(kIngredientCount - 4);
        const std::string a = kIngredients[(i + r + 1) % kIngredientCount];
        const std::string b = kIngredients[(i + r + 2) % kIngredientCount];
        const std::string c = kIngredients[(i + r + 3) % kIngredientCount];
        for (const char *variation : kVariations)
        {
            demoProducts.push_back({std::to_string(productId++),
                                    ingredient + " " + variation,
                                    {ingredient}});
        }
        demoProducts.push_back({std::to_string(productId++),
                                ingredient + " and " + a + " " + kVariations[1],
                                {ingredient, a}});
        demoProducts.push_back({std::to_string(productId++),
                                ingredient + " and " + b + " " + kVariations[2],
                                {ingredient, b}});
        demoProducts.push_back(
            {std::to_string(productId++),
             ingredient + ", " + a + " and " + c + " " + kVariations[1],
             {ingredient, a, c}});
    }
    // Java sorts by the STRING id ("0" < "1" < "10" < ... < "9") before
    // drawing the cleaning durations, so the random stream must follow that
    // exact order.
    std::sort(demoProducts.begin(),
              demoProducts.end(),
              [](const DemoProduct &x, const DemoProduct &y)
              { return x.id < y.id; });

    const int productCount = static_cast<int>(demoProducts.size());
    solution.products.reserve(productCount);
    for (const DemoProduct &product : demoProducts)
        solution.products.push_back({product.id, product.name});

    solution.cleanupMinutes.assign(
        static_cast<std::size_t>(productCount) * productCount, 0);
    for (int p = 0; p < productCount; ++p)
    {
        const std::set<std::string> &ingredients = demoProducts[p].ingredients;
        for (int previous = 0; previous < productCount; ++previous)
        {
            const bool noCleaning
                = std::includes(ingredients.begin(),
                                ingredients.end(),
                                demoProducts[previous].ingredients.begin(),
                                demoProducts[previous].ingredients.end());
            // The random draw happens ONLY on the full-cleaning branch,
            // exactly like the Java ternary.
            const Minutes cleaning
                = p == previous
                      ? 0
                      : (noCleaning
                             ? noCleaningMinutes
                             : cleaningMinutesMinimum
                                   + random.nextInt(cleaningMinutesMaximum
                                                    - cleaningMinutesMinimum));
            solution.cleanupMinutes[static_cast<std::size_t>(previous)
                                        * productCount
                                    + p] = cleaning;
        }
    }

    // --- lines and operators ---
    for (int i = 0; i < lineCount; ++i)
    {
        Line line;
        line.id = std::to_string(i);
        line.name = "Line " + std::to_string(i + 1);
        line.startDateTime = 0;  // START_DATE_TIME
        solution.lines.push_back(std::move(line));
        solution.operators.push_back({"Operator " + std::to_string(i + 1)});
    }

    // --- jobs ---
    solution.jobs.reserve(jobCount);
    for (int i = 0; i < jobCount; ++i)
    {
        const int productIndex = random.nextInt(productCount);
        Job job;
        job.id = std::to_string(i);
        job.name = demoProducts[productIndex].name;
        job.productIndex = productIndex;
        job.durationMinutes = jobDurationMinutesMinimum
                              + random.nextLong(static_cast<std::int64_t>(
                                                    jobDurationMinutesMaximum)
                                                - jobDurationMinutesMinimum);
        const int targetDayIndex = (i / lineCount)
                                   * averageCleaningAndJobDurationMinutes
                                   / (24 * 60);
        job.minStartTime = static_cast<Minutes>(
                               random.nextInt(std::max(1, targetDayIndex - 2)))
                           * kMinutesPerDay;
        job.idealEndTime
            = (targetDayIndex + random.nextLong(3)) * kMinutesPerDay + 16 * 60;
        job.maxEndTime
            = job.idealEndTime + (1 + random.nextLong(3)) * kMinutesPerDay;
        job.priority = 1;
        job.pinned = false;
        solution.jobs.push_back(std::move(job));
    }
    // Java: jobs.sort(comparing(Job::getName)) — a stable sort.
    std::stable_sort(solution.jobs.begin(),
                     solution.jobs.end(),
                     [](const Job &x, const Job &y)
                     { return x.name < y.name; });

    return solution;
}

}  // namespace fp
