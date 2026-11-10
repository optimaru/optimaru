#ifndef FAB_DECODER_HPP
#define FAB_DECODER_HPP

#include <climits>
#include <cstdint>
#include <vector>

#include "fab/domain.hpp"

namespace fab
{

// ---------------------------------------------------------------------------
// The serial schedule-generation decoder
// ---------------------------------------------------------------------------
// Turns a search state — one machine option and one priority rank per
// operation row — into a schedule. It matches the pure-Python reference
// `construct` (tests/semiconductor_scheduling/reference_semiconductor_scheduling.py, the test
// oracle) in every order-sensitive detail:
//
//   - pins are placed first, as obstacles on their machine and their
//     resources, in pinnedAssignments order;
//   - each step picks, among every lot's next operation, the minimum of
//     (rank, -lot priority, lot id); a pinned pick is appended at its pin;
//   - on a batch tool the pick pulls up to capacity-1 other ready rows whose
//     chosen option has the same machine, family and duration, in the same
//     order (pinned rows included: a pulled pin is placed with the batch);
//   - ready = max over the batch of release time (first step) or
//     previous completion + max(transport(previous group, this group),
//     previous min queue time);
//   - earliestStart pushes the start past machine downtime, then the
//     selected operation's resource downtime / capacity (duplicates counted),
//     then a unary overlap, then an incompatible batch family plus its setup,
//     then a compatible overlap, then the setup from the latest-ending earlier
//     item unless it is a same-family batch continuation ending exactly at
//     the start — at most 10 000 passes (std::runtime_error);
//   - batch ids B001.. are numbered per batch machine in machine order over
//     its items stably sorted by (start, end), grouped by (family, start,
//     end), pin items included.
//
// An option < 0 means "unassigned": the decoder stops that lot there (the row
// and the rest of its route get no assignment).
//
// INCREMENTAL. A row's option and rank are read only from its readyStep on —
// the first step at which it is its lot's next operation. Until then it is
// neither compared nor a batch candidate, so changing it cannot affect the
// earlier steps. truncate(s) forgets steps >= s (every machine / resource
// list is appended in step order, so this pops their tails) and run()
// decodes on from there: after a change of rows R, truncating to
// min over R of readyStep and running again gives exactly the decode from
// scratch.
class Decoder
{
public:
    static constexpr int kNever = INT_MAX;

    struct Placement
    {
        int step = -1;  // -1 = not placed
        int machine = -1;
        Time start = 0;
        Time end = 0;
        std::int64_t seq = -1;  // position in the assignment list
        bool pinned = false;    // placed at its pin (selected as a pinned row)
        int entry = -1;         // its item in the machine's list
    };

    // Binds a prepared schedule, places the pins and decodes nothing yet.
    void bind(const SemiconductorSchedule &schedule);
    // Forget steps >= step (no-op past the end).
    void truncate(int step);
    // Decode on from the current end until no lot has a ready row.
    // option[row] is the option index (< 0 = unassigned); rank[row] any int.
    void run(const int *option, const std::int64_t *rank);

    int stepCount() const { return static_cast<int>(steps_.size()); }
    int readyStep(int row) const { return readyStep_[static_cast<std::size_t>(row)]; }
    const Placement &placement(int row) const
    {
        return placement_[static_cast<std::size_t>(row)];
    }
    // Rows in assignment-list order (the order they were placed).
    const std::vector<int> &output() const { return output_; }
    // The output index of the first row placed at `step` (output().size()
    // when step == stepCount()).
    int outputStartOf(int step) const
    {
        return step >= stepCount() ? static_cast<int>(output_.size())
                                   : steps_[static_cast<std::size_t>(step)];
    }
    // The decoded schedule, batch ids included (a post-pass).
    std::vector<Assignment> assignments() const;

private:
    struct MachineItem
    {
        Time start;
        Time end;
        int family;
        int stamp;  // the step that placed it; -1 = a pin
        int row;
    };
    struct ResourceItem
    {
        Time start;
        Time end;
        int stamp;
    };

    Time earliestStart(int machine, const Operation &operation, Time ready, Time duration) const;
    bool before(int a, int b, const std::int64_t *rank) const;
    void placeRow(int row, int machine, Time start, Time end, bool pinned, int entry, int step);

    const SemiconductorSchedule *schedule_ = nullptr;
    std::vector<std::vector<MachineItem>> machineItems_;
    std::vector<std::vector<ResourceItem>> resourceItems_;
    std::vector<int> pinEntry_;  // per pin: its item index on its machine
    std::vector<Placement> placement_;
    std::vector<int> readyStep_;
    std::vector<int> nextOperation_;  // per lot
    std::vector<int> steps_;          // per step: output index of its first row
    std::vector<int> output_;
    // scratch
    mutable std::vector<int> ready_;
    mutable std::vector<int> batch_;
};

// --- whole-schedule helpers (the test hooks and the solver) -----------------

// The solve start: per row, the option with the least
// (preference cost, duration), the first on ties.
std::vector<int> defaultChoices(const SemiconductorSchedule &schedule);

// The seed state: `schedule.assignments` as a (choices, ranks)
// state. A seeded row takes the LAST option with the seeded machine and
// duration (else keeps choices[row]); ranks follow (unseeded last, seeded
// start, row).
void seedState(const SemiconductorSchedule &schedule,
               const std::vector<int> &choices,
               std::vector<int> &selected,
               std::vector<int> &ranks);

// Decode (option index per row, rank per row); option < 0 = unassigned.
std::vector<Assignment> decode(const SemiconductorSchedule &schedule,
                               const std::vector<int> &option,
                               const std::vector<std::int64_t> &rank);

// Decode the planning values of schedule.rows.
std::vector<Assignment> decodeValues(const SemiconductorSchedule &schedule);

// The per-row (option, rank) arrays of a value vector.
void valuesToState(const SemiconductorSchedule &schedule,
                   std::vector<int> &option,
                   std::vector<std::int64_t> &rank);

}  // namespace fab

#endif  // FAB_DECODER_HPP
