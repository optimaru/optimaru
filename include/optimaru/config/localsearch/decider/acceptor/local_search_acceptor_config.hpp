#ifndef OPTIMARU_CONFIG_LOCALSEARCH_DECIDER_ACCEPTOR_LOCAL_SEARCH_ACCEPTOR_CONFIG_HPP
#define OPTIMARU_CONFIG_LOCALSEARCH_DECIDER_ACCEPTOR_LOCAL_SEARCH_ACCEPTOR_CONFIG_HPP

#include <cstddef>
#include <cstdint>

#include "optimaru/api/score/buildin/hard_soft_score.hpp"

namespace optimaru {

// Mirrors config.localsearch.decider.acceptor.AcceptorType and adds
// record-to-record travel.
enum class AcceptorType {
    HILL_CLIMBING,
    LATE_ACCEPTANCE,
    ENTITY_TABU,
    SIMULATED_ANNEALING,
    GREAT_DELUGE,
    STEP_COUNTING_HILL_CLIMBING,
    RECORD_TO_RECORD_TRAVEL,
};

// Mirrors config.localsearch.decider.acceptor.LocalSearchAcceptorConfig:
// which acceptance strategy local search uses, plus its parameters.
struct LocalSearchAcceptorConfig {
    AcceptorType acceptorType = AcceptorType::LATE_ACCEPTANCE;

    std::size_t lateAcceptanceSize = 400;         // LATE_ACCEPTANCE
    int entityTabuSize = 7;                       // ENTITY_TABU
    // SIMULATED_ANNEALING: must be set to a sensible starting temperature.
    HardSoftScore simulatedAnnealingStartingTemperature{0, 0.0};
    // GREAT_DELUGE: how much the water level rises per step.
    HardSoftScore greatDelugeWaterLevelIncrement{0, 0.0};
    std::int64_t stepCountingHillClimbingSize = 400;  // STEP_COUNTING_HILL_CLIMBING

    // RECORD_TO_RECORD_TRAVEL: the acceptable soft-score gap below the
    // baseline, shrinking from start to end over the solve. Both are
    // fractions of the initial score's soft magnitude (the autofit form), so
    // they need no per-instance tuning; recordToRecordTravelSteps is how many
    // steps the schedule spans (0 = auto, derived from the step limit or a
    // 10^6 default). recordToRecordTravelCompareToBest = false switches from
    // RRT to threshold accepting (baseline = current instead of best).
    double recordToRecordTravelStartGap = 0.02;
    double recordToRecordTravelEndGap = 0.0;
    std::int64_t recordToRecordTravelSteps = 0;
    bool recordToRecordTravelExponential = false;
    bool recordToRecordTravelCompareToBest = true;
};

}  // namespace optimaru

#endif  // OPTIMARU_CONFIG_LOCALSEARCH_DECIDER_ACCEPTOR_LOCAL_SEARCH_ACCEPTOR_CONFIG_HPP
