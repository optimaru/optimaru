// optimaru::GroupPlacement, the bookkeeping behind the "these elements stay
// together" constraints of the routing use cases:
//   (a) the count by hand: whole, split, spread, waiting, a group of one
//   (b) a random walk of list changes, reported in either order, against a
//       fresh count; a preview against the count after applying it
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "optimaru/score/group_placement.hpp"
#include "optimaru/testing/check.hpp"

namespace {

using optimaru::GroupPlacement;
using optimaru::testing::check;
using optimaru::testing::failures;

// The elements not where most of their group is, counted from scratch. Lists
// past `counted` are "nowhere".
std::int64_t apartFromScratch(const std::vector<int>& groupOf,
                              const std::vector<std::vector<int>>& lists, int counted) {
    std::vector<int> place(groupOf.size(), -1);
    for (int list = 0; list < counted; ++list) {
        for (int element : lists[static_cast<std::size_t>(list)])
            place[static_cast<std::size_t>(element)] = list;
    }
    const int groups = groupOf.empty() ? 0 : *std::max_element(groupOf.begin(), groupOf.end()) + 1;
    std::int64_t total = 0;
    for (int group = 0; group < groups; ++group) {
        std::vector<int> places;
        for (std::size_t element = 0; element < groupOf.size(); ++element) {
            if (groupOf[element] == group) places.push_back(place[element]);
        }
        if (places.size() < 2) continue;
        std::sort(places.begin(), places.end());
        std::size_t most = 0;
        for (std::size_t i = 0; i < places.size();) {
            std::size_t j = i;
            while (j < places.size() && places[j] == places[i]) ++j;
            most = std::max(most, j - i);
            i = j;
        }
        total += static_cast<std::int64_t>(places.size() - most);
    }
    return total;
}

void verifyByHand() {
    std::printf("== (a) The count by hand ==\n");
    // Elements 0 1 2 are group 5, element 3 is alone in group 9, 4 and 5 are in none.
    const std::vector<int> groupOf = {5, 5, 5, 9, -1, -1};
    const auto apart = [&groupOf](std::vector<std::vector<int>> lists) {
        GroupPlacement placement;
        placement.reset(groupOf, lists, /*counted_lists=*/3);
        return placement.apart();
    };
    // Three lists that are places, then the pool.
    check(apart({{0, 1, 2, 3}, {4}, {5}, {}}) == 0, "the group on one list: whole");
    check(apart({{3}, {4}, {5}, {0, 1, 2}}) == 0, "all of it in the pool: whole");
    check(apart({{0, 1}, {2}, {}, {3, 4, 5}}) == 1, "one element on another list: 1 apart");
    check(apart({{0, 1}, {}, {}, {2, 3, 4, 5}}) == 1, "one element waiting: 1 apart");
    check(apart({{0}, {}, {}, {1, 2, 3, 4, 5}}) == 1, "one riding while two wait: 1 apart");
    check(apart({{0}, {1}, {2}, {3, 4, 5}}) == 2, "spread over three lists: 2 apart");
    check(apart({{0}, {1}, {}, {2, 3, 4, 5}}) == 2, "two lists and the pool: 2 apart");

    GroupPlacement none;
    none.reset({-1, -1, 7}, {{0, 1}, {2}}, 2);
    check(none.empty() && none.apart() == 0, "a group of one is nothing to keep together");
    std::printf("\n");
}

void verifyRandomWalk() {
    std::printf("== (b) Changes and previews against a fresh count ==\n");
    constexpr int kElements = 40;
    constexpr int kLists = 6;  // the last one is the pool
    constexpr int kCounted = 5;
    std::mt19937_64 rng(7);
    std::vector<int> groupOf(kElements, -1);
    for (int element = 0; element < 30; ++element)
        groupOf[static_cast<std::size_t>(element)] = element / 4;
    std::vector<std::vector<int>> lists(kLists);
    for (int element = 0; element < kElements; ++element) lists[rng() % kLists].push_back(element);

    GroupPlacement placement;
    placement.reset(groupOf, lists, kCounted);
    bool changesEqual = placement.apart() == apartFromScratch(groupOf, lists, kCounted);
    bool previewsEqual = true;
    int moves = 0;
    for (int step = 0; step < 4000; ++step) {
        const auto from = static_cast<std::size_t>(rng() % kLists);
        const auto to = static_cast<std::size_t>(rng() % kLists);
        if (from == to || lists[from].empty()) continue;
        std::vector<int> fromAfter = lists[from];
        std::vector<int> toAfter = lists[to];
        const auto at = static_cast<std::ptrdiff_t>(rng() % fromAfter.size());
        toAfter.push_back(fromAfter[static_cast<std::size_t>(at)]);
        fromAfter.erase(fromAfter.begin() + at);

        // The preview of the move, before anything is written.
        const optimaru::PreviewedUnit changes[2] = {
                {static_cast<int>(from), &fromAfter, nullptr, 0},
                {static_cast<int>(to), &toAfter, nullptr, 0}};
        const std::int64_t before = placement.apart();
        const std::int64_t previewed = placement.preview(changes, 2);
        previewsEqual = previewsEqual && placement.apart() == before;  // a preview writes nothing

        lists[from] = fromAfter;
        lists[to] = toAfter;
        // The two lists are reported in either order.
        if (step % 2 == 0) {
            placement.on_list_changed(static_cast<int>(from), lists[from]);
            placement.on_list_changed(static_cast<int>(to), lists[to]);
        } else {
            placement.on_list_changed(static_cast<int>(to), lists[to]);
            placement.on_list_changed(static_cast<int>(from), lists[from]);
        }
        const std::int64_t fresh = apartFromScratch(groupOf, lists, kCounted);
        changesEqual = changesEqual && placement.apart() == fresh;
        previewsEqual = previewsEqual && previewed == fresh;
        ++moves;
    }
    std::printf("    %d moves\n", moves);
    check(changesEqual && moves > 2000,
          "the count after every change equals a fresh count, whichever list is reported first");
    check(previewsEqual, "every preview equals the count after the move, and writes nothing");
    std::printf("\n");
}

}  // namespace

int main() {
    verifyByHand();
    verifyRandomWalk();
    std::printf("%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
