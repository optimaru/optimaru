#include "cs/score_calculator.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace cs
{

namespace
{

// Interns each distinct string into a dense id and returns the sorted,
// deduplicated id vector — the set representation every tag comparison uses.
std::vector<int> internSorted(std::unordered_map<std::string, int> &pool,
                              const std::vector<std::string> &values)
{
    std::vector<int> ids;
    ids.reserve(values.size());
    for (const std::string &value : values)
    {
        auto it = pool.find(value);
        if (it == pool.end())
            it = pool.emplace(value, static_cast<int>(pool.size())).first;
        ids.push_back(it->second);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

}  // namespace

int ScoreCalculator::overlappingCount(const std::vector<int> &left,
                                      const std::vector<int> &right)
{
    int count = 0;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < left.size() && j < right.size())
    {
        if (left[i] < right[j])
            ++i;
        else if (right[j] < left[i])
            ++j;
        else
        {
            ++count;
            ++i;
            ++j;
        }
    }
    return count;
}

int ScoreCalculator::missingCount(const std::vector<int> &required,
                                  const std::vector<int> &available)
{
    return static_cast<int>(required.size())
           - overlappingCount(required, available);
}

void ScoreCalculator::prepare(ConferenceSchedule &schedule)
{
    if (schedule.prepared)
        return;

    const int slotCount = schedule.timeslotCount();
    const int roomCount = schedule.roomCount();
    const int typeCount = static_cast<int>(schedule.talkTypes.size());
    const int speakerCount = static_cast<int>(schedule.speakers.size());
    const int talkCount = schedule.talkCount();

    // Flat per-timeslot columns + dense day indices.
    schedule.slotStart.assign(slotCount, 0);
    schedule.slotEnd.assign(slotCount, 0);
    schedule.slotDur.assign(slotCount, 0);
    schedule.slotDay.assign(slotCount, 0);
    schedule.daysSorted.clear();
    for (int s = 0; s < slotCount; ++s)
    {
        const Timeslot &slot = schedule.timeslots[s];
        schedule.slotStart[s] = slot.startMinute;
        schedule.slotEnd[s] = slot.endMinute;
        schedule.slotDur[s] = slot.durationInMinutes();
        schedule.slotDay[s] = slot.startMinute / 1440;
        schedule.daysSorted.push_back(schedule.slotDay[s]);
    }
    std::sort(schedule.daysSorted.begin(), schedule.daysSorted.end());
    schedule.daysSorted.erase(
        std::unique(schedule.daysSorted.begin(), schedule.daysSorted.end()),
        schedule.daysSorted.end());
    schedule.dayCount = static_cast<int>(schedule.daysSorted.size());
    schedule.slotDayIndex.assign(slotCount, 0);
    for (int s = 0; s < slotCount; ++s)
    {
        schedule.slotDayIndex[s]
            = static_cast<int>(std::lower_bound(schedule.daysSorted.begin(),
                                                schedule.daysSorted.end(),
                                                schedule.slotDay[s])
                               - schedule.daysSorted.begin());
    }

    // Compatibility tables (talkType x slot / talkType x room).
    schedule.typeSlotAllowed.assign(
        static_cast<std::size_t>(typeCount) * slotCount, 0);
    for (int s = 0; s < slotCount; ++s)
    {
        for (int t : schedule.timeslots[s].talkTypes)
        {
            if (t >= 0 && t < typeCount)
                schedule.typeSlotAllowed[static_cast<std::size_t>(t) * slotCount
                                         + s] = 1;
        }
    }
    schedule.typeRoomAllowed.assign(
        static_cast<std::size_t>(typeCount) * roomCount, 0);
    for (int r = 0; r < roomCount; ++r)
    {
        for (int t : schedule.rooms[r].talkTypes)
        {
            if (t >= 0 && t < typeCount)
                schedule.typeRoomAllowed[static_cast<std::size_t>(t) * roomCount
                                         + r] = 1;
        }
    }

    // Unavailability tables.
    schedule.roomUnavailableFlat.assign(
        static_cast<std::size_t>(roomCount) * slotCount, 0);
    for (int r = 0; r < roomCount; ++r)
    {
        for (int s : schedule.rooms[r].unavailableTimeslots)
        {
            if (s >= 0 && s < slotCount)
                schedule
                    .roomUnavailableFlat[static_cast<std::size_t>(r) * slotCount
                                         + s] = 1;
        }
    }
    schedule.speakerUnavailableFlat.assign(
        static_cast<std::size_t>(speakerCount) * slotCount, 0);
    for (int k = 0; k < speakerCount; ++k)
    {
        for (int s : schedule.speakers[k].unavailableTimeslots)
        {
            if (s >= 0 && s < slotCount)
                schedule.speakerUnavailableFlat[static_cast<std::size_t>(k)
                                                    * slotCount
                                                + s] = 1;
        }
    }

    // Interned sorted tag ids (one shared pool).
    std::unordered_map<std::string, int> pool;
    schedule.slotTagIds.resize(slotCount);
    for (int s = 0; s < slotCount; ++s)
        schedule.slotTagIds[s] = internSorted(pool, schedule.timeslots[s].tags);
    schedule.roomTagIds.resize(roomCount);
    for (int r = 0; r < roomCount; ++r)
        schedule.roomTagIds[r] = internSorted(pool, schedule.rooms[r].tags);

    schedule.talkThemeIds.resize(talkCount);
    schedule.talkSectorIds.resize(talkCount);
    schedule.talkAudienceIds.resize(talkCount);
    schedule.talkContentIds.resize(talkCount);
    schedule.talkMutexIds.resize(talkCount);
    schedule.talkPrereqSorted.resize(talkCount);
    schedule.talkSpeakersSorted.resize(talkCount);
    schedule.talkLanguageId.assign(talkCount, -1);
    schedule.talkPrefIds.resize(talkCount);
    auto sortedUnique = [](std::vector<int> v)
    {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    };
    for (int i = 0; i < talkCount; ++i)
    {
        const Talk &talk = schedule.talks[i];
        schedule.talkThemeIds[i] = internSorted(pool, talk.themeTrackTags);
        schedule.talkSectorIds[i] = internSorted(pool, talk.sectorTags);
        schedule.talkAudienceIds[i] = internSorted(pool, talk.audienceTypes);
        schedule.talkContentIds[i] = internSorted(pool, talk.contentTags);
        schedule.talkMutexIds[i]
            = internSorted(pool, talk.mutuallyExclusiveTalksTags);
        schedule.talkPrereqSorted[i] = sortedUnique(talk.prerequisiteTalks);
        schedule.talkSpeakersSorted[i] = sortedUnique(talk.speakers);
        schedule.talkLanguageId[i]
            = internSorted(pool, {talk.language}).front();
        PreferenceTagIds &p = schedule.talkPrefIds[i];
        p.requiredTimeslot = internSorted(pool, talk.requiredTimeslotTags);
        p.preferredTimeslot = internSorted(pool, talk.preferredTimeslotTags);
        p.prohibitedTimeslot = internSorted(pool, talk.prohibitedTimeslotTags);
        p.undesiredTimeslot = internSorted(pool, talk.undesiredTimeslotTags);
        p.requiredRoom = internSorted(pool, talk.requiredRoomTags);
        p.preferredRoom = internSorted(pool, talk.preferredRoomTags);
        p.prohibitedRoom = internSorted(pool, talk.prohibitedRoomTags);
        p.undesiredRoom = internSorted(pool, talk.undesiredRoomTags);
    }
    schedule.speakerPrefIds.resize(speakerCount);
    for (int k = 0; k < speakerCount; ++k)
    {
        const Speaker &speaker = schedule.speakers[k];
        PreferenceTagIds &p = schedule.speakerPrefIds[k];
        p.requiredTimeslot = internSorted(pool, speaker.requiredTimeslotTags);
        p.preferredTimeslot = internSorted(pool, speaker.preferredTimeslotTags);
        p.prohibitedTimeslot
            = internSorted(pool, speaker.prohibitedTimeslotTags);
        p.undesiredTimeslot = internSorted(pool, speaker.undesiredTimeslotTags);
        p.requiredRoom = internSorted(pool, speaker.requiredRoomTags);
        p.preferredRoom = internSorted(pool, speaker.preferredRoomTags);
        p.prohibitedRoom = internSorted(pool, speaker.prohibitedRoomTags);
        p.undesiredRoom = internSorted(pool, speaker.undesiredRoomTags);
    }

    schedule.prepared = true;
}

Breakdown ScoreCalculator::computeBreakdown(ConferenceSchedule &schedule)
{
    prepare(schedule);
    return breakdownOfPrepared(schedule);
}

Breakdown
ScoreCalculator::breakdownOfPrepared(const ConferenceSchedule &schedule)
{
    Breakdown b;
    const int slotCount = schedule.timeslotCount();
    const int talkCount = schedule.talkCount();
    const int speakerCount = static_cast<int>(schedule.speakers.size());
    const int pause = schedule.minimumConsecutiveTalksPauseInMinutes;

    // Crowd-control occupancy (risk>0 talks per timeslot) and per-speaker
    // day spans, accumulated during the unary pass.
    std::vector<int> crowdCnt(slotCount, 0);
    std::vector<std::int64_t> speakerMinDay(speakerCount, 0);
    std::vector<std::int64_t> speakerMaxDay(speakerCount, 0);
    std::vector<char> speakerHasTalk(speakerCount, 0);

    for (int i = 0; i < talkCount; ++i)
    {
        const Talk &talk = schedule.talks[i];
        if (talk.slotRoom < 0)
            continue;  // unassigned: contributes nothing
        const int s = schedule.timeslotIndexOf(talk.slotRoom);
        const int r = schedule.roomIndexOf(talk.slotRoom);
        const std::int64_t dur = schedule.slotDur[s];

        // h0 (synthetic): the quickstart's per-TalkType value ranges.
        if (!typeAllowsSlot(schedule, talk.talkType, s))
            b.talkTypeCompatibility += std::max<std::int64_t>(dur, 1);
        if (!typeAllowsRoom(schedule, talk.talkType, r))
            b.talkTypeCompatibility += std::max<std::int64_t>(dur, 1);

        // h1: roomUnavailableTimeslot.
        if (roomUnavailableAt(schedule, r, s))
            b.roomUnavailableTimeslot += dur;

        // h3: speakerUnavailableTimeslot (per distinct unavailable speaker —
        // a Speaker-fact join in Java).
        for (int k : schedule.talkSpeakersSorted[i])
        {
            if (speakerUnavailableAt(schedule, k, s))
                b.speakerUnavailableTimeslot += dur;
        }

        // h9-h16 / s12-s19: the eight talk + eight speaker tag constraints.
        const PreferenceTagIds &tp = schedule.talkPrefIds[i];
        const std::vector<int> &slotTags = schedule.slotTagIds[s];
        const std::vector<int> &roomTags = schedule.roomTagIds[r];
        b.talkRequiredTimeslotTags
            += missingCount(tp.requiredTimeslot, slotTags) * dur;
        b.talkProhibitedTimeslotTags
            += overlappingCount(tp.prohibitedTimeslot, slotTags) * dur;
        b.talkPreferredTimeslotTags
            += missingCount(tp.preferredTimeslot, slotTags) * dur;
        b.talkUndesiredTimeslotTags
            += overlappingCount(tp.undesiredTimeslot, slotTags) * dur;
        b.talkRequiredRoomTags += missingCount(tp.requiredRoom, roomTags) * dur;
        b.talkProhibitedRoomTags
            += overlappingCount(tp.prohibitedRoom, roomTags) * dur;
        b.talkPreferredRoomTags
            += missingCount(tp.preferredRoom, roomTags) * dur;
        b.talkUndesiredRoomTags
            += overlappingCount(tp.undesiredRoom, roomTags) * dur;
        for (int k : talk.speakers)
        {
            const PreferenceTagIds &sp = schedule.speakerPrefIds[k];
            b.speakerRequiredTimeslotTags
                += missingCount(sp.requiredTimeslot, slotTags) * dur;
            b.speakerProhibitedTimeslotTags
                += overlappingCount(sp.prohibitedTimeslot, slotTags) * dur;
            b.speakerPreferredTimeslotTags
                += missingCount(sp.preferredTimeslot, slotTags) * dur;
            b.speakerUndesiredTimeslotTags
                += overlappingCount(sp.undesiredTimeslot, slotTags) * dur;
            b.speakerRequiredRoomTags
                += missingCount(sp.requiredRoom, roomTags) * dur;
            b.speakerProhibitedRoomTags
                += overlappingCount(sp.prohibitedRoom, roomTags) * dur;
            b.speakerPreferredRoomTags
                += missingCount(sp.preferredRoom, roomTags) * dur;
            b.speakerUndesiredRoomTags
                += overlappingCount(sp.undesiredRoom, roomTags) * dur;
        }

        // h8 prep: crowd-control occupancy.
        if (talk.crowdControlRisk > 0)
            ++crowdCnt[s];

        // s20 prep: per-speaker day span over assigned talks.
        const std::int64_t day = schedule.slotDay[s];
        for (int k : talk.speakers)
        {
            if (!speakerHasTalk[k])
            {
                speakerHasTalk[k] = 1;
                speakerMinDay[k] = day;
                speakerMaxDay[k] = day;
            }
            else
            {
                speakerMinDay[k] = std::min(speakerMinDay[k], day);
                speakerMaxDay[k] = std::max(speakerMaxDay[k], day);
            }
        }
    }

    // h8: crowdControl — a risk>0 talk sharing its timeslot with >= 2 other
    // risk>0 talks (Java: the groupBy count exists and differs from 1).
    for (int i = 0; i < talkCount; ++i)
    {
        const Talk &talk = schedule.talks[i];
        if (talk.slotRoom < 0 || talk.crowdControlRisk <= 0)
            continue;
        const int s = schedule.timeslotIndexOf(talk.slotRoom);
        if (crowdCnt[s] >= 3)
            b.crowdControl += schedule.slotDur[s];
    }

    // s20: speakerMakespan — each day beyond the first counts for 8 hours.
    for (int k = 0; k < speakerCount; ++k)
    {
        if (!speakerHasTalk[k])
            continue;
        const std::int64_t span = speakerMaxDay[k] - speakerMinDay[k];
        if (span > 1)
            b.speakerMakespan += (span - 1) * 8 * 60;
    }

    // Pairwise constraints.
    for (int i = 0; i < talkCount; ++i)
    {
        const Talk &a = schedule.talks[i];
        if (a.slotRoom < 0)
            continue;
        const int sa = schedule.timeslotIndexOf(a.slotRoom);
        const int ra = schedule.roomIndexOf(a.slotRoom);
        for (int j = i + 1; j < talkCount; ++j)
        {
            const Talk &c = schedule.talks[j];
            if (c.slotRoom < 0)
                continue;
            const int sb = schedule.timeslotIndexOf(c.slotRoom);
            const int rb = schedule.roomIndexOf(c.slotRoom);
            const std::int64_t overlap = overlapMinutes(schedule, sa, sb);
            const std::int64_t combined
                = schedule.slotDur[sa] + schedule.slotDur[sb];
            const bool sameSlot = sa == sb;
            const bool sameDay = schedule.slotDay[sa] == schedule.slotDay[sb];

            const int themes = overlappingCount(schedule.talkThemeIds[i],
                                                schedule.talkThemeIds[j]);
            const int sectors = overlappingCount(schedule.talkSectorIds[i],
                                                 schedule.talkSectorIds[j]);
            const int audiences = overlappingCount(schedule.talkAudienceIds[i],
                                                   schedule.talkAudienceIds[j]);
            const int contents = overlappingCount(schedule.talkContentIds[i],
                                                  schedule.talkContentIds[j]);

            if (overlap > 0)
            {
                // h2: roomConflict.
                if (ra == rb)
                    b.roomConflict += overlap;
                // h4: speakerConflict (per shared speaker).
                const int shared
                    = overlappingCount(schedule.talkSpeakersSorted[i],
                                       schedule.talkSpeakersSorted[j]);
                if (shared > 0)
                    b.speakerConflict += shared * overlap;
                // h6: talkMutuallyExclusiveTalksTags.
                const int mutex = overlappingCount(schedule.talkMutexIds[i],
                                                   schedule.talkMutexIds[j]);
                if (mutex > 0)
                    b.talkMutuallyExclusiveTalksTags += mutex * overlap;
                // s1 / s3 / s5 / s8.
                b.themeTrackConflict += themes * overlap;
                b.sectorConflict += sectors * overlap;
                b.audienceTypeThemeTrackConflict
                    += themes * audiences * overlap;
                b.contentConflict += contents * overlap;
            }

            // h5: talkPrerequisiteTalks (both orders; x in y.prerequisites
            // means x must end before y starts).
            if (std::binary_search(schedule.talkPrereqSorted[j].begin(),
                                   schedule.talkPrereqSorted[j].end(),
                                   i)
                && schedule.slotEnd[sa] > schedule.slotStart[sb])
                b.talkPrerequisiteTalks += combined;
            if (std::binary_search(schedule.talkPrereqSorted[i].begin(),
                                   schedule.talkPrereqSorted[i].end(),
                                   j)
                && schedule.slotEnd[sb] > schedule.slotStart[sa])
                b.talkPrerequisiteTalks += combined;

            // h7: consecutiveTalksPause (pairs sharing >= 1 speaker).
            if (overlappingCount(schedule.talkSpeakersSorted[i],
                                 schedule.talkSpeakersSorted[j])
                    > 0
                && !pauseExists(schedule, sa, sb, pause))
                b.consecutiveTalksPause += combined;

            // s2: themeTrackRoomStability (same day, different rooms).
            if (sameDay && themes > 0 && ra != rb)
                b.themeTrackRoomStability += themes * combined;

            // s4 / s6 / s9: same-timeslot diversity rewards.
            if (sameSlot)
            {
                const std::int64_t slotDur = schedule.slotDur[sa];
                if (audiences > 0)
                    b.audienceTypeDiversity += audiences * slotDur;
                if (a.audienceLevel != c.audienceLevel)
                    b.audienceLevelDiversity += slotDur;
                if (schedule.talkLanguageId[i] != schedule.talkLanguageId[j])
                    b.languageDiversity += slotDur;
            }

            // s7: contentAudienceLevelFlowViolation (both orders).
            if (contents > 0)
            {
                if (a.audienceLevel < c.audienceLevel
                    && schedule.slotEnd[sa] > schedule.slotStart[sb])
                    b.contentAudienceLevelFlowViolation += contents * combined;
                if (c.audienceLevel < a.audienceLevel
                    && schedule.slotEnd[sb] > schedule.slotStart[sa])
                    b.contentAudienceLevelFlowViolation += contents * combined;
            }

            // s10: sameDayTalks (shared content/theme on different days).
            if (!sameDay && (contents > 0 || themes > 0))
                b.sameDayTalks += (contents + themes) * combined;

            // s11: popularTalks (both orders: the more popular talk sits in
            // the smaller room).
            if (a.favoriteCount < c.favoriteCount
                && schedule.rooms[ra].capacity > schedule.rooms[rb].capacity)
                b.popularTalks += combined;
            if (c.favoriteCount < a.favoriteCount
                && schedule.rooms[rb].capacity > schedule.rooms[ra].capacity)
                b.popularTalks += combined;
        }
    }

    return b;
}

Score ScoreCalculator::calculateScore(ConferenceSchedule &schedule)
{
    const Score score = computeBreakdown(schedule).toScore();
    schedule.score = score;
    return score;
}

}  // namespace cs
