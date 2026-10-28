#include "sls/score_calculator.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace sls
{

void ScoreCalculator::prepare(LeagueSchedule &schedule)
{
    if (schedule.prepared)
        return;

    const int teamCount = schedule.teamCount();

    // Distance matrix: empty means "all zero" (handy for constraint tests);
    // anything else must be a full teamCount x teamCount matrix.
    if (schedule.distanceToTeam.empty())
    {
        schedule.distanceToTeam.assign(teamCount,
                                       std::vector<int>(teamCount, 0));
    }
    if (static_cast<int>(schedule.distanceToTeam.size()) != teamCount)
    {
        throw std::invalid_argument(
            "distanceToTeam must be a teamCount x teamCount matrix");
    }
    for (const std::vector<int> &row : schedule.distanceToTeam)
    {
        if (static_cast<int>(row.size()) != teamCount)
        {
            throw std::invalid_argument(
                "distanceToTeam must be a teamCount x teamCount matrix");
        }
    }

    // Round-index existence table (constraints probe index - 1 / index + 1).
    schedule.maxRoundIndex = -1;
    for (const Round &round : schedule.rounds)
    {
        if (round.index < 0)
            throw std::invalid_argument("round indices must be >= 0");
        schedule.maxRoundIndex = std::max(schedule.maxRoundIndex, round.index);
    }
    schedule.roundIndexExists.assign(
        static_cast<std::size_t>(schedule.maxRoundIndex + 1), 0);
    for (const Round &round : schedule.rounds)
        schedule.roundIndexExists[round.index] = 1;

    // Intern the ordered (home, away) pairs of the matches into dense ids and
    // resolve each match's reversed pair (repeatMatchOnNextDay needs "does a
    // reversed fixture sit on the next round index").
    std::unordered_map<int, int> pairIds;
    auto keyOf
        = [teamCount](int home, int away) { return home * teamCount + away; };
    for (Match &match : schedule.matches)
    {
        if (match.homeTeam < 0 || match.homeTeam >= teamCount
            || match.awayTeam < 0 || match.awayTeam >= teamCount)
        {
            throw std::invalid_argument("match team indices out of range");
        }
        const int key = keyOf(match.homeTeam, match.awayTeam);
        auto it = pairIds.find(key);
        if (it == pairIds.end())
        {
            it = pairIds.emplace(key, static_cast<int>(pairIds.size())).first;
        }
        match.pairId = it->second;
    }
    for (Match &match : schedule.matches)
    {
        auto it = pairIds.find(keyOf(match.awayTeam, match.homeTeam));
        match.reversePairId = (it != pairIds.end()) ? it->second : -1;
    }
    schedule.pairCount = static_cast<int>(pairIds.size());

    schedule.prepared = true;
}

std::int64_t ScoreCalculator::runPenalty(const std::vector<int> &cnt)
{
    std::int64_t penalty = 0;
    std::int64_t length = 0;
    for (int c : cnt)
    {
        if (c > 0)
        {
            ++length;
        }
        else
        {
            if (length >= ScoreCalculator::MAX_CONSECUTIVE)
                penalty += length;
            length = 0;
        }
    }
    if (length >= ScoreCalculator::MAX_CONSECUTIVE)
        penalty += length;
    return penalty;
}

Breakdown ScoreCalculator::computeBreakdown(LeagueSchedule &schedule)
{
    prepare(schedule);
    Breakdown b;
    const int teamCount = schedule.teamCount();
    const int indexCount = schedule.maxRoundIndex + 1;

    // Assigned matches bucketed by round *index* (the constraint key).
    std::vector<std::vector<int>> roundMatches(indexCount);
    std::vector<int> homeCnt(static_cast<std::size_t>(teamCount) * indexCount,
                             0);
    std::vector<int> awayCnt(static_cast<std::size_t>(teamCount) * indexCount,
                             0);
    std::vector<int> pairCnt(
        static_cast<std::size_t>(schedule.pairCount) * indexCount, 0);

    for (int m = 0; m < schedule.matchCount(); ++m)
    {
        const Match &match = schedule.matches[m];
        if (match.round < 0)
            continue;  // unassigned: contributes nothing
        const int ri = schedule.roundIndexOf(match.round);
        roundMatches[ri].push_back(m);
        ++homeCnt[static_cast<std::size_t>(match.homeTeam) * indexCount + ri];
        ++awayCnt[static_cast<std::size_t>(match.awayTeam) * indexCount + ri];
        ++pairCnt[static_cast<std::size_t>(match.pairId) * indexCount + ri];

        // soft 5 + 9: first/last round-index hops.
        if (ri == 0 || !schedule.roundIndexExists[ri - 1])
            b.startToAwayHop
                += schedule.distance(match.awayTeam, match.homeTeam);
        if (ri == schedule.maxRoundIndex || !schedule.roundIndexExists[ri + 1])
            b.awayToEndHop += schedule.distance(match.homeTeam, match.awayTeam);

        // soft 10: classic matches outside weekends/holidays.
        if (match.classicMatch
            && !schedule.rounds[match.round].weekendOrHoliday)
            ++b.classicMatchCount;
    }

    // hard 1: same-round pairs sharing at least one team (counted once even
    // when they share both).
    for (const std::vector<int> &bucket : roundMatches)
    {
        for (std::size_t i = 0; i < bucket.size(); ++i)
        {
            const Match &a = schedule.matches[bucket[i]];
            for (std::size_t j = i + 1; j < bucket.size(); ++j)
            {
                const Match &c = schedule.matches[bucket[j]];
                if (a.homeTeam == c.homeTeam || a.homeTeam == c.awayTeam
                    || a.awayTeam == c.homeTeam || a.awayTeam == c.awayTeam)
                {
                    ++b.matchesOnSameDay;
                }
            }
        }
    }

    // hard 2 + 3: consecutive home/away round-index runs per team.
    for (int t = 0; t < teamCount; ++t)
    {
        const std::size_t base = static_cast<std::size_t>(t) * indexCount;
        std::vector<int> home(homeCnt.begin() + base,
                              homeCnt.begin() + base + indexCount);
        std::vector<int> away(awayCnt.begin() + base,
                              awayCnt.begin() + base + indexCount);
        b.consecutiveHomeMatches += runPenalty(home);
        b.consecutiveAwayMatches += runPenalty(away);
    }

    // hard 4: reversed fixture on the next round index (once per match).
    for (const Match &match : schedule.matches)
    {
        if (match.round < 0 || match.reversePairId < 0)
            continue;
        const int ri = schedule.roundIndexOf(match.round);
        if (ri + 1 < indexCount
            && pairCnt[static_cast<std::size_t>(match.reversePairId)
                           * indexCount
                       + ri + 1]
                   > 0)
        {
            ++b.repeatMatchOnNextDay;
        }
    }

    // soft 6 + 7 + 8: hops between matches on consecutive round indices.
    for (int ri = 0; ri + 1 < indexCount; ++ri)
    {
        for (int mi : roundMatches[ri])
        {
            const Match &m1 = schedule.matches[mi];
            for (int mj : roundMatches[ri + 1])
            {
                const Match &m2 = schedule.matches[mj];
                if (m2.awayTeam == m1.homeTeam)
                    b.homeToAwayHop
                        += schedule.distance(m1.homeTeam, m2.homeTeam);
                if (m2.awayTeam == m1.awayTeam)
                    b.awayToAwayHop
                        += schedule.distance(m1.homeTeam, m2.homeTeam);
                if (m2.homeTeam == m1.awayTeam)
                    b.awayToHomeHop
                        += schedule.distance(m1.homeTeam, m1.awayTeam);
            }
        }
    }

    return b;
}

Score ScoreCalculator::calculateScore(LeagueSchedule &schedule)
{
    const Breakdown b = computeBreakdown(schedule);
    Score score;
    score.hard = -b.hardTotal();
    // 0.0 - x avoids IEEE negative zero when the soft total is 0.
    score.soft = 0.0 - static_cast<double>(b.softTotal());
    schedule.score = score;
    return score;
}

}  // namespace sls
