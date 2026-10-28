#ifndef OPTIMARU_API_SOLVER_SOLVER_FACTORY_HPP
#define OPTIMARU_API_SOLVER_SOLVER_FACTORY_HPP

#include <utility>

#include "optimaru/api/solver/solver.hpp"
#include "optimaru/config/solver/solver_config.hpp"

namespace optimaru {

// Mirrors api.solver.SolverFactory: builds Solver instances from a
// SolverConfig.
//
//   auto solver = optimaru::SolverFactory<MySolution>::create(config).buildSolver();
template <typename Solution>
class SolverFactory {
public:
    static SolverFactory create(SolverConfig config) {
        return SolverFactory(std::move(config));
    }

    Solver<Solution> buildSolver() const { return Solver<Solution>(config_); }

    const SolverConfig& solverConfig() const { return config_; }

private:
    explicit SolverFactory(SolverConfig config) : config_(std::move(config)) {}
    SolverConfig config_;
};

}  // namespace optimaru

#endif  // OPTIMARU_API_SOLVER_SOLVER_FACTORY_HPP
