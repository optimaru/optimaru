// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_MODEL_SOLUTION_TRAITS_HPP
#define OPTIMARU_MODEL_SOLUTION_TRAITS_HPP

#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace optimaru {

// Which shape a solution's decisions take.
//   Assignment  every entity takes one value from [0, value_count)
//   Sequence    elements are placed, in order, into one of several lists
//   Chain       every entity links to a predecessor: another entity or an anchor
enum class Shape { Assignment, Sequence, Chain };

// ---------------------------------------------------------------------------
// SolutionTraits
// ---------------------------------------------------------------------------
// How the engine reads and writes a solution without knowing its types. A use
// case specializes this template for its solution class, says which Shape it
// has, and provides the matching static hooks.
//
// Assignment: one int value per entity, in [0, value_count); -1 = unassigned.
//
//   template <>
//   struct optimaru::SolutionTraits<MySolution> {
//       static constexpr optimaru::Shape shape = optimaru::Shape::Assignment;
//       static int entity_count(const MySolution&);
//       static int value_count(const MySolution&);
//       static int value_of(const MySolution&, int entity_index);
//       static void assign(MySolution&, int entity_index, int value);
//       // optional, see below:
//       static bool is_pinned(const MySolution&, int entity_index);
//   };
//
// Sequence: list_count lists of element ids (e.g. vehicles holding visit
// sequences); every element id in [0, element_count) is in at most one list,
// and one in no list is unassigned.
//
//   template <>
//   struct optimaru::SolutionTraits<MySolution> {
//       static constexpr optimaru::Shape shape = optimaru::Shape::Sequence;
//       static int list_count(const MySolution&);
//       static int element_count(const MySolution&);
//       static int list_size(const MySolution&, int list_index);
//       static int element_at(const MySolution&, int list_index, int position);
//       static void insert_element(MySolution&, int list_index, int position, int element);
//       static int remove_element(MySolution&, int list_index, int position);  // -> element
//       static void reverse_segment(MySolution&, int list_index, int first, int last);
//   };
//
// Chain: every entity links to its predecessor, which is another entity, an
// anchor (encoded with chain::anchor) or chain::kUnlinked. The engine works on
// a list view of the chains (ChainView) and keeps the solution's links in
// step through every move, so all sequence moves apply. Its objective is a
// ChainObjective.
//
//   template <>
//   struct optimaru::SolutionTraits<MySolution> {
//       static constexpr optimaru::Shape shape = optimaru::Shape::Chain;
//       static int anchor_count(const MySolution&);
//       static int entity_count(const MySolution&);
//       static int previous_of(const MySolution&, int entity_index);
//       static void link(MySolution&, int entity_index, int previous);
//   };
template <typename Solution>
struct SolutionTraits;  // no primary definition: each solution specializes it

// ---------------------------------------------------------------------------
// Pinning (assignment shape, optional)
// ---------------------------------------------------------------------------
// Traits MAY declare `static bool is_pinned(const Solution&, int entity)`. A
// pinned entity keeps its current value and counts toward every constraint,
// but is never moved, picked as a swap partner or ruined. It must have a
// value: solving throws std::invalid_argument for a pinned entity that is
// unassigned. With every entity pinned there is no local search at all.
// Traits without the hook pin nothing.
namespace detail {
template <class T, class S, class = void>
struct has_is_pinned : std::false_type {};
template <class T, class S>
struct has_is_pinned<T, S, std::void_t<decltype(T::is_pinned(std::declval<const S&>(), 0))>>
    : std::true_type {};
}  // namespace detail

template <class Traits, class Solution>
inline bool entity_is_pinned(const Solution& solution, int entity) {
    if constexpr (detail::has_is_pinned<Traits, Solution>::value) {
        return Traits::is_pinned(solution, entity);
    } else {
        return false;
    }
}

// A pinned entity keeps its value, so it needs one. Runs on the calling thread,
// before any solver thread starts: an exception escaping one ends the process.
template <class Traits, class Solution>
inline void require_pinned_values(const Solution& solution) {
    if constexpr (Traits::shape == Shape::Assignment &&
                  detail::has_is_pinned<Traits, Solution>::value) {
        const int n = Traits::entity_count(solution);
        for (int e = 0; e < n; ++e) {
            if (Traits::is_pinned(solution, e) && Traits::value_of(solution, e) < 0) {
                throw std::invalid_argument("entity " + std::to_string(e) +
                                            " is pinned but has no value");
            }
        }
    }
}

// The chain shape's `previous` encoding: another entity (>= 0), an anchor
// (encoded negative), or kUnlinked.
namespace chain {
constexpr int kUnlinked = std::numeric_limits<int>::min();
constexpr int anchor(int anchor_index) { return -(anchor_index + 1); }
constexpr bool is_anchor(int previous) { return previous < 0 && previous != kUnlinked; }
constexpr int anchor_index(int previous) { return -previous - 1; }
}  // namespace chain

}  // namespace optimaru

#endif  // OPTIMARU_MODEL_SOLUTION_TRAITS_HPP
