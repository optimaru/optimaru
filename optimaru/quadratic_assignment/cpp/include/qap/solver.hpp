#ifndef QAP_SOLVER_HPP
#define QAP_SOLVER_HPP

#include <utility>

#include "optimaru/score/decomposed_objective.hpp"
#include "optimaru/model/editor.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/solver_config.hpp"
#include "qap/domain.hpp"

namespace qap {

// Thin domain-side wrapper around the generic engine in optimaru/cpp/optimaru/: it
// starts from a complete permutation (the identity, or a warm-start seed that
// must be a bijection), wires up the incremental score calculator and
// delegates the search to optimaru::Solver<QuadraticAssignmentProblem> —
// SWAP moves only (two facilities exchange their locations), so every state
// the search visits is a permutation, judged by the configured acceptor
// (late acceptance by default).
//
// Deliberately NOT exposed, because each assigns values one entity at a time
// and would break the permutation: change moves (the engine also falls back
// to a change move when the swap weight is 0), ruin-and-recreate, descent,
// value recommenders, ejection chains and iterated local search. The final
// state is checked to be a bijection (std::logic_error otherwise).
class Solver {
public:
    // The limits, the seed, the parallel starts and the callback are the
    // shared ones of optimaru/solver_config.hpp; there are no ruin & recreate
    // knobs (see above). Here:
    //   time_limit 0           the solve returns its start (the identity, or
    //                          the seed) unchanged
    //   history_length 0       clamp(8 * facility count, 400, 3200)
    //   parallel_solver_count  the engine's multi-start; ignored by the
    //                          constraint-assembly overload
    // The acceptance rule and its parameters are the shared AcceptanceConfig
    // (late acceptance by default).
    struct Config : optimaru::SolverConfigBase<Score>, optimaru::AcceptanceConfig {};

    Solver() = default;
    explicit Solver(Config config) : config_(std::move(config)) {}

    // Solves in place: prepares (validating the facts), starts from the
    // identity or — with problem.warmStart — from the current assignment (a
    // full bijection, else std::invalid_argument), searches with swaps,
    // writes the best permutation and problem.score (a full recompute), and
    // resets problem.warmStart to false.
    void solve(QuadraticAssignmentProblem& problem);

    // Solves with a caller-assembled plugin calculator (see
    // qap/constraints.hpp): pick components and weights, then pass the
    // assembly here. The same start and search apply. The final problem.score
    // is computed by THIS calculator and the calculator is left primed on the
    // final solution, so constraint_totals() explains exactly that score.
    // Single-start: the caller-owned calculator cannot be cloned, so
    // parallel_solver_count is ignored.
    void solve(QuadraticAssignmentProblem& problem,
               optimaru::DecomposedObjective<QuadraticAssignmentProblem>& calculator);

private:
    Config config_;
};

}  // namespace qap

#endif  // QAP_SOLVER_HPP
