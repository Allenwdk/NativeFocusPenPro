// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef UKE_PEN_STROKE_H
#define UKE_PEN_STROKE_H

#include <stdint.h>

namespace uke_pen {
// Measured force may bridge one missed BLE update within a panel contact.
constexpr uint64_t pressureMaxAgeNs = 240000000;
constexpr uint64_t pressureInputMaxAgeNs = 120000000;
constexpr uint64_t pressureLeadNs = 8000000;
constexpr uint64_t pressureStartWaitNs = 16000000;
constexpr uint64_t liftConfirmNs = 16000000;
constexpr uint64_t contactEvidenceAgeNs = 50000000;
constexpr uint64_t edgeLiftConfirmNs = 32000000;
constexpr uint64_t hoverPositionAgeNs = 100000000;
constexpr uint64_t pressedPositionAgeNs = 150000000;
constexpr uint64_t maxPressedPositionAgeNs = 500000000;
constexpr unsigned hidAxisMax = 32767;

struct PanelPoint { uint16_t x, y; };

inline uint16_t blendCornerAxis(unsigned coordinate, unsigned corrected,
                                unsigned perpendicular, unsigned panelPixels) {
    const unsigned near = 48 * hidAxisMax / panelPixels;
    const unsigned limit = 96 * hidAxisMax / panelPixels;
    const unsigned distance = perpendicular > hidAxisMax / 2 ? hidAxisMax - perpendicular : perpendicular;
    if (distance >= limit) return static_cast<uint16_t>(coordinate);
    if (distance <= near) return static_cast<uint16_t>(corrected);
    const unsigned span = limit - near;
    return static_cast<uint16_t>((coordinate * (distance - near) + corrected * (limit - distance)) / span);
}

inline PanelPoint cornerCorrection(PanelPoint point, PanelPoint corrected) {
    return {blendCornerAxis(point.x, corrected.x, point.y, 2136),
            blendCornerAxis(point.y, corrected.y, point.x, 3200)};
}

inline uint64_t positionAgeLimit(bool tipDown, bool contactFresh) {
    if (!tipDown) return hoverPositionAgeNs;
    return contactFresh ? maxPressedPositionAgeNs : pressedPositionAgeNs;
}

// The Novatek solver uses portrait coordinates. The integrated digitizer
// reports panel axes, as the Piano input owner does. BackBoard handles UI rotation.
inline PanelPoint toPanel(int32_t x, int32_t y, int32_t xMax, int32_t yMax) {
    return {static_cast<uint16_t>(y * static_cast<int64_t>(hidAxisMax) / yMax),
            static_cast<uint16_t>((xMax - x) * static_cast<int64_t>(hidAxisMax) / xMax)};
}

// Uke's outer electrodes report physical corner contacts inward. Adjust only
// the outer 96 panel pixels and join the interior mapping without a step.
inline uint16_t calibrateEdgeAxis(uint16_t coordinate, unsigned panelPixels) {
    const unsigned near = 48 * hidAxisMax / panelPixels;
    const unsigned limit = 96 * hidAxisMax / panelPixels;
    const unsigned target = near * 2 / 5;
    const bool farSide = coordinate > hidAxisMax / 2;
    const unsigned distance = farSide ? hidAxisMax - coordinate : coordinate;
    if (distance >= limit) return coordinate;
    const unsigned adjusted = distance <= near
        ? distance * target / near
        : target + (distance - near) * (limit - target) / (limit - near);
    return static_cast<uint16_t>(farSide ? hidAxisMax - adjusted : adjusted);
}

inline PanelPoint calibratePanelEdges(PanelPoint point) {
    return {calibrateEdgeAxis(point.x, 3200), calibrateEdgeAxis(point.y, 2136)};
}

inline uint16_t calibrateMeasuredEdge(unsigned coordinate, unsigned panelPixels,
                                      unsigned nearInset, unsigned farInset) {
    const bool farSide = coordinate > hidAxisMax / 2;
    const unsigned distance = farSide ? hidAxisMax - coordinate : coordinate;
    const unsigned limit = 96 * hidAxisMax / panelPixels;
    if (distance >= limit) return static_cast<uint16_t>(coordinate);
    const unsigned inset = farSide ? farInset : nearInset;
    const unsigned adjusted = distance <= inset ? 0 : (distance - inset) * limit / (limit - inset);
    return static_cast<uint16_t>(farSide ? hidAxisMax - adjusted : adjusted);
}

inline PanelPoint calibrateCsotEdges(PanelPoint point) {
    // Mean nominal HID inset of two inside-out strokes through each
    // straight edge, measured at the final position before range loss.
    constexpr unsigned left = 253, right = 280, top = 490, bottom = 536;
    return {calibrateMeasuredEdge(point.x, 3200, left, right),
            calibrateMeasuredEdge(point.y, 2136, top, bottom)};
}

inline bool atPanelEdge(PanelPoint point) {
    // Match the digitizer's existing FromEdgeTip region.
    constexpr unsigned edgeX = 48 * hidAxisMax / 3048;
    constexpr unsigned edgeY = 48 * hidAxisMax / 2032;
    return point.x <= edgeX || point.x >= hidAxisMax - edgeX ||
           point.y <= edgeY || point.y >= hidAxisMax - edgeY;
}

struct EdgeContact {
    uint64_t startedNs = 0;
    uint64_t hoverNs = 0;

    bool liftDue(uint64_t now) const {
        return hoverNs && now >= hoverNs && now - hoverNs >= edgeLiftConfirmNs;
    }
    bool deferLift(uint64_t now, bool positivePressure, bool confirmedZero) {
        if (!startedNs || confirmedZero || (!hoverNs && !positivePressure)) return false;
        if (!hoverNs) hoverNs = now;
        return now >= hoverNs && !liftDue(now);
    }
};

enum class PanelTip { unknown, hover, contact };

struct PressureHistory {
    struct Report { uint64_t ns; uint16_t force; };
    static constexpr unsigned capacity = 32;
    Report reports[capacity]{};
    unsigned next = 0;
    unsigned count = 0;

    void append(uint64_t ns, uint16_t force) {
        reports[next] = {ns, force};
        next = (next + 1) % capacity;
        if (count < capacity) ++count;
    }

    Report forOnset(uint64_t onset, uint64_t lift) const {
        Report closest{};
        uint64_t best = pressureStartWaitNs + pressureLeadNs + 1;
        uint64_t latestZeroBeforeOnset = lift;
        for (unsigned i = 0; i < count; ++i) {
            const auto &r = reports[(next + capacity - 1 - i) % capacity];
            if (r.ns <= lift || (r.ns < onset && onset - r.ns > pressureLeadNs)) break;
            if (r.ns > onset && r.ns - onset > pressureStartWaitNs) continue;
            if (!r.force) {
                if (r.ns <= onset && r.ns > latestZeroBeforeOnset) latestZeroBeforeOnset = r.ns;
                continue;
            }
            if (r.ns <= latestZeroBeforeOnset) break;
            const uint64_t distance = r.ns >= onset ? r.ns - onset : onset - r.ns;
            if (distance < best) {
                closest = r;
                best = distance;
            }
        }
        return closest;
    }
};

// Both values come from the validated NT36532 type 29 frame. The first
// identifies the tip state on Uke; its value is independent of BLE force.
inline PanelTip panelTip(uint32_t marker, uint8_t hover) {
    if (marker == 0 && hover == 1) return PanelTip::hover;
    if (marker == 0x10 && hover == 0) return PanelTip::contact;
    return PanelTip::unknown;
}

struct PanelContact {
    uint64_t contactNs = 0;
    uint64_t hoverNs = 0;
    uint64_t startNs = 0;
    uint64_t liftNs = 0;
    uint64_t forceNs = 0;
    uint16_t force = 0;

    bool fresh(uint64_t now) const {
        return contactNs && now >= contactNs && now - contactNs < contactEvidenceAgeNs;
    }

    uint16_t startForce(uint64_t frameNs, uint64_t sampleNs, uint16_t sample) {
        if (!sample || sampleNs <= liftNs ||
            (sampleNs < startNs && startNs - sampleNs > pressureLeadNs) ||
            (sampleNs > frameNs && sampleNs - frameNs > pressureStartWaitNs)) return 0;
        forceNs = sampleNs;
        force = sample;
        return force;
    }

    uint16_t update(PanelTip tip, uint64_t frameNs, bool valid, uint64_t ageNs, uint16_t sample) {
        if (tip == PanelTip::hover) {
            if (contactNs) liftNs = frameNs;
            contactNs = forceNs = 0;
            hoverNs = frameNs;
            force = 0;
            return 0;
        }
        if (tip != PanelTip::contact) return 0;
        if (!contactNs) startNs = frameNs;
        contactNs = frameNs;
        if (valid && sample && ageNs < frameNs && ageNs < pressureInputMaxAgeNs) {
            const uint64_t sampleNs = frameNs - ageNs;
            if (sampleNs > liftNs && sampleNs + pressureLeadNs >= startNs && sampleNs >= forceNs) {
                forceNs = sampleNs;
                force = sample;
            }
        }
        if (!forceNs || (frameNs >= forceNs && frameNs - forceNs >= pressureMaxAgeNs)) return 0;
        return force;
    }
};
}
#endif
