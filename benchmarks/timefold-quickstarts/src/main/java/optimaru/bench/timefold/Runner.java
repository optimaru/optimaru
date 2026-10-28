package optimaru.bench.timefold;

import java.io.File;
import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import ai.timefold.solver.core.api.score.Score;
import ai.timefold.solver.core.api.score.stream.ConstraintProvider;
import ai.timefold.solver.core.api.solver.SolutionManager;
import ai.timefold.solver.core.api.solver.SolverFactory;
import ai.timefold.solver.core.config.solver.SolverConfig;

import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.SerializationFeature;
import com.fasterxml.jackson.datatype.jsr310.JavaTimeModule;
import com.fasterxml.jackson.module.paramnames.ParameterNamesModule;

/**
 * Solves or scores a Timefold quickstart solution given as the quickstart's
 * own JSON (the shape its REST API accepts).
 *
 * <pre>
 * java -jar timefold-quickstarts-bench.jar solve USE_CASE IN.json OUT.json SPENT_MILLIS SEED
 * java -jar timefold-quickstarts-bench.jar score USE_CASE IN.json OUT.json
 * </pre>
 *
 * {@code solve} writes the score and every planning variable as ids
 * ({@link Graph#assignment}); {@code score} writes the score of the given
 * solution. The solver runs with Timefold's default
 * configuration (construction heuristic + local search) — only the
 * termination and the seed are set.
 */
public final class Runner {

    private record UseCase(Class<?> solution, List<Class<?>> entities,
            Class<? extends ConstraintProvider> constraints, java.util.function.Consumer<Object> prepare) {
    }

    private static final Map<String, UseCase> USE_CASES = new LinkedHashMap<>();

    static {
        add("bed-allocation", org.acme.bedallocation.domain.BedPlan.class,
                org.acme.bedallocation.solver.BedAllocationConstraintProvider.class,
                org.acme.bedallocation.domain.Stay.class);
        add("conference-scheduling", org.acme.conferencescheduling.domain.ConferenceSchedule.class,
                org.acme.conferencescheduling.solver.ConferenceSchedulingConstraintProvider.class,
                org.acme.conferencescheduling.domain.Talk.class);
        add("facility-location", org.acme.facilitylocation.domain.FacilityLocationProblem.class,
                org.acme.facilitylocation.solver.FacilityLocationConstraintProvider.class,
                org.acme.facilitylocation.domain.Consumer.class, org.acme.facilitylocation.domain.Facility.class);
        add("flight-crew-scheduling", org.acme.flighcrewscheduling.domain.FlightCrewSchedule.class,
                org.acme.flighcrewscheduling.solver.FlightCrewSchedulingConstraintProvider.class,
                org.acme.flighcrewscheduling.domain.FlightAssignment.class);
        add("food-packaging", org.acme.foodpackaging.domain.PackagingSchedule.class,
                org.acme.foodpackaging.solver.FoodPackagingConstraintProvider.class,
                org.acme.foodpackaging.domain.Line.class, org.acme.foodpackaging.domain.Job.class,
                org.acme.foodpackaging.domain.Operator.class);
        add("maintenance-scheduling", org.acme.maintenancescheduling.domain.MaintenanceSchedule.class,
                org.acme.maintenancescheduling.solver.MaintenanceScheduleConstraintProvider.class,
                org.acme.maintenancescheduling.domain.Job.class);
        add("meeting-scheduling", org.acme.meetingschedule.domain.MeetingSchedule.class,
                org.acme.meetingschedule.solver.MeetingSchedulingConstraintProvider.class,
                org.acme.meetingschedule.domain.MeetingAssignment.class);
        add("order-picking", org.acme.orderpicking.domain.OrderPickingSolution.class,
                org.acme.orderpicking.solver.OrderPickingConstraintProvider.class,
                org.acme.orderpicking.domain.Trolley.class, org.acme.orderpicking.domain.PickTask.class);
        USE_CASES.computeIfPresent("order-picking", (name, useCase) -> new UseCase(useCase.solution(),
                useCase.entities(), useCase.constraints(), Runner::rebuildOrders));
        add("project-job-scheduling", org.acme.projectjobschedule.domain.ProjectJobSchedule.class,
                org.acme.projectjobschedule.solver.ProjectJobSchedulingConstraintProvider.class,
                org.acme.projectjobschedule.domain.Allocation.class);
        add("sports-league-scheduling", org.acme.sportsleagueschedule.domain.LeagueSchedule.class,
                org.acme.sportsleagueschedule.solver.SportsLeagueSchedulingConstraintProvider.class,
                org.acme.sportsleagueschedule.domain.Match.class);
        add("task-assigning", org.acme.taskassigning.domain.TaskAssigningSolution.class,
                org.acme.taskassigning.solver.TaskAssigningConstraintProvider.class,
                org.acme.taskassigning.domain.Employee.class, org.acme.taskassigning.domain.Task.class);
        add("tournament-scheduling", org.acme.tournamentschedule.domain.TournamentSchedule.class,
                org.acme.tournamentschedule.solver.TournamentScheduleConstraintProvider.class,
                org.acme.tournamentschedule.domain.TeamAssignment.class);
    }

    private static void add(String name, Class<?> solution, Class<? extends ConstraintProvider> constraints,
            Class<?>... entities) {
        USE_CASES.put(name, new UseCase(solution, List.of(entities), constraints, problem -> {
        }));
    }

    /**
     * Order picking keeps {@code OrderItem.order} out of its JSON, so the
     * orders are rebuilt from the item ids, which the adapter writes as
     * {@code "<order id>/<item index>"}.
     */
    private static void rebuildOrders(Object problem) {
        var solution = (org.acme.orderpicking.domain.OrderPickingSolution) problem;
        Map<String, org.acme.orderpicking.domain.Order> orders = new LinkedHashMap<>();
        for (var task : solution.getPickTasks()) {
            var item = task.getOrderItem();
            String orderId = item.getId().substring(0, item.getId().lastIndexOf('/'));
            var order = orders.computeIfAbsent(orderId,
                    id -> new org.acme.orderpicking.domain.Order(id, new java.util.ArrayList<>()));
            order.getItems().add(item);
            item.setOrder(order);
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 4 || !USE_CASES.containsKey(args[1])) {
            System.err.println("usage: solve|score USE_CASE IN.json OUT.json [SPENT_MILLIS SEED]; use cases: "
                    + USE_CASES.keySet());
            System.exit(2);
        }
        UseCase useCase = USE_CASES.get(args[1]);
        ObjectMapper mapper = new ObjectMapper()
                .registerModule(new JavaTimeModule())
                .registerModule(new ParameterNamesModule())
                .registerModule(IdKeys.module())
                // Timefold requires a stable iteration order for value ranges.
                .registerModule(new com.fasterxml.jackson.databind.module.SimpleModule("optimaru-ordered-sets")
                        .addAbstractTypeMapping(java.util.Set.class, java.util.LinkedHashSet.class)
                        .addAbstractTypeMapping(java.util.SequencedSet.class, java.util.LinkedHashSet.class))
                .disable(SerializationFeature.WRITE_DATES_AS_TIMESTAMPS)
                .disable(DeserializationFeature.FAIL_ON_UNKNOWN_PROPERTIES);
        // "__lists" carries list variables a quickstart keeps out of its JSON.
        com.fasterxml.jackson.databind.node.ObjectNode tree =
                (com.fasterxml.jackson.databind.node.ObjectNode) mapper.readTree(new File(args[2]));
        com.fasterxml.jackson.databind.JsonNode lists = tree.remove("__lists");
        Object problem = mapper.treeToValue(tree, useCase.solution());
        Graph.relink(problem);
        useCase.prepare().accept(problem);
        if (lists != null) {
            Graph.applyLists(problem, mapper.convertValue(lists,
                    new com.fasterxml.jackson.core.type.TypeReference<
                            Map<String, Map<String, Map<String, List<String>>>>>() {
                    }));
        }
        SolverConfig config = new SolverConfig()
                .withSolutionClass(useCase.solution())
                .withEntityClassList(useCase.entities())
                .withConstraintProviderClass(useCase.constraints());
        Map<String, Object> out = new LinkedHashMap<>();
        switch (args[0]) {
            case "solve" -> {
                config.withTerminationSpentLimit(Duration.ofMillis(Long.parseLong(args[4])))
                        .withRandomSeed(Long.parseLong(args[5]));
                Object solved = solve(config, problem);
                out.put("score", score(config, solved).toString());
                out.put("assignment", Graph.assignment(solved));
            }
            case "score" -> out.put("score", score(config, problem).toString());
            default -> throw new IllegalArgumentException("unknown mode " + args[0]);
        }
        mapper.writeValue(new File(args[3]), out);
    }

    @SuppressWarnings("unchecked")
    private static Object solve(SolverConfig config, Object problem) {
        return SolverFactory.create(config).buildSolver().solve(problem);
    }

    @SuppressWarnings({ "unchecked", "rawtypes" })
    private static Score<?> score(SolverConfig config, Object solution) {
        SolutionManager manager = SolutionManager.create(SolverFactory.create(config));
        return manager.update(solution);
    }
}
