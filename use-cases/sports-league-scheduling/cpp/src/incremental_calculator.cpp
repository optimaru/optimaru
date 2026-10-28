#include "sls/incremental_calculator.hpp"

#include <cstddef>

#include "sls/score_calculator.hpp"

namespace sls
{

namespace
{

// Soft weight of one classic match outside a weekend/holiday round (the
// quickstart's 1000-per-match penalty, mirrored by Breakdown::softTotal).
constexpr std::int64_t CLASSIC_PENALTY = 1000;

// Penalty of the count row segment [l, r] (both inclusive): sum of the
// lengths of every maximal consecutive run covering >= MAX_CONSECUTIVE
// distinct indices. Only presence matters (several matches on one round
// index count once — see ScoreCalculator's runPenalty). The segment
// boundaries always sit on zeros (or the row ends), so runs never extend
// past them.
std::int64_t
regionPenalty(const std::vector<int> &cnt, std::size_t base, int l, int r)
{
    std::int64_t penalty = 0;
    std::int64_t length = 0;
    for (int i = l; i <= r; ++i)
    {
        if (cnt[base + i] > 0)
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

}  // namespace

void SportsLeagueIncrementalScoreCalculator::resetWorkingSolution(
    LeagueSchedule &workingSolution)
{
    ScoreCalculator::prepare(workingSolution);
    schedule_ = &workingSolution;
    indexCount_ = workingSolution.maxRoundIndex + 1;

    roundMatches_.assign(indexCount_, {});
    posInRound_.assign(workingSolution.matches.size(), -1);
    homeCnt_.assign(
        static_cast<std::size_t>(workingSolution.teamCount()) * indexCount_, 0);
    awayCnt_.assign(
        static_cast<std::size_t>(workingSolution.teamCount()) * indexCount_, 0);
    pairCnt_.assign(
        static_cast<std::size_t>(workingSolution.pairCount) * indexCount_, 0);

    sameDay_ = 0;
    consecutive_ = 0;
    repeat_ = 0;
    startHop_ = 0;
    homeAwayHop_ = 0;
    awayAwayHop_ = 0;
    awayHomeHop_ = 0;
    endHop_ = 0;
    classic_ = 0;

    matchCost_.assign(workingSolution.matches.size(), 0);

    assign_.assign(workingSolution.matches.size(), -1);
    for (int m = 0; m < workingSolution.matchCount(); ++m)
    {
        const int round = workingSolution.matches[m].round;
        assign_[m] = round;
        if (round >= 0)
            insert(m, round);
    }
}

// Applies delta to cnt[team][roundIndex] and re-prices the maximal run around
// roundIndex. The window [l, r] is bounded by zeros at positions != roundIndex
// (or the row ends), which the delta cannot change — so pricing the same
// window before and after the write captures the exact run merge/split/grow.
void SportsLeagueIncrementalScoreCalculator::bumpRun(std::vector<int> &cnt,
                                                     int team,
                                                     int roundIndex,
                                                     int delta)
{
    const std::size_t base = static_cast<std::size_t>(team) * indexCount_;
    int l = roundIndex;
    while (l > 0 && cnt[base + l - 1] > 0)
        --l;
    int r = roundIndex;
    while (r + 1 < indexCount_ && cnt[base + r + 1] > 0)
        ++r;
    const std::int64_t before = regionPenalty(cnt, base, l, r);
    cnt[base + roundIndex] += delta;
    consecutive_ += regionPenalty(cnt, base, l, r) - before;
}

// Adds match `matchIndex` at planning value `round`, accumulating the deltas
// against the OTHER matches already registered. The match enters the
// roundMatches_ bucket only at the very end, so every scan below sees
// everyone but itself.
void SportsLeagueIncrementalScoreCalculator::insert(int matchIndex, int round)
{
    const Match &match = schedule_->matches[matchIndex];
    const int ri = schedule_->roundIndexOf(round);

    // hard 1: same-round pairs sharing a team (shared-both still counts once).
    for (int other : roundMatches_[ri])
    {
        const Match &o = schedule_->matches[other];
        if (o.homeTeam == match.homeTeam || o.homeTeam == match.awayTeam
            || o.awayTeam == match.homeTeam || o.awayTeam == match.awayTeam)
        {
            ++sameDay_;
        }
    }

    // soft 6-8: hop pairs (match, y) with y on round index ri + 1. Every hop
    // is credited to both matches of the pair (see matchCost_).
    if (ri + 1 < indexCount_)
    {
        for (int other : roundMatches_[ri + 1])
        {
            const Match &y = schedule_->matches[other];
            std::int64_t hop = 0;
            if (y.awayTeam == match.homeTeam)
            {
                const int d = schedule_->distance(match.homeTeam, y.homeTeam);
                homeAwayHop_ += d;
                hop += d;
            }
            if (y.awayTeam == match.awayTeam)
            {
                const int d = schedule_->distance(match.homeTeam, y.homeTeam);
                awayAwayHop_ += d;
                hop += d;
            }
            if (y.homeTeam == match.awayTeam)
            {
                const int d
                    = schedule_->distance(match.homeTeam, match.awayTeam);
                awayHomeHop_ += d;
                hop += d;
            }
            matchCost_[matchIndex] += hop;
            matchCost_[other] += hop;
        }
    }
    // soft 6-8: hop pairs (x, match) with x on round index ri - 1.
    if (ri > 0)
    {
        for (int other : roundMatches_[ri - 1])
        {
            const Match &x = schedule_->matches[other];
            std::int64_t hop = 0;
            if (match.awayTeam == x.homeTeam)
            {
                const int d = schedule_->distance(x.homeTeam, match.homeTeam);
                homeAwayHop_ += d;
                hop += d;
            }
            if (match.awayTeam == x.awayTeam)
            {
                const int d = schedule_->distance(x.homeTeam, match.homeTeam);
                awayAwayHop_ += d;
                hop += d;
            }
            if (match.homeTeam == x.awayTeam)
            {
                const int d = schedule_->distance(x.homeTeam, x.awayTeam);
                awayHomeHop_ += d;
                hop += d;
            }
            matchCost_[matchIndex] += hop;
            matchCost_[other] += hop;
        }
    }

    // hard 4: the match's own predicate (a reversed fixture on ri + 1), plus
    // the reversed fixtures on ri - 1 that this match newly satisfies (their
    // ifExists flips only when no same-pair match sat on ri before).
    if (match.reversePairId >= 0)
    {
        if (ri + 1 < indexCount_
            && pairCnt_[pairRoundOffset(match.reversePairId, ri + 1)] > 0)
        {
            ++repeat_;
        }
        if (ri > 0 && pairCnt_[pairRoundOffset(match.pairId, ri)] == 0)
        {
            repeat_ += pairCnt_[pairRoundOffset(match.reversePairId, ri - 1)];
        }
    }
    ++pairCnt_[pairRoundOffset(match.pairId, ri)];

    // soft 5 + 9: first/last round-index hops.
    if (ri == 0 || !schedule_->roundIndexExists[ri - 1])
    {
        const int d = schedule_->distance(match.awayTeam, match.homeTeam);
        startHop_ += d;
        matchCost_[matchIndex] += d;
    }
    if (ri + 1 >= indexCount_ || !schedule_->roundIndexExists[ri + 1])
    {
        const int d = schedule_->distance(match.homeTeam, match.awayTeam);
        endHop_ += d;
        matchCost_[matchIndex] += d;
    }

    // soft 10: classic match outside weekends/holidays.
    if (match.classicMatch && !schedule_->rounds[round].weekendOrHoliday)
    {
        ++classic_;
        matchCost_[matchIndex] += CLASSIC_PENALTY;
    }

    // hard 2 + 3: consecutive home/away runs.
    bumpRun(homeCnt_, match.homeTeam, ri, +1);
    bumpRun(awayCnt_, match.awayTeam, ri, +1);

    posInRound_[matchIndex] = static_cast<int>(roundMatches_[ri].size());
    roundMatches_[ri].push_back(matchIndex);
}

// Exact mirror of insert: leave the bucket first, then subtract the deltas
// against the remaining matches.
void SportsLeagueIncrementalScoreCalculator::retract(int matchIndex, int round)
{
    const Match &match = schedule_->matches[matchIndex];
    const int ri = schedule_->roundIndexOf(round);

    std::vector<int> &bucket = roundMatches_[ri];
    const int pos = posInRound_[matchIndex];
    bucket[pos] = bucket.back();
    posInRound_[bucket[pos]] = pos;
    bucket.pop_back();
    posInRound_[matchIndex] = -1;

    bumpRun(homeCnt_, match.homeTeam, ri, -1);
    bumpRun(awayCnt_, match.awayTeam, ri, -1);

    if (match.classicMatch && !schedule_->rounds[round].weekendOrHoliday)
    {
        --classic_;
        matchCost_[matchIndex] -= CLASSIC_PENALTY;
    }

    if (ri == 0 || !schedule_->roundIndexExists[ri - 1])
    {
        const int d = schedule_->distance(match.awayTeam, match.homeTeam);
        startHop_ -= d;
        matchCost_[matchIndex] -= d;
    }
    if (ri + 1 >= indexCount_ || !schedule_->roundIndexExists[ri + 1])
    {
        const int d = schedule_->distance(match.homeTeam, match.awayTeam);
        endHop_ -= d;
        matchCost_[matchIndex] -= d;
    }

    --pairCnt_[pairRoundOffset(match.pairId, ri)];
    if (match.reversePairId >= 0)
    {
        if (ri > 0 && pairCnt_[pairRoundOffset(match.pairId, ri)] == 0)
        {
            repeat_ -= pairCnt_[pairRoundOffset(match.reversePairId, ri - 1)];
        }
        if (ri + 1 < indexCount_
            && pairCnt_[pairRoundOffset(match.reversePairId, ri + 1)] > 0)
        {
            --repeat_;
        }
    }

    for (int other : bucket)
    {
        const Match &o = schedule_->matches[other];
        if (o.homeTeam == match.homeTeam || o.homeTeam == match.awayTeam
            || o.awayTeam == match.homeTeam || o.awayTeam == match.awayTeam)
        {
            --sameDay_;
        }
    }

    if (ri + 1 < indexCount_)
    {
        for (int other : roundMatches_[ri + 1])
        {
            const Match &y = schedule_->matches[other];
            std::int64_t hop = 0;
            if (y.awayTeam == match.homeTeam)
            {
                const int d = schedule_->distance(match.homeTeam, y.homeTeam);
                homeAwayHop_ -= d;
                hop += d;
            }
            if (y.awayTeam == match.awayTeam)
            {
                const int d = schedule_->distance(match.homeTeam, y.homeTeam);
                awayAwayHop_ -= d;
                hop += d;
            }
            if (y.homeTeam == match.awayTeam)
            {
                const int d
                    = schedule_->distance(match.homeTeam, match.awayTeam);
                awayHomeHop_ -= d;
                hop += d;
            }
            matchCost_[matchIndex] -= hop;
            matchCost_[other] -= hop;
        }
    }
    if (ri > 0)
    {
        for (int other : roundMatches_[ri - 1])
        {
            const Match &x = schedule_->matches[other];
            std::int64_t hop = 0;
            if (match.awayTeam == x.homeTeam)
            {
                const int d = schedule_->distance(x.homeTeam, match.homeTeam);
                homeAwayHop_ -= d;
                hop += d;
            }
            if (match.awayTeam == x.awayTeam)
            {
                const int d = schedule_->distance(x.homeTeam, match.homeTeam);
                awayAwayHop_ -= d;
                hop += d;
            }
            if (match.homeTeam == x.awayTeam)
            {
                const int d = schedule_->distance(x.homeTeam, x.awayTeam);
                awayHomeHop_ -= d;
                hop += d;
            }
            matchCost_[matchIndex] -= hop;
            matchCost_[other] -= hop;
        }
    }
}

// Tournament selection on matchCost_. Ties keep the first draw, so an
// all-zero cost row degenerates to uniform random selection.
//
// While the solution is infeasible this declines to recommend (-1 = the
// engine's uniform fallback): travel guidance there pulls the search toward
// the expensive matches instead of the violated ones, and measurably delays
// — on the largest bench instance, prevents — reaching 0 hard.
int SportsLeagueIncrementalScoreCalculator::costliestMatch(std::mt19937_64 &rng,
                                                           int sample) const
{
    const auto matchCount = static_cast<std::uint64_t>(matchCost_.size());
    if (matchCount == 0 || sameDay_ + consecutive_ + repeat_ > 0)
        return -1;
    int best = static_cast<int>(rng() % matchCount);
    for (int i = 1; i < sample; ++i)
    {
        const int candidate = static_cast<int>(rng() % matchCount);
        if (matchCost_[static_cast<std::size_t>(candidate)]
            > matchCost_[static_cast<std::size_t>(best)])
        {
            best = candidate;
        }
    }
    return best;
}

void SportsLeagueIncrementalScoreCalculator::beforeVariableChanged(
    int matchIndex)
{
    const int old = assign_[matchIndex];
    if (old >= 0)
        retract(matchIndex, old);  // unassigned: nothing to retract
    assign_[matchIndex] = -1;
}

void SportsLeagueIncrementalScoreCalculator::afterVariableChanged(
    int matchIndex)
{
    const int now = schedule_->matches[matchIndex].round;  // the new value
    assign_[matchIndex] = now;
    if (now >= 0)
        insert(matchIndex, now);  // now unassigned: nothing to insert
}

Score SportsLeagueIncrementalScoreCalculator::calculateScore()
{
    Score score;
    score.hard = -(sameDay_ + consecutive_ + repeat_);
    // 0.0 - x avoids IEEE negative zero when the soft total is 0.
    score.soft = 0.0
                 - static_cast<double>(startHop_ + homeAwayHop_ + awayAwayHop_
                                       + awayHomeHop_ + endHop_
                                       + CLASSIC_PENALTY * classic_);
    return score;
}

}  // namespace sls
