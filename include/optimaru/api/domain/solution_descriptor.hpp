#ifndef OPTIMARU_API_DOMAIN_SOLUTION_DESCRIPTOR_HPP
#define OPTIMARU_API_DOMAIN_SOLUTION_DESCRIPTOR_HPP

#include <limits>
#include <type_traits>
#include <utility>

namespace optimaru {

// ---------------------------------------------------------------------------
// Optional pinning hook.
// ---------------------------------------------------------------------------
// A descriptor MAY declare `static bool isPinned(const Solution&, int entity)`
// to mark an entity as fixed: it keeps its current value, is counted by every
// constraint, but is never selected for a move, a swap partner, or a ruin. A
// descriptor that omits it pins nothing (default false) — so existing
// specializations (and other quickstarts) are unaffected.
namespace detail {
template <class D, class S, class = void>
struct has_is_pinned : std::false_type {};
template <class D, class S>
struct has_is_pinned<D, S, std::void_t<decltype(D::isPinned(std::declval<const S&>(), 0))>>
    : std::true_type {};
}  // namespace detail

template <class Descriptor, class Solution>
inline bool entityPinned(const Solution& solution, int entity) {
    if constexpr (detail::has_is_pinned<Descriptor, Solution>::value) {
        return Descriptor::isPinned(solution, entity);
    } else {
        return false;
    }
}

// Which planning-variable shape a solution uses (Timefold's basic
// @PlanningVariable vs @PlanningListVariable vs chained @PlanningVariable).
enum class VariableKind { BASIC, LIST, CHAINED };

// Encoding helpers for the CHAINED kind's `previous` variable: it points at
// either another entity (>= 0), an anchor (encoded negative), or nothing.
namespace chained {
constexpr int kUnassignedPrevious = std::numeric_limits<int>::min();
constexpr int anchor(int anchorIndex) { return -(anchorIndex + 1); }
constexpr bool isAnchor(int previous) {
    return previous < 0 && previous != kUnassignedPrevious;
}
constexpr int anchorIndex(int previous) { return -previous - 1; }
}  // namespace chained

// ---------------------------------------------------------------------------
// SolutionDescriptor
// ---------------------------------------------------------------------------
// The C++ stand-in for Timefold's @PlanningSolution / @PlanningEntity /
// @PlanningVariable annotations: it tells the engine how to enumerate the
// planning entities and read/write their planning variable, without the engine
// knowing the domain types. A quickstart specializes this template for its
// solution class and declares which variable kind it uses.
//
// BASIC — one int-valued planning variable per entity, value range
// [0, valueCount); -1 means unassigned (uninitialized):
//
//   template <>
//   struct optimaru::SolutionDescriptor<MySolution> {
//       static constexpr optimaru::VariableKind kind = optimaru::VariableKind::BASIC;
//       static int entityCount(const MySolution&);
//       static int valueCount(const MySolution&);
//       static int getVariable(const MySolution&, int entityIndex);
//       static void setVariable(MySolution&, int entityIndex, int value);
//   };
//
// LIST — the entities each hold an ordered list of element ids (e.g. vehicles
// holding visit sequences); every element id in [0, elementCount) appears in
// at most one list. Elements missing from all lists are unassigned:
//
//   template <>
//   struct optimaru::SolutionDescriptor<MySolution> {
//       static constexpr optimaru::VariableKind kind = optimaru::VariableKind::LIST;
//       static int listCount(const MySolution&);
//       static int elementCount(const MySolution&);
//       static int listSize(const MySolution&, int listIndex);
//       static int getElement(const MySolution&, int listIndex, int position);
//       static void insertElement(MySolution&, int listIndex, int position, int element);
//       static int removeElement(MySolution&, int listIndex, int position);  // -> element
//       static void reverseSegment(MySolution&, int listIndex, int first, int last);
//   };
//
// CHAINED — the classic linked-chain model (Timefold's chained
// @PlanningVariable): every entity's variable points at its predecessor —
// another entity, an anchor (encoded with chained::anchor), or
// chained::kUnassignedPrevious. Internally the engine works on a materialized
// list view (ChainedWorkingSolution) and keeps the domain's previous pointers
// synchronized through every move, so the full LIST move repertoire applies.
// Constraints are written as a ChainedIncrementalScoreCalculator:
//
//   template <>
//   struct optimaru::SolutionDescriptor<MySolution> {
//       static constexpr optimaru::VariableKind kind = optimaru::VariableKind::CHAINED;
//       static int anchorCount(const MySolution&);
//       static int entityCount(const MySolution&);
//       static int getPrevious(const MySolution&, int entityIndex);
//       static void setPrevious(MySolution&, int entityIndex, int previous);
//   };
template <typename Solution>
struct SolutionDescriptor;  // no primary definition: each solution specializes it

}  // namespace optimaru

#endif  // OPTIMARU_API_DOMAIN_SOLUTION_DESCRIPTOR_HPP
