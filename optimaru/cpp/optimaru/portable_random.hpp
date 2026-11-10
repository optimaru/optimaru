// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_PORTABLE_RANDOM_HPP
#define OPTIMARU_PORTABLE_RANDOM_HPP

#include <cstdint>
#include <random>

namespace optimaru {

// Uniform in [0, n), for n > 0, from the search's own generator: the bounded
// draw the moves, the ruins and the selection schemes share.
inline int random_index(std::mt19937_64& rng, int n) {
    return static_cast<int>(rng() % static_cast<std::uint64_t>(n));
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
    double next_double() { return static_cast<double>(engine_() >> 11) * 0x1.0p-53; }

private:
    std::mt19937_64 engine_;
};

}  // namespace optimaru

#endif  // OPTIMARU_PORTABLE_RANDOM_HPP
