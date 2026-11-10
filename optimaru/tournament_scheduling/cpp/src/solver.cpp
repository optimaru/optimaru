#include "ts/solver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "ts/incremental_calculator.hpp"
#include "ts/score_calculator.hpp"

namespace ts {

namespace {

using Calculator = TournamentSchedulingIncrementalScoreCalculator;

// ---------------------------------------------------------------------------
// TeamCountMove — the domain macro move (adopted on the quality bench)
// ---------------------------------------------------------------------------
// Why a macro move: medium 3 (assignmentCountImbalanceCost) is
// LoadBalance::unfairness over the teams that currently hold at least one
// assignment — an unloaded team
// simply drops out of the balance. With A assignments over n loaded teams the
// best reachable unfairness is sqrt(r * (n - r) / n), r = A mod n: ZERO
// whenever n divides A. On the demo (72 assignments, 7 teams) the 7-team
// optimum is sqrt(10/7) = 1.19523 medium — 1.19523e6 of engine soft — while a
// 6-team schedule with 12 assignments each scores medium 0. Change/swap moves
// can never cross that gap: emptying a team walks through states like
// {12,12,12,12,12,11,1} (unfairness ~10), an order of magnitude WORSE than
// either end, so late acceptance rejects every step of the path.
//
// So this operator walks it in one candidate move: pick a target loaded-team
// count n (biased toward the counts whose theoretical optimum is best) and
// reshape the schedule to load exactly n teams — draining teams into the
// least-loaded free ones when n is below the current count, activating spare
// teams by pulling assignments off the fullest ones when it is above. The
// result is scored and accepted/rejected as a single move like any other; the
// operator never bypasses the score, and a reshape that breaks a hard
// constraint or lands worse is rolled back.
//
// Both directions are needed: draining is what escapes the initial all-teams
// state, but drains alone strand the search on the first count that happened
// to improve — the demo's 12-team scale-up settles on 8 loaded teams
// (medium 1.22474) and can no longer reach the 10-team layout that scores
// medium 0, because every path there is uphill (measured: R1 vs R2 on M).
//
// When no target beats the current count the operator falls back to a
// builtin-equivalent random change move, so the engine's adaptive roulette
// keeps getting outcome feedback for it (an operator that reports "no move"
// is rewarded by nothing and would keep its selection weight forever).
class TeamCountMove final : public optimaru::MoveOperator<TournamentSchedule> {
public:
    TeamCountMove(const Calculator* calculator, std::vector<double> idealPenalty,
                  std::vector<int> targetOrder, std::vector<char> drainable,
                  std::vector<int> penaltyDays, std::vector<std::vector<int>> assignmentsOfDay)
        : calculator_(calculator),
          idealPenalty_(std::move(idealPenalty)),
          targetOrder_(std::move(targetOrder)),
          drainable_(std::move(drainable)),
          penaltyDays_(std::move(penaltyDays)),
          assignmentsOfDay_(std::move(assignmentsOfDay)),
          inDrainSet_(drainable_.size(), 0),
          dayOrder_(assignmentsOfDay_.size()) {
        for (std::size_t d = 0; d < dayOrder_.size(); ++d) dayOrder_[d] = static_cast<int>(d);
    }

    const char* name() const override { return "teamCount"; }

    bool apply(optimaru::OperatorContext<TournamentSchedule>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        touched_.clear();
        const int loaded = calculator_->loadedTeamCount();

        // Targets worth trying: loaded-team counts whose theoretical optimum
        // beats the current count's. Ordered best-first; the pick is
        // geometric so the best target dominates while the rest stay
        // reachable (the best may be unreachable under this instance's
        // unavailability pattern).
        candidates_.clear();
        for (int n : targetOrder_) {
            if (n != loaded && idealPenalty_[n] < idealPenalty_[loaded]) candidates_.push_back(n);
        }
        if (candidates_.empty()) return applyRandomChange(ctx, rng, entities);
        std::size_t pick = 0;
        while (pick + 1 < candidates_.size() && (rng() & 1U) == 0U) ++pick;
        const int target = candidates_[pick];
        if (target > loaded) {
            if (applyGrow(ctx, rng, entities, target)) return true;
            return applyRandomChange(ctx, rng, entities);
        }

        if (!selectDrainSet(loaded - target, rng)) return applyRandomChange(ctx, rng, entities);

        TournamentSchedule& schedule = ctx.editor.solution();
        const int assignmentCount = static_cast<int>(schedule.teamAssignments.size());
        ctx.editor.begin_batch();
        for (int i = 0; i < assignmentCount; ++i) {
            const TeamAssignment& assignment = schedule.teamAssignments[i];
            if (assignment.team < 0 || !inDrainSet_[assignment.team]) continue;
            const int replacement = bestReplacement(assignment.team, assignment.day, rng);
            if (replacement < 0) continue;
            touched_.emplace_back(i, assignment.team);
            entities.push_back(i);
            ctx.editor.assign(i, replacement);
        }
        ctx.editor.end_batch();
        clearDrainSet();
        if (touched_.empty()) return applyRandomChange(ctx, rng, entities);
        return true;
    }

    void undo_last(optimaru::OperatorContext<TournamentSchedule>& ctx) override {
        ctx.editor.begin_batch();
        for (auto it = touched_.rbegin(); it != touched_.rend(); ++it)
            ctx.editor.assign(it->first, it->second);
        ctx.editor.end_batch();
        touched_.clear();
    }

private:
    // The teams to empty: `count` of the currently loaded, drainable ones.
    // Half the time a random draw, half the time the coverage-greedy pick
    // below — the random draw alone rarely finds a workable subset (dropping
    // 4 of 12 teams is 495 subsets, only a few of which keep every day
    // coverable), and a deterministic pick would propose the same subset
    // forever.
    bool selectDrainSet(int count, std::mt19937_64& rng) {
        if (count <= 0) return false;
        const std::vector<std::int64_t>& loads = calculator_->teamLoads();
        pool_.clear();
        for (std::size_t t = 0; t < loads.size(); ++t) {
            if (loads[t] > 0 && drainable_[t]) pool_.push_back(static_cast<int>(t));
        }
        if (static_cast<int>(pool_.size()) < count) return false;
        std::shuffle(pool_.begin(), pool_.end(), rng);
        if ((rng() & 1U) == 0U) return markDrainSet(count);

        // Coverage-greedy: a day holding c assignments needs c teams that are
        // both kept and available on it, or the drain cannot come back
        // hard-feasible. Remove teams one at a time, each time the one that
        // costs the fewest such coverage slots (ties: the team with the most
        // unavailable days, then the shuffle order), tracking how much slack
        // every day has left.
        slack_.assign(assignmentsOfDay_.size(), 0);
        for (std::size_t d = 0; d < assignmentsOfDay_.size(); ++d) {
            int available = 0;
            for (std::size_t t = 0; t < loads.size(); ++t) {
                if (loads[t] > 0 &&
                    calculator_->penaltyCount(static_cast<int>(t), static_cast<int>(d)) == 0)
                    ++available;
            }
            slack_[d] = available - static_cast<int>(assignmentsOfDay_[d].size());
        }
        for (int picked = 0; picked < count; ++picked) {
            int best = -1;
            int bestCost = 0;
            for (std::size_t i = static_cast<std::size_t>(picked); i < pool_.size(); ++i) {
                const int team = pool_[i];
                int cost = 0;
                for (std::size_t d = 0; d < assignmentsOfDay_.size(); ++d) {
                    if (calculator_->penaltyCount(team, static_cast<int>(d)) == 0)
                        cost += slack_[d] <= 0 ? 1000 : 1;
                }
                if (best < 0 || cost < bestCost ||
                    (cost == bestCost && penaltyDays_[team] > penaltyDays_[pool_[best]])) {
                    best = static_cast<int>(i);
                    bestCost = cost;
                }
            }
            std::swap(pool_[static_cast<std::size_t>(picked)],
                      pool_[static_cast<std::size_t>(best)]);
            const int drained = pool_[static_cast<std::size_t>(picked)];
            for (std::size_t d = 0; d < assignmentsOfDay_.size(); ++d) {
                if (calculator_->penaltyCount(drained, static_cast<int>(d)) == 0) --slack_[d];
            }
        }
        return markDrainSet(count);
    }

    bool markDrainSet(int count) {
        for (int i = 0; i < count; ++i) inDrainSet_[pool_[i]] = 1;
        drainSetSize_ = count;
        return true;
    }

    void clearDrainSet() {
        for (int i = 0; i < drainSetSize_; ++i) inDrainSet_[pool_[i]] = 0;
        drainSetSize_ = 0;
    }

    // The opposite direction: bring (target - loaded) idle teams back in, each
    // filled up to the target's fair share by taking assignments off the
    // fullest team playing that day. Donors never fall below the fair share,
    // so the reshape lands on an evenly balanced layout in one move.
    bool applyGrow(optimaru::OperatorContext<TournamentSchedule>& ctx, std::mt19937_64& rng,
                   std::vector<int>& entities, int target) {
        TournamentSchedule& schedule = ctx.editor.solution();
        const std::vector<std::int64_t>& loads = calculator_->teamLoads();
        pool_.clear();
        for (std::size_t t = 0; t < loads.size(); ++t) {
            if (loads[t] == 0) pool_.push_back(static_cast<int>(t));
        }
        const int need = target - calculator_->loadedTeamCount();
        if (need <= 0 || static_cast<int>(pool_.size()) < need) return false;
        std::shuffle(pool_.begin(), pool_.end(), rng);
        if ((rng() & 1U) == 0U) {
            std::stable_sort(pool_.begin(), pool_.end(),
                             [&](int a, int b) { return penaltyDays_[a] < penaltyDays_[b]; });
        }

        const std::int64_t fairShare =
                static_cast<std::int64_t>(schedule.teamAssignments.size()) / target;
        ctx.editor.begin_batch();
        for (int i = 0; i < need; ++i) {
            const int team = pool_[i];
            std::shuffle(dayOrder_.begin(), dayOrder_.end(), rng);
            std::int64_t taken = 0;
            for (int day : dayOrder_) {
                if (taken >= fairShare) break;
                if (calculator_->cellCount(team, day) != 0 ||
                    calculator_->penaltyCount(team, day) != 0)
                    continue;
                int donor = -1;
                std::int64_t donorLoad = fairShare;
                for (int a : assignmentsOfDay_[day]) {
                    const TeamAssignment& assignment = schedule.teamAssignments[a];
                    if (assignment.pinned || assignment.team < 0) continue;
                    if (loads[assignment.team] > donorLoad) {
                        donor = a;
                        donorLoad = loads[assignment.team];
                    }
                }
                if (donor < 0) continue;
                touched_.emplace_back(donor, schedule.teamAssignments[donor].team);
                entities.push_back(donor);
                ctx.editor.assign(donor, team);
                ++taken;
            }
        }
        ctx.editor.end_batch();
        return !touched_.empty();
    }

    // Least-loaded kept team that keeps the move hard-feasible: not already
    // playing that day (teamConflictCost) and not unavailable on it
    // (teamUnavailabilityCost). Falls back to ignoring the unavailability so
    // a drain is completed and judged by the acceptor rather than left
    // half-done.
    int bestReplacement(int team, int day, std::mt19937_64& rng) const {
        const std::vector<std::int64_t>& loads = calculator_->teamLoads();
        const int teamCount = static_cast<int>(loads.size());
        int best = -1;
        int relaxed = -1;
        std::int64_t bestLoad = 0;
        std::int64_t relaxedLoad = 0;
        std::uint64_t bestTies = 0;
        std::uint64_t relaxedTies = 0;
        for (int u = 0; u < teamCount; ++u) {
            if (u == team || inDrainSet_[u] || loads[u] == 0) continue;
            if (calculator_->cellCount(u, day) != 0) continue;
            if (calculator_->penaltyCount(u, day) == 0) {
                if (best < 0 || loads[u] < bestLoad) {
                    best = u;
                    bestLoad = loads[u];
                    bestTies = 1;
                } else if (loads[u] == bestLoad && rng() % ++bestTies == 0) {
                    best = u;
                }
            } else {
                if (relaxed < 0 || loads[u] < relaxedLoad) {
                    relaxed = u;
                    relaxedLoad = loads[u];
                    relaxedTies = 1;
                } else if (loads[u] == relaxedLoad && rng() % ++relaxedTies == 0) {
                    relaxed = u;
                }
            }
        }
        return best >= 0 ? best : relaxed;
    }

    // Builtin-equivalent change move (see the class comment).
    bool applyRandomChange(optimaru::OperatorContext<TournamentSchedule>& ctx, std::mt19937_64& rng,
                           std::vector<int>& entities) {
        TournamentSchedule& schedule = ctx.editor.solution();
        const int assignmentCount = static_cast<int>(schedule.teamAssignments.size());
        const int teamCount = schedule.teamCount();
        if (assignmentCount == 0 || teamCount < 2) return false;
        int e = -1;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const int candidate =
                    static_cast<int>(rng() % static_cast<std::uint64_t>(assignmentCount));
            if (!schedule.teamAssignments[candidate].pinned) {
                e = candidate;
                break;
            }
        }
        if (e < 0) return false;
        const int oldTeam = schedule.teamAssignments[e].team;
        int v = static_cast<int>(rng() % static_cast<std::uint64_t>(teamCount - 1));
        if (oldTeam >= 0 && v >= oldTeam) ++v;
        touched_.emplace_back(e, oldTeam);
        entities.push_back(e);
        ctx.editor.assign(e, v);
        return true;
    }

    const Calculator* calculator_;
    std::vector<double> idealPenalty_;  // loaded-team count -> best possible
    std::vector<int> targetOrder_;      // counts, best ideal first
    std::vector<char> drainable_;       // team holds no pinned assignment
    std::vector<int> penaltyDays_;      // team -> unavailability fact count
    std::vector<std::vector<int>> assignmentsOfDay_;
    std::vector<char> inDrainSet_;
    std::vector<int> dayOrder_;
    std::vector<int> pool_;
    std::vector<int> candidates_;
    std::vector<int> slack_;                    // day -> spare available kept teams
    std::vector<std::pair<int, int>> touched_;  // (assignment, previous team)
    int drainSetSize_ = 0;
};

// The theoretical best engine soft penalty of a schedule that loads exactly
// n teams, assuming a hard-feasible layout: both levels are
// LoadBalance::unfairness values whose minimum over integer loads is
// sqrt(r * (k - r) / k) with r = total mod k over k bins. The medium level
// spreads `assignmentCount` over n teams; the soft level spreads the
// instance's confrontations — sum of C(assignments on a day, 2), the count
// reached when no team plays twice a day — over C(n, 2) team pairs. It is
// only a lower bound (unavailability may put it out of reach), which is
// exactly what it is used for: ordering which drains are worth proposing.
double idealPenaltyFor(int loadedTeams, std::int64_t assignmentCount, std::int64_t confrontations) {
    const auto minUnfairness = [](std::int64_t total, std::int64_t bins) {
        if (bins <= 0) return 0.0;
        const std::int64_t rest = total % bins;
        return std::sqrt(static_cast<double>(rest * (bins - rest)) / static_cast<double>(bins));
    };
    return minUnfairness(assignmentCount, loadedTeams) * kMediumScale +
           minUnfairness(confrontations, ScoreCalculator::pairs(loadedTeams));
}

// Engine configuration shared by every start.
void configureEngine(const Solver::Config& config, const TournamentSchedule& schedule,
                     optimaru::Settings& engineConfig) {
    engineConfig = optimaru::settings_from(config);
    // history_length 0 = auto-scale with the instance (see Config).
    engineConfig.search.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(8 * schedule.teamAssignments.size(), 400, 3200);
    engineConfig.search.moves.change_weight = config.change_weight;
    engineConfig.search.moves.swap_weight = config.swap_weight;
    engineConfig.search.descent_weight = config.descent_weight;
    engineConfig.search.custom_move_probability = config.custom_move_probability;
}

// Builds the macro-move operator for this instance, or nullptr when reshaping
// can never pay off (no room below the team count, or every team pinned in
// place).
std::unique_ptr<TeamCountMove> makeTeamCountMove(const TournamentSchedule& schedule,
                                                 const Calculator* calculator) {
    const int teamCount = schedule.teamCount();
    const int dayCount = schedule.dayCount();
    const std::int64_t assignmentCount = static_cast<std::int64_t>(schedule.teamAssignments.size());

    std::vector<std::int64_t> perDay(static_cast<std::size_t>(dayCount), 0);
    std::vector<std::vector<int>> assignmentsOfDay(static_cast<std::size_t>(dayCount));
    std::vector<char> drainable(teamCount, 1);
    for (int i = 0; i < static_cast<int>(schedule.teamAssignments.size()); ++i) {
        const TeamAssignment& assignment = schedule.teamAssignments[i];
        ++perDay[assignment.day];
        assignmentsOfDay[assignment.day].push_back(i);
        if (assignment.pinned && assignment.team >= 0) drainable[assignment.team] = 0;
    }
    std::int64_t confrontations = 0;
    std::int64_t maxPerDay = 1;
    for (std::int64_t count : perDay) {
        confrontations += ScoreCalculator::pairs(count);
        maxPerDay = std::max(maxPerDay, count);
    }
    // A day with c assignments needs c distinct teams to stay hard-feasible
    // and no team can play twice a day, so no schedule loads fewer than
    // max(maxPerDay, ceil(assignments / days)) teams.
    const int minTeams = static_cast<int>(
            std::max(maxPerDay,
                     dayCount > 0 ? (assignmentCount + dayCount - 1) / dayCount : assignmentCount));
    if (minTeams >= teamCount) return nullptr;
    bool anyDrainable = false;
    for (char ok : drainable) anyDrainable = anyDrainable || ok != 0;
    if (!anyDrainable) return nullptr;

    std::vector<double> idealPenalty(static_cast<std::size_t>(teamCount) + 1, 0.0);
    std::vector<int> targetOrder;
    for (int n = minTeams; n <= teamCount; ++n) {
        idealPenalty[n] = idealPenaltyFor(n, assignmentCount, confrontations);
        targetOrder.push_back(n);
    }
    std::stable_sort(targetOrder.begin(), targetOrder.end(),
                     [&](int a, int b) { return idealPenalty[a] < idealPenalty[b]; });

    std::vector<int> penaltyDays(teamCount, 0);
    for (const UnavailabilityPenalty& penalty : schedule.unavailabilityPenalties) {
        ++penaltyDays[penalty.team];
    }

    return std::make_unique<TeamCountMove>(calculator, std::move(idealPenalty),
                                           std::move(targetOrder), std::move(drainable),
                                           std::move(penaltyDays), std::move(assignmentsOfDay));
}

}  // namespace

void Solver::solve(TournamentSchedule& schedule) {
    if (schedule.teamAssignments.empty() || schedule.teamCount() == 0) {
        ScoreCalculator::score(schedule);
        return;
    }
    optimaru::check_config(config_);

    // Independent parallel starts: each start is a single-start Solver of its
    // own, with its own calculator — the guided move operator reads live
    // calculator state, so the engine's built-in multi-start (which shares
    // one config across threads) cannot be used.
    if (config_.parallel_solver_count > 1) {
        optimaru::solve_parallel_starts(
                config_, schedule, [](const Config& config, TournamentSchedule& candidate, int) {
                    Solver worker(config);
                    worker.solve(candidate);
                });
        return;
    }

    optimaru::Settings engineConfig;
    configureEngine(config_, schedule, engineConfig);

    // The single calculator instance of this start, created before the engine
    // so the guided move operator can bind to its live aggregates; the engine
    // resets it on the working solution at solve start.
    Calculator calculator;

    if (config_.value_hint) {
        // Guided change values: uniform among the teams that can take this
        // assignment's day without breaking a hard constraint (not already
        // playing it, not unavailable on it). The current team occupies its
        // own cell, so it is excluded automatically; -1 falls back to the
        // engine's uniform draw when no feasible team exists.
        const TournamentSchedule* solutionPtr = &schedule;
        const Calculator* calculatorPtr = &calculator;
        const int teamCount = schedule.teamCount();
        engineConfig.search.value_hint = [solutionPtr, calculatorPtr, teamCount](
                                                 std::mt19937_64& rng, int entityIndex) -> int {
            const int day = solutionPtr->teamAssignments[entityIndex].day;
            int picked = -1;
            std::uint32_t seen = 0;
            for (int u = 0; u < teamCount; ++u) {
                if (calculatorPtr->cellCount(u, day) != 0 ||
                    calculatorPtr->penaltyCount(u, day) != 0)
                    continue;
                if (rng() % ++seen == 0) picked = u;
            }
            return picked;
        };
    }

    // Construction heuristic + late-acceptance local search (+ ALNS kicks).
    optimaru::Solver<TournamentSchedule> engine(std::move(engineConfig));
    if (config_.stop_flag) engine.share_stop_flag(config_.stop_flag);
    if (config_.team_count_moves) {
        std::unique_ptr<TeamCountMove> teamCount = makeTeamCountMove(schedule, &calculator);
        if (teamCount) engine.add_move_operator(std::move(teamCount));
    }
    engine.solve(schedule, calculator);

    // The authoritative final score: a full recompute on the best solution.
    schedule.score = ScoreCalculator::score(schedule);
}

void Solver::solve(TournamentSchedule& schedule,
                   optimaru::DecomposedObjective<TournamentSchedule>& calculator) {
    optimaru::check_config(config_);
    if (!schedule.teamAssignments.empty() && schedule.teamCount() != 0) {
        // No TeamCountMove here: that operator reads the monolithic
        // calculator's live aggregates, which a caller-assembled plugin does
        // not expose. The built-in move mix plus the ALNS kicks remain.
        optimaru::Settings engineConfig;
        configureEngine(config_, schedule, engineConfig);
        optimaru::Solver<TournamentSchedule> engine(std::move(engineConfig));
        if (config_.stop_flag) engine.share_stop_flag(config_.stop_flag);
        engine.solve(schedule, calculator);
    }

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraint_totals() explains exactly this score.
    calculator.rebuild(schedule);
    schedule.score = calculator.score();
}

}  // namespace ts
