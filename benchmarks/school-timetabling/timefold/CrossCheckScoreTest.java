package org.acme.schooltimetabling.solver;

import java.io.File;
import java.time.DayOfWeek;
import java.time.LocalTime;
import java.util.ArrayList;
import java.util.List;

import ai.timefold.solver.core.api.score.HardSoftScore;
import ai.timefold.solver.core.api.solver.SolutionManager;
import ai.timefold.solver.core.api.solver.SolverFactory;
import ai.timefold.solver.core.config.solver.SolverConfig;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.acme.schooltimetabling.domain.Lesson;
import org.acme.schooltimetabling.domain.Room;
import org.acme.schooltimetabling.domain.Timeslot;
import org.acme.schooltimetabling.domain.Timetable;
import org.junit.jupiter.api.Test;

/**
 * Re-scores the fixed cross-check instance dumped by the C++ port's
 * `st_quality_bench crosscheck <out.json>` with the STOCK
 * TimetableConstraintProvider, so the two score calculators can be compared
 * byte-for-byte.
 *
 * (day, period) -> Timeslot mapping: day d -> DayOfWeek.of(d + 1); period p
 * (1-based) -> start 09:00 + 55min * p, duration 45min. Consecutive periods
 * therefore leave a 10-minute gap (within the stock provider's
 * "0 <= gap <= 30min" reward window) and skipping a period leaves 65 minutes
 * (outside it) — exactly the C++ calculator's |dPeriod| == 1 adjacency.
 *
 * Run: mvn test -Dtest=CrossCheckScoreTest -Dcrosscheck.file=/path/to/crosscheck.json
 */
class CrossCheckScoreTest {

    static Timeslot toTimeslot(int index, int day, int period) {
        LocalTime start = LocalTime.of(9, 0).plusMinutes(55L * period);
        return new Timeslot(String.valueOf(index), DayOfWeek.of(day + 1), start, start.plusMinutes(45));
    }

    static Timetable load(File json) throws Exception {
        JsonNode root = new ObjectMapper().readTree(json);
        List<Timeslot> timeslots = new ArrayList<>();
        for (JsonNode ts : root.get("timeslots")) {
            timeslots.add(toTimeslot(timeslots.size(), ts.get("day").asInt(), ts.get("period").asInt()));
        }
        List<Room> rooms = new ArrayList<>();
        for (JsonNode room : root.get("rooms")) {
            rooms.add(new Room(String.valueOf(rooms.size()), room.asText()));
        }
        List<Lesson> lessons = new ArrayList<>();
        for (JsonNode l : root.get("lessons")) {
            Lesson lesson = new Lesson(l.get("id").asText(), l.get("subject").asText(),
                    l.get("teacher").asText(), l.get("studentGroup").asText());
            int timeslot = l.get("timeslot").asInt();
            int room = l.get("room").asInt();
            if (timeslot >= 0) {
                lesson.setTimeslot(timeslots.get(timeslot));
            }
            if (room >= 0) {
                lesson.setRoom(rooms.get(room));
            }
            lessons.add(lesson);
        }
        return new Timetable(json.getName(), timeslots, rooms, lessons);
    }

    @Test
    void printScore() throws Exception {
        String path = System.getProperty("crosscheck.file");
        if (path == null) {
            throw new IllegalStateException("pass -Dcrosscheck.file=<crosscheck.json dumped by st_quality_bench>");
        }
        Timetable timetable = load(new File(path));

        SolverConfig config = new SolverConfig()
                .withSolutionClass(Timetable.class)
                .withEntityClasses(Lesson.class)
                .withConstraintProviderClass(TimetableConstraintProvider.class);
        SolutionManager<Timetable, HardSoftScore> solutionManager =
                SolutionManager.create(SolverFactory.create(config));
        HardSoftScore score = solutionManager.update(timetable);
        System.out.println("CROSSCHECK_SCORE=" + score);
    }
}
