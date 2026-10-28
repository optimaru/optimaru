#ifndef OPTIMARU_API_DOMAIN_VARIABLE_VARIABLE_LISTENER_HPP
#define OPTIMARU_API_DOMAIN_VARIABLE_VARIABLE_LISTENER_HPP

namespace optimaru {

// ---------------------------------------------------------------------------
// VariableListener  (shadow variables)
// ---------------------------------------------------------------------------
// Mirrors api.domain.variable.VariableListener: maintains SHADOW variables —
// values on the solution model that are derived from the genuine planning
// variables (e.g. a visit's arrival time derived from its position in a
// route). The ScoreDirector invokes listeners around every genuine-variable
// mutation, so the score calculator can simply READ shadow fields and trust
// them:
//
//   calculator.before*Changed(...)   // retract, OLD shadow values still valid
//   <genuine variable mutation>
//   listener.after*Changed(...)      // recompute the affected shadow values
//   calculator.after*Changed(...)    // insert, NEW shadow values valid
//
// resetWorkingSolution() must rebuild every shadow value from scratch; it runs
// before the calculator's reset (at solve start, after an LNS best-restore,
// and on every FULL_ASSERT verification — so a buggy listener surfaces as
// ScoreCorruptionError).
//
// Register with Solver::addVariableListener(); multiple listeners run in
// registration order (upstream's dependency-graph ordering is not ported).
// Shadow values of unassigned elements are unspecified — a calculator must
// not read them.
template <typename Solution>
class VariableListener {
public:
    virtual ~VariableListener() = default;

    virtual void resetWorkingSolution(Solution& workingSolution) = 0;

    // BASIC kind: entityIndex's genuine variable just changed.
    virtual void afterVariableChanged(Solution& /*workingSolution*/, int /*entityIndex*/) {}

    // LIST kind: listIndex's contents just changed.
    virtual void afterListChanged(Solution& /*workingSolution*/, int /*listIndex*/) {}
};

}  // namespace optimaru

#endif  // OPTIMARU_API_DOMAIN_VARIABLE_VARIABLE_LISTENER_HPP
