# Java cross-validation & benchmark tests

These are the **Java-side** tests used to validate and benchmark the C++
school-timetabling port against the real Timefold `school-timetabling`
quickstart (the STOCK `TimetableConstraintProvider` — the six upstream
constraints). They are kept here so the cross-validation is reproducible; they
are *not* part of the C++ build.

| File | Purpose |
| --- | --- |
| `CrossCheckScoreTest.java` | Reads the fixed instance + fixed assignment dumped by `st_quality_bench crosscheck <out.json>`, rebuilds it with the stock domain (`Timeslot`/`Lesson`/`Room`) and prints the stock provider's score via `SolutionManager.update` — must be byte-identical with the C++ `CROSSCHECK_SCORE=...` line (`-Dcrosscheck.file=<json>`) |
| `QualityBenchTest.java` | Solution-quality head-to-head: reads the instance JSONs dumped by `st_quality_bench`, solves each with the real Timefold solver for the same wall-clock budget, prints the trajectory / first-feasible time / final score / move-evaluation speed, and writes `<instance>.solution_java.json` for the shared re-scorer `bench/evaluate.py` (`-Dqbench.dir=<dir>` `-Dqbench.seconds=10`) |

## (day, period) → Timeslot mapping

The C++ domain models timeslots as `(day, period)`; the stock quickstart uses
`DayOfWeek` + start/end `LocalTime`, and its two "consecutive lessons"
constraints reward/penalize pairs whose gap (end → next start) is within
`[0, 30]` minutes. The tests map period `p` (1-based) to
**start 09:00 + 55min × p, duration 45min**, so:

- consecutive periods (Δperiod = 1) leave a 10-minute gap → inside the window,
- skipping a period leaves 65 minutes → outside the window,

which is exactly the C++ calculator's `|Δperiod| == 1` adjacency. The
`crosscheck` instance exercises all six constraints with nonzero
contributions, so any drift in this mapping (or in either score calculator)
fails the byte-identical gate.

## How to run

1. Clone the quickstarts repo (the Java `school-timetabling` module is *not*
   in this repo):

   ```sh
   git clone --depth 1 -b v2.6.0 https://github.com/TimefoldAI/timefold-quickstarts.git
   ```

2. Copy these files into its test source tree:

   ```sh
   cp *.java \
     timefold-quickstarts/use-cases/school-timetabling/src/test/java/org/acme/schooltimetabling/solver/
   ```

3. The quickstarts repo ships no Maven wrapper; if `mvn` is not installed,
   bootstrap the official wrapper (only-script type) once:

   ```sh
   cd timefold-quickstarts/use-cases/school-timetabling
   mkdir -p .mvn/wrapper
   curl -fsSL -o mvnw https://raw.githubusercontent.com/apache/maven-wrapper/maven-wrapper-3.3.4/maven-wrapper-distribution/src/resources/only-mvnw
   chmod +x mvnw
   printf 'distributionUrl=https://repo.maven.apache.org/maven2/org/apache/maven/apache-maven/3.9.11/apache-maven-3.9.11-bin.zip\nwrapperVersion=3.3.4\ndistributionType=only-script\n' \
     > .mvn/wrapper/maven-wrapper.properties
   ```

4. Run (JDK 21+):

   ```sh
   # C++ side first: dumps the JSONs these tests read
   ./build/st-bench/st_quality_bench crosscheck /tmp/qbench/crosscheck.json
   ./build/st-bench/st_quality_bench 6 12 7 1 10 /tmp/qbench/instance_S.json

   cd timefold-quickstarts/use-cases/school-timetabling
   ./mvnw test -Dtest=CrossCheckScoreTest -Dsurefire.failIfNoSpecifiedTests=false \
     -Dcrosscheck.file=/tmp/qbench/crosscheck.json    # prints CROSSCHECK_SCORE=...
   ./mvnw test -Dtest=QualityBenchTest -Dsurefire.failIfNoSpecifiedTests=false \
     -Dqbench.dir=/tmp/qbench -Dqbench.seconds=10     # prints QBENCH_JAVA_FINAL ...
   ```

   The whole 3-way comparison (crosscheck parity gate → C++ / CP-SAT / Java
   quality bench on identical instances → shared re-scoring) is orchestrated
   by `../run_bench.py`.

## Cross-validation result

The same fixed instance + assignment produces byte-identical scores on both
sides (all six constraints contribute: 1 room + 1 teacher + 1 group conflict,
stability 4, efficiency 3, variety 2):

```
C++   (st_quality_bench crosscheck) -> CROSSCHECK_SCORE=-3hard/-3soft
Java  (CrossCheckScoreTest)         -> CROSSCHECK_SCORE=-3hard/-3soft
```

Note: the C++ domain's `teacherUnavailable` extension is deliberately left
empty in every benchmark instance (0 contribution), so the comparison against
the six stock constraints stays fair.
