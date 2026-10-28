package optimaru.bench.timefold.platform;

import ai.timefold.solver.core.api.score.stream.ConstraintJustification;

/** A constraint match explanation with a human-readable description. */
public interface ModelConstraintJustification extends ConstraintJustification {

    String description();
}
