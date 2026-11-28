#ifndef CL_SEARCH_MOVES_LIST_EDIT_HPP
#define CL_SEARCH_MOVES_LIST_EDIT_HPP

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "cl/construct/construction.hpp"
#include "cl/model/domain.hpp"
#include "optimaru/search/operators.hpp"
#include "optimaru/utils/random.hpp"

namespace cl {

// What the domain's moves share: writing whole lists through the editor,
// taking them back, drawing one of a list, and the small move they fall back
// on.
//
// In the moves a container is "in use" when its list is not empty, placed or
// not; usedContainerCost and containers_used() count only containers with a
// placed item. Not a bug: the acceptor judges every move by the score.
//
// The three moves keep decodes of their own (a PackingCache each), reset for
// the problem they are built with. The solver builds its moves per solve
// (cl/solver.cpp), so the facts cannot change under them; a move reused for
// another problem would have to reset its cache first.

// One of `list`'s elements, uniformly (the list is not empty).
template <typename T>
const T& randomOf(const std::vector<T>& list, std::mt19937_64& rng) {
    return list[static_cast<std::size_t>(
            optimaru::random_index(rng, static_cast<int>(list.size())))];
}

// The lists a move replaced, as they were, so undo_last() can put them back.
class SavedLists {
public:
    void clear() { saved_.clear(); }
    bool empty() const { return saved_.empty(); }
    // What the first list replaced since clear() held.
    const std::vector<int>& first() const { return saved_.front().second; }

    // Replace container `list`'s loading order by `items`, through the
    // editor, remembering what it held.
    void replace(optimaru::OperatorContext<ContainerLoadingProblem>& ctx, int list,
                 const std::vector<int>& items) {
        ContainerLoadingProblem& problem = ctx.editor.solution();
        std::vector<int>& current = problem.containers[static_cast<std::size_t>(list)].items;
        saved_.emplace_back(list, current);
        ctx.editor.begin_list_edit(list);
        current = items;
        ctx.editor.end_list_edit(list);
    }

    void restore(optimaru::OperatorContext<ContainerLoadingProblem>& ctx) {
        ContainerLoadingProblem& problem = ctx.editor.solution();
        for (auto saved = saved_.rbegin(); saved != saved_.rend(); ++saved) {
            ctx.editor.begin_list_edit(saved->first);
            problem.containers[static_cast<std::size_t>(saved->first)].items = saved->second;
            ctx.editor.end_list_edit(saved->first);
        }
        saved_.clear();
    }

private:
    std::vector<std::pair<int, std::vector<int>>> saved_;
};

// The last resort of a move that finds nothing of its own to do: one random
// item goes to another container that is in use, at the place its unloading
// order asks for. A move that answered "nothing possible" instead would be
// drawn again and again without ever being judged. False only when no other
// container is in use, or when eight draws found no item that is in a list
// (none is, or nearly none while a solution is being rebuilt).
inline bool relocateOneItem(optimaru::OperatorContext<ContainerLoadingProblem>& ctx,
                            std::mt19937_64& rng, std::vector<int>& entities, SavedLists& saved) {
    ContainerLoadingProblem& problem = ctx.editor.solution();
    const int itemCount = static_cast<int>(problem.items.size());
    if (itemCount == 0) return false;
    int item = -1;
    for (int attempt = 0; attempt < 8 && item < 0; ++attempt) {
        const int drawn = optimaru::random_index(rng, itemCount);
        if (ctx.element_list[static_cast<std::size_t>(drawn)] >= 0) item = drawn;
    }
    if (item < 0) return false;
    const int source = ctx.element_list[static_cast<std::size_t>(item)];
    std::vector<int> others;
    for (std::size_t c = 0; c < problem.containers.size(); ++c) {
        if (static_cast<int>(c) != source && !problem.containers[c].items.empty())
            others.push_back(static_cast<int>(c));
    }
    if (others.empty()) return false;
    const int target = randomOf(others, rng);
    std::vector<int> from = problem.containers[static_cast<std::size_t>(source)].items;
    from.erase(from.begin() + ctx.element_position[static_cast<std::size_t>(item)]);
    std::vector<int> to = problem.containers[static_cast<std::size_t>(target)].items;
    to.insert(to.begin() + insertPosition(problem, to, item), item);
    saved.clear();
    saved.replace(ctx, source, from);
    saved.replace(ctx, target, to);
    entities.push_back(item);
    return true;
}

}  // namespace cl

#endif  // CL_SEARCH_MOVES_LIST_EDIT_HPP
