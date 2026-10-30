#ifndef ES_CONSTRAINTS_DAY_PATTERN_HPP
#define ES_CONSTRAINTS_DAY_PATTERN_HPP

#include "es/constraints/common.hpp"

namespace es {

struct DayPatternProjectionContext final : optimaru::SharedConstraintContext {
    static constexpr std::int16_t OFF = -1;
    static constexpr std::int16_t WILDCARD = -2;
    static constexpr std::int16_t NEVER = -3;
    static constexpr std::int16_t MULTIPLE = -4;
    static constexpr std::int16_t WORK = -5;         // token: any worked day (not OFF)
    static constexpr std::int16_t NOT_LOCATION = -6; // token "!x": worked day not at location x

    void prepare(const EmployeeSchedule& schedule) {
        if (preparedGeneration == generation) return;
        preparedGeneration = generation;
        horizon = 0;
        day.resize(schedule.shifts.size());
        locationCode.resize(schedule.shifts.size());
        codeOf.clear();
        std::int16_t nextCode = 0;
        for (std::size_t i = 0; i < schedule.shifts.size(); ++i) {
            day[i] = static_cast<std::int32_t>(schedule.shifts[i].startDay);
            auto [it, inserted] = codeOf.emplace(schedule.shifts[i].location, nextCode);
            if (inserted) ++nextCode;
            locationCode[i] = it->second;
            horizon = std::max(horizon, day[i] + 1);
        }
        projectedUnit = -1;
        projectedMembers.clear();
        projectedDayLocation.clear();
    }

    const std::vector<std::int16_t>& project(int unit, const std::vector<int>& members) {
        if (projectedUnit == unit && projectedMembers == members) return projectedDayLocation;
        projectedUnit = unit;
        projectedMembers = members;
        projectedDayLocation.assign(horizon, OFF);
        for (int entity : members) {
            std::int16_t& dayLocation = projectedDayLocation[day[entity]];
            if (dayLocation == OFF) {
                dayLocation = locationCode[entity];
            } else if (dayLocation != locationCode[entity]) {
                dayLocation = MULTIPLE;
            }
        }
        return projectedDayLocation;
    }

    std::uint64_t preparedGeneration = std::numeric_limits<std::uint64_t>::max();
    std::map<std::string, std::int16_t> codeOf;
    std::vector<std::int32_t> day;
    std::vector<std::int16_t> locationCode;
    std::int32_t horizon = 0;
    int projectedUnit = -1;
    std::vector<int> projectedMembers;
    std::vector<std::int16_t> projectedDayLocation;
};

// Forbidden ordered day pattern (per employee, sliding over the horizon).
// `pattern` is a comma-separated token list; each token is a location, "OFF"
// (a day with no work), "*" (any day), "WORK" (any worked day) or "!x" (a worked
// day not at location x). Penalty = 1 per horizon window matching the pattern.
// Example: "Location A,OFF,Location B": a day at A, an off day, then a day at B.
struct DayPattern final : EsUnitConstraint {
    DayPattern(double weight, std::string pattern)
        : EsUnitConstraint("Day pattern[" + pattern + "]", weight, /*hard=*/false),
          context_(&localContext_) {
        std::size_t start = 0;
        while (start <= pattern.size()) {
            std::size_t comma = pattern.find(',', start);
            std::string tok = pattern.substr(
                    start, comma == std::string::npos ? std::string::npos : comma - start);
            tokens_.push_back(tok);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    const char* sharedContextKey() const override { return "es.day-pattern-projection.v1"; }
    std::unique_ptr<optimaru::SharedConstraintContext> createSharedContext() const override {
        return std::make_unique<DayPatternProjectionContext>();
    }
    void bindSharedContext(optimaru::SharedConstraintContext& context) override {
        auto* typed = dynamic_cast<DayPatternProjectionContext*>(&context);
        if (typed == nullptr) throw std::logic_error("Invalid DayPattern shared context type");
        context_ = typed;
        contextIsShared_ = true;
    }
    void onReset(const EmployeeSchedule& s) override {
        if (!contextIsShared_) localContext_.beginReset();
        context_->prepare(s);
        tokenCode_.resize(tokens_.size());
        notCode_.assign(tokens_.size(), DayPatternProjectionContext::NEVER);
        for (std::size_t i = 0; i < tokens_.size(); ++i) {
            if (tokens_[i] == "*") {
                tokenCode_[i] = DayPatternProjectionContext::WILDCARD;
            } else if (tokens_[i] == "OFF") {
                tokenCode_[i] = DayPatternProjectionContext::OFF;
            } else if (tokens_[i] == "WORK") {
                tokenCode_[i] = DayPatternProjectionContext::WORK;
            } else if (tokens_[i].size() > 1 && tokens_[i][0] == '!') {
                tokenCode_[i] = DayPatternProjectionContext::NOT_LOCATION;
                const auto it = context_->codeOf.find(tokens_[i].substr(1));
                notCode_[i] = it == context_->codeOf.end() ? DayPatternProjectionContext::NEVER
                                                           : it->second;
            } else {
                const auto it = context_->codeOf.find(tokens_[i]);
                tokenCode_[i] = it == context_->codeOf.end()
                                        ? DayPatternProjectionContext::NEVER
                                        : it->second;
            }
        }
    }
    double cost(const EmployeeSchedule&, int unit,
                const std::vector<int>& members) const override {
        const std::vector<std::int16_t>& dayLocation = context_->project(unit, members);
        const std::int32_t plen = static_cast<std::int32_t>(tokenCode_.size());
        double c = 0;
        for (std::int32_t d = 0; d + plen <= context_->horizon; ++d) {
            bool hit = true;
            for (std::int32_t k = 0; k < plen; ++k) {
                const std::int16_t token = tokenCode_[k];
                const std::int16_t loc = dayLocation[d + k];
                bool ok;
                if (token == DayPatternProjectionContext::WILDCARD) {
                    ok = true;
                } else if (token == DayPatternProjectionContext::WORK) {
                    ok = loc != DayPatternProjectionContext::OFF;  // any worked day
                } else if (token == DayPatternProjectionContext::NOT_LOCATION) {
                    ok = loc != DayPatternProjectionContext::OFF && loc != notCode_[k];
                } else {
                    ok = loc == token;
                }
                if (!ok) {
                    hit = false;
                    break;
                }
            }
            if (hit) c += 1;
        }
        return c;
    }

private:
    std::vector<std::string> tokens_;
    std::vector<std::int16_t> tokenCode_;
    std::vector<std::int16_t> notCode_;  // per token: the excluded location of a "!x" token
    DayPatternProjectionContext localContext_;
    DayPatternProjectionContext* context_;
    bool contextIsShared_ = false;
};

}  // namespace es

#endif  // ES_CONSTRAINTS_DAY_PATTERN_HPP
