#include "vrp/incremental_calculator.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>
#include <vector>

namespace vrp
{

void VehicleRoutingIncrementalScoreCalculator::resetWorkingSolution(
    VehicleRoutePlan &workingSolution)
{
    plan_ = &workingSolution;
    vehicleCount_ = static_cast<int>(plan_->vehicles.size());

    terms_.assign(static_cast<std::size_t>(vehicleCount_), RouteTerms{});
    caches_.assign(static_cast<std::size_t>(vehicleCount_) + 1, ListCache{});
    Seconds earliestDeparture = std::numeric_limits<Seconds>::max();
    for (const Vehicle &vehicle : plan_->vehicles)
        earliestDeparture = std::min(earliestDeparture, vehicle.departureTime);
    noWaiting_ = true;
    for (const Visit &visit : plan_->visits)
        noWaiting_ = noWaiting_ && visit.minStartTime <= earliestDeparture;
    overloadTotal_ = 0;
    latenessTotal_ = 0;
    drivingTotal_ = 0;

    for (int v = 0; v < vehicleCount_; ++v)
    {
        terms_[static_cast<std::size_t>(v)] = evaluateRoute(v);
        const RouteTerms &t = terms_[static_cast<std::size_t>(v)];
        overloadTotal_ += t.overload;
        latenessTotal_ += t.lateness;
        drivingTotal_ += t.driving;
    }
    unassignedTotal_ = unassignedMinutes();
    for (int l = 0; l <= vehicleCount_; ++l)
    {
        if (noWaiting_)
            rebuildCache(l);
        else
            caches_[static_cast<std::size_t>(l)] = ListCache{};
    }
}

void VehicleRoutingIncrementalScoreCalculator::beforeListChanged(int listIndex)
{
    if (listIndex >= vehicleCount_)
    {
        // The unassigned pool: its whole contribution is the medium penalty.
        unassignedTotal_ = 0;
        return;
    }
    const RouteTerms &t = terms_[static_cast<std::size_t>(listIndex)];
    overloadTotal_ -= t.overload;
    latenessTotal_ -= t.lateness;
    drivingTotal_ -= t.driving;
}

void VehicleRoutingIncrementalScoreCalculator::afterListChanged(int listIndex)
{
    // The cache only serves previews; without them it would be pure overhead.
    if (noWaiting_)
        rebuildCache(listIndex);
    if (listIndex >= vehicleCount_)
    {
        // The pool's penalty is the last prefix sum when the cache is live.
        unassignedTotal_ = noWaiting_
                               ? caches_[static_cast<std::size_t>(listIndex)].cumPenalty.back()
                               : unassignedMinutes();
        return;
    }
    RouteTerms t = termsFromCache(listIndex);
    overloadTotal_ += t.overload;
    latenessTotal_ += t.lateness;
    drivingTotal_ += t.driving;
    terms_[static_cast<std::size_t>(listIndex)] = t;
}

// One walk of the route derives all three per-vehicle terms: arrival times
// propagate forward, so capacity, lateness and travel time come out together.
VehicleRoutingIncrementalScoreCalculator::RouteTerms
VehicleRoutingIncrementalScoreCalculator::evaluateRoute(int vehicleIndex) const
{
    return evaluateElements(
        vehicleIndex, plan_->vehicles[static_cast<std::size_t>(vehicleIndex)].visitIndices);
}

VehicleRoutingIncrementalScoreCalculator::RouteTerms
VehicleRoutingIncrementalScoreCalculator::evaluateElements(int vehicleIndex,
                                                           const std::vector<int> &elements) const
{
    const Vehicle &vehicle = plan_->vehicles[static_cast<std::size_t>(vehicleIndex)];
    RouteTerms terms;
    if (elements.empty())
        return terms;

    std::int64_t totalDemand = 0;
    Seconds previousDeparture = vehicle.departureTime;
    int previousLocation = vehicle.homeLocationIndex;

    for (int visitIndex : elements)
    {
        const Visit &visit = plan_->visits[static_cast<std::size_t>(visitIndex)];
        totalDemand += visit.demand;

        const Seconds drive
            = plan_->drivingTime(previousLocation, visit.locationIndex);
        terms.driving += drive;

        const Seconds arrival = previousDeparture + drive;
        // Faithful to the Java quickstart: lateness is measured from
        // arrivalTime, not from the (possibly later) service start.
        const Seconds serviceFinished = arrival + visit.serviceDuration;
        if (serviceFinished > visit.maxEndTime)
            terms.lateness += ceilToMinutes(serviceFinished - visit.maxEndTime);

        previousDeparture
            = std::max(arrival, visit.minStartTime) + visit.serviceDuration;
        previousLocation = visit.locationIndex;
    }
    terms.driving += vehicle.homeLeg(plan_->drivingSeconds, previousLocation);

    if (totalDemand > vehicle.capacity)
        terms.overload = totalDemand - vehicle.capacity;
    return terms;
}

// The cache already holds the route's load, service and driving totals; the
// full walk is only needed when some visit can actually finish late.
VehicleRoutingIncrementalScoreCalculator::RouteTerms
VehicleRoutingIncrementalScoreCalculator::termsFromCache(int vehicleIndex) const
{
    const Vehicle &vehicle = plan_->vehicles[static_cast<std::size_t>(vehicleIndex)];
    const ListCache &c = caches_[static_cast<std::size_t>(vehicleIndex)];
    const std::size_t size = vehicle.visitIndices.size();
    if (size == 0)
        return RouteTerms{};
    if (!noWaiting_)  // no cache without previews
        return evaluateRoute(vehicleIndex);
    RouteTerms terms;
    const int lastLocation
        = plan_->visits[static_cast<std::size_t>(vehicle.visitIndices[size - 1])].locationIndex;
    terms.driving = c.cumDist[size] + vehicle.homeLeg(plan_->drivingSeconds, lastLocation);
    terms.overload = std::max<std::int64_t>(0, c.cumLoad[size] - vehicle.capacity);
    terms.lateness = c.lateness;
    return terms;
}

std::int64_t VehicleRoutingIncrementalScoreCalculator::unassignedMinutes() const
{
    std::int64_t total = 0;
    for (int visitIndex : plan_->unassignedVisitIndices)
    {
        total += plan_->visits[static_cast<std::size_t>(visitIndex)]
                     .unassignedPenalty();
    }
    return total;
}

Score VehicleRoutingIncrementalScoreCalculator::currentScore() const
{
    return Score{-(overloadTotal_ + latenessTotal_),
                 -unassignedTotal_,
                 -drivingTotal_};
}

optimaru::HardSoftScore
VehicleRoutingIncrementalScoreCalculator::calculateScore()
{
    return toEngineScore(currentScore());
}

// ---------------------------------------------------------------------------
// Move previews
// ---------------------------------------------------------------------------

void VehicleRoutingIncrementalScoreCalculator::rebuildCache(int listIndex)
{
    const bool pool = listIndex >= vehicleCount_;
    const std::vector<int> &elements
        = pool ? plan_->unassignedVisitIndices
               : plan_->vehicles[static_cast<std::size_t>(listIndex)].visitIndices;
    ListCache &cache = caches_[static_cast<std::size_t>(listIndex)];
    const std::size_t n = elements.size();
    cache.cumLoad.assign(n + 1, 0);
    cache.cumService.assign(n + 1, 0);
    cache.cumPenalty.assign(n + 1, 0);
    cache.cumDist.assign(n + 1, 0);
    cache.cumRevDist.assign(n + 1, 0);
    cache.prefixMinEnd.assign(n + 1, std::numeric_limits<Seconds>::max());
    cache.suffixMinEnd.assign(n + 1, std::numeric_limits<Seconds>::max());
    cache.lateness = 0;
    const Seconds departure
        = pool ? 0 : plan_->vehicles[static_cast<std::size_t>(listIndex)].departureTime;
    int previousLocation
        = pool ? -1 : plan_->vehicles[static_cast<std::size_t>(listIndex)].homeLocationIndex;
    for (std::size_t k = 0; k < n; ++k)
    {
        const Visit &visit = plan_->visits[static_cast<std::size_t>(elements[k])];
        cache.cumLoad[k + 1] = cache.cumLoad[k] + visit.demand;
        cache.cumService[k + 1] = cache.cumService[k] + visit.serviceDuration;
        cache.cumPenalty[k + 1] = cache.cumPenalty[k] + visit.unassignedPenalty();
        cache.cumDist[k + 1]
            = cache.cumDist[k]
              + (previousLocation < 0
                     ? 0
                     : plan_->drivingTime(previousLocation, visit.locationIndex));
        cache.cumRevDist[k + 1]
            = cache.cumRevDist[k]
              + (k == 0 ? 0 : plan_->drivingTime(visit.locationIndex, previousLocation));
        cache.prefixMinEnd[k + 1] = std::min(cache.prefixMinEnd[k], visit.maxEndTime);
        if (!pool)
        {
            // No visit ever waits (noWaiting_), so arrival = departure +
            // driving so far + service so far: the same walk evaluateRoute
            // does, folded into the cache rebuild.
            const Seconds serviceFinished
                = departure + cache.cumDist[k + 1] + cache.cumService[k + 1];
            if (serviceFinished > visit.maxEndTime)
                cache.lateness += ceilToMinutes(serviceFinished - visit.maxEndTime);
        }
        previousLocation = visit.locationIndex;
    }
    for (std::size_t k = n; k-- > 0;)
    {
        cache.suffixMinEnd[k]
            = std::min(cache.suffixMinEnd[k + 1],
                       plan_->visits[static_cast<std::size_t>(elements[k])].maxEndTime);
    }
}

Seconds VehicleRoutingIncrementalScoreCalculator::minEndOf(const ListCache &cache,
                                                           const std::vector<int> &elements,
                                                           std::size_t begin,
                                                           std::size_t end) const
{
    if (begin == 0)
        return cache.prefixMinEnd[end];
    if (end == elements.size())
        return cache.suffixMinEnd[begin];
    // A middle piece (an intra-route swap or Or-opt) is scanned: exact, and
    // bounded by the route length, which is what the move already costs to
    // apply when the preview declines.
    Seconds result = std::numeric_limits<Seconds>::max();
    for (std::size_t k = begin; k < end; ++k)
        result = std::min(result, plan_->visits[static_cast<std::size_t>(elements[k])].maxEndTime);
    return result;
}

void VehicleRoutingIncrementalScoreCalculator::Run::insertAt(int index, const Piece &piece)
{
    // `index` counts elements across the run; split the piece it falls in.
    assert(count + 2 <= kCapacity);  // a split adds two pieces
    int offset = 0;
    for (int i = 0; i < count; ++i)
    {
        const int size = pieces[i].end - pieces[i].begin;
        if (index <= offset)
        {
            for (int j = count; j > i; --j)
                pieces[j] = pieces[j - 1];
            pieces[i] = piece;
            ++count;
            return;
        }
        if (index < offset + size)
        {
            const Piece head{pieces[i].list, pieces[i].begin,
                             pieces[i].begin + (index - offset), false};
            const Piece tail{pieces[i].list, head.end, pieces[i].end, false};
            for (int j = count + 1; j > i + 2; --j)
                pieces[j] = pieces[j - 2];
            pieces[i] = head;
            pieces[i + 1] = piece;
            pieces[i + 2] = tail;
            count += 2;
            return;
        }
        offset += size;
    }
    add(piece);
}

// A plan with waiting has no prefix-sum caches: write each run out as a visit
// sequence and walk it like evaluateRoute does. Always exact, never declines.
bool VehicleRoutingIncrementalScoreCalculator::previewRunsByWalk(
    const Run *runs, int runCount, optimaru::HardSoftScore &out) const
{
    std::int64_t overload = overloadTotal_;
    std::int64_t lateness = latenessTotal_;
    std::int64_t driving = drivingTotal_;
    std::int64_t unassigned = unassignedTotal_;
    for (int t = 0; t < runCount; ++t)
    {
        const Run &run = runs[t];
        walkScratch_.clear();
        for (int i = 0; i < run.count; ++i)
        {
            const Piece &piece = run.pieces[i];
            const std::vector<int> &elements = elementsOf(piece.list);
            if (piece.reversed)
            {
                for (int k = piece.end; k-- > piece.begin;)
                    walkScratch_.push_back(elements[static_cast<std::size_t>(k)]);
            }
            else
            {
                walkScratch_.insert(walkScratch_.end(), elements.begin() + piece.begin,
                                    elements.begin() + piece.end);
            }
        }
        if (run.list >= vehicleCount_)
        {
            std::int64_t penalty = 0;
            for (int visitIndex : walkScratch_)
                penalty += plan_->visits[static_cast<std::size_t>(visitIndex)].unassignedPenalty();
            unassigned += penalty - unassignedTotal_;
            continue;
        }
        const RouteTerms &old = terms_[static_cast<std::size_t>(run.list)];
        const RouteTerms fresh = evaluateElements(run.list, walkScratch_);
        overload += fresh.overload - old.overload;
        lateness += fresh.lateness - old.lateness;
        driving += fresh.driving - old.driving;
    }
    out = toEngineScore(Score{-(overload + lateness), -unassigned, -driving});
    return true;
}

// Score the solution with each run's list replaced by the run's pieces.
bool VehicleRoutingIncrementalScoreCalculator::previewRuns(const Run *runs, int runCount,
                                                           optimaru::HardSoftScore &out) const
{
    if (!noWaiting_)
        return previewRunsByWalk(runs, runCount, out);
    std::int64_t overload = overloadTotal_;
    std::int64_t lateness = latenessTotal_;
    std::int64_t driving = drivingTotal_;
    std::int64_t unassigned = unassignedTotal_;
    for (int t = 0; t < runCount; ++t)
    {
        const Run &run = runs[t];
        const int list = run.list;
        if (list >= vehicleCount_)
        {
            // The pool: its whole contribution is the unassigned penalty.
            std::int64_t penalty = 0;
            for (int i = 0; i < run.count; ++i)
            {
                const Piece &piece = run.pieces[i];
                const ListCache &c = caches_[static_cast<std::size_t>(piece.list)];
                penalty += c.cumPenalty[static_cast<std::size_t>(piece.end)]
                           - c.cumPenalty[static_cast<std::size_t>(piece.begin)];
            }
            unassigned += penalty - caches_[static_cast<std::size_t>(list)].cumPenalty.back();
            continue;
        }
        const Vehicle &vehicle = plan_->vehicles[static_cast<std::size_t>(list)];
        const RouteTerms &old = terms_[static_cast<std::size_t>(list)];
        overload -= old.overload;
        lateness -= old.lateness;
        driving -= old.driving;

        std::int64_t load = 0, service = 0, drive = 0;
        Seconds minMaxEnd = std::numeric_limits<Seconds>::max();
        int previousLocation = vehicle.homeLocationIndex;
        bool any = false;
        for (int i = 0; i < run.count; ++i)
        {
            const Piece &piece = run.pieces[i];
            if (piece.end <= piece.begin)
                continue;
            const ListCache &c = caches_[static_cast<std::size_t>(piece.list)];
            const std::vector<int> &elements = elementsOf(piece.list);
            const std::size_t b = static_cast<std::size_t>(piece.begin);
            const std::size_t e = static_cast<std::size_t>(piece.end);
            load += c.cumLoad[e] - c.cumLoad[b];
            service += c.cumService[e] - c.cumService[b];
            minMaxEnd = std::min(minMaxEnd, minEndOf(c, elements, b, e));
            const int firstElement = elements[piece.reversed ? e - 1 : b];
            const int lastElement = elements[piece.reversed ? b : e - 1];
            const int firstLocation
                = plan_->visits[static_cast<std::size_t>(firstElement)].locationIndex;
            drive += plan_->drivingTime(previousLocation, firstLocation);
            drive += piece.reversed ? c.cumRevDist[e] - c.cumRevDist[b + 1]
                                    : c.cumDist[e] - c.cumDist[b + 1];
            previousLocation
                = plan_->visits[static_cast<std::size_t>(lastElement)].locationIndex;
            any = true;
        }
        if (any)
        {
            // Lateness is exact from the sums only as "none": every arrival +
            // service ends before the earliest deadline the route's visits
            // carry. Otherwise the candidate route is walked (O(route), still
            // exact) rather than the preview declined.
            const Seconds finish = vehicle.departureTime + drive + service;
            if (finish > minMaxEnd)
                return previewRunsByWalk(runs, runCount, out);
            drive += vehicle.homeLeg(plan_->drivingSeconds, previousLocation);
            if (load > vehicle.capacity)
                overload += load - vehicle.capacity;
            driving += drive;
        }
    }
    out = toEngineScore(Score{-(overload + lateness), -unassigned, -driving});
    return true;
}

bool VehicleRoutingIncrementalScoreCalculator::previewSegmentMove(
    int fromList, int start, int len, int toList, int insertAt, optimaru::HardSoftScore &out)
{
    if (!previewsEnabled() || len <= 0)
        return false;
    const int fromSize = listSize(fromList);
    const Piece segment{fromList, start, start + len, false};
    Run runs[2];
    runs[0].list = fromList;
    runs[0].add({fromList, 0, start, false});
    runs[0].add({fromList, start + len, fromSize, false});
    if (fromList == toList)
    {
        runs[0].insertAt(insertAt, segment);
        return previewRuns(runs, 1, out);
    }
    runs[1].list = toList;
    runs[1].add({toList, 0, insertAt, false});
    runs[1].add(segment);
    runs[1].add({toList, insertAt, listSize(toList), false});
    return previewRuns(runs, 2, out);
}

bool VehicleRoutingIncrementalScoreCalculator::previewSwap(int l1, int p1, int l2, int p2,
                                                           optimaru::HardSoftScore &out)
{
    if (!previewsEnabled())
        return false;
    Run runs[2];
    if (l1 == l2)
    {
        if (p1 == p2)
        {
            out = calculateScore();
            return true;
        }
        const int a = std::min(p1, p2), b = std::max(p1, p2);
        runs[0].list = l1;
        runs[0].add({l1, 0, a, false});
        runs[0].add({l1, b, b + 1, false});
        runs[0].add({l1, a + 1, b, false});
        runs[0].add({l1, a, a + 1, false});
        runs[0].add({l1, b + 1, listSize(l1), false});
        return previewRuns(runs, 1, out);
    }
    runs[0].list = l1;
    runs[0].add({l1, 0, p1, false});
    runs[0].add({l2, p2, p2 + 1, false});
    runs[0].add({l1, p1 + 1, listSize(l1), false});
    runs[1].list = l2;
    runs[1].add({l2, 0, p2, false});
    runs[1].add({l1, p1, p1 + 1, false});
    runs[1].add({l2, p2 + 1, listSize(l2), false});
    return previewRuns(runs, 2, out);
}

bool VehicleRoutingIncrementalScoreCalculator::previewReverse(int list, int first, int last,
                                                              optimaru::HardSoftScore &out)
{
    if (!previewsEnabled())
        return false;
    Run run;
    run.list = list;
    run.add({list, 0, first, false});
    run.add({list, first, last + 1, true});
    run.add({list, last + 1, listSize(list), false});
    return previewRuns(&run, 1, out);
}

bool VehicleRoutingIncrementalScoreCalculator::previewTailSwap(int l1, int c1, int l2, int c2,
                                                               optimaru::HardSoftScore &out)
{
    if (!previewsEnabled())
        return false;
    Run runs[2];
    runs[0].list = l1;
    runs[0].add({l1, 0, c1, false});
    runs[0].add({l2, c2, listSize(l2), false});
    runs[1].list = l2;
    runs[1].add({l2, 0, c2, false});
    runs[1].add({l1, c1, listSize(l1), false});
    return previewRuns(runs, 2, out);
}

bool VehicleRoutingIncrementalScoreCalculator::previewExchange(int l1, int p1, int len1, int l2,
                                                               int p2, int len2,
                                                               optimaru::HardSoftScore &out)
{
    if (!previewsEnabled())
        return false;
    Run runs[2];
    runs[0].list = l1;
    runs[0].add({l1, 0, p1, false});
    runs[0].add({l2, p2, p2 + len2, false});
    runs[0].add({l1, p1 + len1, listSize(l1), false});
    runs[1].list = l2;
    runs[1].add({l2, 0, p2, false});
    runs[1].add({l1, p1, p1 + len1, false});
    runs[1].add({l2, p2 + len2, listSize(l2), false});
    return previewRuns(runs, 2, out);
}

// Walk the route with the element in: the waiting-plan path and the fallback
// of the cached path when the candidate route may be late.
bool VehicleRoutingIncrementalScoreCalculator::previewInsertByWalk(
    int list, int pos, int element, optimaru::HardSoftScore &out) const
{
    walkScratch_ = plan_->vehicles[static_cast<std::size_t>(list)].visitIndices;
    walkScratch_.insert(walkScratch_.begin() + pos, element);
    const RouteTerms &old = terms_[static_cast<std::size_t>(list)];
    const RouteTerms fresh = evaluateElements(list, walkScratch_);
    out = toEngineScore(Score{-(overloadTotal_ - old.overload + fresh.overload + latenessTotal_
                                - old.lateness + fresh.lateness),
                              -unassignedTotal_, -(drivingTotal_ - old.driving + fresh.driving)});
    return true;
}

bool VehicleRoutingIncrementalScoreCalculator::previewInsert(int list, int pos, int element,
                                                             optimaru::HardSoftScore &out)
{
    if (!previewsEnabled())
        return false;
    // The repair heuristics call this once per candidate slot, so it is the
    // hottest preview; a direct delta beats composing a three-piece run.
    const Visit &visit = plan_->visits[static_cast<std::size_t>(element)];
    if (list >= vehicleCount_)
    {
        out = toEngineScore(Score{-(overloadTotal_ + latenessTotal_),
                                  -(unassignedTotal_ + visit.unassignedPenalty()),
                                  -drivingTotal_});
        return true;
    }
    const Vehicle &vehicle = plan_->vehicles[static_cast<std::size_t>(list)];
    const std::vector<int> &elements = vehicle.visitIndices;
    if (!noWaiting_)  // no cache on a plan with waiting
        return previewInsertByWalk(list, pos, element, out);
    const ListCache &c = caches_[static_cast<std::size_t>(list)];
    const std::size_t size = elements.size();
    const std::size_t at = static_cast<std::size_t>(pos);
    auto locationOf = [&](std::size_t k)
    { return plan_->visits[static_cast<std::size_t>(elements[k])].locationIndex; };
    const int previousLocation = at == 0 ? vehicle.homeLocationIndex : locationOf(at - 1);

    std::int64_t drive = c.cumDist[size] + plan_->drivingTime(previousLocation, visit.locationIndex);
    int lastLocation;
    if (at < size)
    {
        const int nextLocation = locationOf(at);
        drive += plan_->drivingTime(visit.locationIndex, nextLocation)
                 - plan_->drivingTime(previousLocation, nextLocation);
        lastLocation = locationOf(size - 1);
    }
    else
    {
        lastLocation = visit.locationIndex;
    }
    const std::int64_t load = c.cumLoad[size] + visit.demand;
    const std::int64_t service = c.cumService[size] + visit.serviceDuration;
    const Seconds minMaxEnd = std::min(c.prefixMinEnd[size], visit.maxEndTime);
    if (vehicle.departureTime + drive + service > minMaxEnd)  // may be late: walk it
        return previewInsertByWalk(list, pos, element, out);
    drive += vehicle.homeLeg(plan_->drivingSeconds, lastLocation);

    // The check above established the new route has no lateness, so the old
    // route's lateness (non-zero only for a non-metric matrix) goes too.
    const RouteTerms &old = terms_[static_cast<std::size_t>(list)];
    const std::int64_t overload
        = overloadTotal_ - old.overload + std::max<std::int64_t>(0, load - vehicle.capacity);
    out = toEngineScore(Score{-(overload + latenessTotal_ - old.lateness), -unassignedTotal_,
                              -(drivingTotal_ - old.driving + drive)});
    return true;
}

}  // namespace vrp
