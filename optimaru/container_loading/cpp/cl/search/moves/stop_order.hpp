#ifndef CL_SEARCH_MOVES_STOP_ORDER_HPP
#define CL_SEARCH_MOVES_STOP_ORDER_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "cl/model/domain.hpp"
#include "cl/search/moves/list_edit.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/utils/random.hpp"

namespace cl {

// ---------------------------------------------------------------------------
// StopOrderMove -- one container's loading order, kept in stop blocks
// ---------------------------------------------------------------------------
// A vehicle load is one container with items for many stops, and the order
// it is loaded in decides where every stop ends up. The decoder keeps a stop
// out of an earlier stop's way where the room allows, but only a loading
// order in stop blocks (what leaves last first) lets it fill the room behind
// each stop before the next is loaded. The engine's sequence moves know
// nothing of stops: a relocate or a swap mixes the blocks, and once mixed the
// load is rarely put back in order one item at a time.
//
// This operator takes a container whose list holds items with an unloading
// order and rewrites its list:
//
//   1. The items with an unloadOrder are put back in stop blocks, the latest
//      stop first (a stable sort; the items without an order keep their
//      places, since they are in nobody's way).
//   2. Then, drawn uniformly, one change inside one stop's block:
//        - move one item to another place in its block;
//        - swap two items of the block;
//        - order the block afresh by one of the keys a wall is built by:
//          largest first, tallest first, largest base first, deepest first,
//          or at random.
//
// So a container mixed by other moves comes back in blocks, and a container
// in blocks tries another way to build one stop's wall. What the change does
// to the costs is the acceptor's to judge. A container with one stop and one
// item has nothing to change; when no container has an order to work on,
// the move relocates one random item instead (relocateOneItem), so that it is
// judged like any other.
//
// Cost per move: a decode of the container from the first place it changes.
class StopOrderMove final : public optimaru::MoveOperator<ContainerLoadingProblem> {
public:
    const char* name() const override { return "stopOrder"; }

    // Registered only when some item sets an unloadOrder.
    static bool applies(const ContainerLoadingProblem& problem) { return problem.anyUnloadOrder; }

    bool apply(optimaru::OperatorContext<ContainerLoadingProblem>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        ContainerLoadingProblem& problem = ctx.editor.solution();
        saved_.clear();
        candidates_.clear();
        for (std::size_t c = 0; c < problem.containers.size(); ++c) {
            int ordered = 0;
            for (int item : problem.containers[c].items) {
                if (hasOrder(problem, item)) ++ordered;
            }
            if (ordered >= 2) candidates_.push_back(static_cast<int>(c));
        }
        if (candidates_.empty()) return relocateOneItem(ctx, rng, entities, saved_);
        const int container = randomOf(candidates_, rng);
        const std::vector<int>& current =
                problem.containers[static_cast<std::size_t>(container)].items;

        // 1. Stop blocks: the ordered items, latest stop first, into the
        // places ordered items held.
        list_ = current;
        places_.clear();
        ordered_.clear();
        for (std::size_t position = 0; position < list_.size(); ++position) {
            if (!hasOrder(problem, list_[position])) continue;
            places_.push_back(position);
            ordered_.push_back(list_[position]);
        }
        std::stable_sort(ordered_.begin(), ordered_.end(), [&problem](int a, int b) {
            return orderOf(problem, a) > orderOf(problem, b);
        });

        // 2. One change inside one block: [first, last) of ordered_. A draw
        // that changes nothing (a block of one, an order the block already
        // has) is drawn again, a few times.
        blocks_ = ordered_;
        std::size_t first = 0;
        std::size_t last = 0;
        for (int attempt = 0; attempt < 4; ++attempt) {
            ordered_ = blocks_;
            const int pick = optimaru::random_index(rng, static_cast<int>(ordered_.size()));
            const int stop = orderOf(problem, ordered_[static_cast<std::size_t>(pick)]);
            first = static_cast<std::size_t>(pick);
            while (first > 0 && orderOf(problem, ordered_[first - 1]) == stop) --first;
            last = static_cast<std::size_t>(pick) + 1;
            while (last < ordered_.size() && orderOf(problem, ordered_[last]) == stop) ++last;
            changeBlock(problem, rng, first, last);
            for (std::size_t i = 0; i < places_.size(); ++i) list_[places_[i]] = ordered_[i];
            if (list_ != current) break;
        }
        if (list_ == current) return relocateOneItem(ctx, rng, entities, saved_);
        // Every item whose place changed, the sort back into blocks included.
        for (std::size_t position = 0; position < list_.size(); ++position) {
            if (list_[position] != current[position]) entities.push_back(list_[position]);
        }
        saved_.replace(ctx, container, list_);
        return true;
    }

    void undo_last(optimaru::OperatorContext<ContainerLoadingProblem>& ctx) override {
        saved_.restore(ctx);
    }

private:
    // One random change of ordered_[first, last), a block of one stop.
    void changeBlock(const ContainerLoadingProblem& problem, std::mt19937_64& rng,
                     std::size_t first, std::size_t last) {
        const int size = static_cast<int>(last - first);
        if (size < 2) return;
        const auto begin = ordered_.begin() + static_cast<std::ptrdiff_t>(first);
        const auto end = ordered_.begin() + static_cast<std::ptrdiff_t>(last);
        const int kind = optimaru::random_index(rng, 3);
        if (kind == 0) {
            // Move one item to another place of its block.
            const int from = optimaru::random_index(rng, size);
            int to = optimaru::random_index(rng, size - 1);
            if (to >= from) ++to;
            const int item = *(begin + from);
            ordered_.erase(begin + from);
            ordered_.insert(ordered_.begin() + static_cast<std::ptrdiff_t>(first) + to, item);
        } else if (kind == 1) {
            const int a = optimaru::random_index(rng, size);
            int b = optimaru::random_index(rng, size - 1);
            if (b >= a) ++b;
            std::swap(*(begin + a), *(begin + b));
        } else {
            const int key = optimaru::random_index(rng, 5);
            if (key == 4) {
                optimaru::random_shuffle(begin, end, rng);
            } else {
                std::stable_sort(begin, end, [&problem, key](int a, int b) {
                    return keyOf(problem, a, key) > keyOf(problem, b, key);
                });
            }
        }
    }

    static bool hasOrder(const ContainerLoadingProblem& problem, int item) {
        return problem.items[static_cast<std::size_t>(item)].unloadOrder.has_value();
    }
    // The unloadOrder of an item that has one.
    static int orderOf(const ContainerLoadingProblem& problem, int item) {
        return *problem.items[static_cast<std::size_t>(item)].unloadOrder;
    }

    // The keys a block is ordered by, larger first.
    static std::int64_t keyOf(const ContainerLoadingProblem& problem, int item, int key) {
        const Item& box = problem.items[static_cast<std::size_t>(item)];
        switch (key) {
            case 0:
                return box.volume();
            case 1:
                return box.height;
            case 2:
                return static_cast<std::int64_t>(box.length) * box.width;
            default:
                return std::max(box.length, box.width);
        }
    }

    SavedLists saved_;
    std::vector<int> candidates_;
    std::vector<int> list_;
    std::vector<std::size_t> places_;
    std::vector<int> ordered_;
    std::vector<int> blocks_;
};

}  // namespace cl

#endif  // CL_SEARCH_MOVES_STOP_ORDER_HPP
