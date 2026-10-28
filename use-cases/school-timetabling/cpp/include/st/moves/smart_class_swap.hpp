#ifndef ST_MOVES_SMART_CLASS_SWAP_HPP
#define ST_MOVES_SMART_CLASS_SWAP_HPP

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "st/canonical_domain.hpp"
#include "optimaru/preview/api/move/operators.hpp"

namespace st::canon
{

// canonical_solver.cpp 에서 그대로 옮겨 왔다 — 연산자 하나에 파일 하나,
// 엔진 generic/ruin 배치의 도메인 쪽 짝이다.

// Smart same-class swap (a SmartChangeMove-style pattern):
// pick a seed lesson — preferring one involved in a hard violation while any
// exists — evaluate the swap with up to `candidateCount` random classmates
// through the score director, and apply the best one. Same-class swaps keep
// the class occupancy multiset untouched (H3-neutral on dense instances), so
// this concentrates search effort on the moves that can actually improve.
// Selection between this operator and the built-in move mix is adaptive
// (roulette wheel on move outcomes).
//
// HotSource abstracts where the violation guidance lives — the monolithic
// calculator's hot sets or the decomposed calculator's penalized units. It
// provides hasHotViolations() and hotLesson(rng).
template <typename HotSource>
class SmartClassSwapMove final : public optimaru::MoveOperator<Timetable>
{
public:
    SmartClassSwapMove(
        std::vector<std::vector<int>> lessonsOfClass,
        std::vector<int> classOf,
        std::shared_ptr<std::vector<std::vector<int>>> allowedSlots,
        int candidateCount,
        const HotSource *hotSource)
        : lessonsOfClass_(std::move(lessonsOfClass)),
          classOf_(std::move(classOf)),
          allowedSlots_(std::move(allowedSlots)),
          candidateCount_(std::max(1, candidateCount)),
          hotSource_(hotSource)
    {
    }

    const char *name() const override { return "smartClassSwap"; }

    bool apply(optimaru::OperatorContext<Timetable> &ctx,
               std::mt19937_64 &rng,
               std::vector<int> &entities) override
    {
        Timetable &table = ctx.director.workingSolution();
        const int lessonCount = static_cast<int>(table.lessons.size());
        if (lessonCount < 2)
            return false;
        // The best-of-k evaluation runs in the soft phase only: during the
        // hard phase its k-fold cost starves the raw move throughput
        // feasibility needs (measured: M/L stop reaching hard 0), so there
        // the operator falls back to a builtin-equivalent single move (hot
        // change / classmate swap) that keeps the search dynamics intact.
        if (hotSource_->hasHotViolations())
            return applyHardPhaseMove(ctx, rng, entities);

        int a = -1;
        for (int attempt = 0; attempt < 8 && (a < 0 || table.lessons[a].pinned);
             ++attempt)
        {
            a = static_cast<int>(rng()
                                 % static_cast<std::uint64_t>(lessonCount));
        }
        if (a < 0 || table.lessons[a].pinned)
            return false;
        const std::vector<int> &mates = lessonsOfClass_[classOf_[a]];
        if (mates.size() < 2)
            return false;

        const int slotA = table.lessons[a].slot;
        bool found = false;
        Score bestScore;
        int bestMate = -1;
        int tried = 0;
        for (int attempt = 0;
             attempt < candidateCount_ * 3 && tried < candidateCount_;
             ++attempt)
        {
            const int b = mates[rng() % mates.size()];
            if (b == a || table.lessons[b].pinned
                || table.lessons[b].slot == slotA)
                continue;
            ++tried;
            const int slotB = table.lessons[b].slot;
            ctx.director.beginVariableChanges();
            ctx.director.changeVariable(a, slotB);
            ctx.director.changeVariable(b, slotA);
            ctx.director.endVariableChanges();
            const Score score = ctx.director.calculateScore();
            ctx.director.beginVariableChanges();
            ctx.director.changeVariable(a, slotA);
            ctx.director.changeVariable(b, slotB);
            ctx.director.endVariableChanges();
            if (!found || score > bestScore)
            {
                found = true;
                bestScore = score;
                bestMate = b;
            }
        }
        if (!found)
            return false;

        applySwap(ctx, a, bestMate, entities);
        return true;
    }

    void undoLast(optimaru::OperatorContext<Timetable> &ctx) override
    {
        if (lastB_ >= 0)
        {
            ctx.director.beginVariableChanges();
            ctx.director.changeVariable(lastA_, lastSlotA_);
            ctx.director.changeVariable(lastB_, lastSlotB_);
            ctx.director.endVariableChanges();
        }
        else
        {
            ctx.director.changeVariable(lastA_, lastSlotA_);
        }
    }

private:
    // Hard phase: one builtin-equivalent move seeded on a hard-violating
    // lesson — 75% a change to a random allowed slot, 25% a classmate swap.
    bool applyHardPhaseMove(optimaru::OperatorContext<Timetable> &ctx,
                            std::mt19937_64 &rng,
                            std::vector<int> &entities)
    {
        Timetable &table = ctx.director.workingSolution();
        const int lessonCount = static_cast<int>(table.lessons.size());
        int a = hotSource_->hotLesson(rng);
        for (int attempt = 0; attempt < 8 && (a < 0 || table.lessons[a].pinned);
             ++attempt)
        {
            a = static_cast<int>(rng()
                                 % static_cast<std::uint64_t>(lessonCount));
        }
        if (a < 0 || table.lessons[a].pinned)
            return false;

        std::uniform_real_distribution<double> unit(0.0, 1.0);
        if (unit(rng) < 0.25)
        {
            const std::vector<int> &mates = lessonsOfClass_[classOf_[a]];
            const int slotA = table.lessons[a].slot;
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                const int b = mates[rng() % mates.size()];
                if (b == a || table.lessons[b].pinned
                    || table.lessons[b].slot == slotA)
                    continue;
                applySwap(ctx, a, b, entities);
                return true;
            }
        }
        const std::vector<int> &slots = (*allowedSlots_)[a];
        if (slots.empty())
            return false;
        const int v = slots[rng() % slots.size()];
        if (v == table.lessons[a].slot)
            return false;
        lastA_ = a;
        lastB_ = -1;
        lastSlotA_ = table.lessons[a].slot;
        ctx.director.changeVariable(a, v);
        entities.push_back(a);
        return true;
    }

    void applySwap(optimaru::OperatorContext<Timetable> &ctx,
                   int a,
                   int b,
                   std::vector<int> &entities)
    {
        Timetable &table = ctx.director.workingSolution();
        lastA_ = a;
        lastB_ = b;
        lastSlotA_ = table.lessons[a].slot;
        lastSlotB_ = table.lessons[b].slot;
        ctx.director.beginVariableChanges();
        ctx.director.changeVariable(a, lastSlotB_);
        ctx.director.changeVariable(b, lastSlotA_);
        ctx.director.endVariableChanges();
        entities.push_back(a);
        entities.push_back(b);
    }

    std::vector<std::vector<int>> lessonsOfClass_;
    std::vector<int> classOf_;
    std::shared_ptr<std::vector<std::vector<int>>> allowedSlots_;
    int candidateCount_;
    const HotSource *hotSource_;
    int lastA_ = -1;
    int lastB_ = -1;
    int lastSlotA_ = -1;
    int lastSlotB_ = -1;
};

}  // namespace st::canon

#endif  // ST_MOVES_SMART_CLASS_SWAP_HPP
