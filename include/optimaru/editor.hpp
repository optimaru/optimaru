// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_EDITOR_HPP
#define OPTIMARU_EDITOR_HPP

#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "optimaru/solution_traits.hpp"
#include "optimaru/derived_state.hpp"
#include "optimaru/score.hpp"
#include "optimaru/objective.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------
// The one way the engine (and tests) change a solution. It pairs the solution
// with its Objective and any DerivedState, and brackets every write as
//
//   objective.before_*  ->  write  ->  derived state updated  ->  objective.after_*
//
// so the objective retracts a contribution while the OLD derived values are
// still in place and adds it back once the NEW ones are. Nothing else ever
// writes a decision variable directly.
template <typename Solution>
class Editor {
    using Traits = SolutionTraits<Solution>;
    using DerivedStates = std::vector<std::unique_ptr<DerivedState<Solution>>>;

public:
    Editor(Solution& solution, Objective<Solution>& objective,
                  ScoreChecks score_checks = ScoreChecks::Off,
                  const DerivedStates* derived_states = nullptr)
        : solution_(solution),
          objective_(objective),
          mode_(score_checks),
          derived_(derived_states) {
        rebuild();
    }

    // Rebuild everything computed from the decision variables: the derived
    // state first, then the objective (which may read it).
    void rebuild() {
        if (derived_) {
            for (const auto& derived : *derived_) derived->rebuild(solution_);
        }
        objective_.rebuild(solution_);
    }

    // --- assignment shape ---

    void assign(int entity_index, int value) {
        objective_.before_assign(entity_index);
        Traits::assign(solution_, entity_index, value);
        if (derived_) {
            for (const auto& derived : *derived_) {
                derived->after_assign(solution_, entity_index);
            }
        }
        objective_.after_assign(entity_index);
    }

    void begin_batch() { objective_.begin_batch(); }
    void end_batch() { objective_.end_batch(); }
    void begin_trial() { objective_.begin_trial(); }
    void begin_rollback() { objective_.begin_rollback(); }
    void end_trial(bool accepted) { objective_.end_trial(accepted); }

    // --- sequence shape ---

    int remove(int list_index, int position) {
        begin_list_edit(list_index);
        int element = Traits::remove_element(solution_, list_index, position);
        end_list_edit(list_index);
        return element;
    }

    void insert(int list_index, int position, int element) {
        begin_list_edit(list_index);
        Traits::insert_element(solution_, list_index, position, element);
        end_list_edit(list_index);
    }

    void reverse(int list_index, int first, int last) {
        begin_list_edit(list_index);
        Traits::reverse_segment(solution_, list_index, first, last);
        end_list_edit(list_index);
    }

    // Composite list mutations (e.g. a tail swap rebuilt in place) may bracket
    // their own Traits writes with this pair, so derived state and the objective
    // still see one coherent change per list.
    void begin_list_edit(int list_index) { objective_.before_list_edit(list_index); }
    void end_list_edit(int list_index) {
        if (derived_) {
            for (const auto& derived : *derived_) {
                derived->after_list_edit(solution_, list_index);
            }
        }
        objective_.after_list_edit(list_index);
        record_touched(list_index);
    }

    // --- touched-list tracking ---
    // While enabled, every list mutation records its list index; the engine
    // uses this to resynchronize its element index after a custom move
    // operator ran (custom operators don't know about the engine's index).
    void track_touched_lists() {
        track_touched_ = true;
        touched_lists_.clear();
    }
    void untrack_touched_lists() { track_touched_ = false; }
    const std::vector<int>& touched_lists() const { return touched_lists_; }

    // --- scoring ---

    Score score() {
        Score incremental = objective_.score();
        if (mode_ == ScoreChecks::Full) {
            rebuild();  // re-derives the derived state too: catches its bugs
            Score full = objective_.score();
            if (!scores_agree(incremental, full)) throw ScoreMismatchError(incremental, full);
        }
        return incremental;
    }

    Solution& solution() { return solution_; }
    Objective<Solution>& objective() { return objective_; }
    ScoreChecks score_checks() const { return mode_; }

private:
    void record_touched(int list_index) {
        if (track_touched_) touched_lists_.push_back(list_index);
    }

    Solution& solution_;
    Objective<Solution>& objective_;
    ScoreChecks mode_;
    const DerivedStates* derived_;
    bool track_touched_ = false;
    std::vector<int> touched_lists_;
};

}  // namespace optimaru

#endif  // OPTIMARU_EDITOR_HPP
