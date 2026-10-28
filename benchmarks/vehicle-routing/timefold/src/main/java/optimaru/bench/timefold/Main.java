package optimaru.bench.timefold;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import ai.timefold.solver.core.api.solver.SolverFactory;
import ai.timefold.solver.core.config.solver.SolverConfig;

/**
 * Usage: java -jar timefold-vrp-bench.jar INSTANCE_FILE
 *
 * <pre>
 * nodeCount vehicleCount capacity
 * spentLimitMillis randomSeed
 * demand_0 ... demand_{n-1}          (node 0 is the depot)
 * distance row 0 ... row n-1         (the harness's rounded matrix)
 * </pre>
 *
 * Prints {@code route n1 n2 ...} per vehicle, then {@code score HARD SOFT}.
 * The solver runs with Timefold's default (construction heuristic + local
 * search) configuration; only the termination and the seed are set.
 */
public final class Main {

    public static void main(String[] args) throws IOException {
        List<long[]> rows = Files.readAllLines(Path.of(args[0])).stream()
                .filter(line -> !line.isBlank())
                .map(line -> Arrays.stream(line.trim().split("\\s+")).mapToLong(Long::parseLong).toArray())
                .toList();
        int nodeCount = (int) rows.get(0)[0];
        int vehicleCount = (int) rows.get(0)[1];
        int capacity = (int) rows.get(0)[2];
        long spentLimitMillis = rows.get(1)[0];
        long seed = rows.get(1)[1];
        long[] demands = rows.get(2);
        long[][] distances = rows.subList(3, 3 + nodeCount).toArray(long[][]::new);

        List<Vehicle> vehicles = new ArrayList<>();
        for (int i = 0; i < vehicleCount; i++) {
            vehicles.add(new Vehicle(i, capacity));
        }
        List<Customer> customers = new ArrayList<>();
        for (int node = 1; node < nodeCount; node++) {
            customers.add(new Customer(node, (int) demands[node], distances));
        }

        SolverConfig config = new SolverConfig()
                .withSolutionClass(CvrpSolution.class)
                .withEntityClasses(Vehicle.class, Customer.class)
                .withConstraintProviderClass(CvrpConstraintProvider.class)
                .withTerminationSpentLimit(Duration.ofMillis(spentLimitMillis))
                .withRandomSeed(seed);
        CvrpSolution best = SolverFactory.<CvrpSolution> create(config)
                .buildSolver()
                .solve(new CvrpSolution(vehicles, customers));

        StringBuilder out = new StringBuilder();
        for (Vehicle vehicle : best.getVehicles()) {
            out.append("route");
            for (Customer customer : vehicle.getCustomers()) {
                out.append(' ').append(customer.getNode());
            }
            out.append('\n');
        }
        out.append("score ").append(best.getScore().hardScore())
                .append(' ').append(best.getScore().softScore()).append('\n');
        System.out.print(out);
    }
}
