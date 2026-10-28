#include "cs/incremental_calculator.hpp"

#include <algorithm>
#include <cstddef>
#include <unordered_map>

#include "cs/score_calculator.hpp"

namespace cs
{

namespace
{

// Remaps one talk-tag family from the shared interned pool to dense
// 0..familySize-1 ids (per-slot occupancy tables are sized by the family,
// not the pool). Returns the family size (at least 1 so index arithmetic
// never multiplies by zero).
int densify(const std::vector<std::vector<int>> &poolIds,
            std::vector<std::vector<int>> &denseIds)
{
    std::unordered_map<int, int> map;
    denseIds.assign(poolIds.size(), {});
    for (std::size_t i = 0; i < poolIds.size(); ++i)
    {
        denseIds[i].reserve(poolIds[i].size());
        for (int id : poolIds[i])
        {
            auto it = map.find(id);
            if (it == map.end())
                it = map.emplace(id, static_cast<int>(map.size())).first;
            denseIds[i].push_back(it->second);
        }
    }
    return std::max<int>(1, static_cast<int>(map.size()));
}

}  // namespace

void ConferenceSchedulingIncrementalScoreCalculator::resetWorkingSolution(
    ConferenceSchedule &workingSolution)
{
    ScoreCalculator::prepare(workingSolution);
    schedule_ = &workingSolution;
    slotCount_ = workingSolution.timeslotCount();
    roomCount_ = workingSolution.roomCount();
    talkCount_ = workingSolution.talkCount();
    dayCount_ = workingSolution.dayCount;
    const ConferenceSchedule &s = workingSolution;

    // Dense per-family tag ids.
    themeDim_ = densify(s.talkThemeIds, themeD_);
    sectorDim_ = densify(s.talkSectorIds, sectorD_);
    audDim_ = densify(s.talkAudienceIds, audD_);
    contentDim_ = densify(s.talkContentIds, contentD_);
    mutexDim_ = densify(s.talkMutexIds, mutexD_);

    // Dense audience levels (ascending, so dense order == value order) and
    // languages.
    {
        std::vector<int> levels;
        levels.reserve(talkCount_);
        for (const Talk &talk : s.talks)
            levels.push_back(talk.audienceLevel);
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
        levelDim_ = std::max<int>(1, static_cast<int>(levels.size()));
        lvlD_.assign(talkCount_, 0);
        for (int i = 0; i < talkCount_; ++i)
        {
            lvlD_[i] = static_cast<int>(
                std::lower_bound(
                    levels.begin(), levels.end(), s.talks[i].audienceLevel)
                - levels.begin());
        }

        std::unordered_map<int, int> langMap;
        langD_.assign(talkCount_, 0);
        for (int i = 0; i < talkCount_; ++i)
        {
            auto it = langMap.find(s.talkLanguageId[i]);
            if (it == langMap.end())
                it = langMap
                         .emplace(s.talkLanguageId[i],
                                  static_cast<int>(langMap.size()))
                         .first;
            langD_[i] = it->second;
        }
        langDim_ = std::max<int>(1, static_cast<int>(langMap.size()));
    }

    // Sparse relations: talks per speaker, reverse prerequisite adjacency.
    spkTalks_.assign(s.speakers.size(), {});
    for (int i = 0; i < talkCount_; ++i)
    {
        for (int k : s.talkSpeakersSorted[i])
            spkTalks_[static_cast<std::size_t>(k)].push_back(i);
    }
    prereqRev_.assign(talkCount_, {});
    for (int j = 0; j < talkCount_; ++j)
    {
        for (int i : s.talkPrereqSorted[j])
        {
            if (i >= 0 && i < talkCount_ && i != j)
                prereqRev_[static_cast<std::size_t>(i)].push_back(j);
        }
    }
    seen_.assign(talkCount_, 0);
    epoch_ = 0;

    // popularTalks support: capacity classes and favorite-count ranks.
    {
        capVal_.clear();
        capVal_.reserve(roomCount_);
        for (const Room &room : s.rooms)
            capVal_.push_back(room.capacity);
        std::sort(capVal_.begin(), capVal_.end());
        capVal_.erase(std::unique(capVal_.begin(), capVal_.end()),
                      capVal_.end());
        capDim_ = static_cast<int>(capVal_.size());
        capCls_.assign(roomCount_, 0);
        for (int r = 0; r < roomCount_; ++r)
        {
            capCls_[r] = static_cast<int>(std::lower_bound(capVal_.begin(),
                                                           capVal_.end(),
                                                           s.rooms[r].capacity)
                                          - capVal_.begin());
        }

        std::vector<int> favs;
        favs.reserve(talkCount_);
        for (const Talk &talk : s.talks)
            favs.push_back(talk.favoriteCount);
        std::sort(favs.begin(), favs.end());
        favs.erase(std::unique(favs.begin(), favs.end()), favs.end());
        favDim_ = static_cast<int>(favs.size());
        favRank_.assign(talkCount_, 1);
        for (int i = 0; i < talkCount_; ++i)
        {
            favRank_[i]
                = 1
                  + static_cast<int>(std::lower_bound(favs.begin(),
                                                      favs.end(),
                                                      s.talks[i].favoriteCount)
                                     - favs.begin());
        }
    }

    // Unary count tables.
    hardSlotCnt_.assign(static_cast<std::size_t>(talkCount_) * slotCount_, 0);
    softSlotCnt_.assign(static_cast<std::size_t>(talkCount_) * slotCount_, 0);
    unavailSpkCnt_.assign(static_cast<std::size_t>(talkCount_) * slotCount_, 0);
    slotCompat_.assign(static_cast<std::size_t>(talkCount_) * slotCount_, 0);
    hardRoomCnt_.assign(static_cast<std::size_t>(talkCount_) * roomCount_, 0);
    softRoomCnt_.assign(static_cast<std::size_t>(talkCount_) * roomCount_, 0);
    roomCompat_.assign(static_cast<std::size_t>(talkCount_) * roomCount_, 0);
    for (int i = 0; i < talkCount_; ++i)
    {
        const Talk &talk = s.talks[i];
        const PreferenceTagIds &tp = s.talkPrefIds[i];
        for (int slot = 0; slot < slotCount_; ++slot)
        {
            const std::vector<int> &tags = s.slotTagIds[slot];
            int hard = ScoreCalculator::missingCount(tp.requiredTimeslot, tags)
                       + ScoreCalculator::overlappingCount(
                           tp.prohibitedTimeslot, tags);
            int soft = ScoreCalculator::missingCount(tp.preferredTimeslot, tags)
                       + ScoreCalculator::overlappingCount(tp.undesiredTimeslot,
                                                           tags);
            int unavailable = 0;
            for (int k : s.talkSpeakersSorted[i])
            {
                if (ScoreCalculator::speakerUnavailableAt(s, k, slot))
                    ++unavailable;
            }
            // The Java speaker tag counts iterate the raw speaker list.
            for (int k : talk.speakers)
            {
                const PreferenceTagIds &sp = s.speakerPrefIds[k];
                hard += ScoreCalculator::missingCount(sp.requiredTimeslot, tags)
                        + ScoreCalculator::overlappingCount(
                            sp.prohibitedTimeslot, tags);
                soft
                    += ScoreCalculator::missingCount(sp.preferredTimeslot, tags)
                       + ScoreCalculator::overlappingCount(sp.undesiredTimeslot,
                                                           tags);
            }
            const std::size_t at
                = static_cast<std::size_t>(i) * slotCount_ + slot;
            hardSlotCnt_[at] = hard;
            softSlotCnt_[at] = soft;
            unavailSpkCnt_[at] = unavailable;
            slotCompat_[at]
                = ScoreCalculator::typeAllowsSlot(s, talk.talkType, slot) ? 1
                                                                          : 0;
        }
        for (int room = 0; room < roomCount_; ++room)
        {
            const std::vector<int> &tags = s.roomTagIds[room];
            int hard
                = ScoreCalculator::missingCount(tp.requiredRoom, tags)
                  + ScoreCalculator::overlappingCount(tp.prohibitedRoom, tags);
            int soft
                = ScoreCalculator::missingCount(tp.preferredRoom, tags)
                  + ScoreCalculator::overlappingCount(tp.undesiredRoom, tags);
            for (int k : talk.speakers)
            {
                const PreferenceTagIds &sp = s.speakerPrefIds[k];
                hard += ScoreCalculator::missingCount(sp.requiredRoom, tags)
                        + ScoreCalculator::overlappingCount(sp.prohibitedRoom,
                                                            tags);
                soft += ScoreCalculator::missingCount(sp.preferredRoom, tags)
                        + ScoreCalculator::overlappingCount(sp.undesiredRoom,
                                                            tags);
            }
            const std::size_t at
                = static_cast<std::size_t>(i) * roomCount_ + room;
            hardRoomCnt_[at] = hard;
            softRoomCnt_[at] = soft;
            roomCompat_[at]
                = ScoreCalculator::typeAllowsRoom(s, talk.talkType, room) ? 1
                                                                          : 0;
        }
    }

    // Zero the assignment aggregates.
    const std::size_t S = static_cast<std::size_t>(slotCount_);
    slotCnt_.assign(S, 0);
    cellCnt_.assign(S * roomCount_, 0);
    themeCnt_.assign(S * themeDim_, 0);
    sectorCnt_.assign(S * sectorDim_, 0);
    audCnt_.assign(S * audDim_, 0);
    contentCnt_.assign(S * contentDim_, 0);
    mutexCnt_.assign(S * mutexDim_, 0);
    themeAudCnt_.assign(S * themeDim_ * audDim_, 0);
    themeCellCnt_.assign(S * roomCount_ * themeDim_, 0);
    contentLvlCnt_.assign(S * contentDim_ * levelDim_, 0);
    levelCnt_.assign(S * levelDim_, 0);
    langCnt_.assign(S * langDim_, 0);
    favCntBit_.assign(static_cast<std::size_t>(capDim_) * (favDim_ + 1), 0);
    favDurBit_.assign(static_cast<std::size_t>(capDim_) * (favDim_ + 1), 0);
    capTotalCnt_.assign(capDim_, 0);
    capTotalDur_.assign(capDim_, 0);

    crowdCnt_.assign(slotCount_, 0);
    crowdUnits_ = 0;
    speakerDayCnt_.assign(static_cast<std::size_t>(s.speakers.size())
                              * std::max(dayCount_, 1),
                          0);
    makespanUnits_ = 0;
    hardBase_ = 0;
    softBase_ = 0.0;

    assign_.assign(talkCount_, -1);
    for (int i = 0; i < talkCount_; ++i)
    {
        const int slotRoom = s.talks[i].slotRoom;
        if (slotRoom >= 0)
        {
            insert(i, slotRoom);
            assign_[i] = slotRoom;
        }
    }
}

void ConferenceSchedulingIncrementalScoreCalculator::applyCrowdDelta(
    int slot, int newCount, int oldCount)
{
    const std::int64_t dur = schedule_->slotDur[slot];
    const std::int64_t before = oldCount >= 3 ? oldCount * dur : 0;
    const std::int64_t after = newCount >= 3 ? newCount * dur : 0;
    crowdUnits_ += after - before;
}

std::int64_t ConferenceSchedulingIncrementalScoreCalculator::makespanUnitsOf(
    int speaker) const
{
    const std::size_t base
        = static_cast<std::size_t>(speaker) * std::max(dayCount_, 1);
    int firstDay = -1;
    int lastDay = -1;
    for (int d = 0; d < dayCount_; ++d)
    {
        if (speakerDayCnt_[base + d] > 0)
        {
            if (firstDay < 0)
                firstDay = d;
            lastDay = d;
        }
    }
    if (firstDay < 0)
        return 0;
    const std::int64_t span
        = schedule_->daysSorted[lastDay] - schedule_->daysSorted[firstDay];
    return span > 1 ? (span - 1) * 8 * 60 : 0;
}

void ConferenceSchedulingIncrementalScoreCalculator::addUnaryContribution(
    int talkIndex, int slot, int room, int sign)
{
    const std::int64_t dur = schedule_->slotDur[slot];
    const std::size_t slotAt
        = static_cast<std::size_t>(talkIndex) * slotCount_ + slot;
    const std::size_t roomAt
        = static_cast<std::size_t>(talkIndex) * roomCount_ + room;

    std::int64_t hardUnits = (hardSlotCnt_[slotAt] + hardRoomCnt_[roomAt]) * dur
                             + 100 * unavailSpkCnt_[slotAt] * dur;
    if (ScoreCalculator::roomUnavailableAt(*schedule_, room, slot))
        hardUnits += 100000 * dur;
    const std::int64_t compatUnit = std::max<std::int64_t>(dur, 1);
    if (!slotCompat_[slotAt])
        hardUnits += 100000 * compatUnit;
    if (!roomCompat_[roomAt])
        hardUnits += 100000 * compatUnit;

    const std::int64_t softUnits
        = 20 * (softSlotCnt_[slotAt] + softRoomCnt_[roomAt]) * dur;

    hardBase_ -= sign * hardUnits;
    softBase_ -= static_cast<double>(sign * softUnits);
}

void ConferenceSchedulingIncrementalScoreCalculator::favAdd(int capClass,
                                                            int rank,
                                                            int cnt,
                                                            std::int64_t dur)
{
    const std::size_t base = static_cast<std::size_t>(capClass) * (favDim_ + 1);
    for (int idx = rank; idx <= favDim_; idx += idx & -idx)
    {
        favCntBit_[base + idx] += cnt;
        favDurBit_[base + idx] += dur;
    }
}

void ConferenceSchedulingIncrementalScoreCalculator::favPrefix(
    int capClass, int rank, int &cnt, std::int64_t &dur) const
{
    const std::size_t base = static_cast<std::size_t>(capClass) * (favDim_ + 1);
    cnt = 0;
    dur = 0;
    for (int idx = rank; idx > 0; idx -= idx & -idx)
    {
        cnt += favCntBit_[base + idx];
        dur += favDurBit_[base + idx];
    }
}

void ConferenceSchedulingIncrementalScoreCalculator::applyAggregates(
    int talkIndex, int slot, int room, int sign)
{
    const std::size_t sAt = static_cast<std::size_t>(slot);
    const std::size_t cellAt = sAt * roomCount_ + room;
    slotCnt_[sAt] += sign;
    cellCnt_[cellAt] += sign;
    for (int t : themeD_[talkIndex])
    {
        themeCnt_[sAt * themeDim_ + t] += sign;
        themeCellCnt_[cellAt * themeDim_ + t] += sign;
        const std::size_t taBase = (sAt * themeDim_ + t) * audDim_;
        for (int a : audD_[talkIndex])
            themeAudCnt_[taBase + a] += sign;
    }
    for (int sec : sectorD_[talkIndex])
        sectorCnt_[sAt * sectorDim_ + sec] += sign;
    for (int a : audD_[talkIndex])
        audCnt_[sAt * audDim_ + a] += sign;
    const int lvl = lvlD_[talkIndex];
    for (int c : contentD_[talkIndex])
    {
        contentCnt_[sAt * contentDim_ + c] += sign;
        contentLvlCnt_[(sAt * contentDim_ + c) * levelDim_ + lvl] += sign;
    }
    for (int m : mutexD_[talkIndex])
        mutexCnt_[sAt * mutexDim_ + m] += sign;
    levelCnt_[sAt * levelDim_ + lvl] += sign;
    langCnt_[sAt * langDim_ + langD_[talkIndex]] += sign;

    const int cls = capCls_[room];
    const std::int64_t dur = schedule_->slotDur[slot];
    favAdd(cls, favRank_[talkIndex], sign, sign * dur);
    capTotalCnt_[cls] += sign;
    capTotalDur_[cls] += sign * dur;
}

void ConferenceSchedulingIncrementalScoreCalculator::pairTotals(
    int talkIndex,
    int slot,
    int room,
    std::int64_t &hardUnits,
    std::int64_t &softUnits) const
{
    const ConferenceSchedule &s = *schedule_;
    hardUnits = 0;
    softUnits = 0;  // penalties positive, rewards negative
    const std::int64_t durI = s.slotDur[slot];

    // --- sparse: talks sharing a speaker (speakerConflict, pause) ---------
    auto *self = const_cast<ConferenceSchedulingIncrementalScoreCalculator *>(
        this);  // epoch stamps are scratch state, not score state
    ++self->epoch_;
    for (int k : s.talkSpeakersSorted[talkIndex])
    {
        for (int j : spkTalks_[static_cast<std::size_t>(k)])
        {
            if (j == talkIndex || seen_[j] == epoch_)
                continue;
            self->seen_[j] = epoch_;
            if (assign_[j] < 0)
                continue;
            const int sj = s.timeslotIndexOf(assign_[j]);
            const std::int64_t overlap
                = ScoreCalculator::overlapMinutes(s, slot, sj);
            if (overlap > 0)
            {
                const int shared = ScoreCalculator::overlappingCount(
                    s.talkSpeakersSorted[talkIndex], s.talkSpeakersSorted[j]);
                hardUnits += 10 * shared * overlap;  // speakerConflict
            }
            if (!ScoreCalculator::pauseExists(
                    s, slot, sj, s.minimumConsecutiveTalksPauseInMinutes))
                hardUnits += durI + s.slotDur[sj];  // consecutiveTalksPause
        }
    }

    // --- sparse: prerequisite adjacency (both directions) -----------------
    for (int j : s.talkPrereqSorted[talkIndex])  // j is a prerequisite of i
    {
        if (j == talkIndex || j < 0 || j >= talkCount_ || assign_[j] < 0)
            continue;
        const int sj = s.timeslotIndexOf(assign_[j]);
        if (s.slotEnd[sj] > s.slotStart[slot])
            hardUnits += 10 * (durI + s.slotDur[sj]);
    }
    for (int j : prereqRev_[static_cast<std::size_t>(talkIndex)])
    {  // i is a prerequisite of j
        if (assign_[j] < 0)
            continue;
        const int sj = s.timeslotIndexOf(assign_[j]);
        if (s.slotEnd[slot] > s.slotStart[sj])
            hardUnits += 10 * (durI + s.slotDur[sj]);
    }

    // --- dense: one pass over the slots, reading the occupancy tables -----
    const std::int64_t dayI = s.slotDay[slot];
    const std::int64_t endI = s.slotEnd[slot];
    const std::int64_t startI = s.slotStart[slot];
    const int lvlI = lvlD_[talkIndex];
    for (int sj = 0; sj < slotCount_; ++sj)
    {
        const int cnt = slotCnt_[sj];
        if (cnt == 0)
            continue;
        const std::size_t sAt = static_cast<std::size_t>(sj);
        const std::int64_t durJ = s.slotDur[sj];
        const std::int64_t combined = durI + durJ;
        const std::int64_t overlap
            = ScoreCalculator::overlapMinutes(s, slot, sj);

        std::int64_t themeSum = 0;
        for (int t : themeD_[talkIndex])
            themeSum += themeCnt_[sAt * themeDim_ + t];
        std::int64_t contentSum = 0;
        for (int c : contentD_[talkIndex])
            contentSum += contentCnt_[sAt * contentDim_ + c];

        if (overlap > 0)
        {
            // roomConflict.
            hardUnits += 1000 * overlap * cellCnt_[sAt * roomCount_ + room];
            // talkMutuallyExclusiveTalksTags.
            std::int64_t mutexSum = 0;
            for (int m : mutexD_[talkIndex])
                mutexSum += mutexCnt_[sAt * mutexDim_ + m];
            hardUnits += mutexSum * overlap;
            // themeTrackConflict / sectorConflict / contentConflict.
            softUnits += 10 * themeSum * overlap;
            std::int64_t sectorSum = 0;
            for (int sec : sectorD_[talkIndex])
                sectorSum += sectorCnt_[sAt * sectorDim_ + sec];
            softUnits += 10 * sectorSum * overlap;
            softUnits += 100 * contentSum * overlap;
            // audienceTypeThemeTrackConflict (theme x audience joint).
            std::int64_t taSum = 0;
            for (int t : themeD_[talkIndex])
            {
                const std::size_t taBase = (sAt * themeDim_ + t) * audDim_;
                for (int a : audD_[talkIndex])
                    taSum += themeAudCnt_[taBase + a];
            }
            softUnits += taSum * overlap;
        }

        // Same-timeslot diversity rewards.
        if (sj == slot)
        {
            std::int64_t audSum = 0;
            for (int a : audD_[talkIndex])
                audSum += audCnt_[sAt * audDim_ + a];
            softUnits -= audSum * durI;  // audienceTypeDiversity
            softUnits -= (cnt - levelCnt_[sAt * levelDim_ + lvlI]) * durI;
            softUnits -= 10
                         * (cnt - langCnt_[sAt * langDim_ + langD_[talkIndex]])
                         * durI;  // languageDiversity
        }

        if (dayI == s.slotDay[sj])
        {
            // themeTrackRoomStability (same day, OTHER rooms).
            std::int64_t sameRoomThemes = 0;
            const std::size_t cellAt = sAt * roomCount_ + room;
            for (int t : themeD_[talkIndex])
                sameRoomThemes += themeCellCnt_[cellAt * themeDim_ + t];
            softUnits += 10 * (themeSum - sameRoomThemes) * combined;
        }
        else
        {
            // sameDayTalks (shared content/theme on different days).
            softUnits += 10 * (contentSum + themeSum) * combined;
        }

        // contentAudienceLevelFlowViolation (both orders).
        if (!contentD_[talkIndex].empty())
        {
            if (endI > s.slotStart[sj])
            {
                std::int64_t gt = 0;
                for (int c : contentD_[talkIndex])
                {
                    const std::size_t clBase
                        = (sAt * contentDim_ + c) * levelDim_;
                    for (int l = lvlI + 1; l < levelDim_; ++l)
                        gt += contentLvlCnt_[clBase + l];
                }
                softUnits += 10 * gt * combined;
            }
            if (s.slotEnd[sj] > startI)
            {
                std::int64_t lt = 0;
                for (int c : contentD_[talkIndex])
                {
                    const std::size_t clBase
                        = (sAt * contentDim_ + c) * levelDim_;
                    for (int l = 0; l < lvlI; ++l)
                        lt += contentLvlCnt_[clBase + l];
                }
                softUnits += 10 * lt * combined;
            }
        }
    }

    // --- dense: popularTalks (per-capacity-class Fenwick trees) -----------
    const int capI = s.rooms[room].capacity;
    const int rankI = favRank_[talkIndex];
    for (int cls = 0; cls < capDim_; ++cls)
    {
        if (capTotalCnt_[cls] == 0)
            continue;
        if (capVal_[cls] < capI)
        {
            // j in a smaller room with MORE favorites.
            int cntLe;
            std::int64_t durLe;
            favPrefix(cls, rankI, cntLe, durLe);
            const int cntGt = capTotalCnt_[cls] - cntLe;
            const std::int64_t durGt = capTotalDur_[cls] - durLe;
            softUnits += 10 * (cntGt * durI + durGt);
        }
        else if (capVal_[cls] > capI)
        {
            // j in a bigger room with FEWER favorites.
            int cntLt;
            std::int64_t durLt;
            favPrefix(cls, rankI - 1, cntLt, durLt);
            softUnits += 10 * (cntLt * durI + durLt);
        }
    }
}

bool ConferenceSchedulingIncrementalScoreCalculator::isHotTalk(
    int talkIndex) const
{
    const int slotRoom = assign_[talkIndex];
    if (slotRoom < 0)
        return false;
    const ConferenceSchedule &s = *schedule_;
    const int slot = s.timeslotIndexOf(slotRoom);
    const int room = s.roomIndexOf(slotRoom);
    const std::size_t slotAt
        = static_cast<std::size_t>(talkIndex) * slotCount_ + slot;
    const std::size_t roomAt
        = static_cast<std::size_t>(talkIndex) * roomCount_ + room;
    if (!slotCompat_[slotAt] || !roomCompat_[roomAt])
        return true;  // synthetic talkTypeCompatibility
    if (hardSlotCnt_[slotAt] > 0 || hardRoomCnt_[roomAt] > 0)
        return true;  // required/prohibited timeslot/room tags
    if (unavailSpkCnt_[slotAt] > 0)
        return true;  // speakerUnavailableTimeslot
    if (ScoreCalculator::roomUnavailableAt(s, room, slot))
        return true;  // roomUnavailableTimeslot
    if (cellCnt_[static_cast<std::size_t>(slot) * roomCount_ + room] >= 2)
        return true;  // same-cell roomConflict
    return false;
}

void ConferenceSchedulingIncrementalScoreCalculator::insert(int talkIndex,
                                                            int slotRoom)
{
    const ConferenceSchedule &s = *schedule_;
    const int slot = s.timeslotIndexOf(slotRoom);
    const int room = s.roomIndexOf(slotRoom);
    const Talk &talk = s.talks[talkIndex];

    addUnaryContribution(talkIndex, slot, room, +1);
    std::int64_t hardUnits;
    std::int64_t softUnits;
    pairTotals(talkIndex, slot, room, hardUnits, softUnits);
    hardBase_ -= hardUnits;
    softBase_ -= static_cast<double>(softUnits);
    applyAggregates(talkIndex, slot, room, +1);
    if (talk.crowdControlRisk > 0)
    {
        const int old = crowdCnt_[slot]++;
        applyCrowdDelta(slot, old + 1, old);
    }
    if (!talk.speakers.empty())
    {
        const int day = s.slotDayIndex[slot];
        for (int k : s.talkSpeakersSorted[talkIndex])
        {
            const std::int64_t before = makespanUnitsOf(k);
            ++speakerDayCnt_[static_cast<std::size_t>(k)
                                 * std::max(dayCount_, 1)
                             + day];
            makespanUnits_ += makespanUnitsOf(k) - before;
        }
    }
}

void ConferenceSchedulingIncrementalScoreCalculator::retract(int talkIndex,
                                                             int slotRoom)
{
    const ConferenceSchedule &s = *schedule_;
    const int slot = s.timeslotIndexOf(slotRoom);
    const int room = s.roomIndexOf(slotRoom);
    const Talk &talk = s.talks[talkIndex];

    addUnaryContribution(talkIndex, slot, room, -1);
    applyAggregates(talkIndex, slot, room, -1);
    std::int64_t hardUnits;
    std::int64_t softUnits;
    pairTotals(talkIndex, slot, room, hardUnits, softUnits);
    hardBase_ += hardUnits;
    softBase_ += static_cast<double>(softUnits);
    if (talk.crowdControlRisk > 0)
    {
        const int old = crowdCnt_[slot]--;
        applyCrowdDelta(slot, old - 1, old);
    }
    if (!talk.speakers.empty())
    {
        const int day = s.slotDayIndex[slot];
        for (int k : s.talkSpeakersSorted[talkIndex])
        {
            const std::int64_t before = makespanUnitsOf(k);
            --speakerDayCnt_[static_cast<std::size_t>(k)
                                 * std::max(dayCount_, 1)
                             + day];
            makespanUnits_ += makespanUnitsOf(k) - before;
        }
    }
}

void ConferenceSchedulingIncrementalScoreCalculator::beforeVariableChanged(
    int talkIndex)
{
    const int old = assign_[talkIndex];
    if (old >= 0)
        retract(talkIndex, old);  // unassigned: nothing to retract
    assign_[talkIndex] = -1;
}

void ConferenceSchedulingIncrementalScoreCalculator::afterVariableChanged(
    int talkIndex)
{
    const int now = schedule_->talks[talkIndex].slotRoom;  // the new value
    if (now >= 0)
        insert(talkIndex, now);  // still unassigned: nothing to insert
    assign_[talkIndex] = now;
}

Score ConferenceSchedulingIncrementalScoreCalculator::calculateScore()
{
    Score score;
    score.hard = hardBase_ - crowdUnits_;
    score.soft = softBase_ - static_cast<double>(20 * makespanUnits_);
    return score;
}

}  // namespace cs
