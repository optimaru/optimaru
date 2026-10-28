package optimaru.bench.timefold.platform;

import ai.timefold.solver.core.api.score.stream.ConstraintMetadata;

/** A constraint's id plus descriptive metadata; Timefold only reads {@link #id()}. */
public record ConstraintInfo(String id, String name, String description, ConstraintGroupInfo group)
        implements ConstraintMetadata {
}
