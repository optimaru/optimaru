// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_ACCEPT_ACCEPTORS_HPP
#define OPTIMARU_ACCEPT_ACCEPTORS_HPP

// The Acceptor interface and every built-in acceptance rule, one per file,
// plus the factory.

#include <cstdint>
#include <memory>

#include "optimaru/accept/acceptor.hpp"
#include "optimaru/accept/hill_climbing.hpp"
#include "optimaru/accept/late_acceptance.hpp"
#include "optimaru/accept/entity_tabu.hpp"
#include "optimaru/accept/simulated_annealing.hpp"
#include "optimaru/accept/great_deluge.hpp"
#include "optimaru/accept/step_counting_hill_climbing.hpp"
#include "optimaru/accept/record_to_record_travel.hpp"
#include "optimaru/score/levels.hpp"
#include "optimaru/score/score.hpp"
#include "optimaru/settings.hpp"

namespace optimaru {

// Builds the configured rule (AcceptanceSettings::rule) for a search that
// starts at `initial_score`. Adding a rule is a file here, a line above, an
// enum value and a case below.
template <typename Solution>
std::unique_ptr<Acceptor> make_acceptor(const Settings& config, const Score& initial_score) {
    const AcceptanceSettings& c = config.acceptance;
    switch (c.rule) {
        case AcceptanceRule::HillClimbing:
            return std::make_unique<HillClimbing>();
        case AcceptanceRule::EntityTabu:
            return std::make_unique<EntityTabu>(c.tabu_tenure);
        case AcceptanceRule::SimulatedAnnealing: {
            const bool automatic =
                    c.start_temperature == Score{0, 0.0} && c.start_temperature_fraction > 0.0;
            return std::make_unique<SimulatedAnnealing>(
                    automatic ? automatic_start_temperature(unfolded_soft<Solution>(initial_score),
                                                            c.start_temperature_fraction)
                              : c.start_temperature);
        }
        case AcceptanceRule::GreatDeluge:
            return std::make_unique<GreatDeluge>(initial_score, c.water_level_step);
        case AcceptanceRule::StepCountingHillClimbing:
            return std::make_unique<StepCountingHillClimbing>(c.recount_interval, initial_score);
        case AcceptanceRule::RecordToRecordTravel: {
            // The threshold schedule spans the whole solve, so it needs a
            // step count. Prefer the configured limit; otherwise assume a
            // long run (10^6 steps) — overshooting only makes the
            // threshold decay more slowly, which RRT tolerates, whereas
            // undershooting collapses it to hill climbing immediately.
            const std::int64_t steps =
                    config.stop.step_limit > 0 ? config.stop.step_limit : 1000000;
            return std::make_unique<RecordToRecordTravel>(RecordToRecordTravel::autofit(
                    initial_score.soft, c.rrt_start_gap, c.rrt_end_gap, steps));
        }
        case AcceptanceRule::LateAcceptance:
        default:
            return std::make_unique<LateAcceptance>(c.history_length, initial_score);
    }
}

}  // namespace optimaru

#endif  // OPTIMARU_ACCEPT_ACCEPTORS_HPP
