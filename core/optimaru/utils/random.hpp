// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_UTILS_RANDOM_HPP
#define OPTIMARU_UTILS_RANDOM_HPP

#include <cmath>
#include <cstdint>
#include <random>
#include <utility>

namespace optimaru {

// ---------------------------------------------------------------------------
// Portable draws
// ---------------------------------------------------------------------------
// Every random draw of the engine and the use cases goes through these, from a
// std::mt19937_64, whose output the standard fixes. The std:: distributions
// and std::shuffle are implementation-defined: libstdc++ (the Linux wheels)
// and libc++ (the macOS ones) turn the same generator output into different
// numbers, so a seeded solve would end differently on each. These are written
// out by hand and give the same numbers everywhere. tests/test_layout.py keeps
// the std:: ones out of core/ and the use cases' C++.

// Uniform in [0, n), for n > 0: the bounded draw the moves, the ruins and the
// selection schemes share.
inline int random_index(std::mt19937_64& rng, int n) {
    return static_cast<int>(rng() % static_cast<std::uint64_t>(n));
}

// Uniform in [lo, hi], for lo <= hi.
inline int random_int(std::mt19937_64& rng, int lo, int hi) {
    return lo + random_index(rng, hi - lo + 1);
}

// Uniform in [0, 1): the top 53 bits of one draw.
inline double random_unit(std::mt19937_64& rng) {
    return static_cast<double>(rng() >> 11) * 0x1.0p-53;
}

// Uniform in [lo, hi).
inline double random_real(std::mt19937_64& rng, double lo, double hi) {
    return lo + (hi - lo) * random_unit(rng);
}

// True with probability p.
inline bool random_bool(std::mt19937_64& rng, double p) { return random_unit(rng) < p; }

// Standard normal, by Box-Muller from two draws (the second variate is not
// kept, so a call holds no state between draws).
inline double random_normal(std::mt19937_64& rng) {
    const double u1 = 1.0 - random_unit(rng);  // (0, 1]: log(0) never happens
    const double u2 = random_unit(rng);
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
}

// Fisher-Yates shuffle of [first, last), random-access iterators.
template <typename RandomIt>
void random_shuffle(RandomIt first, RandomIt last, std::mt19937_64& rng) {
    const auto n = last - first;
    for (auto i = n - 1; i > 0; --i) {
        const auto j = static_cast<decltype(i)>(rng() % static_cast<std::uint64_t>(i + 1));
        using std::swap;
        swap(first[i], first[j]);
    }
}

// Seeded random draws that give the same numbers with every standard library:
// std::mt19937_64 (whose output the standard fixes) with hand-written bounded
// draws, because the std:: distributions are implementation-defined and
// differ between libstdc++ and libc++. For generators that must build the
// same instance on every platform, such as the use cases' demo data.
class PortableRandom {
public:
    explicit PortableRandom(std::uint64_t seed) : engine_(seed) {}

    // Uniform in [0, bound); 0 when bound <= 0.
    int next_int(int bound) {
        if (bound <= 0) return 0;
        return static_cast<int>(engine_() % static_cast<std::uint64_t>(bound));
    }

    // Uniform in [0, bound); 0 when bound <= 0.
    std::int64_t next_long(std::int64_t bound) {
        if (bound <= 0) return 0;
        return static_cast<std::int64_t>(engine_() % static_cast<std::uint64_t>(bound));
    }

    // Uniform in [0, 1).
    double next_double() { return random_unit(engine_); }

private:
    std::mt19937_64 engine_;
};

}  // namespace optimaru

#endif  // OPTIMARU_UTILS_RANDOM_HPP
