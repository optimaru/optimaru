#ifndef OPTIMARU_API_SCORE_BUILDIN_HARD_SOFT_SCORE_HPP
#define OPTIMARU_API_SCORE_BUILDIN_HARD_SOFT_SCORE_HPP

#include <cstdint>
#include <cstdio>
#include <string>

namespace optimaru {

// ---------------------------------------------------------------------------
// HardSoftScore
// ---------------------------------------------------------------------------
// A two-level hard/soft score with a decimal soft level. Hard penalties are whole numbers (counts / minutes)
// so hard stays integer; soft may carry a fractional part (e.g. a load-balance
// unfairness decimal), modelled with double. Higher is better; hard dominates
// soft. A solution is feasible when hard >= 0.
struct HardSoftScore {
    std::int64_t hard = 0;
    double soft = 0.0;

    bool isFeasible() const { return hard >= 0; }

    bool operator<(const HardSoftScore& o) const {
        if (hard != o.hard) return hard < o.hard;
        return soft < o.soft;
    }
    bool operator>(const HardSoftScore& o) const { return o < *this; }
    bool operator<=(const HardSoftScore& o) const { return !(o < *this); }
    bool operator>=(const HardSoftScore& o) const { return !(*this < o); }
    bool operator==(const HardSoftScore& o) const { return hard == o.hard && soft == o.soft; }

    // Formats soft like Java's BigDecimal.toString(): integer when whole,
    // otherwise up to 6 decimal places with trailing zeros trimmed.
    std::string toString() const {
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

}  // namespace optimaru

#endif  // OPTIMARU_API_SCORE_BUILDIN_HARD_SOFT_SCORE_HPP
