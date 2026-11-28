#ifndef CL_SEARCH_MOVES_SWITCH_CONTAINER_HPP
#define CL_SEARCH_MOVES_SWITCH_CONTAINER_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "cl/model/domain.hpp"
#include "cl/score/packing.hpp"
#include "cl/search/moves/list_edit.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/utils/random.hpp"

namespace cl {

// ---------------------------------------------------------------------------
// SwitchContainerMove -- a whole load into another kind of container
// ---------------------------------------------------------------------------
// With containers of several sizes and costs (cartons from a catalog), the
// cheapest choice for a load is often a container nothing is in yet, or two
// small ones in place of a large one. No single-item move leads there: the
// first item that goes over opens another container, which costs more before
// it can cost less. This operator moves the load of one container in use at
// once, into empty containers of another size or cost, none of which costs
// more than the one it leaves. Two forms, drawn evenly:
//
//   whole  the loading order goes, as it is, to one such container that is at
//          least as large as the list's volume and can carry its weight.
//          Whether everything still finds a place there is the decoder's
//          answer, which the score judges through the acceptor.
//   split  the items go, in their order, into such containers opened cheapest
//          first: each item into the first opened one that still takes it
//          (its weight limit holds and the decoder places everything), else
//          into the next cheapest that takes it alone. Containers are opened
//          only while together they cost no more than the one that is left.
//
// A source for which the drawn form finds nothing is tried in the other form,
// then the next container in use is tried. When none has such a target, one
// random item is relocated (relocateOneItem) so that the move is judged like
// any other. The solver registers the operator only for a problem whose
// containers differ.
class SwitchContainerMove final : public optimaru::MoveOperator<ContainerLoadingProblem> {
public:
    explicit SwitchContainerMove(const ContainerLoadingProblem& problem) { cache_.reset(problem); }

    const char* name() const override { return "switchContainer"; }

    // Whether two containers of the problem differ in size, cost or weight
    // limit: without that there is nothing to switch to.
    static bool applies(const ContainerLoadingProblem& problem) {
        for (const Container& container : problem.containers) {
            if (differ(container, problem.containers.front())) return true;
        }
        return false;
    }

    bool apply(optimaru::OperatorContext<ContainerLoadingProblem>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        ContainerLoadingProblem& problem = ctx.editor.solution();
        saved_.clear();
        used_.clear();
        empty_.clear();
        for (std::size_t c = 0; c < problem.containers.size(); ++c) {
            (problem.containers[c].items.empty() ? empty_ : used_).push_back(static_cast<int>(c));
        }
        // Every container in use gets one try, from a random one on.
        const auto start = static_cast<std::size_t>(
                optimaru::random_index(rng, static_cast<int>(used_.size())));
        for (std::size_t tried = 0; tried < used_.size() && !empty_.empty(); ++tried) {
            const int source = used_[(start + tried) % used_.size()];
            const Container& from = problem.containers[static_cast<std::size_t>(source)];
            targets_.clear();
            for (int target : empty_) {
                const Container& to = problem.containers[static_cast<std::size_t>(target)];
                if (to.cost <= from.cost && differ(from, to)) targets_.push_back(target);
            }
            if (targets_.empty()) continue;
            const bool wholeFirst = optimaru::random_bool(rng, 0.5);
            const bool moved = wholeFirst ? (whole(ctx, rng, source) || split(ctx, rng, source))
                                          : (split(ctx, rng, source) || whole(ctx, rng, source));
            if (!moved) continue;
            const std::vector<int>& list = saved_.first();
            entities.insert(entities.end(), list.begin(), list.end());
            return true;
        }
        return relocateOneItem(ctx, rng, entities, saved_);
    }

    void undo_last(optimaru::OperatorContext<ContainerLoadingProblem>& ctx) override {
        saved_.restore(ctx);
    }

private:
    static bool differ(const Container& a, const Container& b) {
        return a.length != b.length || a.width != b.width || a.height != b.height ||
               a.cost != b.cost || a.maxWeight != b.maxWeight;
    }

    // The whole list into one of targets_.
    bool whole(optimaru::OperatorContext<ContainerLoadingProblem>& ctx, std::mt19937_64& rng,
               int source) {
        ContainerLoadingProblem& problem = ctx.editor.solution();
        const std::vector<int> list = problem.containers[static_cast<std::size_t>(source)].items;
        std::int64_t volume = 0;
        std::int64_t weight = 0;
        for (int item : list) {
            volume += problem.items[static_cast<std::size_t>(item)].volume();
            weight += problem.items[static_cast<std::size_t>(item)].weight;
        }
        fitting_.clear();
        for (int target : targets_) {
            const Container& to = problem.containers[static_cast<std::size_t>(target)];
            if (to.volume() < volume || (to.maxWeight && weight > *to.maxWeight)) continue;
            fitting_.push_back(target);
        }
        if (fitting_.empty()) return false;
        const int target = randomOf(fitting_, rng);
        saved_.replace(ctx, source, {});
        saved_.replace(ctx, target, list);
        return true;
    }

    // The items, in their order, into targets_ opened cheapest first.
    bool split(optimaru::OperatorContext<ContainerLoadingProblem>& ctx, std::mt19937_64& rng,
               int source) {
        ContainerLoadingProblem& problem = ctx.editor.solution();
        const Container& from = problem.containers[static_cast<std::size_t>(source)];
        const std::vector<int> list = from.items;
        optimaru::random_shuffle(targets_.begin(), targets_.end(), rng);
        std::stable_sort(targets_.begin(), targets_.end(), [&problem](int a, int b) {
            return problem.containers[static_cast<std::size_t>(a)].cost <
                   problem.containers[static_cast<std::size_t>(b)].cost;
        });
        opened_.clear();
        std::size_t next = 0;  // the first of targets_ not opened or passed over yet
        double openedCost = 0.0;
        for (int item : list) {
            const Item& box = problem.items[static_cast<std::size_t>(item)];
            bool placed = false;
            for (Opened& open : opened_) {
                const Container& to = problem.containers[static_cast<std::size_t>(open.container)];
                if (to.maxWeight && open.weight + box.weight > *to.maxWeight) continue;
                trial_ = open.list;
                trial_.push_back(item);
                if (cache_.packing(problem, open.container, trial_).unplaced() > 0) continue;
                open.list.swap(trial_);
                open.weight += box.weight;
                placed = true;
                break;
            }
            // `next` moves on past a container that is opened here too (the
            // increment runs once more after `placed` is set), so no container
            // is opened twice.
            for (; !placed && next < targets_.size(); ++next) {
                const int target = targets_[next];
                const Container& to = problem.containers[static_cast<std::size_t>(target)];
                if (openedCost + to.cost > from.cost) return false;  // dearer than what is left
                if (to.maxWeight && box.weight > *to.maxWeight) continue;
                trial_.assign(1, item);
                if (cache_.packing(problem, target, trial_).unplaced() > 0) continue;
                opened_.push_back({target, box.weight, trial_});
                openedCost += to.cost;
                placed = true;
            }
            if (!placed) return false;
        }
        saved_.replace(ctx, source, {});
        for (const Opened& open : opened_) saved_.replace(ctx, open.container, open.list);
        return true;
    }

    struct Opened {
        int container;
        std::int64_t weight;
        std::vector<int> list;
    };

    PackingCache cache_;
    SavedLists saved_;
    std::vector<int> used_;
    std::vector<int> empty_;
    std::vector<int> targets_;
    std::vector<int> fitting_;
    std::vector<int> trial_;
    std::vector<Opened> opened_;
};

}  // namespace cl

#endif  // CL_SEARCH_MOVES_SWITCH_CONTAINER_HPP
