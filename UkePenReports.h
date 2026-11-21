// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef UKE_PEN_REPORTS_H
#define UKE_PEN_REPORTS_H

#include <stdint.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include "UkePenStroke.h"

struct piano_pen_core;

class UkePenReports {
public:
    explicit UkePenReports(piano_pen_core *decoder);
    ~UkePenReports();
    // Returns true when the current HID connection changes.
    bool poll();
    bool confirmedLift(uint64_t nowNs, uint64_t *sampleNs) const;
    uke_pen::PressureHistory::Report onsetPressure(uint64_t onsetNs, uint64_t liftNs) const {
        return pressureHistory_.forOnset(onsetNs, liftNs);
    }
    uint64_t latestPressureTime() const {
        return pressureHistory_.count
            ? pressureHistory_.reports[(pressureHistory_.next + uke_pen::PressureHistory::capacity - 1) %
                                       uke_pen::PressureHistory::capacity].ns : 0;
    }
    bool connected() const { return connected_; }

private:
    bool dispatchGesture(uint32_t kind, uint32_t flavor, uint32_t phase);
    void acceptBodyGesture(uint8_t value);
    void acceptReport(unsigned id, const uint8_t *, size_t length, uint64_t ticks, uint64_t ns);
    void openRawDevice(uint64_t registryId);
    void closeRawDevice();
    static void receiveRaw(void *, IOReturn, void *, uint32_t, uint32_t, uint8_t *, CFIndex, uint64_t);

    piano_pen_core *decoder_;
    const void *gestureClient_ = nullptr;
    CFTypeRef rawDevice_ = nullptr;
    mach_timebase_info_data_t timebase_{};
    uint8_t rawBuffer_[21]{};
    uint64_t seenTicks_[3]{};
    unsigned rawLoggedMask_ = 0;
    uint64_t senderId_ = 0;
    uint64_t squeezeDeadlineNs_ = 0;
    uint64_t nextSnapshotNs_ = 0;
    uint64_t lastPressureNs_ = 0;
    uint16_t lastPressure_ = 0;
    uke_pen::PressureHistory pressureHistory_;
    uint32_t generation_ = 0;
    bool connected_ = false;
    bool failed_ = false;
    bool squeezeDown_ = false;
    bool gestureFailed_ = false;
    unsigned loggedReports_[3]{};
};

#endif
