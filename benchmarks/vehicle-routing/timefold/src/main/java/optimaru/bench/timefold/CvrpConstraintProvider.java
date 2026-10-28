package optimaru.bench.timefold;

import ai.timefold.solver.core.api.score.HardSoftScore;
import ai.timefold.solver.core.api.score.stream.Constraint;
import ai.timefold.solver.core.api.score.stream.ConstraintCollectors;
import ai.timefold.solver.core.api.score.stream.ConstraintFactory;
import ai.timefold.solver.core.api.score.stream.ConstraintProvider;

/** CVRP: capacity overflow is hard, total round-trip distance is soft. */
public class CvrpConstraintProvider implements ConstraintProvider {

    @Override
    public Constraint[] defineConstraints(ConstraintFactory factory) {
        return new Constraint[] {
                vehicleCapacity(factory),
                totalDistance(factory)
        };
    }

    Constraint vehicleCapacity(ConstraintFactory factory) {
        return factory.forEach(Customer.class)
                .groupBy(Customer::getVehicle, ConstraintCollectors.sum(Customer::getDemand))
                .filter((vehicle, demand) -> demand > vehicle.getCapacity())
                .penalize(HardSoftScore.ONE_HARD, (vehicle, demand) -> demand - vehicle.getCapacity())
                .asConstraint("vehicleCapacity");
    }

    Constraint totalDistance(ConstraintFactory factory) {
        return factory.forEach(Customer.class)
                .penalize(HardSoftScore.ONE_SOFT, Customer::legDistance)
                .asConstraint("totalDistance");
    }
}
