// SPDX-License-Identifier: Apache-2.0
// Part of the optimaru engine; portions derive from Timefold Solver (Apache-2.0). See NOTICE.

#ifndef OPTIMARU_ACCEPT_ENTITY_TABU_HPP
#define OPTIMARU_ACCEPT_ENTITY_TABU_HPP

#include <cstdint>
#include <random>
#include <unordered_map>

#include "optimaru/accept/acceptor.hpp"
#include "optimaru/score/score.hpp"

namespace optimaru {

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

}  // namespace optimaru

#endif  // OPTIMARU_ACCEPT_ENTITY_TABU_HPP
