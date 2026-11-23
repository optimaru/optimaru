// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SCORE_SCORE_HPP
#define OPTIMARU_SCORE_SCORE_HPP

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

namespace optimaru {

// A two-level score: an integer hard level and a decimal soft level. Hard
// penalties are whole numbers (counts, minutes); soft may carry a fractional
// part (e.g. a load-balance unfairness), so it is a double. Higher is better
// and hard dominates soft: scores compare lexicographically. A solution is
// feasible when hard >= 0.
struct Score {
    std::int64_t hard = 0;
    double soft = 0.0;

    bool feasible() const { return hard >= 0; }

    // Above every score: as a target, it is never reached.
    static Score unreachable() {
        return {std::numeric_limits<std::int64_t>::max(), std::numeric_limits<double>::infinity()};
    }

    bool operator<(const Score& o) const {
        if (hard != o.hard) return hard < o.hard;
        return soft < o.soft;
    }
    bool operator>(const Score& o) const { return o < *this; }
    bool operator<=(const Score& o) const { return !(o < *this); }
    bool operator>=(const Score& o) const { return !(*this < o); }
    bool operator==(const Score& o) const { return hard == o.hard && soft == o.soft; }

    // "<hard>hard/<soft>soft", soft printed as an integer when whole and
    // otherwise with up to 6 decimals, trailing zeros trimmed.
    std::string str() const {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6f", soft);
        std::string s(buf);
        if (s.find('.') != std::string::npos) {
            s.erase(s.find_last_not_of('0') + 1);
            if (!s.empty() && s.back() == '.') s.pop_back();
        }
        return std::to_string(hard) + "hard/" + s + "soft";
    }
};

// Whether every score is re-derived from scratch and compared with the
// incremental one. ScoreChecks::Full recomputes after every scored move
// (objective rebuilt, then scored) and throws ScoreMismatchError on a
// difference, which catches a buggy objective. Orders of magnitude slower:
// for debugging only.
enum class ScoreChecks { Off, Full };

// Does an incremental (or previewed) score agree with the full recompute?
// The one definition every check uses: hard exact, soft within
// floating-point noise of the packed medium/soft value.
inline bool scores_agree(const Score& incremental, const Score& full) {
    return incremental.hard == full.hard && std::fabs(incremental.soft - full.soft) <= 1e-6;
}

class ScoreMismatchError : public std::runtime_error {
public:
    ScoreMismatchError(const Score& incremental, const Score& full)
        : std::runtime_error("Score corruption: incremental score " + incremental.str() +
                             " != full recompute " + full.str()) {}
};

}  // namespace optimaru

#endif  // OPTIMARU_SCORE_SCORE_HPP
