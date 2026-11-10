// Self-contained engine tests: every feature of the generic engine verified on
// two toy domains small enough to brute-force.
//   assignment shape — task assignment: cost matrix + conflicting task pairs.
//   sequence shape  — TSP: one (or more) cyclic routes over 2-D points.
// Also doubles as reference code for writing a SolutionTraits and an
// Objective against this engine.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "optimaru/score/constraint_assembly.hpp"
#include "optimaru/worker_threads.hpp"
#include "optimaru/testing/preview_parity.hpp"
#include "optimaru/optimaru.hpp"
#include "optimaru/testing/check.hpp"

using optimaru::Score;

namespace {

using optimaru::testing::check;
using optimaru::testing::failures;

// ===========================================================================
// assignment shape: task assignment
// ===========================================================================
// Assign each task to a worker. Hard: -1 per conflicting task pair on the same
// worker. Soft: -cost[task][worker].
struct TaskAssignment {
    int workerCount = 0;
    std::vector<std::vector<long>> cost;         // [task][worker]
    std::vector<std::pair<int, int>> conflicts;  // task pairs
    std::vector<int> assignment;                 // decision variable; -1 = unassigned
};

}  // namespace

template <>
struct optimaru::SolutionTraits<TaskAssignment> {
    static constexpr optimaru::Shape shape = optimaru::Shape::Assignment;
    static int entity_count(const TaskAssignment& s) {
        return static_cast<int>(s.assignment.size());
    }
    static int value_count(const TaskAssignment& s) { return s.workerCount; }
    static int value_of(const TaskAssignment& s, int e) { return s.assignment[e]; }
    static void assign(TaskAssignment& s, int e, int v) { s.assignment[e] = v; }
};

namespace {

class TaskAssignmentCalculator : public optimaru::Objective<TaskAssignment> {
public:
    void rebuild(TaskAssignment& solution) override {
        sched_ = &solution;
        conflictsOf_.assign(solution.assignment.size(), {});
        for (auto [a, b] : solution.conflicts) {
            conflictsOf_[a].push_back(b);
            conflictsOf_[b].push_back(a);
        }
        costSum_ = 0;
        conflictCount_ = 0;
        for (int t = 0; t < static_cast<int>(solution.assignment.size()); ++t) {
            int w = solution.assignment[t];
            if (w < 0) continue;
            costSum_ += solution.cost[t][w];
            for (int u : conflictsOf_[t]) {
                if (u < t && solution.assignment[u] == w) ++conflictCount_;
            }
        }
    }

    void before_assign(int t) override { applyDelta(t, -1); }
    void after_assign(int t) override { applyDelta(t, +1); }

    Score score() override { return {-conflictCount_, -static_cast<double>(costSum_)}; }

protected:
    void applyDelta(int t, int sign) {
        int w = sched_->assignment[t];
        if (w < 0) return;
        costSum_ += sign * sched_->cost[t][w];
        for (int u : conflictsOf_[t]) {
            if (u != t && sched_->assignment[u] == w) conflictCount_ += sign;
        }
    }

    TaskAssignment* sched_ = nullptr;
    std::vector<std::vector<int>> conflictsOf_;
    long costSum_ = 0;
    std::int64_t conflictCount_ = 0;
};

// One hard point worse than the truth: a start that searches with this
// objective never reaches a feasible target.
class HandicappedCalculator final : public TaskAssignmentCalculator {
public:
    Score score() override {
        Score handicapped = TaskAssignmentCalculator::score();
        --handicapped.hard;
        return handicapped;
    }
};

// Deliberately wrong: forgets the conflict bookkeeping on insert. ScoreChecks::Full
// must catch this as score corruption.
class CorruptedCalculator final : public TaskAssignmentCalculator {
public:
    void after_assign(int t) override {
        int w = sched_->assignment[t];
        if (w >= 0) costSum_ += sched_->cost[t][w];  // conflicts "forgotten"
    }
};

// Conflicted instance: the 6-cycle of conflicts creates real local optima —
// used to test that SA / multi-start escape them.
TaskAssignment buildTaskInstance() {
    TaskAssignment s;
    s.workerCount = 4;
    s.cost = {
            {7, 3, 9, 4}, {2, 8, 5, 6}, {6, 4, 3, 8}, {5, 9, 2, 7}, {8, 2, 6, 3}, {4, 6, 7, 2},
    };
    s.conflicts = {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {0, 5}};
    s.assignment.assign(6, -1);
    return s;
}

// Separable instance (no conflicts): the landscape has no local optima, so
// EVERY acceptor must reach the exact optimum — this isolates acceptor
// correctness from search-hardness.
TaskAssignment buildSeparableInstance() {
    TaskAssignment s = buildTaskInstance();
    s.conflicts.clear();
    return s;
}

Score fullScore(const TaskAssignment& s) {
    long cost = 0;
    std::int64_t conflicts = 0;
    for (int t = 0; t < 6; ++t) cost += s.cost[t][s.assignment[t]];
    for (auto [a, b] : s.conflicts) {
        if (s.assignment[a] == s.assignment[b]) ++conflicts;
    }
    return {-conflicts, -static_cast<double>(cost)};
}

Score bruteForceOptimum(TaskAssignment s) {  // NOLINT: by-value scratch copy
    Score best{std::numeric_limits<std::int64_t>::min(), 0};
    for (int mask = 0; mask < 4096; ++mask) {  // 4^6 assignments
        int m = mask;
        for (int t = 0; t < 6; ++t) {
            s.assignment[t] = m % 4;
            m /= 4;
        }
        Score sc = fullScore(s);
        if (sc > best) best = sc;
    }
    return best;
}

void testAcceptors(const Score& separableOptimum, const Score& optimum) {
    std::printf("== assignment: acceptors ==\n");
    struct Case {
        optimaru::AcceptanceRule type;
        const char* name;
    };
    const Case cases[] = {
            {optimaru::AcceptanceRule::HillClimbing, "hill climbing"},
            {optimaru::AcceptanceRule::LateAcceptance, "late acceptance"},
            {optimaru::AcceptanceRule::EntityTabu, "entity tabu"},
            {optimaru::AcceptanceRule::SimulatedAnnealing, "simulated annealing"},
            {optimaru::AcceptanceRule::GreatDeluge, "great deluge"},
            {optimaru::AcceptanceRule::StepCountingHillClimbing, "step counting hill climbing"},
    };
    // On the separable (local-optimum-free) instance every acceptor must find
    // the exact optimum.
    for (const Case& c : cases) {
        TaskAssignment s = buildSeparableInstance();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(10);
        config.limits.step_limit = 200000;
        config.search.acceptance.rule = c.type;
        config.search.acceptance.history_length = 50;
        config.search.acceptance.tabu_tenure = 3;
        config.search.acceptance.start_temperature = {2, 50.0};
        config.search.acceptance.water_level_step = {0, 0.01};
        config.search.acceptance.recount_interval = 50;
        config.seed = 7;
        optimaru::Solver<TaskAssignment> solver(config);
        TaskAssignmentCalculator calculator;
        Score result = solver.solve(s, calculator);
        check(result == separableOptimum && fullScore(s) == separableOptimum,
              std::string(c.name) + " reaches separable optimum");
    }
    // On the conflicted instance (real local optima) simulated annealing must
    // still reach the global optimum — it accepts worse moves while hot.
    {
        TaskAssignment s = buildTaskInstance();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(10);
        config.limits.step_limit = 200000;
        config.search.acceptance.rule = optimaru::AcceptanceRule::SimulatedAnnealing;
        config.search.acceptance.start_temperature = {2, 50.0};
        config.seed = 7;
        TaskAssignmentCalculator calculator;
        Score result = optimaru::Solver<TaskAssignment>(config).solve(s, calculator);
        check(result == optimum && fullScore(s) == optimum,
              "simulated annealing escapes local optima (conflicted instance)");
    }
}

void testTerminations(const Score& optimum) {
    std::printf("== assignment: terminations ==\n");
    using Clock = std::chrono::steady_clock;

    {  // step_limit fires long before the 30s time_limit.
        TaskAssignment s = buildTaskInstance();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(30);
        config.limits.step_limit = 1000;
        auto t0 = Clock::now();
        TaskAssignmentCalculator calculator;
        optimaru::Solver<TaskAssignment>(config).solve(s, calculator);
        double sec = std::chrono::duration<double>(Clock::now() - t0).count();
        check(sec < 5.0, "step_limit terminates early (" + std::to_string(sec) + "s)");
    }
    {  // target_score = optimum stops as soon as it is reached (SA is used
        // because it reliably reaches the conflicted instance's optimum).
        TaskAssignment s = buildTaskInstance();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(30);
        config.limits.target_score = optimum;
        config.search.acceptance.rule = optimaru::AcceptanceRule::SimulatedAnnealing;
        config.search.acceptance.start_temperature = {2, 50.0};
        auto t0 = Clock::now();
        TaskAssignmentCalculator calculator;
        Score result = optimaru::Solver<TaskAssignment>(config).solve(s, calculator);
        double sec = std::chrono::duration<double>(Clock::now() - t0).count();
        check(result == optimum && sec < 5.0,
              "target_score terminates early (" + std::to_string(sec) + "s)");
    }
    {  // stall_time_limit: the toy optimum is found almost instantly, so
        // 200ms without improvement ends the solve well before 30s.
        TaskAssignment s = buildTaskInstance();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(30);
        config.limits.stall_time_limit = std::chrono::milliseconds(200);
        auto t0 = Clock::now();
        TaskAssignmentCalculator calculator;
        optimaru::Solver<TaskAssignment>(config).solve(s, calculator);
        double sec = std::chrono::duration<double>(Clock::now() - t0).count();
        check(sec < 5.0, "stall_time_limit terminates early (" + std::to_string(sec) + "s)");
    }
    {  // The cooling gradient follows a step limit when it is further along
        // than the wall clock (simulated annealing on a step-limited run).
        optimaru::Limits termination;
        termination.time_limit = std::chrono::seconds(30);
        termination.step_limit = 1000;
        optimaru::StopCondition stepLimited(termination);
        stepLimited.should_stop(250, Score{0, 0.0}, /*forceTimeCheck=*/true);
        check(std::fabs(stepLimited.progress() - 0.25) < 0.01,
              "progress() = step fraction (0.25) on a step-limited run");
        termination.step_limit = 0;
        optimaru::StopCondition timeOnly(termination);
        timeOnly.should_stop(250, Score{0, 0.0}, /*forceTimeCheck=*/true);
        check(timeOnly.progress() < 0.01, "progress() = time fraction without a step limit");
    }
}

void testEnvironmentMode() {
    std::printf("== assignment: ScoreChecks::Full score-corruption detection ==\n");
    {  // A correct calculator survives ScoreChecks::Full.
        TaskAssignment s = buildTaskInstance();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(10);
        config.limits.step_limit = 2000;
        config.score_checks = optimaru::ScoreChecks::Full;
        TaskAssignmentCalculator calculator;
        bool threw = false;
        try {
            optimaru::Solver<TaskAssignment>(config).solve(s, calculator);
        } catch (const optimaru::ScoreMismatchError&) {
            threw = true;
        }
        check(!threw, "correct objective passes ScoreChecks::Full");
    }
    {  // A buggy calculator is caught.
        TaskAssignment s = buildTaskInstance();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(10);
        config.limits.step_limit = 2000;
        config.score_checks = optimaru::ScoreChecks::Full;
        CorruptedCalculator calculator;
        bool threw = false;
        try {
            optimaru::Solver<TaskAssignment>(config).solve(s, calculator);
        } catch (const optimaru::ScoreMismatchError&) {
            threw = true;
        }
        check(threw, "corrupted objective throws ScoreMismatchError");
    }
}

void testMultiStartAndFfd(const Score& optimum) {
    std::printf("== assignment: multi-start + FFD ordering ==\n");
    {  // 4 SA threads on the conflicted instance; the best start must win.
        TaskAssignment s = buildTaskInstance();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(10);
        config.limits.step_limit = 200000;
        config.parallel_starts = 4;
        config.search.acceptance.rule = optimaru::AcceptanceRule::SimulatedAnnealing;
        config.search.acceptance.start_temperature = {2, 50.0};
        optimaru::Solver<TaskAssignment> solver(config);
        Score result = solver.solve_multistart(
                s, [] { return std::make_unique<TaskAssignmentCalculator>(); });
        check(result == optimum && fullScore(s) == optimum, "multi-start (4 threads) optimum");
    }
    {  // A start that reaches the target ends the run. Start 0 can reach the
        // optimum; starts 1 and 2 search with a handicapped objective and
        // never do, so on their own they would run out the 30 s. Their scores
        // are on the handicapped scale, always below start 0's, so start 0
        // is also the one the comparison picks.
        TaskAssignment s = buildTaskInstance();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(30);
        config.limits.target_score = optimum;
        config.parallel_starts = 3;
        config.search.acceptance.rule = optimaru::AcceptanceRule::SimulatedAnnealing;
        config.search.acceptance.start_temperature = {2, 50.0};
        optimaru::Solver<TaskAssignment> solver(config);
        int made = 0;
        const auto start = std::chrono::steady_clock::now();
        const Score result =
                solver.solve_multistart(s, [&made]() -> std::unique_ptr<TaskAssignmentCalculator> {
                    if (made++ == 0) return std::make_unique<TaskAssignmentCalculator>();
                    return std::make_unique<HandicappedCalculator>();
                });
        const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        check(seconds < 10.0, "multi-start: a start that reaches the target stops the others");
        check(result == optimum && fullScore(s) == optimum,
              "multi-start: the start that reached the target is the one returned");
    }
    {  // A start that throws (here: a raising on_new_best, as a Python callback
        // that raises does): caught on its thread, the siblings stop, and the
        // exception comes out of solve_multistart on the calling thread
        // instead of std::terminate.
        TaskAssignment s = buildTaskInstance();
        const TaskAssignment before = s;
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(30);
        config.parallel_starts = 3;
        config.on_new_best = [](std::int64_t, const Score&) { throw std::runtime_error("boom"); };
        optimaru::Solver<TaskAssignment> solver(config);
        const auto start = std::chrono::steady_clock::now();
        std::string caught;
        try {
            solver.solve_multistart(s, [] { return std::make_unique<TaskAssignmentCalculator>(); });
        } catch (const std::runtime_error& error) {
            caught = error.what();
        }
        const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        check(caught == "boom", "multi-start rethrows a start's exception on the calling thread");
        check(seconds < 10.0, "the other starts stop once one fails (no 30 s wait)");
        check(s.assignment == before.assignment,
              "a failed multi-start leaves the solution as it was");
    }
    {  // A thread that cannot be created (std::system_error, here injected
        // through the test seam before start 1): the start already running
        // is stopped and joined, and the error comes out of solve_multistart
        // instead of std::terminate (a joinable std::thread destroyed).
        TaskAssignment s = buildTaskInstance();
        const TaskAssignment before = s;
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(30);
        config.parallel_starts = 3;
        optimaru::Solver<TaskAssignment> solver(config);
        optimaru::worker_thread_start_hook = [](int index) {
            if (index == 1) {
                throw std::system_error(
                        std::make_error_code(std::errc::resource_unavailable_try_again),
                        "injected thread creation failure");
            }
        };
        const auto start = std::chrono::steady_clock::now();
        bool caught = false;
        try {
            solver.solve_multistart(s, [] { return std::make_unique<TaskAssignmentCalculator>(); });
        } catch (const std::system_error& error) {
            caught = std::string(error.what()).find("injected") != std::string::npos;
        }
        optimaru::worker_thread_start_hook = nullptr;
        const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        check(caught, "multi-start rethrows a thread creation failure on the calling thread");
        check(seconds < 10.0, "the start already running is stopped and joined (no 30 s wait)");
        check(s.assignment == before.assignment,
              "a multi-start that could not start leaves the solution as it was");
    }
    {  // run_worker_threads itself: the failure on the very first thread, and
        // on a later one, with the workers that did start all joined.
        std::atomic<bool> stop{false};
        std::atomic<int> ran{0};
        optimaru::worker_thread_start_hook = [](int index) {
            if (index == 2)
                throw std::system_error(std::make_error_code(std::errc::not_enough_memory));
        };
        bool caught = false;
        try {
            optimaru::run_worker_threads(4, stop, [&stop, &ran](int) {
                while (!stop.load()) std::this_thread::yield();
                ++ran;
            });
        } catch (const std::system_error&) {
            caught = true;
        }
        check(caught && stop.load() && ran.load() == 2,
              "run_worker_threads: a failure at thread 2 sets stop, joins threads 0 and 1, "
              "rethrows");
        stop.store(false);
        optimaru::worker_thread_start_hook = [](int) {
            throw std::system_error(
                    std::make_error_code(std::errc::resource_unavailable_try_again));
        };
        caught = false;
        try {
            optimaru::run_worker_threads(3, stop, [](int) {});
        } catch (const std::system_error&) {
            caught = true;
        }
        check(caught && stop.load(), "run_worker_threads: a failure at thread 0 is rethrown too");
        optimaru::worker_thread_start_hook = nullptr;
        stop.store(false);
        std::vector<int> seen(5, 0);
        optimaru::run_worker_threads(5, stop,
                                     [&seen](int i) { seen[static_cast<std::size_t>(i)] = i + 1; });
        check(seen == std::vector<int>({1, 2, 3, 4, 5}) && !stop.load(),
              "run_worker_threads runs work(i) once per index and joins them all");
    }
    {  // request_stop() reaches every start.
        TaskAssignment s = buildTaskInstance();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(30);
        config.parallel_starts = 3;
        optimaru::Solver<TaskAssignment> solver(config);
        solver.request_stop();
        const auto start = std::chrono::steady_clock::now();
        solver.solve_multistart(s, [] { return std::make_unique<TaskAssignmentCalculator>(); });
        const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        check(seconds < 10.0, "request_stop() stops every multi-start start");
    }
    {  // First Fit Decreasing: initialize high-cost tasks first.
        TaskAssignment s = buildSeparableInstance();
        auto rowMax = [&s](int t) {
            return static_cast<double>(*std::max_element(s.cost[t].begin(), s.cost[t].end()));
        };
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(10);
        config.limits.step_limit = 50000;
        config.construction.difficulty = rowMax;
        TaskAssignmentCalculator calculator;
        Score result = optimaru::Solver<TaskAssignment>(config).solve(s, calculator);
        check(result == bruteForceOptimum(buildSeparableInstance()),
              "FFD entityDifficulty ordering");
    }
}

}  // namespace

// ===========================================================================
// sequence shape: TSP (cyclic routes over 2-D points)
// ===========================================================================
namespace {

struct TspSolution {
    std::vector<std::pair<double, double>> city;  // coordinates per element id
    std::vector<std::vector<int>> routes;         // the list decision variable
};

}  // namespace

template <>
struct optimaru::SolutionTraits<TspSolution> {
    static constexpr optimaru::Shape shape = optimaru::Shape::Sequence;
    static int list_count(const TspSolution& s) { return static_cast<int>(s.routes.size()); }
    static int element_count(const TspSolution& s) { return static_cast<int>(s.city.size()); }
    static int list_size(const TspSolution& s, int l) {
        return static_cast<int>(s.routes[l].size());
    }
    static int element_at(const TspSolution& s, int l, int p) { return s.routes[l][p]; }
    static void insert_element(TspSolution& s, int l, int p, int e) {
        s.routes[l].insert(s.routes[l].begin() + p, e);
    }
    static int remove_element(TspSolution& s, int l, int p) {
        int e = s.routes[l][p];
        s.routes[l].erase(s.routes[l].begin() + p);
        return e;
    }
    static void reverse_segment(TspSolution& s, int l, int first, int last) {
        std::reverse(s.routes[l].begin() + first, s.routes[l].begin() + last + 1);
    }
};

namespace {

double dist(const TspSolution& s, int a, int b) {
    double dx = s.city[a].first - s.city[b].first;
    double dy = s.city[a].second - s.city[b].second;
    return std::sqrt(dx * dx + dy * dy);
}

double routeLength(const TspSolution& s, const std::vector<int>& route) {
    if (route.size() < 2) return 0.0;
    double total = 0.0;
    for (std::size_t i = 0; i + 1 < route.size(); ++i) total += dist(s, route[i], route[i + 1]);
    total += dist(s, route.back(), route.front());  // close the cycle
    return total;
}

class TspCalculator final : public optimaru::Objective<TspSolution> {
public:
    void rebuild(TspSolution& solution) override {
        sched_ = &solution;
        length_.assign(solution.routes.size(), 0.0);
        total_ = 0.0;
        for (std::size_t l = 0; l < solution.routes.size(); ++l) {
            length_[l] = routeLength(solution, solution.routes[l]);
            total_ += length_[l];
        }
    }
    // Coarse-grained list hooks: re-derive the touched route's length (O(route)).
    void before_list_edit(int l) override { total_ -= length_[l]; }
    void after_list_edit(int l) override {
        length_[l] = routeLength(*sched_, sched_->routes[l]);
        total_ += length_[l];
    }
    Score score() override { return {0, -total_}; }

private:
    TspSolution* sched_ = nullptr;
    std::vector<double> length_;
    double total_ = 0.0;
};

TspSolution buildTsp(int routeCount) {
    TspSolution s;
    s.city = {{0, 0}, {10, 1}, {9, 8}, {2, 9}, {5, 5}, {12, 6}, {1, 4}, {7, 12}};
    s.routes.assign(routeCount, {});
    return s;
}

double bruteForceTspOptimum(const TspSolution& s) {
    std::vector<int> perm(s.city.size() - 1);
    std::iota(perm.begin(), perm.end(), 1);  // city 0 fixed first (cycle symmetry)
    double best = std::numeric_limits<double>::infinity();
    do {
        std::vector<int> route{0};
        route.insert(route.end(), perm.begin(), perm.end());
        best = std::min(best, routeLength(s, route));
    } while (std::next_permutation(perm.begin(), perm.end()));
    return best;
}

void testListVariable() {
    std::printf("== sequence: TSP construction + local search ==\n");
    const double optimum = bruteForceTspOptimum(buildTsp(1));
    {  // Single route: cheapest-insertion CH + LS must find the optimal tour.
        TspSolution s = buildTsp(1);
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(10);
        config.limits.step_limit = 300000;
        config.seed = 3;
        TspCalculator calculator;
        Score result = optimaru::Solver<TspSolution>(config).solve(s, calculator);
        bool allAssigned = s.routes[0].size() == s.city.size();
        check(allAssigned && std::fabs(-result.soft - optimum) < 1e-9,
              "single-route TSP reaches brute-force optimum (" + std::to_string(optimum) + ")");
        check(std::fabs(routeLength(s, s.routes[0]) - optimum) < 1e-9,
              "returned tour really has the optimal length");
    }
    {  // Two routes under ScoreChecks::Full: exercises relocate/swap/2-opt across
        // lists with every step verified against a full recompute.
        TspSolution s = buildTsp(2);
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(10);
        config.limits.step_limit = 3000;
        config.score_checks = optimaru::ScoreChecks::Full;
        TspCalculator calculator;
        bool threw = false;
        Score result{0, 0};
        try {
            result = optimaru::Solver<TspSolution>(config).solve(s, calculator);
        } catch (const optimaru::ScoreMismatchError&) {
            threw = true;
        }
        std::vector<int> all;
        for (const auto& r : s.routes) all.insert(all.end(), r.begin(), r.end());
        std::sort(all.begin(), all.end());
        bool conserved = all.size() == s.city.size();
        for (std::size_t i = 0; conserved && i < all.size(); ++i)
            conserved = (all[i] == static_cast<int>(i));
        double recomputed = routeLength(s, s.routes[0]) + routeLength(s, s.routes[1]);
        check(!threw, "list moves pass ScoreChecks::Full (2 routes)");
        check(conserved, "every element assigned exactly once after solving");
        check(std::fabs(-result.soft - recomputed) < 1e-9, "returned score matches recompute");
    }
}

}  // namespace

// ===========================================================================
// Performance features: adaptive operator selection, LNS, nearby selection
// ===========================================================================
namespace {

// A task assignment with pins. Each task costs its worker's index, so every
// pinned task on a high worker is a move away from a better score.
struct PinnedTasks {
    int workerCount = 0;
    std::vector<int> assignment;
    std::vector<char> pinned;
};

}  // namespace

template <>
struct optimaru::SolutionTraits<PinnedTasks> {
    static constexpr optimaru::Shape shape = optimaru::Shape::Assignment;
    static int entity_count(const PinnedTasks& s) { return static_cast<int>(s.assignment.size()); }
    static int value_count(const PinnedTasks& s) { return s.workerCount; }
    static int value_of(const PinnedTasks& s, int e) { return s.assignment[e]; }
    static void assign(PinnedTasks& s, int e, int v) { s.assignment[e] = v; }
    static bool is_pinned(const PinnedTasks& s, int e) { return s.pinned[e] != 0; }
};

namespace {

class PinnedTasksCalculator final : public optimaru::Objective<PinnedTasks> {
public:
    void rebuild(PinnedTasks& solution) override {
        solution_ = &solution;
        cost_ = 0;
        for (int w : solution.assignment) cost_ += std::max(w, 0);
    }
    void before_assign(int e) override { cost_ -= std::max(solution_->assignment[e], 0); }
    void after_assign(int e) override { cost_ += std::max(solution_->assignment[e], 0); }
    Score score() override { return {0, -static_cast<double>(cost_)}; }

private:
    PinnedTasks* solution_ = nullptr;
    long cost_ = 0;
};

// A caller's operator, as a domain would write one. `calls` shows whether the
// engine gave it a turn.
class AnyTaskMove final : public optimaru::MoveOperator<PinnedTasks> {
public:
    explicit AnyTaskMove(int& calls) : calls_(calls) {}
    const char* name() const override { return "anyTask"; }
    bool apply(optimaru::OperatorContext<PinnedTasks>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        ++calls_;
        PinnedTasks& s = ctx.editor.solution();
        task_ = static_cast<int>(rng() % s.assignment.size());
        if (s.pinned[task_]) return false;
        previous_ = s.assignment[task_];
        ctx.editor.assign(task_, static_cast<int>(rng() % s.workerCount));
        entities.push_back(task_);
        return true;
    }
    void undo_last(optimaru::OperatorContext<PinnedTasks>& ctx) override {
        ctx.editor.assign(task_, previous_);
    }

private:
    int& calls_;
    int task_ = 0;
    int previous_ = 0;
};

PinnedTasks solvePinned(std::vector<int> assignment, std::vector<char> pinned, int* customCalls) {
    PinnedTasks s{6, std::move(assignment), std::move(pinned)};
    optimaru::Settings config;
    config.limits.step_limit = 4000;
    config.seed = 3;
    config.score_checks = optimaru::ScoreChecks::Full;
    config.search.descent_weight = 0.05;
    config.search.custom_move_probability = 0.3;
    config.search.ruin_recreate.unimproved_step_trigger = 20;
    config.search.ruin_recreate.as_move_weight = 0.1;
    optimaru::Solver<PinnedTasks> solver(config);
    if (customCalls != nullptr)
        solver.add_move_operator(std::make_unique<AnyTaskMove>(*customCalls));
    PinnedTasksCalculator calculator;
    solver.solve(s, calculator);
    return s;
}

template <typename Solve>
bool refusesPinnedWithoutValue(Solve&& solve) {
    try {
        solve();
    } catch (const std::invalid_argument& error) {
        return std::string(error.what()) == "entity 1 is pinned but has no value";
    }
    return false;
}

void testPinnedEntities() {
    std::printf("== assignment: pinned entities ==\n");
    const std::vector<int> high{5, 4, 5, 3, 5, 4, 5, 2};

    int calls = 0;
    PinnedTasks all = solvePinned(high, std::vector<char>(high.size(), 1), &calls);
    check(all.assignment == high, "all pinned: every value kept");
    check(calls == 0, "all pinned: no custom operator runs");

    PinnedTasks reported{6, high, std::vector<char>(high.size(), 1)};
    optimaru::Settings reporting;
    std::vector<double> softScores;
    reporting.on_new_best = [&](std::int64_t, const Score& score) {
        softScores.push_back(score.soft);
    };
    PinnedTasksCalculator reportingCalculator;
    optimaru::Solver<PinnedTasks>(reporting).solve(reported, reportingCalculator);
    check(softScores == std::vector<double>{-33.0},
          "all pinned: on_new_best reports the score once");

    std::vector<char> allButOne(high.size(), 1);
    allButOne[3] = 0;
    std::vector<int> oneFree = high;
    oneFree[3] = 0;
    check(solvePinned(high, allButOne, nullptr).assignment == oneFree,
          "one free entity: it moves, no pin does");

    std::vector<char> half(high.size(), 0);
    std::vector<int> expected(high.size(), 0);
    for (std::size_t e = 0; e < high.size(); e += 2) {
        half[e] = 1;
        expected[e] = high[e];
    }
    check(solvePinned(high, half, nullptr).assignment == expected,
          "half pinned: pins kept, the rest optimal");

    std::vector<int> freeUnassigned = high;
    freeUnassigned[3] = -1;
    check(solvePinned(freeUnassigned, allButOne, nullptr).assignment == oneFree,
          "construction places an unassigned entity that is not pinned");

    std::vector<int> pinnedUnassigned = high;
    pinnedUnassigned[1] = -1;
    check(refusesPinnedWithoutValue([&] { solvePinned(pinnedUnassigned, allButOne, nullptr); }),
          "a pinned entity with no value is refused");

    // Off the calling thread an escaping exception would end the process, so
    // these two must refuse before they start one.
    PinnedTasks multiStart{6, pinnedUnassigned, allButOne};
    optimaru::Settings threaded;
    threaded.parallel_starts = 2;
    optimaru::Solver<PinnedTasks> multiStartSolver(threaded);
    check(refusesPinnedWithoutValue([&] {
              multiStartSolver.solve_multistart(
                      multiStart, [] { return std::make_unique<PinnedTasksCalculator>(); });
          }),
          "multi-start refuses it on the calling thread");

    PinnedTasks managed{6, pinnedUnassigned, allButOne};
    PinnedTasksCalculator managedCalculator;
    check(refusesPinnedWithoutValue(
                  [&] { optimaru::solve_in_background({}, managed, managedCalculator); }),
          "solve_in_background refuses it on the calling thread");

    PinnedTasks noValues{0, pinnedUnassigned, allButOne};
    PinnedTasksCalculator noValuesCalculator;
    check(refusesPinnedWithoutValue([&] {
              optimaru::Solver<PinnedTasks>(optimaru::Settings{})
                      .solve(noValues, noValuesCalculator);
          }),
          "an empty value range does not skip the check");
}

void testRouletteWheelSelector() {
    std::printf("== LNS: roulette-wheel operator selection adapts ==\n");
    optimaru::RouletteWheelSelector selector(3, {33.0, 9.0, 3.0, 0.0}, 0.8);
    for (int i = 0; i < 50; ++i) {
        selector.update(1, optimaru::MoveOutcome::NewBest);
        selector.update(0, optimaru::MoveOutcome::Rejected);
        selector.update(2, optimaru::MoveOutcome::Rejected);
    }
    std::mt19937_64 rng(1);
    int picks[3] = {0, 0, 0};
    for (int i = 0; i < 3000; ++i) ++picks[selector.select(rng)];
    check(picks[1] > 2500,
          "rewarded operator dominates selection (" + std::to_string(picks[1]) + "/3000)");
}

class CountingMove final : public optimaru::MoveOperator<TaskAssignment> {
public:
    explicit CountingMove(int& calls) : calls_(calls) {}
    const char* name() const override { return "counting"; }
    bool apply(optimaru::OperatorContext<TaskAssignment>&, std::mt19937_64&,
               std::vector<int>&) override {
        ++calls_;
        return false;
    }
    void undo_last(optimaru::OperatorContext<TaskAssignment>&) override {}

private:
    int& calls_;
};

void testCustomMoveProbability() {
    std::printf("== Local search: custom move probability gate ==\n");
    auto run = [](double probability) {
        TaskAssignment s = buildTaskInstance();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::minutes(1);
        config.limits.step_limit = 1000;
        config.search.ruin_recreate.enabled = false;
        config.search.custom_move_probability = probability;
        config.seed = 7;
        int calls = 0;
        optimaru::Solver<TaskAssignment> solver(config);
        solver.add_move_operator(std::make_unique<CountingMove>(calls));
        TaskAssignmentCalculator calculator;
        solver.solve(s, calculator);
        return calls;
    };

    check(run(0.0) == 0, "zero gate bypasses custom move selection");
    check(run(1.0) > 0, "one gate preserves unrestricted custom move selection");
}

void testLnsEscapesLocalOptimum(const Score& optimum) {
    std::printf("== LNS: ruin & recreate escapes local optima ==\n");
    // Hill climbing alone gets permanently stuck on the conflicted instance
    // (shown in the acceptor tests); with LNS kicks it must reach the optimum.
    TaskAssignment s = buildTaskInstance();
    optimaru::Settings config;
    // The step limit must be the ONLY binding criterion: with a 10s wall-clock
    // limit next to it, a slow runner stops at whatever step it happens to
    // reach, so the same seed explores a different amount of search on every
    // machine. That is how a real regression in the ruin order passed CI while
    // failing locally 3/3 — the check could not do its job. The budget below
    // is far above the ~0.1s these 100000 steps take; it exists only so the
    // run cannot hang forever.
    config.limits.time_limit = std::chrono::minutes(10);
    config.limits.step_limit = 100000;
    config.search.acceptance.rule = optimaru::AcceptanceRule::HillClimbing;
    config.search.ruin_recreate.enabled = true;
    config.search.ruin_recreate.unimproved_step_trigger = 200;
    // 6 entities: ruin 2-4 of them per kick (the default 10-30% would remove
    // just one, too weak to escape the conflict cycle's local optimum).
    config.search.ruin_recreate.min_ruin_fraction = 0.34;
    config.search.ruin_recreate.max_ruin_fraction = 0.67;
    config.seed = 7;
    TaskAssignmentCalculator calculator;
    Score result = optimaru::Solver<TaskAssignment>(config).solve(s, calculator);
    check(result == optimum && fullScore(s) == optimum,
          "hill climbing + LNS reaches the global optimum");

    // The same under ScoreChecks::Full: ruin (unassign) and greedy recreate must
    // keep the incremental score consistent at every step.
    TaskAssignment s2 = buildTaskInstance();
    config.limits.step_limit = 3000;
    config.search.ruin_recreate.unimproved_step_trigger = 50;
    config.score_checks = optimaru::ScoreChecks::Full;
    TaskAssignmentCalculator calculator2;
    bool threw = false;
    try {
        optimaru::Solver<TaskAssignment>(config).solve(s2, calculator2);
    } catch (const optimaru::ScoreMismatchError&) {
        threw = true;
    }
    check(!threw, "assignment-shape ruin & recreate passes ScoreChecks::Full");
}

// 100 uniform-random cities (fixed instance seed): the standard benchmark
// shape for nearby selection + LNS. Verified over a 5-seed sweep: the boosted
// solver beats plain local search on every seed, even when the boosted run
// gets only HALF the outer steps (its LNS polish moves are extra evaluations,
// so halving the outer budget makes the total evaluation counts comparable).
TspSolution buildRandomTsp(int routeCount, int cityCount) {
    TspSolution s;
    std::mt19937_64 g(99);
    std::uniform_real_distribution<double> u(0.0, 300.0);
    for (int i = 0; i < cityCount; ++i) {
        double x = u(g), y = u(g);
        s.city.emplace_back(x, y);
    }
    s.routes.assign(routeCount, {});
    return s;
}

void testNearbyAndLnsOnRandomTsp() {
    std::printf("== sequence: nearby selection + LNS on random TSP (100 cities) ==\n");

    auto solveWith = [&](bool performanceFeatures) {
        TspSolution s = buildRandomTsp(1, 100);
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(60);
        config.limits.step_limit = performanceFeatures ? 200000 : 400000;  // fairness: see above
        config.seed = 11;
        if (performanceFeatures) {
            TspSolution ref = s;  // meter closes over the coordinates
            config.search.nearby.distance = [ref](int a, int b) { return dist(ref, a, b); };
            config.search.nearby.size = 10;
            config.search.ruin_recreate.enabled = true;  // trigger/polish: engine defaults
        }
        TspCalculator calculator;
        Score result = optimaru::Solver<TspSolution>(config).solve(s, calculator);
        return -result.soft;  // tour length
    };

    double plain = solveWith(false);
    double boosted = solveWith(true);
    std::printf("    plain LS (400k steps) tour = %.3f, nearby+LNS (200k steps) tour = %.3f\n",
                plain, boosted);
    check(boosted < plain - 1e-9, "nearby+LNS beats plain LS with half the outer step budget");

    // Consistency: 2 routes with nearby + LNS under ScoreChecks::Full, plus element
    // conservation (ruin/recreate must not lose or duplicate visits).
    TspSolution s = buildRandomTsp(2, 24);
    optimaru::Settings config;
    config.limits.time_limit = std::chrono::seconds(30);
    config.limits.step_limit = 3000;
    config.seed = 5;
    TspSolution ref = s;
    config.search.nearby.distance = [ref](int a, int b) { return dist(ref, a, b); };
    config.search.ruin_recreate.enabled = true;
    config.search.ruin_recreate.unimproved_step_trigger = 100;
    config.search.ruin_recreate.polish_steps =
            50;  // keep ScoreChecks::Full (full recompute per move) fast
    config.score_checks = optimaru::ScoreChecks::Full;
    TspCalculator calculator;
    bool threw = false;
    try {
        optimaru::Solver<TspSolution>(config).solve(s, calculator);
    } catch (const optimaru::ScoreMismatchError&) {
        threw = true;
    }
    std::vector<int> all;
    for (const auto& r : s.routes) all.insert(all.end(), r.begin(), r.end());
    std::sort(all.begin(), all.end());
    bool conserved = all.size() == s.city.size();
    for (std::size_t i = 0; conserved && i < all.size(); ++i) {
        conserved = (all[i] == static_cast<int>(i));
    }
    check(!threw, "sequence nearby moves + ruin & recreate pass ScoreChecks::Full");
    check(conserved, "elements conserved through ruin & recreate");
}

}  // namespace

// ===========================================================================
// Derived state (DerivedState): arrival times derived from route order
// ===========================================================================
namespace {

struct ShadowTsp {
    std::vector<std::pair<double, double>> city;
    std::vector<double> dueTime;           // problem fact per city
    std::vector<std::vector<int>> routes;  // genuine list variables
    std::vector<double> arrival;           // SHADOW: distance from route start
};

}  // namespace

template <>
struct optimaru::SolutionTraits<ShadowTsp> {
    static constexpr optimaru::Shape shape = optimaru::Shape::Sequence;
    static int list_count(const ShadowTsp& s) { return (int)s.routes.size(); }
    static int element_count(const ShadowTsp& s) { return (int)s.city.size(); }
    static int list_size(const ShadowTsp& s, int l) { return (int)s.routes[l].size(); }
    static int element_at(const ShadowTsp& s, int l, int p) { return s.routes[l][p]; }
    static void insert_element(ShadowTsp& s, int l, int p, int e) {
        s.routes[l].insert(s.routes[l].begin() + p, e);
    }
    static int remove_element(ShadowTsp& s, int l, int p) {
        int e = s.routes[l][p];
        s.routes[l].erase(s.routes[l].begin() + p);
        return e;
    }
    static void reverse_segment(ShadowTsp& s, int l, int a, int b) {
        std::reverse(s.routes[l].begin() + a, s.routes[l].begin() + b + 1);
    }
};

namespace {

double sDist(const ShadowTsp& s, int a, int b) {
    double dx = s.city[a].first - s.city[b].first, dy = s.city[a].second - s.city[b].second;
    return std::sqrt(dx * dx + dy * dy);
}

// The shadow supplier: arrival[e] = cumulative distance from the route start.
class ArrivalTimeListener final : public optimaru::DerivedState<ShadowTsp> {
public:
    void rebuild(ShadowTsp& s) override {
        for (int l = 0; l < (int)s.routes.size(); ++l) after_list_edit(s, l);
    }
    void after_list_edit(ShadowTsp& s, int l) override {
        double t = 0;
        for (std::size_t p = 0; p < s.routes[l].size(); ++p) {
            if (p > 0) t += sDist(s, s.routes[l][p - 1], s.routes[l][p]);
            s.arrival[s.routes[l][p]] = t;
        }
    }
};

// Reads ONLY the shadow: soft = -(total distance) - 10 * total lateness.
class LatenessCalculator final : public optimaru::Objective<ShadowTsp> {
public:
    void rebuild(ShadowTsp& s) override {
        sched_ = &s;
        perList_.assign(s.routes.size(), 0.0);
        total_ = 0;
        for (int l = 0; l < (int)s.routes.size(); ++l) {
            perList_[l] = listCost(l);
            total_ += perList_[l];
        }
    }
    void before_list_edit(int l) override { total_ -= perList_[l]; }
    void after_list_edit(int l) override {  // shadows already updated
        perList_[l] = listCost(l);
        total_ += perList_[l];
    }
    Score score() override { return {0, -total_}; }

private:
    double listCost(int l) const {
        const auto& r = sched_->routes[l];
        double cost = 0;
        for (std::size_t p = 0; p < r.size(); ++p) {
            if (p > 0) cost += sDist(*sched_, r[p - 1], r[p]);
            double late = sched_->arrival[r[p]] - sched_->dueTime[r[p]];
            if (late > 0) cost += 10.0 * late;
        }
        return cost;
    }
    ShadowTsp* sched_ = nullptr;
    std::vector<double> perList_;
    double total_ = 0;
};

// Deliberately broken: never updates the shadow. ScoreChecks::Full must catch it.
class StaleArrivalListener final : public optimaru::DerivedState<ShadowTsp> {
public:
    void rebuild(ShadowTsp& s) override {
        ArrivalTimeListener fresh;
        fresh.rebuild(s);
    }
    void after_list_edit(ShadowTsp&, int) override {}  // "forgets" to update
};

ShadowTsp buildShadowTsp() {
    ShadowTsp s;
    std::mt19937_64 g(7);
    std::uniform_real_distribution<double> u(0.0, 100.0);
    for (int i = 0; i < 12; ++i) {
        s.city.emplace_back(u(g), u(g));
        s.dueTime.push_back(60.0 + u(g));
    }
    s.arrival.assign(12, 0.0);
    s.routes.assign(2, {});
    return s;
}

void testShadowVariables() {
    std::printf("== Shadow variables (VariableListener) ==\n");
    {  // Full solve (moves + LNS + best-restore) under ScoreChecks::Full: shadows
        // must stay consistent through every mutation path.
        ShadowTsp s = buildShadowTsp();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(30);
        config.limits.step_limit = 3000;
        config.search.ruin_recreate.enabled = true;
        config.search.ruin_recreate.unimproved_step_trigger = 100;
        config.search.ruin_recreate.polish_steps = 50;
        config.search.moves.or_opt_weight = 0.15;
        config.search.moves.tail_swap_weight = 0.15;
        config.search.moves.cross_exchange_weight = 0.15;
        config.score_checks = optimaru::ScoreChecks::Full;
        config.seed = 4;
        optimaru::Solver<ShadowTsp> solver(config);
        solver.add_derived_state(std::make_unique<ArrivalTimeListener>());
        LatenessCalculator calculator;
        bool threw = false;
        Score result{0, 0};
        try {
            result = solver.solve(s, calculator);
        } catch (const optimaru::ScoreMismatchError&) {
            threw = true;
        }
        check(!threw, "shadow-reading solve passes ScoreChecks::Full (all mutation paths)");

        // The returned solution's shadows equal a from-scratch recompute.
        std::vector<double> returned = s.arrival;
        ArrivalTimeListener fresh;
        fresh.rebuild(s);
        bool same = true;
        std::vector<int> all;
        for (const auto& r : s.routes) all.insert(all.end(), r.begin(), r.end());
        for (int e : all) same = same && std::fabs(returned[e] - s.arrival[e]) < 1e-9;
        LatenessCalculator verify;
        verify.rebuild(s);
        Score recomputed = verify.score();
        check(same && recomputed.hard == result.hard &&
                      std::fabs(recomputed.soft - result.soft) < 1e-6,
              "returned shadows and score match a from-scratch recompute");
    }
    {  // A listener that forgets to update the shadow is caught as corruption.
        ShadowTsp s = buildShadowTsp();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(30);
        config.limits.step_limit = 2000;
        config.score_checks = optimaru::ScoreChecks::Full;
        optimaru::Solver<ShadowTsp> solver(config);
        solver.add_derived_state(std::make_unique<StaleArrivalListener>());
        LatenessCalculator calculator;
        bool threw = false;
        try {
            solver.solve(s, calculator);
        } catch (const optimaru::ScoreMismatchError&) {
            threw = true;
        }
        check(threw, "stale shadow listener throws ScoreMismatchError");
    }
}

}  // namespace

// ===========================================================================
// Chained variables: linked-chain VRP over previous pointers
// ===========================================================================
namespace {

struct ChainedVrp {
    std::vector<std::pair<double, double>> depot;  // anchor coordinates
    std::vector<std::pair<double, double>> visit;  // entity coordinates
    std::vector<int> previous;                     // chained decision variable
};

}  // namespace

template <>
struct optimaru::SolutionTraits<ChainedVrp> {
    static constexpr optimaru::Shape shape = optimaru::Shape::Chain;
    static int anchor_count(const ChainedVrp& s) { return (int)s.depot.size(); }
    static int entity_count(const ChainedVrp& s) { return (int)s.visit.size(); }
    static int previous_of(const ChainedVrp& s, int e) { return s.previous[e]; }
    static void link(ChainedVrp& s, int e, int p) { s.previous[e] = p; }
};

namespace {

double cDist(std::pair<double, double> a, std::pair<double, double> b) {
    double dx = a.first - b.first, dy = a.second - b.second;
    return std::sqrt(dx * dx + dy * dy);
}

double chainCost(const ChainedVrp& s, int anchor, const std::vector<int>& chain) {
    double cost = 0;
    std::pair<double, double> at = s.depot[anchor];
    for (int e : chain) {
        cost += cDist(at, s.visit[e]);
        at = s.visit[e];
    }
    return cost;
}

class ChainDistanceCalculator final : public optimaru::ChainObjective<ChainedVrp> {
public:
    void rebuild(ChainedVrp& s, const std::vector<std::vector<int>>& chains) override {
        perChain_.assign(chains.size(), 0.0);
        total_ = 0;
        for (std::size_t a = 0; a < chains.size(); ++a) {
            perChain_[a] = chainCost(s, (int)a, chains[a]);
            total_ += perChain_[a];
        }
    }
    void before_chain_edit(ChainedVrp&, int a, const std::vector<int>&) override {
        total_ -= perChain_[a];
    }
    void after_chain_edit(ChainedVrp& s, int a, const std::vector<int>& chain) override {
        perChain_[a] = chainCost(s, a, chain);
        total_ += perChain_[a];
    }
    Score score() override { return {0, -total_}; }

private:
    std::vector<double> perChain_;
    double total_ = 0;
};

// Independent decode of the chains from the previous pointers (does not use
// the engine's internal list view — verifies the pointers themselves).
std::vector<std::vector<int>> decodeChains(const ChainedVrp& s) {
    int n = (int)s.visit.size(), anchors = (int)s.depot.size();
    std::vector<int> successor(n, -1), head(anchors, -1);
    for (int e = 0; e < n; ++e) {
        int p = s.previous[e];
        if (p == optimaru::chain::kUnlinked) continue;
        if (optimaru::chain::is_anchor(p)) {
            head[optimaru::chain::anchor_index(p)] = e;
        } else {
            successor[p] = e;
        }
    }
    std::vector<std::vector<int>> chains(anchors);
    for (int a = 0; a < anchors; ++a) {
        for (int e = head[a]; e != -1; e = successor[e]) chains[a].push_back(e);
    }
    return chains;
}

void testChainedVariables() {
    std::printf("== Chained variables (previous-pointer VRP) ==\n");
    ChainedVrp s;
    s.depot = {{0, 0}, {100, 100}};
    std::mt19937_64 g(21);
    std::uniform_real_distribution<double> u(0.0, 100.0);
    for (int i = 0; i < 10; ++i) s.visit.emplace_back(u(g), u(g));
    s.previous.assign(10, optimaru::chain::kUnlinked);

    optimaru::Settings config;
    config.limits.time_limit = std::chrono::seconds(30);
    config.limits.step_limit = 3000;
    config.seed = 9;
    config.search.moves.or_opt_weight = 0.15;
    config.search.moves.tail_swap_weight = 0.15;
    config.search.moves.cross_exchange_weight = 0.15;
    config.search.ruin_recreate.enabled = true;
    config.search.ruin_recreate.unimproved_step_trigger = 100;
    config.search.ruin_recreate.polish_steps = 50;
    ChainedVrp ref = s;
    config.search.nearby.distance = [ref](int a, int b) {
        return cDist(ref.visit[a], ref.visit[b]);
    };
    config.score_checks = optimaru::ScoreChecks::Full;

    optimaru::Solver<ChainedVrp> solver(config);
    ChainDistanceCalculator calculator;
    bool threw = false;
    Score result{0, 0};
    try {
        result = solver.solve(s, calculator);
    } catch (const optimaru::ScoreMismatchError&) {
        threw = true;
    }
    check(!threw, "chained solve passes ScoreChecks::Full (all moves + LNS)");

    // Validity of the returned previous pointers, decoded independently.
    std::vector<std::vector<int>> chains = decodeChains(s);
    std::vector<int> all;
    for (const auto& c : chains) all.insert(all.end(), c.begin(), c.end());
    std::sort(all.begin(), all.end());
    bool valid = all.size() == s.visit.size();
    for (std::size_t i = 0; valid && i < all.size(); ++i) valid = (all[i] == (int)i);
    check(valid, "every visit appears exactly once in the decoded chains");

    // The pointers encode the same solution the solver scored.
    double recomputed = 0;
    for (std::size_t a = 0; a < chains.size(); ++a) {
        recomputed += chainCost(s, (int)a, chains[a]);
    }
    check(std::fabs(-result.soft - recomputed) < 1e-6,
          "pointer-decoded chains reproduce the returned score");
}

}  // namespace

// ===========================================================================
// Engine breadth: expanded moves, score explanation, background solve, limits
// ===========================================================================
namespace {

// Cost components for the composite/analysis tests: the task-assignment score
// split into two independently-maintained constraints.
class TaskCostComponent final : public optimaru::Objective<TaskAssignment> {
public:
    void rebuild(TaskAssignment& solution) override {
        sched_ = &solution;
        cost_ = 0;
        for (std::size_t t = 0; t < solution.assignment.size(); ++t) {
            if (solution.assignment[t] >= 0) cost_ += solution.cost[t][solution.assignment[t]];
        }
    }
    void before_assign(int t) override {
        if (sched_->assignment[t] >= 0) cost_ -= sched_->cost[t][sched_->assignment[t]];
    }
    void after_assign(int t) override {
        if (sched_->assignment[t] >= 0) cost_ += sched_->cost[t][sched_->assignment[t]];
    }
    Score score() override { return {0, -static_cast<double>(cost_)}; }

private:
    TaskAssignment* sched_ = nullptr;
    long cost_ = 0;
};

class TaskConflictComponent final : public optimaru::Objective<TaskAssignment> {
public:
    void rebuild(TaskAssignment& solution) override {
        sched_ = &solution;
        conflictsOf_.assign(solution.assignment.size(), {});
        for (auto [a, b] : solution.conflicts) {
            conflictsOf_[a].push_back(b);
            conflictsOf_[b].push_back(a);
        }
        conflicts_ = 0;
        for (int t = 0; t < static_cast<int>(solution.assignment.size()); ++t) {
            int w = solution.assignment[t];
            if (w < 0) continue;
            for (int u : conflictsOf_[t]) {
                if (u < t && solution.assignment[u] == w) ++conflicts_;
            }
        }
    }
    void before_assign(int t) override { applyDelta(t, -1); }
    void after_assign(int t) override { applyDelta(t, +1); }
    Score score() override { return {-conflicts_, 0.0}; }

private:
    void applyDelta(int t, int sign) {
        int w = sched_->assignment[t];
        if (w < 0) return;
        for (int u : conflictsOf_[t]) {
            if (u != t && sched_->assignment[u] == w) conflicts_ += sign;
        }
    }
    TaskAssignment* sched_ = nullptr;
    std::vector<std::vector<int>> conflictsOf_;
    std::int64_t conflicts_ = 0;
};

void testExpandedMoves() {
    std::printf("== Moves: Or-opt + tail swap (2-opt*) ==\n");
    {  // 2 routes with the full move mix under ScoreChecks::Full + conservation.
        TspSolution s = buildRandomTsp(2, 24);
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(30);
        config.limits.step_limit = 4000;
        config.seed = 5;
        config.search.moves.or_opt_weight = 0.2;
        config.search.moves.tail_swap_weight = 0.2;
        config.search.moves.cross_exchange_weight = 0.2;
        config.score_checks = optimaru::ScoreChecks::Full;
        TspCalculator calculator;
        bool threw = false;
        try {
            optimaru::Solver<TspSolution>(config).solve(s, calculator);
        } catch (const optimaru::ScoreMismatchError&) {
            threw = true;
        }
        std::vector<int> all;
        for (const auto& r : s.routes) all.insert(all.end(), r.begin(), r.end());
        std::sort(all.begin(), all.end());
        bool conserved = all.size() == s.city.size();
        for (std::size_t i = 0; conserved && i < all.size(); ++i) {
            conserved = (all[i] == static_cast<int>(i));
        }
        check(!threw, "Or-opt + tail swap pass ScoreChecks::Full (2 routes)");
        check(conserved, "elements conserved with the expanded move mix");
    }
    {  // Single route: expanded mix still reaches the brute-force optimum
        // (tail swap auto-disables with one list).
        TspSolution s = buildTsp(1);
        const double optimum = bruteForceTspOptimum(buildTsp(1));
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(10);
        config.limits.step_limit = 300000;
        config.seed = 3;
        config.search.moves.or_opt_weight = 0.2;
        config.search.moves.tail_swap_weight = 0.2;
        config.search.moves.cross_exchange_weight = 0.2;
        TspCalculator calculator;
        optimaru::Score result = optimaru::Solver<TspSolution>(config).solve(s, calculator);
        check(std::fabs(-result.soft - optimum) < 1e-9,
              "expanded move mix still reaches the TSP optimum");
    }
}

void testScoreAnalysis(const Score& optimum) {
    std::printf("== Score analysis (SolutionManager + named composite) ==\n");
    TaskAssignment s = buildTaskInstance();
    for (int t = 0; t < 6; ++t) s.assignment[t] = t % 4;

    optimaru::WeightedObjective<TaskAssignment> composite;
    composite.add(std::make_unique<TaskConflictComponent>(), "task conflict");
    composite.add(std::make_unique<TaskCostComponent>(), "task cost");

    optimaru::ScoreBreakdown analysis = optimaru::explain_score(s, composite);
    Score expected = fullScore(s);
    check(analysis.score == expected, "analysis total matches the full recompute");
    check(analysis.constraints.size() == 2 && analysis.constraints[0].name == "task conflict" &&
                  analysis.constraints[0].score == Score{expected.hard, 0.0} &&
                  analysis.constraints[1].name == "task cost" &&
                  analysis.constraints[1].score == Score{0, expected.soft},
          "per-constraint breakdown is named and correct");

    // The composite is a first-class calculator: solving with it reaches the
    // same optimum as the monolithic calculator.
    TaskAssignment s2 = buildTaskInstance();
    optimaru::Settings config;
    config.limits.time_limit = std::chrono::seconds(10);
    config.limits.step_limit = 200000;
    config.search.acceptance.rule = optimaru::AcceptanceRule::SimulatedAnnealing;
    config.search.acceptance.start_temperature = {2, 50.0};
    config.seed = 7;
    optimaru::WeightedObjective<TaskAssignment> composite2;
    composite2.add(std::make_unique<TaskConflictComponent>(), "task conflict");
    composite2.add(std::make_unique<TaskCostComponent>(), "task cost");
    Score result = optimaru::Solver<TaskAssignment>(config).solve(s2, composite2);
    check(result == optimum, "solving through the composite reaches the optimum");
}

// ---------------------------------------------------------------------------
// DecomposedObjective: the plugin-style cost contract. Unit = worker,
// members = tasks currently assigned to it. Verifies that the incremental
// delta path (UnitConstraint::delta_cost), the full-re-cost fallback and a
// from-scratch recompute agree exactly at every step of a random walk —
// including a shaped (exponent != 1) constraint and a global constraint.
struct TaConflict final : optimaru::UnitConstraint<TaskAssignment> {
    std::vector<std::vector<char>> adj;
    TaConflict(const TaskAssignment& s, double weight)
        : UnitConstraint("conflict", weight, optimaru::Level::Hard),
          adj(s.assignment.size(), std::vector<char>(s.assignment.size(), 0)) {
        for (auto [a, b] : s.conflicts) adj[a][b] = adj[b][a] = 1;
    }
    double cost(const TaskAssignment&, int, const std::vector<int>& members) const override {
        double c = 0;
        for (std::size_t a = 0; a < members.size(); ++a) {
            for (std::size_t b = a + 1; b < members.size(); ++b) {
                if (adj[members[a]][members[b]]) c += 1;
            }
        }
        return c;
    }
    bool supports_delta() const override { return true; }
    double delta_cost(const TaskAssignment&, int, const std::vector<int>& members,
                      int entity) const override {
        double c = 0;
        for (int t : members) {
            if (t != entity && adj[entity][t]) c += 1;
        }
        return c;
    }
};

template <bool WithDelta>
struct TaCost final : optimaru::UnitConstraint<TaskAssignment> {
    explicit TaCost(double weight, double exp = 1.0)
        : UnitConstraint(WithDelta ? "cost" : "cost (no delta)", weight, optimaru::Level::Soft) {
        exponent = exp;
    }
    double cost(const TaskAssignment& s, int unit, const std::vector<int>& members) const override {
        double c = 0;
        for (int t : members) c += static_cast<double>(s.cost[t][unit]);
        return c;
    }
    bool supports_delta() const override { return WithDelta; }
    double delta_cost(const TaskAssignment& s, int unit, const std::vector<int>&,
                      int entity) const override {
        return static_cast<double>(s.cost[entity][unit]);
    }
};

struct TaBalance final : optimaru::GlobalConstraint<TaskAssignment> {
    explicit TaBalance(double weight)
        : GlobalConstraint("balance", weight, optimaru::Level::Soft) {}
    std::vector<long> counts;
    void reset(const TaskAssignment&,
               const std::vector<std::vector<int>>& members_per_unit) override {
        counts.clear();
        for (const auto& m : members_per_unit) counts.push_back(static_cast<long>(m.size()));
    }
    void on_unit_changed(const TaskAssignment&, int unit,
                         const std::vector<int>& members) override {
        counts[unit] = static_cast<long>(members.size());
    }
    bool supports_member_change() const override { return true; }
    void on_member_changed(const TaskAssignment& s, int unit, const std::vector<int>& members, int,
                           bool) override {
        on_unit_changed(s, unit, members);
    }
    double cost() const override {
        double c = 0;
        for (long n : counts) c += static_cast<double>(n) * static_cast<double>(n);
        return c;
    }
};

void testDecomposedDeltaCalculator() {
    std::printf("== DecomposedObjective (delta path vs fallback vs recompute) ==\n");
    TaskAssignment s = buildTaskInstance();
    for (int t = 0; t < 6; ++t) s.assignment[t] = t % 4;

    auto build = [&s](bool withDelta) {
        auto calc = std::make_unique<optimaru::DecomposedObjective<TaskAssignment>>();
        calc->add(std::make_unique<TaConflict>(s, 2.0));
        if (withDelta) {
            calc->emplace<TaCost<true>>(1.0, /*exponent=*/1.0);
            calc->emplace<TaCost<true>>(0.5, /*exponent=*/2.0);  // shaped, still delta
        } else {
            calc->emplace<TaCost<false>>(1.0, 1.0);
            calc->emplace<TaCost<false>>(0.5, 2.0);
        }
        calc->emplace<TaBalance>(0.25);
        return calc;
    };

    auto delta = build(true);
    auto fallback = build(false);
    delta->rebuild(s);
    fallback->rebuild(s);

    std::mt19937_64 rng(42);
    bool scores_agree = true;
    bool totalsAgree = true;
    for (int step = 0; step < 3000; ++step) {
        const int t = static_cast<int>(rng() % 6);
        // Occasionally unassign, so join/leave of the -1 "unit" is exercised.
        const int w = (rng() % 8 == 0) ? -1 : static_cast<int>(rng() % 4);
        delta->before_assign(t);
        fallback->before_assign(t);
        s.assignment[t] = w;
        delta->after_assign(t);
        fallback->after_assign(t);

        const Score ds = delta->score();
        if (!(ds == fallback->score())) scores_agree = false;
        auto fresh = build(true);
        fresh->rebuild(s);
        if (!(ds == fresh->score())) scores_agree = false;

        if (step % 500 == 0) {
            const auto a = delta->constraint_totals();
            const auto b = fresh->constraint_totals();
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (std::abs(a[i].cost - b[i].cost) > 1e-9) totalsAgree = false;
            }
        }
    }
    check(scores_agree, "delta path == full-re-cost fallback == fresh recompute, 3000 moves");
    check(totalsAgree, "constraintTotals stay exact on the delta path");

    // Composite move + rejected-move fast rollback: fallback constraints are
    // re-costed once per touched unit, then restored from their transaction
    // snapshot while delta/global state follows the inverse variable writes.
    const Score beforeTrial = fallback->score();
    const int old0 = s.assignment[0];
    const int old1 = s.assignment[1];
    auto change = [&](int entity, int value) {
        fallback->before_assign(entity);
        s.assignment[entity] = value;
        fallback->after_assign(entity);
    };
    fallback->begin_trial();
    fallback->begin_batch();
    change(0, (old0 + 1) % 4);
    change(1, (old1 + 2) % 4);
    fallback->end_batch();
    auto candidateFresh = build(false);
    candidateFresh->rebuild(s);
    check(fallback->score() == candidateFresh->score(),
          "batched composite move == fresh recompute");

    fallback->begin_rollback();
    fallback->begin_batch();
    change(1, old1);
    change(0, old0);
    fallback->end_batch();
    fallback->end_trial(false);
    check(fallback->score() == beforeTrial,
          "rejected composite move restores the exact pre-move score");

    // An all-delta assembly does not need transaction snapshots: every unit
    // and global contribution is restored by the inverse notifications.
    const Score beforeDeltaTrial = delta->score();
    auto deltaChange = [&](int entity, int value) {
        delta->before_assign(entity);
        s.assignment[entity] = value;
        delta->after_assign(entity);
    };
    delta->begin_trial();
    delta->begin_batch();
    deltaChange(0, (old0 + 1) % 4);
    deltaChange(1, (old1 + 2) % 4);
    delta->end_batch();
    delta->begin_rollback();
    delta->begin_batch();
    deltaChange(1, old1);
    deltaChange(0, old0);
    delta->end_batch();
    delta->end_trial(false);
    check(delta->score() == beforeDeltaTrial,
          "all-delta rejected move restores score without snapshots");
}

// ===========================================================================
// DecomposedObjective: sequence move previews
// ===========================================================================
// The calculator serves the engine's move previews for any assembly by
// re-costing the candidate list contents, and a global constraint joins in
// through its preview contract. Checked against the applied score on a
// three-route TSP for every move kind; a global without the contract must
// switch previews off for the whole assembly.
struct TspRouteLength final : optimaru::UnitConstraint<TspSolution> {
    TspRouteLength() : UnitConstraint("routeLength", 1.0, optimaru::Level::Soft) {}
    double cost(const TspSolution& s, int, const std::vector<int>& members) const override {
        return routeLength(s, members);
    }
};

// Longest route across the fleet: a statistic that does not decompose, with
// the per-route lengths kept so a preview can re-take the maximum.
struct TspLongestRoute final : optimaru::GlobalConstraint<TspSolution> {
    explicit TspLongestRoute(bool previews)
        : GlobalConstraint("longestRoute", 1.0, optimaru::Level::Soft), previews_(previews) {}
    void reset(const TspSolution& s, const std::vector<std::vector<int>>& members) override {
        lengths_.clear();
        for (const auto& m : members) lengths_.push_back(routeLength(s, m));
    }
    void on_unit_changed(const TspSolution& s, int unit, const std::vector<int>& members) override {
        lengths_[unit] = routeLength(s, members);
    }
    double cost() const override { return *std::max_element(lengths_.begin(), lengths_.end()); }
    bool supports_preview() const override { return previews_; }
    double preview_cost(const TspSolution& s, const optimaru::PreviewedUnit* changes,
                        int count) const override {
        double longest = 0.0;
        for (std::size_t u = 0; u < lengths_.size(); ++u) {
            double length = lengths_[u];
            for (int i = 0; i < count; ++i)
                if (changes[i].unit == static_cast<int>(u))
                    length = routeLength(s, *changes[i].members);
            longest = std::max(longest, length);
        }
        return longest;
    }
    bool previews_;
    std::vector<double> lengths_;
};

// The piecewise form of the same two costs (see UnitConstraint::piecewise_cost):
// per-route prefix sums of the inter-city legs, forward and reversed, held in
// one SharedConstraintContext the calculator refreshes once per list change,
// so a preview is O(pieces) instead of a walk of the candidate route.
struct TspSumsContext final : optimaru::SharedConstraintContext {
    const TspSolution* solution = nullptr;
    std::vector<std::vector<double>> cumDist,
            cumRevDist;  // internal legs, index k = first k cities
    int refreshes = 0;

    void prepare(const TspSolution& s) {
        if (solution == &s && preparedGeneration == generation) return;
        solution = &s;
        preparedGeneration = generation;
        cumDist.assign(s.routes.size(), {});
        cumRevDist.assign(s.routes.size(), {});
    }
    void on_list_changed(int list) override {
        if (solution == nullptr) return;
        ++refreshes;
        const std::vector<int>& route = solution->routes[static_cast<std::size_t>(list)];
        std::vector<double>& fwd = cumDist[static_cast<std::size_t>(list)];
        std::vector<double>& rev = cumRevDist[static_cast<std::size_t>(list)];
        fwd.assign(route.size() + 1, 0.0);
        rev.assign(route.size() + 1, 0.0);
        for (std::size_t k = 1; k < route.size(); ++k) {
            fwd[k + 1] = fwd[k] + dist(*solution, route[k - 1], route[k]);
            rev[k + 1] = rev[k] + dist(*solution, route[k], route[k - 1]);
        }
    }
    // Closed-tour length of a route made of `pieces`.
    double lengthOf(const optimaru::ListPiece* pieces, int count) const {
        double total = 0.0;
        int first = -1, previous = -1, cities = 0;
        for (int i = 0; i < count; ++i) {
            const optimaru::ListPiece& piece = pieces[i];
            if (piece.size() <= 0) continue;
            int head, tail;
            if (piece.detached()) {
                head = tail = piece.begin;
            } else {
                const std::vector<int>& route =
                        solution->routes[static_cast<std::size_t>(piece.list)];
                head = route[static_cast<std::size_t>(piece.reversed ? piece.end - 1
                                                                     : piece.begin)];
                tail = route[static_cast<std::size_t>(piece.reversed ? piece.begin
                                                                     : piece.end - 1)];
                const std::vector<double>& sums =
                        piece.reversed ? cumRevDist[piece.list] : cumDist[piece.list];
                total += sums[static_cast<std::size_t>(piece.end)] -
                         sums[static_cast<std::size_t>(piece.begin + 1)];
            }
            if (previous >= 0) total += dist(*solution, previous, head);
            if (first < 0) first = head;
            previous = tail;
            cities += piece.size();
        }
        if (cities < 2) return 0.0;
        return total + dist(*solution, previous, first);
    }
    std::uint64_t preparedGeneration = 0;
};

struct TspRouteLengthSums final : optimaru::UnitConstraint<TspSolution> {
    explicit TspRouteLengthSums(bool piecewise = true)
        : UnitConstraint("routeLength", 1.0, optimaru::Level::Soft), piecewise_(piecewise) {}
    const char* shared_context_key() const override { return "tsp.sums"; }
    std::unique_ptr<optimaru::SharedConstraintContext> create_shared_context() const override {
        return std::make_unique<TspSumsContext>();
    }
    void bind_shared_context(optimaru::SharedConstraintContext& c) override {
        sums_ = static_cast<TspSumsContext*>(&c);
    }
    void on_reset(const TspSolution& s) override { sums_->prepare(s); }
    double cost(const TspSolution& s, int, const std::vector<int>& members) const override {
        ++costCalls;
        return routeLength(s, members);
    }
    bool supports_piecewise_preview() const override { return piecewise_; }
    bool piecewise_cost(const TspSolution&, int, const optimaru::ListPiece* pieces, int count,
                        double& out) const override {
        ++piecewiseCalls;
        out = sums_->lengthOf(pieces, count);
        return true;
    }
    bool piecewise_;
    TspSumsContext* sums_ = nullptr;
    mutable int costCalls = 0;
    mutable int piecewiseCalls = 0;
};

struct TspLongestRouteSums final : optimaru::GlobalConstraint<TspSolution> {
    TspLongestRouteSums() : GlobalConstraint("longestRoute", 1.0, optimaru::Level::Soft) {}
    const char* shared_context_key() const override { return "tsp.sums"; }
    std::unique_ptr<optimaru::SharedConstraintContext> create_shared_context() const override {
        return std::make_unique<TspSumsContext>();
    }
    void bind_shared_context(optimaru::SharedConstraintContext& c) override {
        sums_ = static_cast<TspSumsContext*>(&c);
    }
    void reset(const TspSolution& s, const std::vector<std::vector<int>>& members) override {
        sums_->prepare(s);
        lengths_.clear();
        for (const auto& m : members) lengths_.push_back(routeLength(s, m));
    }
    void on_unit_changed(const TspSolution& s, int unit, const std::vector<int>& members) override {
        lengths_[unit] = routeLength(s, members);
    }
    double cost() const override { return *std::max_element(lengths_.begin(), lengths_.end()); }
    bool supports_preview() const override { return true; }
    double preview_cost(const TspSolution& s, const optimaru::PreviewedUnit* changes,
                        int count) const override {
        ++materializedCalls;
        return longest(changes, count, [&](const optimaru::PreviewedUnit& c) {
            return routeLength(s, *c.members);
        });
    }
    bool supports_piecewise_preview() const override { return true; }
    bool piecewise_preview_cost(const TspSolution&, const optimaru::PreviewedUnit* changes,
                                int count, double& out) const override {
        ++piecewiseCalls;
        out = longest(changes, count, [&](const optimaru::PreviewedUnit& c) {
            return sums_->lengthOf(c.pieces, c.piece_count);
        });
        return true;
    }
    template <typename LengthOf>
    double longest(const optimaru::PreviewedUnit* changes, int count, LengthOf&& lengthOf) const {
        double result = 0.0;
        for (std::size_t u = 0; u < lengths_.size(); ++u) {
            double length = lengths_[u];
            for (int i = 0; i < count; ++i)
                if (changes[i].unit == static_cast<int>(u)) length = lengthOf(changes[i]);
            result = std::max(result, length);
        }
        return result;
    }
    TspSumsContext* sums_ = nullptr;
    std::vector<double> lengths_;
    mutable int materializedCalls = 0;
    mutable int piecewiseCalls = 0;
};

void testDecomposedPiecewisePreviews() {
    std::printf("== DecomposedObjective: piecewise sequence previews ==\n");
    TspSolution s = buildRandomTsp(3, 24);
    for (int c = 0; c < 24; ++c) s.routes[c % 3].push_back(c);
    // The same harness run (same seed, hence the same moves) with the unit
    // constraint's piecewise contract on and off: both agree with the applied
    // score, and the piecewise run never calls cost() for a preview.
    struct Run {
        optimaru::testing::PreviewParity parity;
        int costCalls = 0, piecewiseUnit = 0, piecewiseGlobal = 0, materializedGlobal = 0;
    };
    auto run = [&](bool piecewise) {
        optimaru::DecomposedObjective<TspSolution> calculator;
        auto unitOwned = std::make_unique<TspRouteLengthSums>(piecewise);
        auto globalOwned = std::make_unique<TspLongestRouteSums>();
        TspRouteLengthSums* unit = unitOwned.get();
        TspLongestRouteSums* global = globalOwned.get();
        calculator.add(std::move(unitOwned));
        calculator.add(std::move(globalOwned));
        optimaru::Editor<TspSolution> editor(s, calculator);
        Run result;
        result.parity =
                optimaru::testing::run_preview_parity(editor, calculator, /*seed=*/23, 3000);
        result.costCalls = unit->costCalls;
        result.piecewiseUnit = unit->piecewiseCalls;
        result.piecewiseGlobal = global->piecewiseCalls;
        result.materializedGlobal = global->materializedCalls;
        return result;
    };
    {
        const Run on = run(true);
        const Run off = run(false);
        std::printf(
                "    %d moves checked, %d previewed, %d declined; %d piecewise unit answers, "
                "%d piecewise global answers, %d materialized global answers; cost() calls "
                "%d piecewise vs %d materialized\n",
                on.parity.checked, on.parity.previewed, on.parity.declined, on.piecewiseUnit,
                on.piecewiseGlobal, on.materializedGlobal, on.costCalls, off.costCalls);
        check(on.parity.all_equal() && on.parity.declined == 0 && on.parity.previewed > 1500,
              "piecewise unit + piecewise global: every preview equals the applied score");
        check(on.piecewiseUnit >= on.parity.previewed &&
                      on.piecewiseGlobal == on.parity.previewed && on.materializedGlobal == 0,
              "an all-piecewise assembly never materializes a candidate route");
        check(off.parity.all_equal() && off.parity.checked == on.parity.checked &&
                      off.costCalls - on.costCalls >= on.parity.previewed,
              "piecewise previews do not call cost(); the materialized path does, once per "
              "previewed list");
    }
    {
        // The shared context prepared by a global alone (the unit constraint
        // beside it is the plain one): the calculator refreshes the context
        // only after every constraint's reset, so the eager context has the
        // solution when it builds its tables.
        optimaru::DecomposedObjective<TspSolution> calculator;
        calculator.emplace<TspRouteLength>();
        calculator.emplace<TspLongestRouteSums>();
        optimaru::Editor<TspSolution> editor(s, calculator);
        const optimaru::testing::PreviewParity parity =
                optimaru::testing::run_preview_parity(editor, calculator, /*seed=*/31, 1500);
        check(parity.all_equal() && parity.declined == 0 && parity.previewed > 500,
              "a context prepared by a global alone: every preview equals the applied score");
    }
    {
        // Piecewise unit beside a materializing global: the unit still answers
        // from its sums, the global gets the written-out routes.
        optimaru::DecomposedObjective<TspSolution> calculator;
        auto unitOwned = std::make_unique<TspRouteLengthSums>();
        TspRouteLengthSums* unit = unitOwned.get();
        calculator.add(std::move(unitOwned));
        calculator.emplace<TspLongestRoute>(/*previews=*/true);
        optimaru::Editor<TspSolution> editor(s, calculator);
        const optimaru::testing::PreviewParity parity =
                optimaru::testing::run_preview_parity(editor, calculator, /*seed=*/29, 2000);
        check(parity.all_equal() && parity.declined == 0 &&
                      unit->piecewiseCalls >= parity.previewed,
              "piecewise unit + materializing global: every preview equals the applied score");
    }
}

void testDecomposedListPreviews() {
    std::printf("== DecomposedObjective: sequence move previews ==\n");
    TspSolution s = buildRandomTsp(3, 24);
    for (int c = 0; c < 24; ++c) s.routes[c % 3].push_back(c);

    auto run = [&](bool globalPreviews, int trials) {
        optimaru::DecomposedObjective<TspSolution> calculator;
        calculator.emplace<TspRouteLength>();
        calculator.emplace<TspLongestRoute>(globalPreviews);
        optimaru::Editor<TspSolution> editor(s, calculator);
        return optimaru::testing::run_preview_parity(editor, calculator, /*seed=*/17, trials);
    };
    const optimaru::testing::PreviewParity served = run(true, 3000);
    std::printf("    %d moves checked, %d previewed, %d declined\n", served.checked,
                served.previewed, served.declined);
    check(served.all_equal() && served.declined == 0 && served.previewed > 1500,
          "unit + previewing global: every preview equals the applied score");
    const optimaru::testing::PreviewParity declined = run(false, 300);
    check(declined.previewed == 0 && declined.declined > 0,
          "a global without the preview contract declines every preview");
    {
        // has_previews(), what the iterated local search asks before a
        // draw: false until the solution is set, false with a previewless
        // global at a non-zero weight, true once that global weighs nothing.
        optimaru::DecomposedObjective<TspSolution> calculator;
        calculator.emplace<TspRouteLength>();
        auto previewless = std::make_unique<TspLongestRoute>(/*previews=*/false);
        TspLongestRoute* global = previewless.get();
        calculator.add(std::move(previewless));
        check(!calculator.has_previews(), "supportsPreviews() is false before the solution is set");
        calculator.rebuild(s);
        check(!calculator.has_previews(),
              "supportsPreviews() is false with a weighted global that cannot preview");
        global->weight = 0.0;
        calculator.rebuild(s);
        check(calculator.has_previews(),
              "supportsPreviews() is true once that global weighs nothing");
    }
}

// ===========================================================================
// ConstraintAssembly: state the cost function by name
// ===========================================================================
// A domain hands back a default assembly and the caller re-states only what
// differs. What has to hold: the tweaks land on the right component, the
// result scores exactly like the same assembly built by hand, and a name that
// is not in the assembly THROWS rather than silently changing nothing.
void testConstraintAssembly() {
    std::printf("== ConstraintAssembly (named, chained cost-function edits) ==\n");
    TaskAssignment s = buildTaskInstance();
    for (int t = 0; t < 6; ++t) s.assignment[t] = t % 4;

    // The domain's "defaults", as a factory would return them.
    auto defaults = [&s] {
        optimaru::ConstraintAssembly<TaskAssignment> assembly;
        assembly.add(std::make_unique<TaConflict>(s, 2.0));
        assembly.emplace<TaCost<true>>(1.0, /*exponent=*/1.0);
        assembly.emplace<TaBalance>(0.25);
        return assembly;
    };

    check(defaults().size() == 3, "the factory hands back every default component");
    check(defaults().contains("conflict") && defaults().contains("balance"),
          "components are addressable by the names they carry");

    // Named edits == the same assembly built by hand, scored on one solution.
    auto scoreOf = [&s](optimaru::ConstraintAssembly<TaskAssignment> assembly) {
        optimaru::DecomposedObjective<TaskAssignment> calc;
        assembly.into(calc);
        calc.rebuild(s);
        return calc.score();
    };

    optimaru::ConstraintAssembly<TaskAssignment> byHand;
    byHand.add(std::make_unique<TaConflict>(s, 5.0));  // conflict re-weighted
    byHand.emplace<TaBalance>(0.25);                   // cost dropped
    check(scoreOf(defaults().weight("conflict", 5.0).drop("cost")) == scoreOf(std::move(byHand)),
          "weight() + drop() == the same assembly listed by hand");

    // only() keeps exactly what it names.
    auto isolated = defaults().only({"balance"});
    check(isolated.size() == 1 && isolated.contains("balance"),
          "only() keeps just the named components");

    // add() replaces by name rather than double-counting.
    auto replaced = defaults();
    replaced.add(std::make_unique<TaConflict>(s, 9.0));
    check(replaced.size() == 3, "add() of an existing name replaces instead of appending");

    // A level flip is a word, not a make_unique dance.
    auto flipped = defaults().soft("conflict");
    optimaru::DecomposedObjective<TaskAssignment> flippedCalc;
    flipped.into(flippedCalc);
    flippedCalc.rebuild(s);
    check(flippedCalc.score().hard == 0,
          "soft() empties the hard level of a formerly hard constraint");

    // Typos must not silently leave the cost function unchanged.
    int threw = 0;
    for (auto&& edit : {std::function<void()>([&] { defaults().weight("conflcit", 1.0); }),
                        std::function<void()>([&] { defaults().drop("nope"); }),
                        std::function<void()>([&] { defaults().only({"balance", "nope"}); })}) {
        try {
            edit();
        } catch (const std::invalid_argument&) {
            ++threw;
        }
    }
    check(threw == 3, "an unknown constraint name throws on weight/drop/only");

    // A name held several times: its name alone does not say which instance,
    // so every by-name edit and lookup throws instead of touching the first
    // and leaving the others in the score.
    auto repeated = [&] { return defaults().add_instance(std::make_unique<TaConflict>(s, 3.0)); };
    check(repeated().size() == 4 && repeated().instance_count("conflict") == 2 &&
                  repeated().instance_count("cost") == 1 && repeated().instance_count("nope") == 0,
          "instance_count() counts the instances of a name");
    const std::vector<std::function<void()>> ambiguous = {
            [&] { repeated().weight("conflict", 0.0); },
            [&] { repeated().level("conflict", optimaru::Level::Soft); },
            [&] { repeated().hard("conflict"); },
            [&] { repeated().soft("conflict"); },
            [&] { repeated().exponent("conflict", 2.0); },
            [&] { repeated().offset("conflict", 1.0); },
            [&] { repeated().clip("conflict", false); },
            [&] { repeated().find("conflict"); },
            [&] { repeated().drop("conflict"); },
            [&] { repeated().add(std::make_unique<TaConflict>(s, 9.0)); },
    };
    threw = 0;
    for (const auto& edit : ambiguous) {
        try {
            edit();
        } catch (const std::invalid_argument&) {
            ++threw;
        }
    }
    check(threw == static_cast<int>(ambiguous.size()),
          "a by-name edit or lookup of a name held several times throws");

    // The instance overloads say which one (from 0, in the order added).
    check(repeated().find("conflict", 0)->weight == 2.0 &&
                  repeated().find("conflict", 1)->weight == 3.0 &&
                  repeated().find("conflict", 2) == nullptr &&
                  repeated().find("nope", 0) == nullptr,
          "find(name, instance) reads one instance, nullptr past the last");
    check(scoreOf(repeated().weight("conflict", 1, 0.0)) == scoreOf(defaults()),
          "weight(name, 1, 0) takes the second instance out of the score");
    check(scoreOf(repeated().weight("conflict", 0, 5.0).drop("conflict", 1)) ==
                  scoreOf(defaults().weight("conflict", 5.0)),
          "weight(name, 0, w) + drop(name, 1) == the single instance re-weighted");
    check(scoreOf(repeated().soft("conflict", 0).soft("conflict", 1)).hard == 0,
          "soft(name, instance) flips each instance");
    auto knobs = repeated()
                         .soft("conflict", 0)
                         .level("conflict", 1, optimaru::Level::Soft)
                         .hard("conflict", 1)
                         .clip("conflict", 1, false)
                         .exponent("conflict", 1, 2.0)
                         .offset("conflict", 0, 1.0);
    const optimaru::ConstraintMetadata& first = *knobs.find("conflict", 0);
    const optimaru::ConstraintMetadata& other = *knobs.find("conflict", 1);
    check(first.level == optimaru::Level::Soft && first.clip && first.exponent == 1.0 &&
                  first.offset == 1.0 && other.level == optimaru::Level::Hard && !other.clip &&
                  other.exponent == 2.0 && other.offset == 0.0,
          "level / hard / clip / exponent / offset by instance leave the other instance alone");
    auto second = repeated().drop("conflict", 0);
    check(second.instance_count("conflict") == 1 && second.find("conflict")->weight == 3.0,
          "after drop(name, 0) the name is unambiguous again and names the former second");
    auto none = repeated().drop_all("conflict");
    check(none.size() == 2 && !none.contains("conflict"), "drop_all() removes every instance");
    check(repeated().only({"conflict"}).size() == 2, "only() keeps every instance of a name");
    threw = 0;
    for (auto&& edit : {std::function<void()>([&] { repeated().weight("conflict", 2, 1.0); }),
                        std::function<void()>([&] { repeated().drop("conflict", 2); }),
                        std::function<void()>([&] { repeated().weight("nope", 0, 1.0); }),
                        std::function<void()>([&] { repeated().drop_all("nope"); })}) {
        try {
            edit();
        } catch (const std::invalid_argument&) {
            ++threw;
        }
    }
    check(threw == 4, "an instance or a name the assembly does not hold throws");

    // into() spends the assembly: the components moved out.
    auto spent = defaults();
    optimaru::DecomposedObjective<TaskAssignment> sink;
    spent.into(sink);
    check(spent.empty(), "into() leaves the assembly empty");
    std::printf("\n");
}

// ===========================================================================
// solver_config.hpp: the Config bases, their Settings, and parallel starts
// ===========================================================================
namespace {

// What solve_parallel_starts needs of a problem: copies, and a score.
struct ToyProblem {
    Score score{0, 0.0};
    int solved_by = -1;  // the start that wrote this copy
};

struct ToyConfig : optimaru::SolverConfigBase<Score>, optimaru::RuinRecreateConfig {
    double ruin_recreate_as_move_weight = 0.25;
    int ruin_recreate_as_move_max_ruin = 7;
};

// A start that "searches" until the run's stop flag is raised (30 s at most).
bool waitForStop(const ToyConfig& start) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!start.stop_flag->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return start.stop_flag->load();
}

}  // namespace

void testSolverConfig() {
    std::printf("== solver_config.hpp: settings_from, check_config, solve_parallel_starts ==\n");
    {  // settings_from puts every field of the two bases in its place.
        ToyConfig config;
        config.time_limit = std::chrono::milliseconds(1234);
        config.target_score = Score{0, -5.0};
        config.stall_time_limit = std::chrono::milliseconds(77);
        config.step_limit = 11;
        config.stall_step_limit = 13;
        config.seed = 99;
        config.parallel_solver_count = 6;
        config.score_checks = optimaru::ScoreChecks::Full;
        int calls = 0;
        config.on_new_best = [&calls](std::int64_t, const Score&) { ++calls; };
        config.ruin_recreate = false;
        config.ruin_recreate_unimproved_step_trigger = 3;
        config.ruin_recreate_min_ruin_fraction = 0.2;
        config.ruin_recreate_max_ruin_fraction = 0.4;
        config.ruin_recreate_max_ruin_count = 9;
        config.ruin_recreate_operator_decay = 0.5;
        config.ruin_recreate_rewards = {4.0, 3.0, 2.0, 1.0};
        config.ruin_recreate_polish_steps = 17;
        const optimaru::Settings settings = optimaru::settings_from(config);
        const optimaru::Limits& limits = settings.limits;
        check(settings.seed == 99 && settings.score_checks == optimaru::ScoreChecks::Full &&
                      limits.time_limit == std::chrono::milliseconds(1234) &&
                      limits.stall_time_limit == std::chrono::milliseconds(77) &&
                      limits.step_limit == 11 && limits.stall_step_limit == 13,
              "settings_from: the seed, the score checks and the four limits");
        settings.on_new_best(1, Score{0, 0.0});
        check(limits.target_score == Score{0, -5.0} && calls == 1,
              "settings_from: target_score and on_new_best of an engine-Score Config");
        const optimaru::RuinRecreateSettings& kicks = settings.search.ruin_recreate;
        check(!kicks.enabled && kicks.unimproved_step_trigger == 3 &&
                      kicks.min_ruin_fraction == 0.2 && kicks.max_ruin_fraction == 0.4 &&
                      kicks.max_ruin_count == 9 && kicks.operator_decay == 0.5 &&
                      kicks.rewards == std::vector<double>({4.0, 3.0, 2.0, 1.0}) &&
                      kicks.polish_steps == 17,
              "settings_from: the ruin & recreate knobs");
        check(kicks.as_move_weight == 0.25 && kicks.as_move_max_ruin == 7,
              "settings_from: the two as-move knobs of a Config that declares them");
        check(settings.parallel_starts == 1,
              "settings_from leaves parallel_starts alone (how the starts run is the solver's)");
        const optimaru::Settings defaults = optimaru::settings_from(ToyConfig{});
        check(defaults.limits.target_score == Score::unreachable() && !defaults.on_new_best &&
                      defaults.search.ruin_recreate.enabled,
              "settings_from: a default Config never stops on its target, reports nothing, kicks");
    }
    {  // check_config
        ToyConfig config;
        config.ruin_recreate_rewards = {1.0, 2.0};
        bool refused = false;
        try {
            optimaru::check_config(config);
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        bool accepted = true;
        try {
            optimaru::check_config(ToyConfig{});
        } catch (const std::invalid_argument&) {
            accepted = false;
        }
        check(refused && accepted, "check_config refuses anything but four rewards");
    }
    {  // What each start is handed, and which candidate wins.
        ToyConfig config;
        config.seed = 40;
        config.parallel_solver_count = 4;
        config.on_new_best = [](std::int64_t, const Score&) {};
        ToyProblem problem;
        std::vector<std::uint64_t> seeds(4);
        std::vector<int> counts(4);
        std::vector<int> reports(4);
        std::vector<const std::atomic<bool>*> flags(4);
        std::vector<const ToyProblem*> candidates(4);
        optimaru::solve_parallel_starts(
                config, problem, [&](const ToyConfig& start, ToyProblem& candidate, int i) {
                    const auto index = static_cast<std::size_t>(i);
                    seeds[index] = start.seed;
                    counts[index] = start.parallel_solver_count;
                    reports[index] = start.on_new_best ? 1 : 0;
                    flags[index] = start.stop_flag.get();
                    candidates[index] = &candidate;
                    // Starts 1 and 2 tie for the best score.
                    candidate.score = Score{0, i == 1 || i == 2 ? -1.0 : -5.0};
                    candidate.solved_by = i;
                });
        check(seeds == std::vector<std::uint64_t>({40, 41, 42, 43}),
              "solve_parallel_starts: start i searches with seed + i");
        check(counts == std::vector<int>({1, 1, 1, 1}),
              "solve_parallel_starts: every start is one start");
        check(reports == std::vector<int>({1, 0, 0, 0}),
              "solve_parallel_starts: only start 0 reports new bests");
        check(flags[0] != nullptr && !flags[0]->load() &&
                      std::count(flags.begin(), flags.end(), flags[0]) == 4,
              "solve_parallel_starts: the starts share one stop flag, lowered");
        const bool ownCopies =
                std::count(candidates.begin(), candidates.end(), &problem) == 0 &&
                std::set<const ToyProblem*>(candidates.begin(), candidates.end()).size() == 4;
        check(ownCopies, "solve_parallel_starts: every start searches a copy of its own");
        check(problem.solved_by == 1 && problem.score == Score{0, -1.0},
              "solve_parallel_starts: the best score wins, the lowest start on ties");
        check(config.seed == 40 && config.parallel_solver_count == 4 && !config.stop_flag,
              "solve_parallel_starts leaves the caller's Config as it was");
    }
    {  // A start that reaches the target ends the run: the others stop and
        // hand over their best so far, and the best of all still wins.
        ToyConfig config;
        config.parallel_solver_count = 3;
        config.target_score = Score{0, -10.0};
        ToyProblem problem;
        std::atomic<int> stopped{0};
        const auto began = std::chrono::steady_clock::now();
        optimaru::solve_parallel_starts(
                config, problem, [&stopped](const ToyConfig& start, ToyProblem& candidate, int i) {
                    candidate.solved_by = i;
                    if (i == 1) {  // at the target straight away
                        candidate.score = Score{0, -10.0};
                        return;
                    }
                    if (waitForStop(start)) ++stopped;
                    // What these two had when they were stopped: start 2 beyond
                    // the target, start 0 short of it.
                    candidate.score = Score{0, i == 2 ? -5.0 : -20.0};
                });
        const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        check(stopped.load() == 2 && seconds < 10.0,
              "solve_parallel_starts: a start that reaches the target stops the others");
        check(problem.solved_by == 2 && problem.score == Score{0, -5.0},
              "solve_parallel_starts: the best of all starts wins, not the first at the target");
    }
    {  // No start reaches the target: nobody is stopped, each runs to its end.
        ToyConfig config;
        config.parallel_solver_count = 3;
        config.target_score = Score{0, 0.0};
        ToyProblem problem;
        std::atomic<int> stopped{0};
        optimaru::solve_parallel_starts(
                config, problem, [&stopped](const ToyConfig& start, ToyProblem& candidate, int i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20 * (3 - i)));
                    if (start.stop_flag->load()) ++stopped;
                    candidate.score = Score{0, -1.0 - i};
                    candidate.solved_by = i;
                });
        check(stopped.load() == 0 && problem.solved_by == 0,
              "solve_parallel_starts: a target no start reaches stops nobody");
    }
    {  // One start (anything below two): no copy, no thread, the Config as it is.
        for (const int count : {1, 0, -3}) {
            ToyConfig config;
            config.seed = 7;
            config.parallel_solver_count = count;
            ToyProblem problem;
            const ToyConfig* seenConfig = nullptr;
            const ToyProblem* seenProblem = nullptr;
            int index = -1;
            const std::thread::id caller = std::this_thread::get_id();
            bool onCallerThread = false;
            optimaru::solve_parallel_starts(
                    config, problem, [&](const ToyConfig& start, ToyProblem& candidate, int i) {
                        seenConfig = &start;
                        seenProblem = &candidate;
                        index = i;
                        onCallerThread = std::this_thread::get_id() == caller;
                    });
            check(seenConfig == &config && seenProblem == &problem && index == 0 && onCallerThread,
                  "solve_parallel_starts: parallel_solver_count " + std::to_string(count) +
                          " solves the problem itself, on the calling thread");
        }
    }
    {  // A start that throws: the others stop, the error is rethrown, the
        // problem is left as it was.
        ToyConfig config;
        config.parallel_solver_count = 3;
        ToyProblem problem;
        problem.score = Score{-3, -3.0};
        std::atomic<int> stopped{0};
        std::string caught;
        const auto began = std::chrono::steady_clock::now();
        try {
            optimaru::solve_parallel_starts(
                    config, problem,
                    [&stopped](const ToyConfig& start, ToyProblem& candidate, int i) {
                        if (i == 1) throw std::runtime_error("start 1 failed");
                        if (waitForStop(start)) ++stopped;
                        candidate.score = Score{0, 0.0};
                        candidate.solved_by = i;
                    });
        } catch (const std::runtime_error& error) {
            caught = error.what();
        }
        const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        check(caught == "start 1 failed",
              "solve_parallel_starts rethrows a start's exception on the calling thread");
        check(stopped.load() == 2 && seconds < 10.0,
              "solve_parallel_starts: the other starts see the stop flag raised (no 30 s wait)");
        check(problem.score == Score{-3, -3.0} && problem.solved_by == -1,
              "solve_parallel_starts: a failed run leaves the problem as it was");
    }
    {  // Two starts fail: the one rethrown is the first in start order, not the
        // first in time (start 0 fails only after start 2 has).
        ToyConfig config;
        config.parallel_solver_count = 3;
        ToyProblem problem;
        std::string caught;
        try {
            optimaru::solve_parallel_starts(config, problem,
                                            [](const ToyConfig& start, ToyProblem&, int i) {
                                                if (i == 2)
                                                    throw std::runtime_error("start 2 failed");
                                                waitForStop(start);
                                                if (i == 0)
                                                    throw std::runtime_error("start 0 failed");
                                            });
        } catch (const std::runtime_error& error) {
            caught = error.what();
        }
        check(caught == "start 0 failed",
              "solve_parallel_starts: of two failures, the first in start order is rethrown");
    }
    {  // A thread that cannot be created (injected before start 1): the start
        // already running is stopped and joined, and the error comes out here.
        ToyConfig config;
        config.parallel_solver_count = 3;
        ToyProblem problem;
        std::atomic<int> stopped{0};
        optimaru::worker_thread_start_hook = [](int index) {
            if (index == 1) {
                throw std::system_error(
                        std::make_error_code(std::errc::resource_unavailable_try_again),
                        "injected thread creation failure");
            }
        };
        const auto began = std::chrono::steady_clock::now();
        bool caught = false;
        try {
            optimaru::solve_parallel_starts(
                    config, problem,
                    [&stopped](const ToyConfig& start, ToyProblem& candidate, int i) {
                        if (waitForStop(start)) ++stopped;
                        candidate.solved_by = i;
                    });
        } catch (const std::system_error& error) {
            caught = std::string(error.what()).find("injected") != std::string::npos;
        }
        optimaru::worker_thread_start_hook = nullptr;
        const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        check(caught && stopped.load() == 1 && seconds < 10.0 && problem.solved_by == -1,
              "solve_parallel_starts: a thread creation failure stops and joins the start "
              "already running, is rethrown, and leaves the problem as it was");
    }
    std::printf("\n");
}

void testBackgroundSolveAndLimits() {
    std::printf("== background solve + stall_step_limit ==\n");
    using Clock = std::chrono::steady_clock;
    {  // stall_step_limit fires long before the 30s time_limit.
        TaskAssignment s = buildTaskInstance();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(30);
        config.limits.stall_step_limit = 5000;
        auto t0 = Clock::now();
        TaskAssignmentCalculator calculator;
        optimaru::Solver<TaskAssignment>(config).solve(s, calculator);
        double sec = std::chrono::duration<double>(Clock::now() - t0).count();
        check(sec < 5.0, "stall_step_limit terminates early (" + std::to_string(sec) + "s)");
    }
    {  // solve_in_background: request_stop stops a 30s solve quickly.
        TaskAssignment s = buildTaskInstance();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(30);
        TaskAssignmentCalculator calculator;
        auto t0 = Clock::now();
        auto job = optimaru::solve_in_background(config, s, calculator);
        bool wasActive = job.status() == optimaru::SolveStatus::Running;
        job.request_stop();
        Score final = job.wait();
        double sec = std::chrono::duration<double>(Clock::now() - t0).count();
        bool done = job.status() == optimaru::SolveStatus::Idle;
        check(wasActive && done && sec < 5.0,
              "BackgroundSolve reports status and request_stop stops in " + std::to_string(sec) +
                      "s");
        check(final == fullScore(s), "job's final best score matches the returned solution");
    }
    {  // Assigning a new job over a running one stops and joins the old one
        // first, so the Solver its thread is running is never freed under it.
        TaskAssignment s1 = buildTaskInstance();
        TaskAssignment s2 = buildTaskInstance();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(30);
        TaskAssignmentCalculator c1, c2;
        auto t0 = Clock::now();
        auto job = optimaru::solve_in_background(config, s1, c1);
        job = optimaru::solve_in_background(config, s2, c2);
        job.request_stop();
        job.wait();
        double sec = std::chrono::duration<double>(Clock::now() - t0).count();
        check(sec < 5.0, "assigning over a running BackgroundSolve stops the old solve (" +
                                 std::to_string(sec) + "s)");
    }
    {  // An exception inside the background solve reaches wait() instead of
        // terminating the process.
        TaskAssignment s = buildTaskInstance();
        optimaru::Settings config;
        config.limits.time_limit = std::chrono::seconds(30);
        config.on_new_best = [](std::int64_t, const Score&) {
            throw std::runtime_error("on_new_best failed");
        };
        TaskAssignmentCalculator calculator;
        auto job = optimaru::solve_in_background(config, s, calculator);
        bool rethrown = false;
        try {
            job.wait();
        } catch (const std::runtime_error& e) {
            rethrown = std::string(e.what()) == "on_new_best failed";
        }
        check(rethrown && job.status() == optimaru::SolveStatus::Idle,
              "a failing background solve rethrows from wait()");
    }
}

}  // namespace

int main() {
    const Score optimum = bruteForceOptimum(buildTaskInstance());
    const Score separableOptimum = bruteForceOptimum(buildSeparableInstance());
    testAcceptors(separableOptimum, optimum);
    testTerminations(optimum);
    testEnvironmentMode();
    testMultiStartAndFfd(optimum);
    testListVariable();
    testPinnedEntities();
    testRouletteWheelSelector();
    testCustomMoveProbability();
    testLnsEscapesLocalOptimum(optimum);
    testNearbyAndLnsOnRandomTsp();
    testExpandedMoves();
    testScoreAnalysis(optimum);
    testDecomposedDeltaCalculator();
    testDecomposedListPreviews();
    testDecomposedPiecewisePreviews();
    testConstraintAssembly();
    testSolverConfig();
    testBackgroundSolveAndLimits();
    testShadowVariables();
    testChainedVariables();

    if (failures == 0) {
        std::printf("\nAll engine tests PASSED.\n");
        return 0;
    }
    std::printf("\n%d engine test(s) FAILED.\n", failures);
    return 1;
}
