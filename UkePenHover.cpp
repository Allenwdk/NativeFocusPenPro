// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "UkePenFrames.h"
#include "UkePenReports.h"
#include "UkePenStroke.h"
#include "piano-pen-core.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <sys/sysctl.h>

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unistd.h>

namespace {
constexpr uint64_t staleNs = uke_pen::hoverPositionAgeNs;
volatile std::sig_atomic_t stopped = 0;

void stop(int) { stopped = 1; }

uint64_t continuousNs(const mach_timebase_info_data_t &timebase) {
    return static_cast<uint64_t>(static_cast<__uint128_t>(mach_continuous_time()) * timebase.numer / timebase.denom);
}

int storedCornerPreference(CFStringRef key) {
    // -1 is the system's explicit Off action.
    int value = INT32_MIN;
    const auto property = CFPreferencesCopyValue(
        key, CFSTR("com.apple.springboard"), CFSTR("mobile"), kCFPreferencesAnyHost);
    if (!property) return value;
    if (CFGetTypeID(property) == CFNumberGetTypeID())
        CFNumberGetValue(static_cast<CFNumberRef>(property), kCFNumberIntType, &value);
    CFRelease(property);
    return value;
}

void logStoredCornerPreferences(void *) {
    os_log(OS_LOG_DEFAULT, "UKE PEN stored corner preferences left=%d right=%d left_touches=%d right_touches=%d",
           storedCornerPreference(CFSTR("SBBottomLeftCornerGestureFeature")),
           storedCornerPreference(CFSTR("SBBottomRightCornerGestureFeature")),
           storedCornerPreference(CFSTR("SBBottomLeftCornerGestureTouchTypes")),
           storedCornerPreference(CFSTR("SBBottomRightCornerGestureTouchTypes")));
}

struct PenOutput {
    UkePenHoverReport report{};
    bool inRange = false;
    unsigned events = 0;
    unsigned submitted = 0, moved = 0, failed = 0;
    uint16_t lastX = 0, lastY = 0;
    bool lastTipDown = false;

    bool tipDown() const { return report.tip_state == UKE_PEN_TIP_CONTACT; }

    bool publish(bool range) {
        report.in_range = range;
        if (!range) {
            report.tip_state = UKE_PEN_TIP_HOVER;
            report.pressure = 0;
            report.tilt_x = report.tilt_y = 0;
        }
        const int result = sysctlbyname(UKE_PEN_HOVER_SYSCTL, nullptr, nullptr, &report, sizeof(report)) ? errno : 0;
        if (events++ < 8 || inRange != range || lastTipDown != tipDown() || result) {
            if (!result || events <= 8) {
                os_log(OS_LOG_DEFAULT, "UKE PEN native report errno=%d range=%d x=%u y=%u pressure=%u tip=%d",
                       result, range, report.x, report.y, report.pressure, tipDown());
            }
        }
        if (!result) {
            ++submitted;
            if (range && (!inRange || report.x != lastX || report.y != lastY)) ++moved;
            lastX = report.x;
            lastY = report.y;
            inRange = range;
            lastTipDown = tipDown();
        } else {
            ++failed;
        }
        return !result;
    }

    ~PenOutput() {
        if (inRange) publish(false);
    }
};

bool validRecord(const UkePenFrame &frame, size_t length) {
    return frame.version == UKE_PEN_FRAME_ABI &&
           frame.length >= UKE_PEN_SPI_PREFIX + 100 && frame.length <= sizeof frame.bytes &&
           length == offsetof(UkePenFrame, bytes) + frame.length;
}
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    char error[160]{};
    std::unique_ptr<piano_pen_core, decltype(&piano_pen_core_destroy)> owned(
        piano_pen_core_create(argv[1], 3, PIANO_PEN_PORTRAIT, uke_pen::pressureInputMaxAgeNs, error, sizeof error),
        piano_pen_core_destroy);
    auto *core = owned.get();
    if (!core) {
        os_log_error(OS_LOG_DEFAULT, "UKE PEN native calibration rejected %{public}s", error);
        return 1;
    }
    piano_pen_info info{};
    mach_timebase_info_data_t timebase{};
    if (piano_pen_core_info(core, &info) || !info.x_max || !info.y_max ||
        mach_timebase_info(&timebase) != KERN_SUCCESS || !timebase.denom) {
        return 1;
    }

    os_log(OS_LOG_DEFAULT, "UKE PEN native worker ready bounds=%d/%d panel axes rotated",
           info.x_max, info.y_max);
    dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), nullptr, logStoredCornerPreferences);
    UkePenReports reports(core);
    PenOutput output;
    uke_pen::PanelContact contact;
    uke_pen::EdgeContact edgeContact;
    auto lastTip = uke_pen::PanelTip::unknown;
    uint64_t lastSequence = 0, lastValid = 0;
    uint32_t generation = 0;
    unsigned rejects = 0, tipEdges = 0, positionLosses = 0;
    unsigned interruptedEdges = 0;
    unsigned onsetPressureLogs = 0;
    uint64_t nextCornerReadNs = 0;
    unsigned reads = 0, coordinates = 0, unmeasured = 0, forceExpired = 0, cadenceLogs = 0, dropped = 0;
    uint64_t cadenceStart = 0, lastCoordinate = 0, longestGap = 0, maxQueueAge = 0, maxPressureAge = 0;
    const auto releaseTip = [&](uint64_t now, uint64_t liftNs) {
        contact.update(uke_pen::PanelTip::hover, liftNs, false, 0, 0);
        edgeContact = {};
        output.report.tip_state = UKE_PEN_TIP_HOVER;
        output.report.pressure = 0;
        const bool released = lastValid && now >= lastValid && now - lastValid < staleNs && output.publish(true);
        if (!released && output.inRange) output.publish(false);
    };
    const auto expirePosition = [&](uint64_t now) {
        const uint64_t limit = uke_pen::positionAgeLimit(output.tipDown(), contact.fresh(now));
        if (!lastValid || now < lastValid || now - lastValid < limit) return;
        if (output.inRange && positionLosses++ < 16)
            os_log(OS_LOG_DEFAULT, "UKE PEN position expired age_ns=%llu pressure=%u",
                   (unsigned long long)(now - lastValid), output.report.pressure);
        if (output.inRange) output.publish(false);
        contact = {};
        edgeContact = {};
        lastTip = uke_pen::PanelTip::unknown;
        uke_pen_core_reset_position(core);
        lastValid = 0;
    };
    while (!stopped) {
        if (reports.poll()) {
            if (output.inRange) output.publish(false);
            contact = {};
            edgeContact = {};
            lastTip = uke_pen::PanelTip::unknown;
            lastValid = lastSequence = 0;
        }
        const uint64_t now = continuousNs(timebase);
        if (cadenceLogs < 24 && cadenceStart && now - cadenceStart >= 1000000000) {
            os_log(OS_LOG_DEFAULT, "UKE PEN cadence ns=%llu read=%u xy=%u moved=%u hid=%u contact_no_force=%u force_expired=%u drop=%u hid_error=%u xy_gap_us=%llu queue_us=%llu pressure_age_us=%llu",
                   (unsigned long long)(now - cadenceStart), reads, coordinates, output.moved,
                   output.submitted, unmeasured, forceExpired, dropped, output.failed,
                   (unsigned long long)(longestGap / 1000), (unsigned long long)(maxQueueAge / 1000),
                   (unsigned long long)(maxPressureAge / 1000));
            ++cadenceLogs;
            cadenceStart = lastCoordinate = longestGap = maxQueueAge = maxPressureAge = 0;
            reads = coordinates = unmeasured = forceExpired = dropped = output.moved = output.submitted = output.failed = 0;
        }
        uint64_t liftNs;
        if (output.tipDown() && !contact.fresh(now) &&
            reports.confirmedLift(now, &liftNs) && liftNs >= contact.contactNs) {
            releaseTip(now, liftNs);
        }
        UkePenFrame frame;
        size_t length = sizeof frame;
        const int result = sysctlbyname(UKE_PEN_FRAME_SYSCTL, &frame, &length, nullptr, 0);
        if (result || !length) {
            if (result && errno != EINTR) {
                os_log_error(OS_LOG_DEFAULT, "UKE PEN native frame reader failed errno=%d", errno);
                break;
            }
            if (edgeContact.liftDue(now)) releaseTip(now, edgeContact.hoverNs);
            expirePosition(now);
            if (!lastValid && output.inRange) output.publish(false);
            // The frame read already waited on the producer. Only retain the
            // longer idle backoff when no pen coordinates are being decoded.
            if (!lastValid) usleep(reports.connected() ? 4000 : 16000);
            continue;
        }
        const uint64_t receivedAt = continuousNs(timebase);
        if (!cadenceStart) cadenceStart = receivedAt;
        ++reads;
        if (!validRecord(frame, length) || frame.continuous_ns > receivedAt ||
            receivedAt - frame.continuous_ns >= staleNs) {
            expirePosition(receivedAt);
            continue;
        }
        if (frame.generation != generation) {
            if (output.inRange) output.publish(false);
            if (generation) {
                contact = {};
                edgeContact = {};
                lastTip = uke_pen::PanelTip::unknown;
                lastValid = 0;
                piano_pen_core_reset(core);
            }
        } else if (lastSequence && frame.sequence != lastSequence + 1) {
            if (frame.sequence > lastSequence) dropped += frame.sequence - lastSequence - 1;
            uke_pen_core_drop_mutual_parts(core);
        }
        generation = frame.generation;
        lastSequence = frame.sequence;
        if (receivedAt - frame.continuous_ns > maxQueueAge) maxQueueAge = receivedAt - frame.continuous_ns;
        piano_pen_output sample{};
        if (piano_pen_core_process29(core, frame.bytes + UKE_PEN_SPI_PREFIX,
                frame.length - UKE_PEN_SPI_PREFIX, frame.continuous_ns, &sample, error, sizeof error)) {
            if (++rejects <= 8) os_log_error(OS_LOG_DEFAULT, "UKE PEN native frame rejected %{public}s", error);
            expirePosition(receivedAt);
            continue;
        }
        const auto tip = uke_pen::panelTip(sample.raw_pressure, frame.bytes[UKE_PEN_SPI_PREFIX + 80]);
        if (tip == uke_pen::PanelTip::unknown || frame.continuous_ns < contact.hoverNs) {
            expirePosition(receivedAt);
            continue;
        }
        if (tip != lastTip && tipEdges++ < 48)
            os_log(OS_LOG_DEFAULT, "UKE PEN panel tip=%d ns=%llu pressure=%d",
                   tip == uke_pen::PanelTip::contact, (unsigned long long)frame.continuous_ns, sample.pressure);
        lastTip = tip;
        const auto effectiveTip = reports.connected() ? tip : uke_pen::PanelTip::hover;
        const bool down = effectiveTip == uke_pen::PanelTip::contact;
        const bool lifting = output.lastTipDown && !down;
        const bool pressureValid = (sample.valid & PIANO_PEN_PRESSURE_VALID) != 0;
        if (lifting && edgeContact.startedNs) {
            const bool positive = contact.force && contact.forceNs <= frame.continuous_ns &&
                frame.continuous_ns - contact.forceNs < uke_pen::pressureInputMaxAgeNs;
            const bool samplePositive = pressureValid && sample.pressure && sample.pressure_age_ns < frame.continuous_ns &&
                frame.continuous_ns - sample.pressure_age_ns > contact.hoverNs;
            const bool zero = reports.confirmedLift(receivedAt, &liftNs) && liftNs >= edgeContact.startedNs;
            // Freeze the last submitted point while the panel and BLE
            // disagree. A recovered contact keeps the original edge gesture.
            if (edgeContact.deferLift(frame.continuous_ns, positive || samplePositive, zero)) continue;
        }
        if (down && edgeContact.hoverNs) {
            if (interruptedEdges++ < 24)
                os_log(OS_LOG_DEFAULT, "UKE PEN edge contact recovered gap_us=%llu",
                       (unsigned long long)((frame.continuous_ns - edgeContact.hoverNs) / 1000));
            edgeContact.hoverNs = 0;
        }
        if (!down) edgeContact = {};
        uint16_t pressure = contact.update(effectiveTip, frame.continuous_ns,
            pressureValid, sample.pressure_age_ns,
            static_cast<uint16_t>(sample.pressure));
        const bool hasCoordinates = (sample.valid & PIANO_PEN_COORDINATES_VALID) != 0;
        if (hasCoordinates) {
            ++coordinates;
            if (lastCoordinate && frame.continuous_ns >= lastCoordinate &&
                frame.continuous_ns - lastCoordinate > longestGap)
                longestGap = frame.continuous_ns - lastCoordinate;
            lastCoordinate = frame.continuous_ns;
        }
        if (!hasCoordinates && !lifting) {
            expirePosition(receivedAt);
            continue;
        }
        if (lifting && !hasCoordinates && (!lastValid || receivedAt - lastValid >= staleNs)) {
            output.publish(false);
            continue;
        }
        if (down && !output.lastTipDown && !pressure) {
            const uint64_t deadline = frame.continuous_ns + uke_pen::pressureStartWaitNs;
            bool changed = false;
            do {
                const auto sample = reports.onsetPressure(frame.continuous_ns, contact.liftNs);
                pressure = contact.startForce(frame.continuous_ns, sample.ns, sample.force);
                if (pressure || continuousNs(timebase) >= deadline) break;
                usleep(1000);
                changed = reports.poll();
            } while (!changed && !stopped);
            if (changed) {
                if (output.inRange) output.publish(false);
                contact = {};
                edgeContact = {};
                lastTip = uke_pen::PanelTip::unknown;
                lastValid = lastSequence = 0;
                continue;
            }
            if (stopped) break;
            if (onsetPressureLogs++ < 48) {
                const auto latest = reports.latestPressureTime();
                const auto frameNs = static_cast<int64_t>(frame.continuous_ns);
                os_log(OS_LOG_DEFAULT, "UKE PEN onset pressure=%u waited_us=%llu latest_report_offset_us=%lld",
                       pressure, (unsigned long long)((continuousNs(timebase) - receivedAt) / 1000),
                       latest ? (long long)((static_cast<int64_t>(latest) - frameNs) / 1000) : LLONG_MIN);
            }
        }
        output.report.sequence = frame.sequence;
        output.report.generation = frame.generation;
        output.report.tip_state = down ? UKE_PEN_TIP_CONTACT : UKE_PEN_TIP_HOVER;
        output.report.pressure = pressure;
        if (down && !pressure) {
            ++unmeasured;
            if (contact.forceNs) ++forceExpired;
        }
        if (pressure && frame.continuous_ns >= contact.forceNs &&
            frame.continuous_ns - contact.forceNs > maxPressureAge)
            maxPressureAge = frame.continuous_ns - contact.forceNs;
        // Close ink at the last submitted contact point. The next hover
        // frame moves independently of the stroke's final position.
        if (!lifting) {
            const auto point = uke_pen::toPanel(sample.x, sample.y, info.x_max, info.y_max);
#if defined(UKE_PEN_CSOT_EDGE_CALIBRATION)
            const auto corrected = uke_pen::calibrateCsotEdges(point);
#else
            const auto corrected = uke_pen::calibratePanelEdges(point);
#endif
            const auto calibrated = uke_pen::cornerCorrection(point, corrected);
            if (down && !output.lastTipDown) {
                edgeContact.startedNs = uke_pen::atPanelEdge(point) ? frame.continuous_ns : 0;
                if (edgeContact.startedNs && receivedAt >= nextCornerReadNs) {
                    nextCornerReadNs = receivedAt + 3000000000ULL;
                    os_log(OS_LOG_DEFAULT, "UKE PEN edge calibration raw=%u,%u mapped=%u,%u",
                           point.x, point.y, calibrated.x, calibrated.y);
                    dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), nullptr, logStoredCornerPreferences);
                }
            }
            output.report.x = calibrated.x;
            output.report.y = calibrated.y;
            const bool hasTilt = (sample.valid & PIANO_PEN_TILT_VALID) != 0;
            output.report.tilt_x = hasTilt ? sample.tilt_y : 0;
            output.report.tilt_y = hasTilt ? -sample.tilt_x : 0;
        } else {
            output.report.x = output.lastX;
            output.report.y = output.lastY;
        }
        if (hasCoordinates) lastValid = frame.continuous_ns;
        output.publish(true);
    }
    return 0;
}
