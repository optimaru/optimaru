#ifndef ST_MOVES_UNASSIGN_HPP
#define ST_MOVES_UNASSIGN_HPP

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "st/moves/smart_class_swap.hpp"

namespace st::canon
{

// canonical_solver.cpp 에서 그대로 옮겨 왔다 — 연산자 하나에 파일 하나,
// 엔진 generic/ruin 배치의 도메인 쪽 짝이다.

// Unassign move (only registered when the assembly demotes H1 to soft): take
// one assigned lesson out of the timetable. The BASIC move mix only ever
// draws values from [0, slotCount), so without this operator "leave it
// unassigned" is not a choice the search can make — demoting H1 to soft would
// change the score of a solution the search can never reach, and the run would
// come back with every lesson placed and hard violations instead. The opposite
// direction needs no operator: for an unassigned lesson the builtin change
// move sees oldValue == -1 and takes the valueRecommender's allowed slot.
//
// Seeded on a hard-violating lesson when there is one, because that is where
// unassigning pays: hard beats soft lexicographically, so dropping a lesson
// that carries a violation is an improvement on its own, and H1's soft weight
// (the CP-SAT unassigned penalty, ~10000 against soft terms of 1~30) is what
// makes the search put it back as soon as a conflict-free slot opens up.
template <typename HotSource>
class UnassignMove final : public optimaru::MoveOperator<Timetable>
{
public:
    explicit UnassignMove(const HotSource *hotSource) : hotSource_(hotSource) {}

    const char *name() const override { return "unassign"; }

    bool apply(optimaru::OperatorContext<Timetable> &ctx,
               std::mt19937_64 &rng,
               std::vector<int> &entities) override
    {
        Timetable &table = ctx.director.workingSolution();
        const int lessonCount = static_cast<int>(table.lessons.size());
        int a = hotSource_->hotLesson(rng);
        for (int attempt = 0; attempt < 8 && !removable(table, a); ++attempt)
        {
            a = static_cast<int>(rng()
                                 % static_cast<std::uint64_t>(lessonCount));
        }
        if (!removable(table, a))
            return false;

        // A block (연강) leaves as a whole or not at all: half a block is not
        // a block, and H8 prices it as such. Taking the group out together is
        // what keeps "drop it" reachable for block lessons at all.
        last_.clear();
        const int group = table.lessons[a].blockGroup;
        if (group < 0)
        {
            last_.emplace_back(a, table.lessons[a].slot);
        }
        else
        {
            for (int member : table.blockMembers[group])
            {
                if (table.lessons[member].pinned)
                    return false;  // pinned member: the group cannot leave
                if (table.lessons[member].slot >= 0)
                    last_.emplace_back(member, table.lessons[member].slot);
            }
            if (last_.empty())
                return false;
        }

        ctx.director.beginVariableChanges();
        for (const auto &[lesson, _] : last_)
            ctx.director.changeVariable(lesson, -1);
        ctx.director.endVariableChanges();
        for (const auto &[lesson, _] : last_)
            entities.push_back(lesson);
        return true;
    }

    void undoLast(optimaru::OperatorContext<Timetable> &ctx) override
    {
        ctx.director.beginVariableChanges();
        for (const auto &[lesson, slot] : last_)
            ctx.director.changeVariable(lesson, slot);
        ctx.director.endVariableChanges();
    }

private:
    // Pinned lessons are H2 (fixed slots) and never move; an already
    // unassigned lesson has nothing to remove.
    static bool removable(const Timetable &table, int lesson)
    {
        return lesson >= 0 && !table.lessons[lesson].pinned
               && table.lessons[lesson].slot >= 0;
    }

    const HotSource *hotSource_;
    std::vector<std::pair<int, int>> last_;  // (lesson, slot before the drop)
};

}  // namespace st::canon

#endif  // ST_MOVES_UNASSIGN_HPP
