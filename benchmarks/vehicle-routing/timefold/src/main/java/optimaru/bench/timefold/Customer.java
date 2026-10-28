package optimaru.bench.timefold;

import ai.timefold.solver.core.api.domain.common.PlanningId;
import ai.timefold.solver.core.api.domain.entity.PlanningEntity;
import ai.timefold.solver.core.api.domain.variable.InverseRelationShadowVariable;
import ai.timefold.solver.core.api.domain.variable.NextElementShadowVariable;
import ai.timefold.solver.core.api.domain.variable.PreviousElementShadowVariable;

/** A CVRPLIB customer; node 0 is the depot and never a Customer. */
@PlanningEntity
public class Customer {

    @PlanningId
    private int node;
    private int demand;
    private long[][] distances;

    @InverseRelationShadowVariable(sourceVariableName = "customers")
    private Vehicle vehicle;
    @PreviousElementShadowVariable(sourceVariableName = "customers")
    private Customer previous;
    @NextElementShadowVariable(sourceVariableName = "customers")
    private Customer next;

    public Customer() {
    }

    public Customer(int node, int demand, long[][] distances) {
        this.node = node;
        this.demand = demand;
        this.distances = distances;
    }

    /** The leg into this customer, plus the leg back to the depot when it ends the route. */
    public long legDistance() {
        long in = distances[previous == null ? 0 : previous.node][node];
        return next == null ? in + distances[node][0] : in;
    }

    public int getNode() {
        return node;
    }

    public int getDemand() {
        return demand;
    }

    public Vehicle getVehicle() {
        return vehicle;
    }

    public void setVehicle(Vehicle vehicle) {
        this.vehicle = vehicle;
    }

    public Customer getPrevious() {
        return previous;
    }

    public void setPrevious(Customer previous) {
        this.previous = previous;
    }

    public Customer getNext() {
        return next;
    }

    public void setNext(Customer next) {
        this.next = next;
    }
}
