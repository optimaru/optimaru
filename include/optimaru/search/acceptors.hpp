// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_SEARCH_ACCEPTORS_HPP
#define OPTIMARU_SEARCH_ACCEPTORS_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "optimaru/score.hpp"

namespace optimaru {

// ---------------------------------------------------------------------------
// Acceptor
// ---------------------------------------------------------------------------
// Decides whether local search keeps a candidate move. Each step the search
// samples one move, scores it, and asks:
//
//   accepts(candidate, current, best, ...)   keep this candidate?
//   on_accepted(entities, count)              it was kept (tabu memory)
//   on_step_end(current)                      the step is over, whatever the
//                                             outcome (history-based rules)
//
// `move_entities` lists what the move touched (entity indices on the
// assignment shape, element ids on the sequence shape); only entity tabu reads
// it. `progress` runs 0 -> 1 over the solve's limits (see
// StopCondition::progress); only simulated annealing reads it.
class Acceptor {
public:
    virtual ~Acceptor() = default;

    virtual bool accepts(const Score& candidate, const Score& current, const Score& best,
                         const int* move_entities, int move_entity_count, double progress,
                         std::mt19937_64& rng) = 0;
    virtual void on_accepted(const int* /*move_entities*/, int /*move_entity_count*/) {}
    virtual void on_step_end(const Score& /*current*/) {}
};

// Keep a candidate that is not worse than the current score.
class HillClimbing final : public Acceptor {
public:
    bool accepts(const Score& candidate, const Score& current, const Score&, const int*, int,
                 double, std::mt19937_64&) override {
        return candidate >= current;
    }
};

// Late acceptance hill climbing (Burke & Bykov): keep a candidate that is not
// worse than the current score, or than the score `history_length` steps ago.
class LateAcceptance final : public Acceptor {
public:
    LateAcceptance(std::size_t history_length, const Score& initial_score)
        : history_(history_length, initial_score) {}

    bool accepts(const Score& candidate, const Score& current, const Score&, const int*, int,
                 double, std::mt19937_64&) override {
        return candidate >= current || candidate >= history_[pos_];
    }
    void on_step_end(const Score& current) override {
        history_[pos_] = current;
        pos_ = (pos_ + 1) % history_.size();
    }

private:
    std::vector<Score> history_;
    std::size_t pos_ = 0;
};

// Tabu on entities: a kept move makes its entities tabu for `tenure` steps;
// a move touching a tabu entity is refused unless it beats the best score
// (aspiration). Otherwise it behaves like hill climbing: the tabu memory is
// what pushes the search off recently visited ground. (Textbook tabu search
// evaluates a whole neighbourhood per step; this is the one-candidate-per-
// step form.)
class EntityTabu final : public Acceptor {
public:
    explicit EntityTabu(int tenure) : tenure_(tenure) {}

    bool accepts(const Score& candidate, const Score& current, const Score& best,
                 const int* move_entities, int move_entity_count, double,
                 std::mt19937_64&) override {
        for (int i = 0; i < move_entity_count; ++i) {
            auto it = tabu_until_.find(move_entities[i]);
            if (it != tabu_until_.end() && it->second > step_ && !(candidate > best)) {
                return false;  // tabu, and no aspiration
            }
        }
        return candidate >= current;
    }
    void on_accepted(const int* move_entities, int move_entity_count) override {
        for (int i = 0; i < move_entity_count; ++i) {
            tabu_until_[move_entities[i]] = step_ + tenure_;
        }
    }
    void on_step_end(const Score&) override { ++step_; }

private:
    int tenure_;
    std::int64_t step_ = 0;
    std::unordered_map<int, std::int64_t> tabu_until_;
};

// Simulated annealing (Kirkpatrick et al.): a worse candidate is kept with
// probability exp(delta / temperature), taken per score level, with the
// temperature falling linearly from `start_temperature` to 0 as `progress`
// runs from 0 to 1.
class SimulatedAnnealing final : public Acceptor {
public:
    explicit SimulatedAnnealing(const Score& start_temperature)
        : start_temperature_(start_temperature) {}

    bool accepts(const Score& candidate, const Score& current, const Score&, const int*, int,
                 double progress, std::mt19937_64& rng) override {
        if (candidate >= current) return true;
        double probability = 1.0;
        double hard_delta = static_cast<double>(candidate.hard - current.hard);
        if (hard_delta < 0) {
            double temp = static_cast<double>(start_temperature_.hard) * (1.0 - progress);
            if (temp <= 0) return false;
            probability *= std::exp(hard_delta / temp);
        }
        double soft_delta = candidate.soft - current.soft;
        if (soft_delta < 0) {
            double temp = start_temperature_.soft * (1.0 - progress);
            if (temp <= 0) return false;
            probability *= std::exp(soft_delta / temp);
        }
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        return unit(rng) < probability;
    }

private:
    Score start_temperature_;
};

// Great deluge (Dueck 1993): keep any candidate at or above a "water level"
// that starts at the initial score and rises by a fixed step every step, so
// the acceptable region tightens over time. A zero step degenerates to hill
// climbing anchored at the initial score.
class GreatDeluge final : public Acceptor {
public:
    GreatDeluge(const Score& initial_score, const Score& level_step)
        : level_(initial_score), step_(level_step) {}

    bool accepts(const Score& candidate, const Score& current, const Score&, const int*, int,
                 double, std::mt19937_64&) override {
        return candidate >= current || candidate >= level_;
    }
    void on_step_end(const Score&) override {
        level_.hard += step_.hard;
        level_.soft += step_.soft;
    }

private:
    Score level_;
    Score step_;
};

// Step counting hill climbing (Bykov & Petrovic): like late acceptance, but
// the bar is a single score, reset to the current score every `interval`
// steps, instead of a rolling history.
class StepCountingHillClimbing final : public Acceptor {
public:
    StepCountingHillClimbing(std::int64_t interval, const Score& initial_score)
        : interval_(interval), bar_(initial_score) {}

    bool accepts(const Score& candidate, const Score& current, const Score&, const int*, int,
                 double, std::mt19937_64&) override {
        return candidate >= current || candidate >= bar_;
    }
    void on_step_end(const Score& current) override {
        if (++count_ >= interval_) {
            bar_ = current;
            count_ = 0;
        }
    }

private:
    std::int64_t interval_;
    std::int64_t count_ = 0;
    Score bar_;
};

// ---------------------------------------------------------------------------
// Record-to-record travel (Dueck 1993) / threshold accepting (Dueck 1990)
// ---------------------------------------------------------------------------
// Keeps a candidate that trails the baseline by no more than a threshold that
// shrinks every step:
//
//     keep  <=>  baseline.soft - candidate.soft <= threshold
//     threshold <- max(end_threshold, threshold - step)          (linear)
//     threshold <- max(end_threshold, threshold * step)          (exponential)
//
// `compare_to_best` picks the baseline: the best score so far gives classic
// RRT (the search may wander a fixed distance below the record), the current
// score gives threshold accepting (the wander is relative to where it already
// is).
//
// Santini, Ropke & Hvattum (2018) found RRT competitive with — often better
// than — simulated annealing across ALNS benchmarks, and it has one practical
// advantage here: its threshold is in score units, so it can be fitted to the
// initial objective without a temperature to guess (see autofit()).
//
// Feasibility is never traded away: a candidate whose hard score is worse than
// the baseline's is refused outright, and one that is better is always kept.
// The threshold governs the soft level only, which is where the engine's
// medium level is packed on multi-level domains.
class RecordToRecordTravel final : public Acceptor {
public:
    enum class Method { Linear, Exponential };

    RecordToRecordTravel(double start_threshold, double end_threshold, double step,
                         Method method = Method::Linear, bool compare_to_best = true)
        : end_threshold_(end_threshold),
          step_(step),
          method_(method),
          compare_to_best_(compare_to_best),
          threshold_(start_threshold) {}

    // A threshold schedule from fractions of the initial objective: start at
    // start_gap of |initial_soft|, end at end_gap, reaching the end in
    // `steps` steps.
    static RecordToRecordTravel autofit(double initial_soft, double start_gap, double end_gap,
                                        long long steps, Method method = Method::Linear,
                                        bool compare_to_best = true) {
        const double magnitude = std::fabs(initial_soft);
        const double start = start_gap * magnitude;
        const double end = end_gap * magnitude;
        const auto n = static_cast<double>(steps < 1 ? 1 : steps);
        double step;
        if (method == Method::Linear) {
            step = (start - end) / n;
        } else {
            // Guard the degenerate start == 0 case: any ratio would be
            // undefined, and a zero-threshold RRT is just hill climbing.
            step = start > 0.0 ? std::pow(end / start, 1.0 / n) : 1.0;
        }
        return RecordToRecordTravel(start, end, step, method, compare_to_best);
    }

    bool accepts(const Score& candidate, const Score& current, const Score& best, const int*,
                 int, double, std::mt19937_64&) override {
        const Score& baseline = compare_to_best_ ? best : current;
        const bool accepted = candidate.hard != baseline.hard
                                      ? candidate.hard > baseline.hard
                                      : baseline.soft - candidate.soft <= threshold_;
        advance();
        return accepted;
    }

private:
    void advance() {
        const double next = method_ == Method::Linear ? threshold_ - step_ : threshold_ * step_;
        threshold_ = std::max(end_threshold_, next);
    }

    double end_threshold_;
    double step_;
    Method method_;
    bool compare_to_best_;
    double threshold_;
};

}  // namespace optimaru

#endif  // OPTIMARU_SEARCH_ACCEPTORS_HPP
