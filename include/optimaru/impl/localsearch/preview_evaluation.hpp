#ifndef OPTIMARU_IMPL_LOCALSEARCH_PREVIEW_EVALUATION_HPP
#define OPTIMARU_IMPL_LOCALSEARCH_PREVIEW_EVALUATION_HPP

#include <cmath>

#include "optimaru/api/score/director/score_director.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// evaluateWithPreview — the one way a strategy scores a candidate move
// ---------------------------------------------------------------------------
// `preview(calculator, out)` asks the calculator for the score of the moved
// solution without touching it and returns false when it cannot say;
// `apply()` and `undo()` perform and revert the move through the director.
//
// Preview served: its score is returned. Declined: the move is applied,
// scored and undone. Under FULL_ASSERT a served preview is additionally
// checked against the applied score, and a mismatch throws
// ScoreCorruptionError, so every consumer's previews are verified in the mode
// built to catch exactly that.
// `served`, when given, reports whether the preview answered (so a caller can
// bound how much slow-path work it tolerates); `applied`, when given, whether
// apply() and undo() ran (a caller passing a no-op undo then knows the move is
// in place).
template <typename Solution, typename Preview, typename Apply, typename Undo>
HardSoftScore evaluateWithPreview(ScoreDirector<Solution>& director, Preview&& preview,
                                  Apply&& apply, Undo&& undo, bool* served = nullptr,
                                  bool* applied = nullptr) {
    HardSoftScore previewed;
    const bool wasServed = preview(director.calculator(), previewed);
    if (served != nullptr) *served = wasServed;
    if (applied != nullptr) *applied = false;
    if (wasServed && director.environmentMode() != EnvironmentMode::FULL_ASSERT) return previewed;
    if (applied != nullptr) *applied = true;
    apply();
    const HardSoftScore actual = director.calculateScore();
    undo();
    if (wasServed && !scoresAgree(previewed, actual)) throw ScoreCorruptionError(previewed, actual);
    return actual;
}

}  // namespace optimaru

#endif  // OPTIMARU_IMPL_LOCALSEARCH_PREVIEW_EVALUATION_HPP
