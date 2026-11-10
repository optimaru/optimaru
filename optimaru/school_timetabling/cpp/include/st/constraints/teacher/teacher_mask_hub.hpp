#ifndef ST_CONSTRAINTS_TEACHER_TEACHER_MASK_HUB_HPP
#define ST_CONSTRAINTS_TEACHER_TEACHER_MASK_HUB_HPP

#include "st/bit_ops.hpp"
#include "st/constraints/global/global_constraint.hpp"

namespace st::canon {

// Shared projection of the per-(teacher, day) mask family
// (teacherMaxPeriodsCost, teacherMaxConsecutivePeriodsCost, teacherGapCost,
// teacherWorkdayCost, teacherFreeDayCost):
// ONE occupancy/busy-mask table with every family metric folded per flip —
// exactly the monolithic setTeacherDayMask. Family members subscribing to
// the same hub receive the identical join/leave stream from the calculator
// (globals are all notified per member change), so the hub applies each
// event once — on the first subscriber's call — and absorbs the remaining
// subscriberCount-1 duplicate deliveries by counting. A component
// constructed without a hub gets a private one (standalone components stay
// correct; assemblies wire one hub across the family to erase the 5x
// duplicated projection upkeep). A hub must only ever be shared by
// constraints registered with the SAME calculator.
class TeacherMaskHub {
public:
    enum Metric {
        kMaxPeriods = 0,             // teacherMaxPeriodsCost: daily + weekly
        kMaxConsecutivePeriods = 1,  // teacherMaxConsecutivePeriodsCost
        kGap = 2,                    // teacherGapCost
        kWorkday = 3,                // teacherWorkdayCost
        kFreeDay = 4,                // teacherFreeDayCost
        kMetricCount = 5
    };

    void addSubscriber() { ++subscribers_; }
    void removeSubscriber() { --subscribers_; }

    void reset(const Timetable& table, const std::vector<std::vector<int>>& members_per_unit) {
        if (skipResets_ > 0) {
            --skipResets_;
            return;
        }
        skipResets_ = subscribers_ - 1;
        skipEvents_ = 0;
        masks_.reset(table.teacherCount, table.slotCount(), table.dayCount);
        week_.assign(table.teacherCount, 0);
        totals_.fill(0);
        // Dense per-teacher limit rows: one cache line instead of four
        // scattered vector reads per mask flip.
        limits_.resize(table.teacherCount);
        for (int teacher = 0; teacher < table.teacherCount; ++teacher) {
            limits_[teacher] = {table.teacherMaxPerDay[teacher], table.teacherMaxPerWeek[teacher],
                                table.teacherMaxConsecutive[teacher],
                                table.teacherPreferredFreeDays[teacher]};
        }
        // Flat (day, period-bit) slot projection and per-lesson teacher
        // lists, so the per-event path never strides the Lesson structs.
        const int slotCount = table.slotCount();
        slotDay_.resize(slotCount);
        slotBit_.resize(slotCount);
        for (int slot = 0; slot < slotCount; ++slot) {
            slotDay_[slot] = table.dayOfSlot[slot];
            slotBit_[slot] = detail::periodBit(table, slot);
        }
        const int lessonCount = static_cast<int>(table.lessons.size());
        teacherOff_.assign(lessonCount + 1, 0);
        teacherIds_.clear();
        for (int i = 0; i < lessonCount; ++i) {
            for (int teacher : table.lessons[i].teachers) teacherIds_.push_back(teacher);
            teacherOff_[i + 1] = static_cast<int>(teacherIds_.size());
        }
        for (std::size_t unit = 0; unit < members_per_unit.size(); ++unit) {
            for (int entity : members_per_unit[unit])
                applyLesson(table, static_cast<int>(unit), entity,
                            /*joined=*/true);
        }
    }

    void on_member_changed(const Timetable& table, int unit, int entity, bool joined) {
        if (skipEvents_ > 0) {
            --skipEvents_;
            return;
        }
        skipEvents_ = subscribers_ - 1;
        applyLesson(table, unit, entity, joined);
    }

    std::int64_t total(Metric metric) const { return totals_[metric]; }

private:
    void applyLesson(const Timetable& /*table*/, int slot, int entity, bool joined) {
        const int day = slotDay_[slot];
        const std::uint16_t bit = slotBit_[slot];
        for (int k = teacherOff_[entity]; k < teacherOff_[entity + 1]; ++k) {
            const int teacher = teacherIds_[k];
            masks_.apply(teacher, slot, day, bit, joined,
                         [&](std::uint16_t oldMask, std::uint16_t newMask) {
                             maskChanged(teacher, day, oldMask, newMask);
                         });
        }
    }

    // One (teacher, day) busy-mask flip -> every family metric, the
    // monolithic setTeacherDayMask fold (subscription-independent: the
    // unused totals are simply never read).
    void maskChanged(int teacher, int day, std::uint16_t oldMask, std::uint16_t newMask) {
        const TeacherLimits& limits = limits_[teacher];
        const int oldBusy = st::bits::popcount(oldMask);
        const int newBusy = st::bits::popcount(newMask);
        totals_[kMaxPeriods] += ScoreCalculator::max0(newBusy - limits.maxPerDay) -
                                ScoreCalculator::max0(oldBusy - limits.maxPerDay);
        totals_[kMaxConsecutivePeriods] +=
                ScoreCalculator::runExcessOf(newMask, limits.maxConsecutive) -
                ScoreCalculator::runExcessOf(oldMask, limits.maxConsecutive);
        totals_[kGap] += ScoreCalculator::gapOf(newMask) - ScoreCalculator::gapOf(oldMask);
        const int workdayDelta = (newMask != 0 ? 1 : 0) - (oldMask != 0 ? 1 : 0);
        totals_[kWorkday] += workdayDelta;
        if ((limits.preferredFreeDays & (1U << day)) != 0) totals_[kFreeDay] += workdayDelta;

        const int oldWeek = week_[teacher];
        const int newWeek = oldWeek + newBusy - oldBusy;
        week_[teacher] = newWeek;
        if (limits.maxPerWeek >= 0) {
            totals_[kMaxPeriods] += ScoreCalculator::max0(newWeek - limits.maxPerWeek) -
                                    ScoreCalculator::max0(oldWeek - limits.maxPerWeek);
        }
    }

    struct TeacherLimits {
        int maxPerDay;
        int maxPerWeek;
        int maxConsecutive;
        unsigned preferredFreeDays;
    };

    detail::OwnerDayMasks masks_;
    std::vector<int> week_;  // teacher -> weekly distinct busy periods
    std::vector<TeacherLimits> limits_;
    std::vector<std::int32_t> slotDay_;
    std::vector<std::uint16_t> slotBit_;
    std::vector<int> teacherOff_;
    std::vector<std::int32_t> teacherIds_;
    std::array<std::int64_t, kMetricCount> totals_{};
    int subscribers_ = 0;
    int skipResets_ = 0;
    int skipEvents_ = 0;
};

// Shared base of the mask family: a thin subscriber that forwards its
// reset/event stream to the hub and reads back one metric total. The level
// is the member's kDefaultLevel (the knobs re-state it per instance).
struct TeacherDayMaskConstraint : MemberChangeGlobalConstraint {
    TeacherDayMaskConstraint(std::string constraint_name, double constraintWeight,
                             optimaru::Level constraintLevel, TeacherMaskHub::Metric metric,
                             std::shared_ptr<TeacherMaskHub> hub)
        : MemberChangeGlobalConstraint(std::move(constraint_name), constraintWeight,
                                       constraintLevel),
          metric_(metric),
          hub_(hub != nullptr ? std::move(hub) : std::make_shared<TeacherMaskHub>()) {
        hub_->addSubscriber();
    }
    ~TeacherDayMaskConstraint() override { hub_->removeSubscriber(); }
    TeacherDayMaskConstraint(const TeacherDayMaskConstraint&) = delete;
    TeacherDayMaskConstraint& operator=(const TeacherDayMaskConstraint&) = delete;

    void reset(const Timetable& table,
               const std::vector<std::vector<int>>& members_per_unit) override {
        detail::requirePrepared(table);
        hub_->reset(table, members_per_unit);
    }
    void on_member_changed(const Timetable& table, int unit, const std::vector<int>& /*members*/,
                           int entity, bool joined) override {
        hub_->on_member_changed(table, unit, entity, joined);
    }
    double cost() const override { return static_cast<double>(hub_->total(metric_)); }

    const std::shared_ptr<TeacherMaskHub>& maskHub() const { return hub_; }

private:
    TeacherMaskHub::Metric metric_;
    std::shared_ptr<TeacherMaskHub> hub_;
};

// Finds a hub to share when adding a mask-family component to `calculator`:
// the hub of an already-registered family member, or nullptr (the new
// component then creates its own). Assembly helpers use this so every
// family member of one calculator shares one projection.
inline std::shared_ptr<TeacherMaskHub> sharedTeacherMaskHub(
        const DecomposedCalculator& calculator) {
    for (const auto& global : calculator.global_constraint_list()) {
        const auto* member = dynamic_cast<const TeacherDayMaskConstraint*>(global.get());
        if (member != nullptr) return member->maskHub();
    }
    return nullptr;
}

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_TEACHER_TEACHER_MASK_HUB_HPP
