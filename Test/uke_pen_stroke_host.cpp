// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../UkePenStroke.h"
#include <cassert>

int main() {
    using namespace uke_pen;
    const auto topRight = toPanel(0, 31999, 21359, 31999);
    const auto bottomLeft = toPanel(21359, 0, 21359, 31999);
    assert(topRight.x == hidAxisMax && topRight.y == hidAxisMax);
    assert(bottomLeft.x == 0 && bottomLeft.y == 0);
    const auto downStart = toPanel(10679, 1000, 21359, 31999);
    const auto downEnd = toPanel(10679, 2000, 21359, 31999);
    assert(downEnd.x > downStart.x && downStart.y == downEnd.y);

    constexpr uint64_t ms = 1000000;
    assert(positionAgeLimit(false, true) == 100 * ms);
    assert(positionAgeLimit(true, false) == 150 * ms);
    assert(positionAgeLimit(true, true) == 500 * ms);
    const auto hover = panelTip(0, 1);
    const auto contact = panelTip(0x10, 0);
    assert(hover == PanelTip::hover && contact == PanelTip::contact);
    assert(panelTip(0x10, 1) == PanelTip::unknown);
    assert(panelTip(0x10, 2) == PanelTip::unknown);
    assert(panelTip(0, 0) == PanelTip::unknown);
    PanelContact state;
    assert(state.update(hover, 100 * ms, true, 0, 1200) == 0);
    assert(state.update(contact, 104 * ms, false, 0, 0) == 0);
    assert(state.fresh(104 * ms));
    assert(state.update(contact, 108 * ms, true, 0, 16383) == 16383);
    assert(state.update(contact, 120 * ms, true, 0, 0) == 16383);
    assert(state.update(contact, 227 * ms, false, 0, 0) == 16383);
    assert(state.update(contact, 228 * ms, false, 0, 0) == 16383);
    assert(state.update(contact, 347 * ms, false, 0, 0) == 16383);
    assert(state.update(contact, 348 * ms, false, 0, 0) == 0);
    assert(state.fresh(348 * ms));
    assert(state.update(contact, 350 * ms, true, 0, 8200) == 8200);
    assert(state.update(hover, 354 * ms, true, 4 * ms, 8200) == 0);
    assert(!state.fresh(354 * ms));
    assert(state.update(contact, 358 * ms, true, 8 * ms, 8200) == 0);
    assert(state.update(contact, 362 * ms, true, 0, 250) == 250);
    assert(state.fresh(411 * ms) && !state.fresh(412 * ms));
    state = {};
    assert(state.update(contact, 420 * ms, false, 0, 0) == 0);
    assert(state.update(hover, 430 * ms, false, 0, 0) == 0);
    assert(state.update(hover, 434 * ms, true, 2 * ms, 750) == 0);
    assert(state.update(contact, 438 * ms, true, 6 * ms, 750) == 750);
    assert(state.update(hover, 450 * ms, true, 0, 750) == 0);
    assert(state.update(contact, 465 * ms, true, 9 * ms, 750) == 0);
    assert(state.update(contact, 469 * ms, true, 12 * ms, 830) == 830);
    PanelContact onset;
    assert(onset.update(contact, 500 * ms, false, 0, 0) == 0);
    assert(onset.startForce(500 * ms, 517 * ms, 900) == 0);
    assert(onset.startForce(500 * ms, 491 * ms, 900) == 0);
    assert(onset.startForce(500 * ms, 514 * ms, 900) == 900);
    assert(onset.update(contact, 504 * ms, false, 0, 0) == 900);
    assert(onset.update(contact, 518 * ms, true, 0, 1200) == 1200);
    assert(onset.update(hover, 522 * ms, false, 0, 0) == 0);
    assert(onset.update(contact, 526 * ms, false, 0, 0) == 0);
    assert(onset.startForce(526 * ms, 522 * ms, 1200) == 0);
    PressureHistory history;
    history.append(594 * ms, 819);
    history.append(603 * ms, 1326);
    history.append(611 * ms, 0);
    assert(history.forOnset(600 * ms, 580 * ms).force == 1326);
    assert(history.forOnset(614 * ms, 580 * ms).force == 0);
    assert(history.forOnset(600 * ms, 605 * ms).force == 0);
    history = {};
    history.append(619 * ms, 1777);
    assert(history.forOnset(600 * ms, 580 * ms).force == 0);
    for (unsigned i = 0; i <= PressureHistory::capacity; ++i)
        history.append((700 + i) * ms, static_cast<uint16_t>(1000 + i));
    assert(history.count == PressureHistory::capacity);
    assert(history.forOnset(728 * ms, 710 * ms).force == 1028);

    assert(atPanelEdge({376, 31864}));
    assert(atPanelEdge({32300, 32004}));
    assert(!atPanelEdge({16384, 16384}));
    const auto leftCorner = calibratePanelEdges({441, 32176});
    const auto rightCorner = calibratePanelEdges({32336, 32248});
    assert(leftCorner.x * 1600 < 11 * hidAxisMax);
    assert((hidAxisMax - rightCorner.x) * 1600 < 11 * hidAxisMax);
    assert((hidAxisMax - leftCorner.y) * 1068 < 11 * hidAxisMax);
    assert((hidAxisMax - rightCorner.y) * 1068 < 11 * hidAxisMax);
    const unsigned panelAxes[] = {3200, 2136};
    for (unsigned width : panelAxes) {
        assert(calibrateEdgeAxis(0, width) == 0);
        assert(calibrateEdgeAxis(hidAxisMax, width) == hidAxisMax);
        const unsigned limit = 96 * hidAxisMax / width;
        assert(calibrateEdgeAxis(limit, width) == limit);
        assert(calibrateEdgeAxis(16384, width) == 16384);
        for (unsigned x = 1; x <= hidAxisMax; ++x)
            assert(calibrateEdgeAxis(x - 1, width) <= calibrateEdgeAxis(x, width));
    }
    const auto leftEdge = calibrateCsotEdges({253, 16384});
    const auto rightEdge = calibrateCsotEdges({32487, 16384});
    const auto topEdge = calibrateCsotEdges({16384, 490});
    const auto bottomEdge = calibrateCsotEdges({16384, 32231});
    assert(leftEdge.x == 0 && leftEdge.y == 16384);
    assert(rightEdge.x == hidAxisMax && rightEdge.y == 16384);
    assert(topEdge.x == 16384 && topEdge.y == 0);
    assert(bottomEdge.x == 16384 && bottomEdge.y == hidAxisMax);

    // Bound the residual for the eight measured coordinate intervals.
    // The lower edge has the largest spread and is still within 3 points.
    assert(calibrateCsotEdges({270, 14638}).x * 1600 < 2 * hidAxisMax);
    assert(calibrateCsotEdges({237, 19582}).x == 0);
    assert(calibrateCsotEdges({32497, 10603}).x == hidAxisMax);
    assert((hidAxisMax - calibrateCsotEdges({32477, 18900}).x) * 1600 < 2 * hidAxisMax);
    assert(calibrateCsotEdges({14143, 493}).y * 1068 < hidAxisMax);
    assert(calibrateCsotEdges({17790, 488}).y == 0);
    assert(calibrateCsotEdges({13676, 32274}).y == hidAxisMax);
    assert((hidAxisMax - calibrateCsotEdges({17335, 32187}).y) * 1068 < 3 * hidAxisMax);
    const auto center = calibrateCsotEdges({16384, 16384});
    assert(center.x == 16384 && center.y == 16384);
    const PanelPoint straightStarts[] = {{253, 16384}, {32487, 16384}, {16384, 490}, {16384, 32231}};
    for (auto point : straightStarts) {
        const auto mapped = cornerCorrection(point, calibrateCsotEdges(point));
        assert(mapped.x == point.x && mapped.y == point.y);
    }
    const PanelPoint corner{447, 32150};
    const auto cornerMapped = cornerCorrection(corner, calibrateCsotEdges(corner));
    assert(cornerMapped.x == calibrateCsotEdges(corner).x && cornerMapped.y == calibrateCsotEdges(corner).y);
    const auto partial = cornerCorrection({253, 1100}, calibrateCsotEdges({253, 1100}));
    assert(partial.x > 0 && partial.x < 253);
    const auto beyond = cornerCorrection({253, 1472}, calibrateCsotEdges({253, 1472}));
    assert(beyond.x == 253);
    const uint16_t samplePositions[] = {0, 500, 16384, 32000, hidAxisMax};
    for (uint16_t along : samplePositions) {
        for (uint16_t value = 1; value <= hidAxisMax; ++value) {
            const auto before = static_cast<uint16_t>(value - 1);
            assert(calibrateCsotEdges({before, along}).x <= calibrateCsotEdges({value, along}).x);
            assert(calibrateCsotEdges({along, before}).y <= calibrateCsotEdges({along, value}).y);
        }
    }
    EdgeContact edge;
    assert(!edge.deferLift(100 * ms, true, false));
    edge.startedNs = 100 * ms;
    assert(!edge.deferLift(125 * ms, false, false));
    assert(edge.deferLift(125 * ms, true, false));
    assert(edge.deferLift(150 * ms, true, false));
    assert(!edge.deferLift(151 * ms, true, true));
    assert(!edge.liftDue(156 * ms));
    assert(edge.liftDue(157 * ms));
    assert(!edge.deferLift(157 * ms, true, false));
}
