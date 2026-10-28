#ifndef OPTIMARU_API_SCORE_DIRECTOR_SCORE_DIRECTOR_HPP
#define OPTIMARU_API_SCORE_DIRECTOR_SCORE_DIRECTOR_HPP

#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "optimaru/api/domain/solution_descriptor.hpp"
#include "optimaru/api/domain/variable/variable_listener.hpp"
#include "optimaru/api/score/buildin/hard_soft_score.hpp"
#include "optimaru/api/score/calculator/incremental_score_calculator.hpp"

namespace optimaru {

// Mirrors Timefold's EnvironmentMode: FULL_ASSERT verifies the incremental
// score against a full recompute (shadow-variable listeners re-run + calculator
// resetWorkingSolution + calculateScore) on every calculateScore() call, and
// throws ScoreCorruptionError on mismatch — this catches both a buggy
// calculator and a buggy VariableListener. Orders of magnitude slower — for
// debugging, never production.
enum class EnvironmentMode { NON_ASSERT, FULL_ASSERT };

// Does an incremental (or previewed) score agree with the full recompute?
// The one definition every FULL_ASSERT check uses: hard exact, soft within
// floating-point noise of the packed medium/soft value.
inline bool scoresAgree(const HardSoftScore& incremental, const HardSoftScore& full) {
    return incremental.hard == full.hard && std::fabs(incremental.soft - full.soft) <= 1e-6;
}

class ScoreCorruptionError : public std::runtime_error {
public:
    ScoreCorruptionError(const HardSoftScore& incremental, const HardSoftScore& full)
        : std::runtime_error("Score corruption: incremental score " + incremental.toString() +
                             " != full recompute " + full.toString()) {}
};

// ---------------------------------------------------------------------------
// ScoreDirector
// ---------------------------------------------------------------------------
// Pairs the working solution with its IncrementalScoreCalculator and the
// shadow-variable listeners, exactly like Timefold's ScoreDirector: every
// planning-variable write goes through one of the mutation methods below,
// which bracket the write with
//   calculator.before*Changed  ->  mutation  ->  listeners  ->  calculator.after*Changed
// so the calculator retracts against OLD shadow values and inserts against
// NEW ones. The engine (and tests) never mutate a planning variable directly.
template <typename Solution>
class ScoreDirector {
    using Descriptor = SolutionDescriptor<Solution>;
    using Listeners = std::vector<std::unique_ptr<VariableListener<Solution>>>;

public:
    ScoreDirector(Solution& workingSolution, IncrementalScoreCalculator<Solution>& calculator,
                  EnvironmentMode environmentMode = EnvironmentMode::NON_ASSERT,
                  const Listeners* variableListeners = nullptr)
        : solution_(workingSolution),
          calculator_(calculator),
          mode_(environmentMode),
          listeners_(variableListeners) {
        resetWorkingSolution();
    }

    // Rebuild all derived state from the genuine variables: shadow listeners
    // first, then the calculator (which may read the shadows).
    void resetWorkingSolution() {
        if (listeners_) {
            for (const auto& listener : *listeners_) listener->resetWorkingSolution(solution_);
        }
        calculator_.resetWorkingSolution(solution_);
    }

    // --- BASIC-variable mutation ---

    void changeVariable(int entityIndex, int value) {
        calculator_.beforeVariableChanged(entityIndex);
        Descriptor::setVariable(solution_, entityIndex, value);
        if (listeners_) {
            for (const auto& listener : *listeners_) {
                listener->afterVariableChanged(solution_, entityIndex);
            }
        }
        calculator_.afterVariableChanged(entityIndex);
    }

    void beginVariableChanges() { calculator_.beginVariableChanges(); }
    void endVariableChanges() { calculator_.endVariableChanges(); }
    void beginMoveEvaluation() { calculator_.beginMoveEvaluation(); }
    void beginMoveRollback() { calculator_.beginMoveRollback(); }
    void endMoveEvaluation(bool accepted) { calculator_.endMoveEvaluation(accepted); }

    // --- LIST-variable mutations ---

    int removeListElement(int listIndex, int position) {
        beginListChange(listIndex);
        int element = Descriptor::removeElement(solution_, listIndex, position);
        endListChange(listIndex);
        return element;
    }

    void insertListElement(int listIndex, int position, int element) {
        beginListChange(listIndex);
        Descriptor::insertElement(solution_, listIndex, position, element);
        endListChange(listIndex);
    }

    void reverseListSegment(int listIndex, int first, int last) {
        beginListChange(listIndex);
        Descriptor::reverseSegment(solution_, listIndex, first, last);
        endListChange(listIndex);
    }

    // Composite list mutations (e.g. a tail swap rebuilt in place) may bracket
    // their own Descriptor writes with this pair, so listeners and calculator
    // still see one coherent change per list.
    void beginListChange(int listIndex) { calculator_.beforeListChanged(listIndex); }
    void endListChange(int listIndex) {
        if (listeners_) {
            for (const auto& listener : *listeners_) {
                listener->afterListChanged(solution_, listIndex);
            }
        }
        calculator_.afterListChanged(listIndex);
        recordTouched(listIndex);
    }

    // --- touched-list tracking ---
    // While enabled, every list mutation records its list index; the engine
    // uses this to resynchronize its element index after a custom move
    // operator ran (custom operators don't know about the engine's index).
    void startTouchedListTracking() {
        trackTouched_ = true;
        touchedLists_.clear();
    }
    void stopTouchedListTracking() { trackTouched_ = false; }
    const std::vector<int>& touchedLists() const { return touchedLists_; }

    // --- scoring ---

    HardSoftScore calculateScore() {
        HardSoftScore incremental = calculator_.calculateScore();
        if (mode_ == EnvironmentMode::FULL_ASSERT) {
            resetWorkingSolution();  // re-derives shadows too: catches listener bugs
            HardSoftScore full = calculator_.calculateScore();
            if (!scoresAgree(incremental, full)) throw ScoreCorruptionError(incremental, full);
        }
        return incremental;
    }

    Solution& workingSolution() { return solution_; }
    IncrementalScoreCalculator<Solution>& calculator() { return calculator_; }
    EnvironmentMode environmentMode() const { return mode_; }

private:
    void recordTouched(int listIndex) {
        if (trackTouched_) touchedLists_.push_back(listIndex);
    }

    Solution& solution_;
    IncrementalScoreCalculator<Solution>& calculator_;
    EnvironmentMode mode_;
    const Listeners* listeners_;
    bool trackTouched_ = false;
    std::vector<int> touchedLists_;
};

}  // namespace optimaru

#endif  // OPTIMARU_API_SCORE_DIRECTOR_SCORE_DIRECTOR_HPP
