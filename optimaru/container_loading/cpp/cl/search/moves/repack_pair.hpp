#ifndef CL_SEARCH_MOVES_REPACK_PAIR_HPP
#define CL_SEARCH_MOVES_REPACK_PAIR_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "cl/model/domain.hpp"
#include "cl/score/packing.hpp"
#include "cl/search/moves/list_edit.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/utils/random.hpp"

namespace cl {

// ---------------------------------------------------------------------------
// RepackPairMove -- two containers loaded again, group by group
// ---------------------------------------------------------------------------
// Two things the single-item moves cannot see their way to. A group spread
// over two containers costs the same until its LAST item has joined the
// others, and a loading order that grew by moves is rarely the one that packs
// its items best. This operator takes the items out of two containers in use
// and loads them again, into those two and, every second time, an empty
// container as a third:
//
//   1. The items are dealt out group by group (the group with the most
//      volume among them first; an item without a group stands for itself).
//      A group goes whole to the first of the containers that has room for
//      all of it: room is a drawn share of the container's volume (70 to
//      100%) and its weight limit, and its items may all go in it. A group
//      no container has that room for is dealt item by item, largest first,
//      each to the first with room for it that it may go in, and to the last
//      it may go in when none has.
//   2. Each container's items are put in the construction's loading order
//      (loadsBefore: what leaves last first, what carries only a limited
//      load last, else largest first) and decoded. What finds no place goes
//      on to the next container.
//
// So the groups the two shared come out whole wherever they fit a container,
// and when everything fits the first, the second is empty. With the third
// container a group can be given a container of its own; whether that is
// worth the container is the score's to say. The partner of the first
// container is one that shares a group with it three times out of four,
// where there is one. Which of the two is filled first is drawn.
//
// The move is made only when every load comes out clean: everything placed,
// no stacking limit, stacking rule, segregation rule or unloading order broken, the weight
// limits kept.
// Otherwise nothing is changed and one random item is relocated instead
// (relocateOneItem), so that the move is judged like any other.
//
// The move keeps decodes of its own (a PackingCache). Cost per move: a few
// decodes of a container.
class RepackPairMove final : public optimaru::MoveOperator<ContainerLoadingProblem> {
public:
    explicit RepackPairMove(const ContainerLoadingProblem& problem) { cache_.reset(problem); }

    const char* name() const override { return "repackPair"; }

    bool apply(optimaru::OperatorContext<ContainerLoadingProblem>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        ContainerLoadingProblem& problem = ctx.editor.solution();
        saved_.clear();
        used_.clear();
        for (std::size_t c = 0; c < problem.containers.size(); ++c) {
            if (!problem.containers[c].items.empty()) used_.push_back(static_cast<int>(c));
        }
        if (used_.size() < 2) return relocateOneItem(ctx, rng, entities, saved_);

        int target[3];
        target[0] = randomOf(used_, rng);
        target[1] = partnerOf(problem, target[0], rng);
        if (optimaru::random_bool(rng, 0.5)) std::swap(target[0], target[1]);
        int targets = 2;
        if (optimaru::random_bool(rng, 0.5)) {
            spare_.clear();
            for (std::size_t c = 0; c < problem.containers.size(); ++c) {
                if (problem.containers[c].items.empty()) spare_.push_back(static_cast<int>(c));
            }
            if (!spare_.empty()) target[targets++] = randomOf(spare_, rng);
        }

        // The items of both, group by group, largest first.
        pool_.clear();
        for (int side = 0; side < 2; ++side) {
            const std::vector<int>& list =
                    problem.containers[static_cast<std::size_t>(target[side])].items;
            pool_.insert(pool_.end(), list.begin(), list.end());
        }
        optimaru::random_shuffle(pool_.begin(), pool_.end(), rng);  // random among equals
        groupVolume_.assign(static_cast<std::size_t>(problem.groupCount), 0.0);
        for (int item : pool_) {
            const int group = problem.groupOf[static_cast<std::size_t>(item)];
            if (group >= 0)
                groupVolume_[static_cast<std::size_t>(group)] +=
                        static_cast<double>(problem.items[static_cast<std::size_t>(item)].volume());
        }
        const auto together = [this, &problem](int item) {
            const int group = problem.groupOf[static_cast<std::size_t>(item)];
            return group >= 0 ? groupVolume_[static_cast<std::size_t>(group)]
                              : static_cast<double>(
                                        problem.items[static_cast<std::size_t>(item)].volume());
        };
        std::stable_sort(pool_.begin(), pool_.end(), [&problem, &together](int a, int b) {
            if (together(a) != together(b)) return together(a) > together(b);
            const int groupA = problem.groupOf[static_cast<std::size_t>(a)];
            const int groupB = problem.groupOf[static_cast<std::size_t>(b)];
            if (groupA != groupB) return groupA < groupB;
            return problem.items[static_cast<std::size_t>(a)].volume() >
                   problem.items[static_cast<std::size_t>(b)].volume();
        });

        // 1. Deal the items out: a group whole where one container has room
        // for it, else item by item.
        double room[3];
        std::int64_t weight[3] = {0, 0, 0};
        for (int t = 0; t < targets; ++t) {
            const double share = optimaru::random_real(rng, 0.70, 1.0);
            room[t] = share *
                      static_cast<double>(
                              problem.containers[static_cast<std::size_t>(target[t])].volume());
            plan_[t].clear();
        }
        const auto takes = [&](int t, double volume, std::int64_t load) {
            const Container& to = problem.containers[static_cast<std::size_t>(target[t])];
            return volume <= room[t] && (!to.maxWeight || weight[t] + load <= *to.maxWeight);
        };
        const auto allowsAll = [&](int t, std::size_t from, std::size_t to) {
            for (std::size_t i = from; i < to; ++i) {
                if (!problem.allowed(pool_[i], target[t])) return false;
            }
            return true;
        };
        const auto give = [&](int t, std::size_t from, std::size_t to) {
            for (std::size_t i = from; i < to; ++i) {
                const Item& box = problem.items[static_cast<std::size_t>(pool_[i])];
                plan_[t].push_back(pool_[i]);
                room[t] -= static_cast<double>(box.volume());
                weight[t] += box.weight;
            }
        };
        for (std::size_t first = 0; first < pool_.size();) {
            const int group = problem.groupOf[static_cast<std::size_t>(pool_[first])];
            std::size_t last = first + 1;
            while (group >= 0 && last < pool_.size() &&
                   problem.groupOf[static_cast<std::size_t>(pool_[last])] == group)
                ++last;
            double volume = 0.0;
            std::int64_t load = 0;
            for (std::size_t i = first; i < last; ++i) {
                volume += static_cast<double>(
                        problem.items[static_cast<std::size_t>(pool_[i])].volume());
                load += problem.items[static_cast<std::size_t>(pool_[i])].weight;
            }
            int whole = -1;
            for (int t = 0; t < targets && whole < 0; ++t) {
                if (takes(t, volume, load) && allowsAll(t, first, last)) whole = t;
            }
            if (whole >= 0) {
                give(whole, first, last);
            } else {
                for (std::size_t i = first; i < last; ++i) {
                    const Item& box = problem.items[static_cast<std::size_t>(pool_[i])];
                    // The first that has room and may take it, else the last it may go
                    // in, else the last.
                    int single = targets - 1;
                    for (int t = targets - 1; t >= 0; --t) {
                        if (problem.allowed(pool_[i], target[t])) {
                            single = t;
                            break;
                        }
                    }
                    for (int t = 0; t < targets - 1; ++t) {
                        if (problem.allowed(pool_[i], target[t]) &&
                            takes(t, static_cast<double>(box.volume()), box.weight)) {
                            single = t;
                            break;
                        }
                    }
                    give(single, i, i + 1);
                }
            }
            first = last;
        }

        // 2. Load each in the construction's order; what one cannot place
        // goes on to the next.
        for (int t = 0; t < targets; ++t) {
            const Container& to = problem.containers[static_cast<std::size_t>(target[t])];
            if (to.maxWeight && weight[t] > *to.maxWeight)
                return relocateOneItem(ctx, rng, entities, saved_);
            if (plan_[t].empty()) continue;
            inLoadingOrder(problem, plan_[t]);
            const Packing& packing = cache_.packing(problem, target[t], plan_[t]);
            if (stackingExcess(problem, packing) + unloadingBlocks(problem, packing) +
                        stackingViolations(problem, packing) + segregationPairs(problem, packing) >
                0)
                return relocateOneItem(ctx, rng, entities, saved_);
            if (packing.unplaced() == 0) continue;
            if (t + 1 == targets) return relocateOneItem(ctx, rng, entities, saved_);
            trial_.clear();
            for (std::size_t position = 0; position < plan_[t].size(); ++position) {
                const int item = plan_[t][position];
                if (packing.boxOf[position] >= 0) {
                    trial_.push_back(item);
                } else {
                    plan_[t + 1].push_back(item);
                    weight[t + 1] += problem.items[static_cast<std::size_t>(item)].weight;
                }
            }
            plan_[t].swap(trial_);
        }
        for (int t = 0; t < targets; ++t) {
            if (plan_[t] != problem.containers[static_cast<std::size_t>(target[t])].items)
                saved_.replace(ctx, target[t], plan_[t]);
        }
        if (saved_.empty()) return relocateOneItem(ctx, rng, entities, saved_);
        entities.insert(entities.end(), pool_.begin(), pool_.end());
        return true;
    }

    void undo_last(optimaru::OperatorContext<ContainerLoadingProblem>& ctx) override {
        saved_.restore(ctx);
    }

private:
    // The construction's order inside a container (loadsBefore).
    static void inLoadingOrder(const ContainerLoadingProblem& problem, std::vector<int>& list) {
        std::stable_sort(list.begin(), list.end(),
                         [&problem](int a, int b) { return loadsBefore(problem, a, b); });
    }

    // Another container in use: one that shares a group with `first` three
    // times out of four, where there is one.
    int partnerOf(const ContainerLoadingProblem& problem, int first, std::mt19937_64& rng) {
        sharing_.clear();
        if (problem.groupCount > 0 && optimaru::random_index(rng, 4) != 0) {
            inFirst_.assign(static_cast<std::size_t>(problem.groupCount), 0);
            for (int item : problem.containers[static_cast<std::size_t>(first)].items) {
                const int group = problem.groupOf[static_cast<std::size_t>(item)];
                if (group >= 0) inFirst_[static_cast<std::size_t>(group)] = 1;
            }
            for (int c : used_) {
                if (c == first) continue;
                for (int item : problem.containers[static_cast<std::size_t>(c)].items) {
                    const int group = problem.groupOf[static_cast<std::size_t>(item)];
                    if (group >= 0 && inFirst_[static_cast<std::size_t>(group)]) {
                        sharing_.push_back(c);
                        break;
                    }
                }
            }
        }
        if (!sharing_.empty()) return randomOf(sharing_, rng);
        int partner = used_[static_cast<std::size_t>(
                optimaru::random_index(rng, static_cast<int>(used_.size()) - 1))];
        if (partner == first) partner = used_.back();
        return partner;
    }

    PackingCache cache_;
    SavedLists saved_;
    std::vector<int> used_;
    std::vector<int> sharing_;
    std::vector<char> inFirst_;
    std::vector<int> pool_;
    std::vector<double> groupVolume_;  // a sum over many items: double, no int64 overflow
    std::vector<int> spare_;
    std::vector<int> plan_[3];
    std::vector<int> trial_;
};

}  // namespace cl

#endif  // CL_SEARCH_MOVES_REPACK_PAIR_HPP
