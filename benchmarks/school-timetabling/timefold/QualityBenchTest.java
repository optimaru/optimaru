package org.acme.schooltimetabling.solver;

import java.io.File;
import java.io.PrintWriter;
import java.util.IdentityHashMap;
import java.util.Map;

import ai.timefold.solver.core.api.solver.Solver;
import ai.timefold.solver.core.api.solver.SolverFactory;
import ai.timefold.solver.core.config.solver.EnvironmentMode;
import ai.timefold.solver.core.config.solver.SolverConfig;
import ai.timefold.solver.core.config.solver.termination.TerminationConfig;
import ai.timefold.solver.core.impl.solver.DefaultSolver;

import org.acme.schooltimetabling.domain.Lesson;
import org.acme.schooltimetabling.domain.Room;
import org.acme.schooltimetabling.domain.Timeslot;
import org.acme.schooltimetabling.domain.Timetable;
import org.junit.jupiter.api.Test;

/**
 * Solution-quality head-to-head vs the C++ port and CP-SAT: reads the SAME
 * instance JSON dumped by the C++ `st_quality_bench` ((day, period) grid,
 * lessons unassigned), solves it with the real Timefold solver and the STOCK
 * TimetableConstraintProvider for the same wall-clock budget, and prints the
 * best-score trajectory, first-feasible time, final score and sustained
 * move-evaluation speed. The best assignment is written next to the instance
 * as <base>.solution_java.json for the shared re-scorer (bench/evaluate.py).
 *
 * The (day, period) -> Timeslot mapping is shared with CrossCheckScoreTest.
 *
 * Run: mvn test -Dtest=QualityBenchTest -Dqbench.dir=... -Dqbench.seconds=10
 */
class QualityBenchTest {

    @Test
    void quality() throws Exception {
        String dir = System.getProperty("qbench.dir");
        if (dir == null) {
            throw new IllegalStateException("pass -Dqbench.dir=<dir with instance_*.json>");
        }
        long seconds = Long.getLong("qbench.seconds", 10L);
        File[] files = new File(dir).listFiles(
                (d, n) -> n.startsWith("instance_") && n.endsWith(".json") && !n.contains(".solution_"));
        java.util.Arrays.sort(files);
        for (File f : files) {
            Timetable problem = CrossCheckScoreTest.load(f);
            System.out.printf("QBENCH_JAVA instance=%s lessons=%d timeslots=%d rooms=%d%n",
                    f.getName(), problem.getLessons().size(), problem.getTimeslots().size(),
                    problem.getRooms().size());

            SolverConfig config = new SolverConfig()
                    .withSolutionClass(Timetable.class)
                    .withEntityClasses(Lesson.class)
                    .withConstraintProviderClass(TimetableConstraintProvider.class)
                    .withEnvironmentMode(EnvironmentMode.NO_ASSERT)
                    .withTerminationConfig(new TerminationConfig().withSecondsSpentLimit(seconds));

            Solver<Timetable> solver = SolverFactory.<Timetable>create(config).buildSolver();
            long t0 = System.nanoTime();
            boolean[] feasibleSeen = { false };
            solver.addEventListener(event -> {
                long ms = (System.nanoTime() - t0) / 1_000_000;
                if (!feasibleSeen[0] && event.getNewBestScore().isFeasible()) {
                    feasibleSeen[0] = true;
                    System.out.printf("QBENCH_JAVA first_feasible t=%dms%n", ms);
                }
                System.out.printf("QBENCH_JAVA best t=%dms score=%s%n", ms, event.getNewBestScore());
            });
            Timetable solved = solver.solve(problem);
            dumpSolution(solved, f);
            // Exact sustained move-evaluation speed from the solver's own
            // counters (this test is plain JUnit — no Quarkus logging — so the
            // "move evaluation speed" INFO log line is not available here).
            long movesPerSec = ((DefaultSolver<Timetable>) solver)
                    .getSolverScope().getMoveEvaluationSpeed();
            System.out.printf("QBENCH_JAVA_FINAL instance=%s score=%s feasible=%s moves_per_sec=%d%n",
                    f.getName(), solved.getScore(), solved.getScore().isFeasible(), movesPerSec);
        }
    }

    private static void dumpSolution(Timetable solved, File instanceFile) throws Exception {
        Map<Timeslot, Integer> timeslotIndex = new IdentityHashMap<>();
        for (Timeslot ts : solved.getTimeslots()) {
            timeslotIndex.put(ts, timeslotIndex.size());
        }
        Map<Room, Integer> roomIndex = new IdentityHashMap<>();
        for (Room room : solved.getRooms()) {
            roomIndex.put(room, roomIndex.size());
        }
        String base = instanceFile.getPath().replaceFirst("\\.json$", "");
        try (PrintWriter out = new PrintWriter(base + ".solution_java.json", "UTF-8")) {
            out.println("{");
            out.println("  \"solver\": \"java\",");
            out.println("  \"score\": \"" + solved.getScore() + "\",");
            out.println("  \"assignment\": [");
            for (int i = 0; i < solved.getLessons().size(); i++) {
                Lesson lesson = solved.getLessons().get(i);
                int ts = lesson.getTimeslot() == null ? -1 : timeslotIndex.get(lesson.getTimeslot());
                int room = lesson.getRoom() == null ? -1 : roomIndex.get(lesson.getRoom());
                out.printf("    {\"id\": \"%s\", \"timeslot\": %d, \"room\": %d}%s%n",
                        lesson.getId(), ts, room, i + 1 < solved.getLessons().size() ? "," : "");
            }
            out.println("  ]");
            out.println("}");
        }
    }
}
