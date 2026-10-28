#ifndef ST_CONSTRAINTS_SLOT_SLOT_DELTA_GROUP_HPP
#define ST_CONSTRAINTS_SLOT_SLOT_DELTA_GROUP_HPP

#include "st/constraints/slot/avoided_period.hpp"
#include "st/constraints/slot/class_conflict.hpp"
#include "st/constraints/slot/non_preferred_period.hpp"
#include "st/constraints/slot/room_capacity.hpp"
#include "st/constraints/slot/slot_not_allowed.hpp"
#include "st/constraints/slot/teacher_conflict.hpp"

namespace st::canon
{

// Compiled marginal-cost evaluator of the per-slot family. One evaluate()
// per member change writes every present constraint's raw delta from dense
// per-lesson tables and the monolithic (owner, slot) occupancy count maps —
// the same closed forms as CanonicalIncrementalScoreCalculator's
// insert()/retract(), so the deltas are bit-for-bit the per-constraint
// deltaCost() values. The count maps are maintained here (join/leave is
// inferred from the already-written planning variable) and rebuilt from the
// solution at every onReset.
struct SlotDeltaGroup final : optimaru::UnitConstraintDeltaGroup<Timetable>
{
    static constexpr std::size_t NONE = std::numeric_limits<std::size_t>::max();

    void add(std::size_t index, const CanonUnitConstraint &constraint) override
    {
        if (dynamic_cast<const ClassConflict *>(&constraint) != nullptr)
        {
            takeIndex(classIndex_, index);
        }
        else if (dynamic_cast<const TeacherConflict *>(&constraint) != nullptr)
        {
            takeIndex(teacherIndex_, index);
        }
        else if (dynamic_cast<const SlotNotAllowed *>(&constraint) != nullptr)
        {
            takeIndex(allowedIndex_, index);
        }
        else if (dynamic_cast<const RoomCapacity *>(&constraint) != nullptr)
        {
            takeIndex(roomIndex_, index);
        }
        else if (dynamic_cast<const AvoidedPeriod *>(&constraint) != nullptr)
        {
            takeIndex(avoidedIndex_, index);
        }
        else if (dynamic_cast<const NonPreferredPeriod *>(&constraint)
                 != nullptr)
        {
            takeIndex(nonPreferredIndex_, index);
        }
        else
        {
            unsupported_ = true;
        }
        indexes_.push_back(index);
    }

    const std::vector<std::size_t> &indexes() const override
    {
        return indexes_;
    }

    // Duplicate components must add up independently, which one shared count
    // map cannot express — fall back to the per-constraint deltas then.
    bool isBeneficial() const override { return !duplicate_ && !unsupported_; }

    // Every delta below is an integer count/excess, so hard members with
    // integral weight take the calculator's direct hard-delta fast path.
    bool hasIntegralRawDeltas() const override { return true; }

    void onReset(const Timetable &table) override
    {
        detail::requirePrepared(table);
        const int lessonCount = static_cast<int>(table.lessons.size());
        slotCount_ = table.slotCount();
        maxPeriod_ = table.maxPeriod;
        periodOfSlot_.assign(table.periodOfSlot.begin(),
                             table.periodOfSlot.end());
        roomCapacity_.assign(table.roomCapacityByType.begin(),
                             table.roomCapacityByType.end());

        if (classIndex_ != NONE)
        {
            lessonClass_.resize(lessonCount);
            for (int i = 0; i < lessonCount; ++i)
                lessonClass_[i] = table.lessons[i].classGroupId;
            classSlotCnt_.assign(
                static_cast<std::size_t>(table.classCount) * slotCount_, 0);
        }
        if (teacherIndex_ != NONE)
        {
            teacherOff_.assign(lessonCount + 1, 0);
            teacherIds_.clear();
            for (int i = 0; i < lessonCount; ++i)
            {
                for (int t : table.lessons[i].teachers)
                    teacherIds_.push_back(t);
                teacherOff_[i + 1] = static_cast<int>(teacherIds_.size());
            }
            teacherSlotCnt_.assign(
                static_cast<std::size_t>(table.teacherCount) * slotCount_, 0);
        }
        if (roomIndex_ != NONE)
        {
            demandOff_.assign(lessonCount + 1, 0);
            demandType_.clear();
            demandAmount_.clear();
            for (int i = 0; i < lessonCount; ++i)
            {
                const Lesson &lesson = table.lessons[i];
                for (int type = 0; type < table.roomTypeCount; ++type)
                {
                    if (lesson.roomTypeDemand[type] > 0)
                    {
                        demandType_.push_back(type);
                        demandAmount_.push_back(lesson.roomTypeDemand[type]);
                    }
                }
                demandOff_[i + 1] = static_cast<int>(demandType_.size());
            }
            roomDemandCnt_.assign(
                static_cast<std::size_t>(table.roomTypeCount) * slotCount_, 0);
        }
        if (allowedIndex_ != NONE)
        {
            notAllowed_.assign(
                static_cast<std::size_t>(lessonCount) * slotCount_, 0);
            for (int i = 0; i < lessonCount; ++i)
            {
                const Lesson &lesson = table.lessons[i];
                for (int s = 0; s < slotCount_; ++s)
                {
                    notAllowed_[static_cast<std::size_t>(i) * slotCount_ + s]
                        = Timetable::slotAllowed(lesson, s) ? 0 : 1;
                }
            }
        }
        if (avoidedIndex_ != NONE || nonPreferredIndex_ != NONE)
        {
            avoidedHits_.assign(
                static_cast<std::size_t>(lessonCount) * maxPeriod_, 0);
            nonPreferredHits_.assign(
                static_cast<std::size_t>(lessonCount) * maxPeriod_, 0);
            for (int i = 0; i < lessonCount; ++i)
            {
                for (const auto &[avoided, preferred] :
                     table.lessons[i].preferenceUnits)
                {
                    for (int p = 0; p < maxPeriod_; ++p)
                    {
                        const std::uint16_t bit
                            = static_cast<std::uint16_t>(1U << p);
                        avoidedHits_[static_cast<std::size_t>(i) * maxPeriod_
                                     + p] += (avoided & bit) != 0 ? 1 : 0;
                        nonPreferredHits_[static_cast<std::size_t>(i)
                                              * maxPeriod_
                                          + p]
                            += (preferred != 0 && (preferred & bit) == 0) ? 1
                                                                          : 0;
                    }
                }
            }
        }

        // Replay the current assignment into the count maps.
        for (int i = 0; i < lessonCount; ++i)
        {
            const int slot = table.lessons[i].slot;
            if (slot >= 0)
                applyCounts(table, i, slot, /*joined=*/true);
        }
    }

    void evaluate(const Timetable &table,
                  int unit,
                  const std::vector<int> & /*members*/,
                  int entity,
                  double *deltas) override
    {
        // The planning variable is already written when the calculator
        // notifies a member change (afterVariableChanged), so it tells join
        // from leave: a join is notified on the NEW slot (variable == unit),
        // a leave on the OLD one (variable != unit). Rolled-back moves arrive
        // as inverse notifications through the same path, keeping the count
        // maps exactly reversible.
        const bool joined = table.lessons[entity].slot == unit;

        if (allowedIndex_ != NONE)
        {
            deltas[allowedIndex_] = static_cast<double>(
                notAllowed_[static_cast<std::size_t>(entity) * slotCount_
                            + unit]);
        }
        if (avoidedIndex_ != NONE || nonPreferredIndex_ != NONE)
        {
            const std::size_t cell
                = static_cast<std::size_t>(entity) * maxPeriod_
                  + periodOfSlot_[unit] - 1;
            if (avoidedIndex_ != NONE)
                deltas[avoidedIndex_] = static_cast<double>(avoidedHits_[cell]);
            if (nonPreferredIndex_ != NONE)
                deltas[nonPreferredIndex_]
                    = static_cast<double>(nonPreferredHits_[cell]);
        }
        if (classIndex_ != NONE)
        {
            int &cnt
                = classSlotCnt_[static_cast<std::size_t>(lessonClass_[entity])
                                    * slotCount_
                                + unit];
            // Counted so `cnt` holds the OTHER members while read: the new
            // pairs the lesson forms (join) / the pairs it dissolves (leave).
            deltas[classIndex_] = static_cast<double>(joined ? cnt++ : --cnt);
        }
        if (teacherIndex_ != NONE)
        {
            std::int64_t pairs = 0;
            for (int k = teacherOff_[entity]; k < teacherOff_[entity + 1]; ++k)
            {
                int &cnt
                    = teacherSlotCnt_[static_cast<std::size_t>(teacherIds_[k])
                                          * slotCount_
                                      + unit];
                pairs += joined ? cnt++ : --cnt;
            }
            deltas[teacherIndex_] = static_cast<double>(pairs);
        }
        if (roomIndex_ != NONE)
        {
            std::int64_t excess = 0;
            for (int k = demandOff_[entity]; k < demandOff_[entity + 1]; ++k)
            {
                const int demand = demandAmount_[k];
                int &cnt
                    = roomDemandCnt_[static_cast<std::size_t>(demandType_[k])
                                         * slotCount_
                                     + unit];
                const int capacity = roomCapacity_[demandType_[k]];
                if (!joined)
                    cnt -= demand;
                excess += ScoreCalculator::max0(cnt + demand - capacity)
                          - ScoreCalculator::max0(cnt - capacity);
                if (joined)
                    cnt += demand;
            }
            deltas[roomIndex_] = static_cast<double>(excess);
        }
    }

private:
    void takeIndex(std::size_t &slot, std::size_t index)
    {
        if (slot != NONE)
            duplicate_ = true;
        slot = index;
    }

    // The count-map part of evaluate() alone, for the onReset replay.
    void applyCounts(const Timetable &table, int entity, int slot, bool joined)
    {
        if (classIndex_ != NONE)
        {
            int &cnt
                = classSlotCnt_[static_cast<std::size_t>(lessonClass_[entity])
                                    * slotCount_
                                + slot];
            cnt += joined ? 1 : -1;
        }
        if (teacherIndex_ != NONE)
        {
            for (int k = teacherOff_[entity]; k < teacherOff_[entity + 1]; ++k)
            {
                teacherSlotCnt_[static_cast<std::size_t>(teacherIds_[k])
                                    * slotCount_
                                + slot] += joined ? 1 : -1;
            }
        }
        if (roomIndex_ != NONE)
        {
            for (int k = demandOff_[entity]; k < demandOff_[entity + 1]; ++k)
            {
                roomDemandCnt_[static_cast<std::size_t>(demandType_[k])
                                   * slotCount_
                               + slot]
                    += joined ? demandAmount_[k] : -demandAmount_[k];
            }
        }
        (void)table;
    }

    std::vector<std::size_t> indexes_;
    std::size_t classIndex_ = NONE;
    std::size_t teacherIndex_ = NONE;
    std::size_t allowedIndex_ = NONE;
    std::size_t roomIndex_ = NONE;
    std::size_t avoidedIndex_ = NONE;
    std::size_t nonPreferredIndex_ = NONE;
    bool duplicate_ = false;
    bool unsupported_ = false;

    int slotCount_ = 0;
    int maxPeriod_ = 0;
    // Dense projections (built once per reset).
    std::vector<std::int32_t> periodOfSlot_;
    std::vector<std::int32_t> roomCapacity_;
    std::vector<std::int32_t> lessonClass_;
    std::vector<int> teacherOff_;
    std::vector<std::int32_t> teacherIds_;
    std::vector<int> demandOff_;
    std::vector<std::int32_t> demandType_;
    std::vector<std::int32_t> demandAmount_;
    std::vector<std::uint8_t> notAllowed_;        // (lesson, slot)
    std::vector<std::uint8_t> avoidedHits_;       // (lesson, period-1)
    std::vector<std::uint8_t> nonPreferredHits_;  // (lesson, period-1)
    // Monolithic occupancy count maps, keyed (owner, slot).
    std::vector<int> classSlotCnt_;
    std::vector<int> teacherSlotCnt_;
    std::vector<int> roomDemandCnt_;
};

inline const char *GroupableSlotConstraint::deltaGroupKey() const
{
    return "st.canon.slot-delta.v1";
}

inline std::unique_ptr<optimaru::UnitConstraintDeltaGroup<Timetable>>
GroupableSlotConstraint::createDeltaGroup() const
{
    return std::make_unique<SlotDeltaGroup>();
}

}  // namespace st::canon

#endif  // ST_CONSTRAINTS_SLOT_SLOT_DELTA_GROUP_HPP
