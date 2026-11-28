#include "bs/construct/construction.hpp"

#include <algorithm>
#include <cstdint>
#include <queue>
#include <utility>
#include <vector>

namespace bs {

namespace {

constexpr int kNone = -1;

struct Greedy {
    BoxSizingProblem& problem;
    const BoxRanking& ranking;
    std::vector<char> open;   // per box
    std::vector<int> free;    // the parcels to place
    std::vector<int> target;  // per parcel: its cheapest open box, kNone = none takes it
    int types = 0;
    int uncovered = 0;

    Greedy(BoxSizingProblem& p, const BoxRanking& r)
        : problem(p), ranking(r), open(p.boxes.size(), 0), target(p.parcels.size(), kNone) {
        for (int b = 0; b < problem.boxCount(); ++b) open[b] = problem.boxes[b].required ? 1 : 0;
        for (int p = 0; p < problem.parcelCount(); ++p) {
            const int box = problem.parcels[p].box;
            if (box >= 0) {
                open[box] = 1;
            } else {
                free.push_back(p);
            }
        }
        for (char o : open) types += o;
        for (int p : free) {
            for (int box : ranking.orderOf(ranking.groupOf[p])) {
                if (open[box] && problem.fits(p, box)) {
                    target[p] = box;
                    break;
                }
            }
            if (target[p] == kNone) ++uncovered;
        }
    }

    bool atLimit() const { return problem.maxBoxTypes && types >= *problem.maxBoxTypes; }

    // Would the parcel move to `box` if it opened?
    bool wouldMove(int parcel, int box) const {
        const int now = target[parcel];
        const int group = ranking.groupOf[parcel];
        return (now == kNone || ranking.rankOf(group, box) < ranking.rankOf(group, now)) &&
               problem.fits(parcel, box);
    }

    void openBox(int box) {
        open[box] = 1;
        ++types;
        for (int p : free) {
            if (!wouldMove(p, box)) continue;
            if (target[p] == kNone) --uncovered;
            target[p] = box;
        }
    }

    // Step 2: the closed size that takes the most parcels nothing open takes.
    int bestCover() const {
        int best = kNone;
        int bestCount = 0;
        for (int box : ranking.order) {  // cheapest first, so a tie keeps the cheaper size
            if (open[box]) continue;
            int count = 0;
            for (int p : free) {
                if (target[p] == kNone && problem.fits(p, box)) ++count;
            }
            if (count > bestCount) {
                best = box;
                bestCount = count;
            }
        }
        return best;
    }

    // Step 3: what opening the size saves, in the units of the soft level.
    double saving(int box) const {
        double saved = 0.0;
        for (int p : free) {
            if (target[p] == kNone || !wouldMove(p, box)) continue;
            const int group = ranking.groupOf[p];
            saved += static_cast<double>(problem.parcels[p].quantity) *
                     (ranking.priceOf(group, target[p]) - ranking.priceOf(group, box));
        }
        return saved - ranking.typeWeight;
    }
};

}  // namespace

void constructGreedy(BoxSizingProblem& problem, const BoxRanking& ranking) {
    Greedy greedy(problem, ranking);
    if (greedy.free.empty()) return;

    while (greedy.uncovered > 0 && !greedy.atLimit()) {
        const int box = greedy.bestCover();
        if (box == kNone) break;  // no candidate takes what is left
        greedy.openBox(box);
    }

    // Lazy greedy: the largest bound first; a bound is a saving computed in
    // an earlier round, and a saving never grows.
    using Entry = std::pair<double, int>;  // (saving, -box): ties to the lower index
    std::priority_queue<Entry> queue;
    for (int box = 0; box < problem.boxCount(); ++box) {
        if (greedy.open[box]) continue;
        const double saved = greedy.saving(box);
        if (saved > 0.0) queue.emplace(saved, -box);
    }
    while (!queue.empty() && !greedy.atLimit()) {
        const int box = -queue.top().second;
        queue.pop();
        const double saved = greedy.saving(box);
        if (saved <= 0.0) continue;
        if (!queue.empty() && Entry(saved, -box) < queue.top()) {
            queue.emplace(saved, -box);  // no longer the largest: back in line
            continue;
        }
        greedy.openBox(box);
    }

    for (int p : greedy.free) {
        int box = greedy.target[p];
        if (box == kNone) box = ranking.cheapestFor(problem, p);
        if (box == kNone) {
            // No candidate takes it: the one where it adds the least to the
            // hard score (the two hard costs at their default weights). A
            // solve never gets here, since it refuses such a parcel
            // (requireSolvable); this is for a caller of constructGreedy
            // alone, which gets a complete assignment either way.
            std::int64_t least = 0;
            for (int candidate : ranking.order) {
                const std::int64_t excess =
                        problem.fitExcess(p, candidate) + problem.weightExcess(p, candidate);
                if (box == kNone || excess < least) {
                    box = candidate;
                    least = excess;
                }
            }
        }
        problem.parcels[p].box = box;
    }
}

}  // namespace bs
