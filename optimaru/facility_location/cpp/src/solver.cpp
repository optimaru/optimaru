#include "fl/solver.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "fl/incremental_calculator.hpp"
#include "fl/score_calculator.hpp"
#include "optimaru/solver.hpp"
#include "optimaru/settings.hpp"
#include "optimaru/search/operators.hpp"

namespace fl {

namespace {

// The share of Config::custom_move_probability that the macro moves are
// drawn with once late acceptance has started over (see runEngine).
constexpr double kLaterStageGateShare = 0.1;

// ---------------------------------------------------------------------------
// CloseFacilityMove — the domain's missing macro move
// ---------------------------------------------------------------------------
// facilitySetupCost is a step function of "does this facility serve anybody",
// and it is about half of the soft score. No built-in move can cross that
// step: a facility only stops costing its setup once its LAST consumer
// leaves, so every single-consumer change on the way there is strictly worse
// and the late-acceptance search rejects it. That is why the plain solver
// converges within a tenth of a second and then never improves again.
//
// This operator does the whole step in one move: pick a used facility and
// empty it, either by redistributing its consumers over the facilities that
// are already open (so the move pays no new setup cost at all) or by
// migrating them together to one alternative site. The acceptor then judges
// the complete trade — exactly one score evaluation, not a sequence of
// individually-worse ones.
//
// Cost is O(consumers + facilities) per move (two censuses plus a short walk
// down each moved consumer's distance-ordered facility list), roughly 10x a
// built-in change move; the adaptive roulette wheel decides how often it is
// worth that. Selection between it and the built-in mix is by move outcome.
class CloseFacilityMove final : public optimaru::MoveOperator<FacilityLocationProblem> {
public:
    // `preference` holds, per consumer, the `keep` nearest facilities in
    // ascending distance (flattened consumerIndex * keep + rank).
    CloseFacilityMove(std::shared_ptr<std::vector<int>> preference, int keep,
                      double migrateProbability)
        : preference_(std::move(preference)),
          keep_(keep),
          migrateProbability_(migrateProbability) {}

    const char* name() const override { return "closeFacility"; }

    bool apply(optimaru::OperatorContext<FacilityLocationProblem>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        FacilityLocationProblem& problem = ctx.editor.solution();
        const int facilityCount = problem.facilityCount();
        const int consumerCount = problem.consumerCount();

        // Census: consumer count and used demand per facility, plus the list
        // of currently used facilities.
        count_.assign(facilityCount, 0);
        load_.assign(facilityCount, 0);
        for (int c = 0; c < consumerCount; ++c) {
            const Consumer& consumer = problem.consumers[c];
            if (consumer.facility >= 0) {
                ++count_[consumer.facility];
                load_[consumer.facility] += consumer.demand;
            }
        }
        used_.clear();
        for (int f = 0; f < facilityCount; ++f) {
            if (count_[f] > 0) used_.push_back(f);
        }
        if (used_.size() < 2) return false;  // nowhere to move the consumers to

        const int victim = used_[rng() % used_.size()];
        members_.clear();
        for (int c = 0; c < consumerCount; ++c) {
            if (problem.consumers[c].facility == victim) members_.push_back(c);
        }
        if (members_.empty()) return false;

        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const bool migrate = unit(rng) < migrateProbability_;

        // The victim is being emptied: its residual capacity is irrelevant
        // and it must never be picked as a target.
        count_[victim] = 0;
        load_[victim] = 0;

        int migrationTarget = -1;
        if (migrate) {
            migrationTarget = pickMigrationTarget(problem, victim, rng);
            if (migrationTarget < 0) return false;
        }

        lastVictim_ = victim;
        ctx.editor.begin_batch();
        for (int c : members_) {
            const int target = migrate ? migrationTarget : pickTarget(problem, c, victim);
            ctx.editor.assign(c, target);
            ++count_[target];
            load_[target] += problem.consumers[c].demand;
            entities.push_back(c);
        }
        ctx.editor.end_batch();
        return true;
    }

    void undo_last(optimaru::OperatorContext<FacilityLocationProblem>& ctx) override {
        ctx.editor.begin_batch();
        for (int c : members_) ctx.editor.assign(c, lastVictim_);
        ctx.editor.end_batch();
    }

private:
    bool fits(const FacilityLocationProblem& problem, int facility, std::int64_t demand) const {
        return load_[facility] + demand <= problem.facilities[facility].capacity;
    }

    // Where one evicted consumer goes: the closest already-open facility with
    // room (no new setup cost), else the closest facility with room at all,
    // else the closest facility, period.
    int pickTarget(const FacilityLocationProblem& problem, int consumerIndex, int victim) const {
        const std::int64_t demand = problem.consumers[consumerIndex].demand;
        const int* order = preference_->data() + static_cast<std::size_t>(consumerIndex) * keep_;
        int roomAnywhere = -1;
        for (int rank = 0; rank < keep_; ++rank) {
            const int f = order[rank];
            if (f == victim) continue;
            if (!fits(problem, f, demand)) continue;
            if (count_[f] > 0) return f;  // already open: the cheapest kind of target
            if (roomAnywhere < 0) roomAnywhere = f;
        }
        if (roomAnywhere >= 0) return roomAnywhere;
        return order[0] == victim ? order[1 % keep_] : order[0];
    }

    // Where the whole facility moves: the closest alternative site that can
    // absorb the victim's entire load, seen from one of its consumers.
    int pickMigrationTarget(const FacilityLocationProblem& problem, int victim,
                            std::mt19937_64& rng) const {
        std::int64_t demand = 0;
        for (int c : members_) demand += problem.consumers[c].demand;
        const int seed = members_[rng() % members_.size()];
        const int* order = preference_->data() + static_cast<std::size_t>(seed) * keep_;
        for (int rank = 0; rank < keep_; ++rank) {
            const int f = order[rank];
            if (f != victim && fits(problem, f, demand)) return f;
        }
        return -1;
    }

    std::shared_ptr<std::vector<int>> preference_;
    int keep_;
    double migrateProbability_;

    // Scratch, reused across moves (no per-move allocation).
    std::vector<int> count_;
    std::vector<std::int64_t> load_;
    std::vector<int> used_;
    std::vector<int> members_;
    int lastVictim_ = -1;
};

// ---------------------------------------------------------------------------
// SwapFacilityMove — close one facility and open another
// ---------------------------------------------------------------------------
// CloseFacilityMove takes a facility out of the solution and nothing puts
// another one in: once the search has settled on its facilities, opening one
// is a change move that pays a whole setup cost for a single consumer, and
// late acceptance turns it down. So the search keeps the facilities it has
// and only improves who goes where.
//
// This move opens a closed facility f and closes an open one g in one step:
//   1. f is drawn uniformly among the closed facilities, and g is the
//      facility of one of the kSeeds consumers nearest to f, so g serves
//      where f stands;
//   2. the consumers of g leave, the farthest from f first, each to the
//      nearest facility with room among the open ones of its shortlist and
//      f;
//   3. a facility that has just gained room takes the consumers that are
//      nearer to it than to the facility they are at, nearest first, while
//      it has room: first f, then (kPullRounds rounds in all) the facilities
//      that gave consumers away.
// A consumer of g that finds no room = no move. The acceptor judges the whole
// trade in one score evaluation.
//
// Cost per move: two O(consumers) passes (the census and the consumers of
// g), and `reach` consumers looked at for each facility that takes.
class SwapFacilityMove final : public optimaru::MoveOperator<FacilityLocationProblem> {
public:
    static constexpr int kSeeds = 8;
    static constexpr int kPullRounds = 2;

    // `preference`: as for CloseFacilityMove. `nearest`: per facility, the
    // `reach` nearest consumers in ascending distance (flattened
    // facilityIndex * reach + rank).
    SwapFacilityMove(std::shared_ptr<std::vector<int>> preference, int keep,
                     std::shared_ptr<std::vector<int>> nearest, int reach)
        : preference_(std::move(preference)),
          keep_(keep),
          nearest_(std::move(nearest)),
          reach_(reach) {}

    const char* name() const override { return "swapFacility"; }

    bool apply(optimaru::OperatorContext<FacilityLocationProblem>& ctx, std::mt19937_64& rng,
               std::vector<int>& entities) override {
        FacilityLocationProblem& problem = ctx.editor.solution();
        const int facilityCount = problem.facilityCount();
        const int consumerCount = problem.consumerCount();

        // Census: the plan starts as the current assignment.
        open_.assign(facilityCount, 0);
        load_.assign(facilityCount, 0);
        plan_.resize(consumerCount);
        for (int c = 0; c < consumerCount; ++c) {
            const Consumer& consumer = problem.consumers[c];
            plan_[c] = consumer.facility;
            if (consumer.facility >= 0) {
                open_[consumer.facility] = 1;
                load_[consumer.facility] += consumer.demand;
            }
        }
        closed_.clear();
        for (int f = 0; f < facilityCount; ++f) {
            if (!open_[f]) closed_.push_back(f);
        }
        if (closed_.empty()) return false;  // nothing to open

        const int opened = closed_[rng() % closed_.size()];
        const int* around = nearest_->data() + static_cast<std::size_t>(opened) * reach_;
        const int closing = plan_[around[rng() % std::min(kSeeds, reach_)]];
        if (closing < 0) return false;  // inside a ruin: that consumer is nowhere
        open_[closing] = 0;
        load_[closing] = 0;
        open_[opened] = 1;

        // The consumers of the closing facility, the farthest from the new
        // one first: they are the ones the other facilities have to take.
        members_.clear();
        for (int c = 0; c < consumerCount; ++c) {
            if (plan_[c] == closing) members_.push_back(c);
        }
        std::sort(members_.begin(), members_.end(), [&problem, opened](int a, int b) {
            const std::int64_t da = problem.distanceOf(a, opened);
            const std::int64_t db = problem.distanceOf(b, opened);
            return da != db ? da > db : a < b;
        });
        touched_.clear();
        for (int c : members_) {
            const int target = nearestWithRoom(problem, c, opened);
            if (target < 0) return false;  // no room for this one: no move
            place(problem, c, target);
        }

        // Pull: whoever gained room takes the consumers nearer to it.
        takers_.assign(1, opened);
        for (int round = 0; round < kPullRounds && !takers_.empty(); ++round) {
            givers_.clear();
            for (int taker : takers_) {
                const int* near = nearest_->data() + static_cast<std::size_t>(taker) * reach_;
                for (int rank = 0; rank < reach_; ++rank) {
                    const int c = near[rank];
                    const int from = plan_[c];
                    if (from == taker || from < 0) continue;
                    if (load_[taker] + problem.consumers[c].demand >
                        problem.facilities[taker].capacity)
                        continue;
                    if (problem.distanceOf(c, taker) >= problem.distanceOf(c, from)) continue;
                    place(problem, c, taker);
                    if (std::find(givers_.begin(), givers_.end(), from) == givers_.end())
                        givers_.push_back(from);
                }
            }
            takers_.swap(givers_);
        }

        moved_.clear();
        ctx.editor.begin_batch();
        for (int c : touched_) {
            const int from = problem.consumers[c].facility;
            if (from == plan_[c]) continue;  // back where it was, or already moved
            moved_.push_back({c, from});
            ctx.editor.assign(c, plan_[c]);
            entities.push_back(c);
        }
        ctx.editor.end_batch();
        return true;
    }

    void undo_last(optimaru::OperatorContext<FacilityLocationProblem>& ctx) override {
        ctx.editor.begin_batch();
        for (const std::pair<int, int>& move : moved_) ctx.editor.assign(move.first, move.second);
        ctx.editor.end_batch();
    }

private:
    // Moves the consumer in the plan.
    void place(const FacilityLocationProblem& problem, int consumerIndex, int facility) {
        const std::int64_t demand = problem.consumers[consumerIndex].demand;
        const int from = plan_[consumerIndex];
        if (from >= 0 && open_[from]) load_[from] -= demand;
        load_[facility] += demand;
        plan_[consumerIndex] = facility;
        touched_.push_back(consumerIndex);
    }

    // The nearest facility with room for the consumer, among the open ones
    // of its shortlist and the one being opened; -1 when none has room.
    int nearestWithRoom(const FacilityLocationProblem& problem, int consumerIndex,
                        int opened) const {
        const std::int64_t demand = problem.consumers[consumerIndex].demand;
        auto fits = [&](int f) { return load_[f] + demand <= problem.facilities[f].capacity; };
        const int* order = preference_->data() + static_cast<std::size_t>(consumerIndex) * keep_;
        int target = fits(opened) ? opened : -1;
        for (int rank = 0; rank < keep_; ++rank) {
            const int f = order[rank];
            if (!open_[f] || !fits(f)) continue;
            if (target < 0 ||
                problem.distanceOf(consumerIndex, f) < problem.distanceOf(consumerIndex, target))
                target = f;
            break;  // the shortlist is in ascending distance
        }
        return target;
    }

    std::shared_ptr<std::vector<int>> preference_;
    int keep_;
    std::shared_ptr<std::vector<int>> nearest_;
    int reach_;

    // Scratch, reused across moves (no per-move allocation).
    std::vector<char> open_;          // per facility, as planned
    std::vector<std::int64_t> load_;  // per facility, as planned
    std::vector<int> plan_;           // per consumer: its facility after the move
    std::vector<int> closed_;
    std::vector<int> members_;
    std::vector<int> touched_;  // consumers the plan moved (may repeat)
    std::vector<int> takers_;
    std::vector<int> givers_;
    std::vector<std::pair<int, int>> moved_;  // (consumer, facility before the move)
};

// Per facility, the `reach` nearest consumers in ascending distance
// (flattened facilityIndex * reach + rank): whom SwapFacilityMove looks at
// for a facility that has room.
std::shared_ptr<std::vector<int>> buildNearestConsumers(const FacilityLocationProblem& problem,
                                                        int reach) {
    auto nearest = std::make_shared<std::vector<int>>(
            static_cast<std::size_t>(problem.facilityCount()) * reach);
    std::vector<std::pair<std::int64_t, int>> byDistance;
    byDistance.reserve(problem.consumers.size());
    for (int f = 0; f < problem.facilityCount(); ++f) {
        byDistance.clear();
        for (int c = 0; c < problem.consumerCount(); ++c)
            byDistance.emplace_back(problem.distanceOf(c, f), c);
        std::partial_sort(byDistance.begin(), byDistance.begin() + reach, byDistance.end());
        for (int rank = 0; rank < reach; ++rank) {
            (*nearest)[static_cast<std::size_t>(f) * reach + rank] = byDistance[rank].second;
        }
    }
    return nearest;
}

// Per consumer, the `keep` nearest facilities in ascending distance
// (flattened consumerIndex * keep + rank): where CloseFacilityMove looks for
// a new home for an evicted consumer.
std::shared_ptr<std::vector<int>> buildFacilityPreference(const FacilityLocationProblem& problem,
                                                          int keep) {
    auto preference = std::make_shared<std::vector<int>>(
            static_cast<std::size_t>(problem.consumerCount()) * keep);
    std::vector<std::pair<std::int64_t, int>> byDistance;
    byDistance.reserve(problem.facilities.size());
    for (int c = 0; c < problem.consumerCount(); ++c) {
        byDistance.clear();
        for (int f = 0; f < problem.facilityCount(); ++f)
            byDistance.emplace_back(problem.distanceOf(c, f), f);
        std::partial_sort(byDistance.begin(), byDistance.begin() + keep, byDistance.end());
        for (int rank = 0; rank < keep; ++rank) {
            (*preference)[static_cast<std::size_t>(c) * keep + rank] = byDistance[rank].second;
        }
    }
    return preference;
}

// Shared engine wiring for both solve() overloads: construction heuristic +
// late-acceptance local search (+ ALNS kicks), plus the domain's own macro
// move, driven by whichever calculator the caller brought (the monolithic
// incremental one, or a plugin assembly).
void runEngine(const Solver::Config& config, FacilityLocationProblem& problem,
               optimaru::Objective<FacilityLocationProblem>& calculator) {
    optimaru::Settings engineConfig = optimaru::settings_from(config);
    // history_length 0 = auto-scale with the instance (see Config).
    engineConfig.search.acceptance.history_length =
            config.history_length != 0
                    ? config.history_length
                    : std::clamp<std::size_t>(8 * problem.consumers.size(), 400, 3200);
    engineConfig.search.moves.change_weight = config.change_weight;
    engineConfig.search.moves.swap_weight = config.swap_weight;
    engineConfig.search.descent_weight = config.descent_weight;
    engineConfig.search.custom_move_probability = config.custom_move_probability;
    // A solve that the construction starts restarts late acceptance with a
    // doubled history when it stalls (AcceptanceSettings::history_growth).
    // The first stage is the descent in which the macro moves close what the
    // construction opened, at the full gate. From the first restart on they
    // are drawn on a tenth of the steps the gate allows: a later stage takes
    // a worse solution for a long while, the macro moves cost a pass over
    // every consumer each, and at the full gate they take the steps that
    // settle who goes where (Config::swap_facility_moves has the
    // measurements). A solve from a given assignment has no stages, for
    // there is no worse score to start over from, and so the full gate
    // throughout.
    if (optimaru::nothing_placed(problem)) {
        engineConfig.search.acceptance.history_growth = 2.0;
        engineConfig.search.acceptance.later_stage_custom_move_share = kLaterStageGateShare;
    }

    // Nearby selection: consumer-to-consumer Euclidean degree distance (the
    // ordering is what matters, so the sqrt/meter scaling is skipped). The
    // problem outlives the engine run, so capturing it by reference is safe.
    if (config.nearby_probability > 0.0 && config.nearby_size > 0) {
        engineConfig.search.nearby.probability = config.nearby_probability;
        engineConfig.search.nearby.size = config.nearby_size;
        const FacilityLocationProblem& fixed = problem;
        engineConfig.search.nearby.distance = [&fixed](int a, int b) {
            const Location& la = fixed.consumers[a].location;
            const Location& lb = fixed.consumers[b].location;
            const double dLat = la.latitude - lb.latitude;
            const double dLon = la.longitude - lb.longitude;
            return dLat * dLat + dLon * dLon;
        };
    }

    // Distance-ordered facility shortlist per consumer: where
    // CloseFacilityMove and SwapFacilityMove look for a new home for each
    // evicted consumer.
    const int preferenceKeep = std::min(config.close_facility_keep, problem.facilityCount());
    std::shared_ptr<std::vector<int>> preference;
    if ((config.close_facility_moves || config.swap_facility_moves) && preferenceKeep > 0)
        preference = buildFacilityPreference(problem, preferenceKeep);

    // Guided change-move targets: draw the consumer's new facility from its
    // k nearest ones with the configured probability (-1 falls back to the
    // engine's uniform draw).
    std::shared_ptr<std::vector<int>> valuePreference;
    const int valueKeep = std::min(config.value_recommender_keep, problem.facilityCount());
    if (config.value_recommender_probability > 0.0 && valueKeep > 0) {
        valuePreference = (valueKeep == preferenceKeep && preference)
                                  ? preference
                                  : buildFacilityPreference(problem, valueKeep);
        const double p = config.value_recommender_probability;
        engineConfig.search.value_hint = [valuePreference, valueKeep, p](std::mt19937_64& rng,
                                                                         int entityIndex) -> int {
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            if (unit(rng) >= p) return -1;
            const std::size_t base = static_cast<std::size_t>(entityIndex) * valueKeep;
            return (*valuePreference)[base + rng() % valueKeep];
        };
    }

    optimaru::Solver<FacilityLocationProblem> engine(std::move(engineConfig));
    if (config.stop_flag) engine.share_stop_flag(config.stop_flag);
    if (preference && config.close_facility_moves) {
        engine.add_move_operator(std::make_unique<CloseFacilityMove>(
                preference, preferenceKeep, config.close_facility_migrate_probability));
    }
    if (preference && config.swap_facility_moves) {
        // Enough consumers for a facility to fill itself from: eight times
        // its even share, at least 64.
        const int reach =
                std::min(problem.consumerCount(),
                         std::max(64, 8 * problem.consumerCount() / problem.facilityCount()));
        engine.add_move_operator(std::make_unique<SwapFacilityMove>(
                preference, preferenceKeep, buildNearestConsumers(problem, reach), reach));
    }
    engine.solve(problem, calculator);
}

}  // namespace

void Solver::solve(FacilityLocationProblem& problem) {
    ScoreCalculator::prepare(problem);  // distance matrix
    if (problem.consumers.empty() || problem.facilities.empty()) {
        ScoreCalculator::score(problem);
        return;
    }
    optimaru::check_config(config_);

    // Independent parallel starts (the school-timetabling pattern): each start
    // is a single-start Solver of its own. The engine's own multi-start cannot
    // be used, because it runs the built-in move mix only — a registered
    // custom operator like CloseFacilityMove is per-Solver and stateful, so
    // it would silently vanish in the worker threads.
    if (config_.parallel_solver_count > 1) {
        optimaru::solve_parallel_starts(
                config_, problem,
                [](const Config& config, FacilityLocationProblem& candidate, int) {
                    Solver worker(config);
                    worker.solve(candidate);
                });
        return;
    }

    FacilityLocationIncrementalScoreCalculator calculator;
    runEngine(config_, problem, calculator);

    // The authoritative final score: a full recompute on the best solution.
    problem.score = ScoreCalculator::score(problem);
}

void Solver::solve(FacilityLocationProblem& problem,
                   optimaru::DecomposedObjective<FacilityLocationProblem>& calculator) {
    ScoreCalculator::prepare(problem);  // distance matrix
    optimaru::check_config(config_);
    if (!problem.consumers.empty() && !problem.facilities.empty())
        runEngine(config_, problem, calculator);

    // Score the final solution with the caller's constraint assembly (custom
    // weights included) and leave the calculator primed on it, so
    // constraint_totals() explains exactly this score.
    calculator.rebuild(problem);
    problem.score = calculator.score();
}

}  // namespace fl
