// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_PREVIEW_EVALUATION_HPP
#define OPTIMARU_SEARCH_PREVIEW_EVALUATION_HPP

#include "optimaru/editor.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// evaluate_with_preview — the one way a strategy scores a candidate move
// ---------------------------------------------------------------------------
// `preview(objective, out)` asks the objective for the score of the moved
// solution without touching it and returns false when it cannot say;
// `apply()` and `undo()` perform and revert the move through the editor.
//
// Preview served: its score is returned. Declined: the move is applied,
// scored and undone. Under ScoreChecks::Full a served preview is additionally
// checked against the applied score, and a mismatch throws
// ScoreMismatchError, so every consumer's previews are verified in the mode
// built to catch exactly that.
// `served`, when given, reports whether the preview answered (so a caller can
// bound how much slow-path work it tolerates); `applied`, when given, whether
// apply() and undo() ran (a caller passing a no-op undo then knows the move is
// in place).
template <typename Solution, typename Preview, typename Apply, typename Undo>
Score evaluate_with_preview(Editor<Solution>& editor, Preview&& preview,
                                  Apply&& apply, Undo&& undo, bool* served = nullptr,
                                  bool* applied = nullptr) {
    Score previewed;
    const bool was_served = preview(editor.objective(), previewed);
    if (served != nullptr) *served = was_served;
    if (applied != nullptr) *applied = false;
    if (was_served && editor.score_checks() != ScoreChecks::Full) return previewed;
    if (applied != nullptr) *applied = true;
    apply();
    const Score actual = editor.score();
    undo();
    if (was_served && !scores_agree(previewed, actual)) throw ScoreMismatchError(previewed, actual);
    return actual;
}

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_PREVIEW_EVALUATION_HPP
