#ifndef ST_MOVES_REASSIGN_HPP
#define ST_MOVES_REASSIGN_HPP

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "st/moves/unassign.hpp"

namespace st::canon
{

// canonical_solver.cpp 에서 그대로 옮겨 왔다 — 연산자 하나에 파일 하나,
// 엔진 generic/ruin 배치의 도메인 쪽 짝이다.

// Re-assign move — the way back in, and the reason partial solving converges
// at all (only registered together with UnassignMove).
//
// Dropping is easy to steer: a lesson that carries a hard violation is a hot
// lesson, so the guidance points straight at it. Coming back has no such
// signal — H1 is a global constraint, so an unassigned lesson never shows up
// in penalizedUnits(), and once the search reaches hard 0 (which the relaxed
// model does almost immediately, by dropping) nothing points at the lessons
// waiting outside. The builtin change move can place them, but it draws one
// random allowed slot: on a dense timetable that lands on a conflict almost
// every time and the step is rejected, so the run ends with far more
// unassigned than the instance requires.
//
// This operator closes that loop: seed on an UNASSIGNED lesson and evaluate
// up to `candidateCount` placements through the score director, applying the
// best (the SmartClassSwapMove best-of-k pattern). Blocks move as a unit —
// a whole consecutive run on one day — because H8 prices a half-placed block
// as a violation, so putting one member back alone would never survive.
template <typename HotSource>
class ReassignMove final : public optimaru::MoveOperator<Timetable>
{
public:
    ReassignMove(std::shared_ptr<std::vector<std::vector<int>>> allowedSlots,
                 int candidateCount)
        : allowedSlots_(std::move(allowedSlots)),
          candidateCount_(std::max(1, candidateCount))
    {
    }

    const char *name() const override { return "reassign"; }

    bool apply(optimaru::OperatorContext<Timetable> &ctx,
               std::mt19937_64 &rng,
               std::vector<int> &entities) override
    {
        Timetable &table = ctx.director.workingSolution();
        const int lesson = seedUnassigned(table, rng);
        if (lesson < 0)
            return false;

        const int group = table.lessons[lesson].blockGroup;
        bool found = false;
        Score bestScore;
        std::vector<std::pair<int, int>> best;
        std::vector<std::pair<int, int>> candidate;
        for (int attempt = 0;
             attempt < candidateCount_ * 3 && !enough(found, attempt);
             ++attempt)
        {
            if (!drawPlacement(table, group, lesson, rng, candidate))
                continue;
            apply(ctx, candidate);
            const Score score = ctx.director.calculateScore();
            revert(ctx, candidate);
            if (!found || score > bestScore)
            {
                found = true;
                bestScore = score;
                best = candidate;
            }
        }
        if (!found)
            return false;

        apply(ctx, best);
        last_ = best;
        for (const auto &[member, _] : best)
            entities.push_back(member);
        return true;
    }

    void undoLast(optimaru::OperatorContext<Timetable> &ctx) override
    {
        revert(ctx, last_);
    }

private:
    bool enough(bool found, int attempt) const
    {
        return found && attempt >= candidateCount_;
    }

    // An unassigned, movable lesson. The cursor walks the lesson vector so a
    // bounded probe finds one without scanning the whole timetable; when
    // almost everything is placed the probes simply come up empty and the
    // operator yields its turn (cheap).
    int seedUnassigned(const Timetable &table, std::mt19937_64 &rng)
    {
        const int lessonCount = static_cast<int>(table.lessons.size());
        if (lessonCount == 0)
            return -1;
        cursor_
            = static_cast<int>(rng() % static_cast<std::uint64_t>(lessonCount));
        const int probes = std::min(lessonCount, 64);
        for (int i = 0; i < probes; ++i)
        {
            const int lesson = (cursor_ + i) % lessonCount;
            if (table.lessons[lesson].slot < 0 && !table.lessons[lesson].pinned)
                return lesson;
        }
        return -1;
    }

    // One candidate placement: (lesson, slot) pairs. A loose lesson takes one
    // allowed slot; a block group takes a consecutive run on one day, which
    // is the only shape H8 accepts.
    bool drawPlacement(const Timetable &table,
                       int group,
                       int lesson,
                       std::mt19937_64 &rng,
                       std::vector<std::pair<int, int>> &out) const
    {
        out.clear();
        if (group < 0)
        {
            const std::vector<int> &slots = (*allowedSlots_)[lesson];
            if (slots.empty())
                return false;
            out.emplace_back(lesson, slots[rng() % slots.size()]);
            return true;
        }

        const std::vector<int> &members = table.blockMembers[group];
        const int size = static_cast<int>(members.size());
        if (size == 0 || table.maxPeriod < size)
            return false;
        for (int member : members)
        {
            if (table.lessons[member].pinned || table.lessons[member].slot >= 0)
                return false;  // the group is not fully out: leave it alone
        }
        const int day = static_cast<int>(
            rng() % static_cast<std::uint64_t>(std::max(1, table.dayCount)));
        const int start
            = 1
              + static_cast<int>(rng()
                                 % static_cast<std::uint64_t>(
                                     std::max(1, table.maxPeriod - size + 1)));
        for (int i = 0; i < size; ++i)
        {
            const int slot = table.slotAt(day, start + i);
            if (slot < 0
                || !Timetable::slotAllowed(table.lessons[members[i]], slot))
                return false;
            out.emplace_back(members[i], slot);
        }
        return true;
    }

    static void apply(optimaru::OperatorContext<Timetable> &ctx,
                      const std::vector<std::pair<int, int>> &placement)
    {
        ctx.director.beginVariableChanges();
        for (const auto &[member, slot] : placement)
            ctx.director.changeVariable(member, slot);
        ctx.director.endVariableChanges();
    }

    static void revert(optimaru::OperatorContext<Timetable> &ctx,
                       const std::vector<std::pair<int, int>> &placement)
    {
        ctx.director.beginVariableChanges();
        for (const auto &[member, _] : placement)
            ctx.director.changeVariable(member, -1);
        ctx.director.endVariableChanges();
    }

    std::shared_ptr<std::vector<std::vector<int>>> allowedSlots_;
    int candidateCount_;
    int cursor_ = 0;
    std::vector<std::pair<int, int>> last_;
};

}  // namespace st::canon

#endif  // ST_MOVES_REASSIGN_HPP
