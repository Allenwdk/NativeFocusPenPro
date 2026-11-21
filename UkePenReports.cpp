// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "UkePenReports.h"
#include "UkePenBle.h"
#include "UkePenStroke.h"
#include "piano-pen-core.h"

#include <cerrno>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <sys/sysctl.h>
#include <time.h>

extern "C" {
CFTypeRef IOHIDEventSystemClientCreate(CFAllocatorRef allocator);
void IOHIDEventSystemClientDispatchEvent(CFTypeRef client, CFTypeRef event);
CFTypeRef IOHIDEventCreateGenericGestureEvent(CFAllocatorRef allocator, uint64_t timestamp,
                                            uint32_t kind, uint32_t options);
void IOHIDEventSetIntegerValue(CFTypeRef event, uint32_t field, CFIndex value);
void IOHIDEventSetSenderID(CFTypeRef event, uint64_t senderId);
void IOHIDEventSetPhase(CFTypeRef event, uint32_t phase);
CFTypeRef IOHIDDeviceCreate(CFAllocatorRef allocator, io_service_t service);
IOReturn IOHIDDeviceOpen(CFTypeRef device, IOOptionBits options);
IOReturn IOHIDDeviceClose(CFTypeRef device, IOOptionBits options);
void IOHIDDeviceScheduleWithRunLoop(CFTypeRef device, CFRunLoopRef loop, CFStringRef mode);
void IOHIDDeviceUnscheduleFromRunLoop(CFTypeRef device, CFRunLoopRef loop, CFStringRef mode);
using RawCallback = void (*)(void *, IOReturn, void *, uint32_t, uint32_t, uint8_t *, CFIndex, uint64_t);
void IOHIDDeviceRegisterInputReportWithTimeStampCallback(CFTypeRef device, uint8_t *buffer, CFIndex length,
                                                       RawCallback callback, void *context);
}

UkePenReports::UkePenReports(piano_pen_core *decoder) : decoder_(decoder) {
    mach_timebase_info(&timebase_);
}

UkePenReports::~UkePenReports() {
    if (squeezeDown_) dispatchGesture(5, 0, 8);
    closeRawDevice();
    if (gestureClient_) CFRelease(gestureClient_);
}

void UkePenReports::openRawDevice(uint64_t registryId) {
    if (!registryId || !timebase_.denom) return;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(registryId));
    if (!service) return;
    CFTypeRef device = IOHIDDeviceCreate(kCFAllocatorDefault, service);
    IOObjectRelease(service);
    if (!device) return;
    const IOReturn result = IOHIDDeviceOpen(device, 0);
    if (result) {
        os_log_error(OS_LOG_DEFAULT, "UKE PEN raw HID queue open failed rc=%x", result);
        CFRelease(device);
        return;
    }
    rawDevice_ = device;
    IOHIDDeviceRegisterInputReportWithTimeStampCallback(device, rawBuffer_, sizeof(rawBuffer_), receiveRaw, this);
    IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    os_log(OS_LOG_DEFAULT, "UKE PEN raw HID queue opened registry=%llu", (unsigned long long)registryId);
}

void UkePenReports::closeRawDevice() {
    if (!rawDevice_) return;
    IOHIDDeviceUnscheduleFromRunLoop(rawDevice_, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOHIDDeviceClose(rawDevice_, 0);
    CFRelease(rawDevice_);
    rawDevice_ = nullptr;
    rawLoggedMask_ = 0;
}

void UkePenReports::receiveRaw(void *context, IOReturn result, void *, uint32_t type, uint32_t id,
                               uint8_t *bytes, CFIndex length, uint64_t ticks) {
    auto *owner = static_cast<UkePenReports *>(context);
    if (result || type != 0 || !bytes || length <= 0 || !ticks || !owner->rawDevice_ || !owner->connected_) return;
    if (!id && length >= 2) id = bytes[0];
    const uint64_t absoluteNow = mach_absolute_time();
    if (ticks > absoluteNow) return;
    const uint64_t continuousNow = mach_continuous_time();
    const uint64_t timestamp = continuousNow - (absoluteNow - ticks);
    const auto convert = [&](uint64_t value) {
        return static_cast<uint64_t>(static_cast<__uint128_t>(value) * owner->timebase_.numer / owner->timebase_.denom);
    };
    if (convert(absoluteNow - ticks) >= 100000000) return;
    if (uke_pen_ble_body_size(id) && !(owner->rawLoggedMask_ & (1u << id))) {
        owner->rawLoggedMask_ |= 1u << id;
        os_log(OS_LOG_DEFAULT, "UKE PEN first raw HID input id=%u", id);
    }
    owner->acceptReport(id, bytes, static_cast<size_t>(length), ticks, convert(timestamp));
}

void UkePenReports::acceptReport(unsigned id, const uint8_t *bytes, size_t length, uint64_t ticks, uint64_t ns) {
    const unsigned body = uke_pen_ble_body_size(id);
    if (!body || (length != body && length != body + 1) || !ticks || !ns) return;
    if (length == body + 1) {
        if (bytes[0] != id) return;
        ++bytes;
    }
    const unsigned slot = id == 2 ? 0 : id == 5 ? 1 : 2;
    if (ticks <= seenTicks_[slot]) return;
    seenTicks_[slot] = ticks;
    if (id == 5) {
        uint8_t report[15] = {5};
        __builtin_memcpy(report + 1, bytes, body);
        if (piano_pen_core_report5(decoder_, report, sizeof(report), ns, nullptr, 0)) return;
        const uint16_t pressure = bytes[0] | static_cast<uint16_t>(bytes[1]) << 8;
        pressureHistory_.append(ns, pressure);
        if (pressure || lastPressure_ || !lastPressureNs_) lastPressureNs_ = ns;
        lastPressure_ = pressure;
        if (loggedReports_[0]++ < 8) os_log(OS_LOG_DEFAULT, "UKE PEN BLE pressure=%u", pressure);
    } else if (id == 2) {
        acceptBodyGesture(bytes[0]);
        const char *gesture = piano_pen_body_gesture_name(bytes[0]);
        if (gesture && loggedReports_[1]++ < 16)
            os_log(OS_LOG_DEFAULT, "UKE PEN BLE body gesture %{public}s", gesture);
    } else if (loggedReports_[2]++ == 0) {
        os_log(OS_LOG_DEFAULT, "UKE PEN BLE motion report length=%u", body);
    }
}

bool UkePenReports::dispatchGesture(uint32_t kind, uint32_t flavor, uint32_t phase) {
    if (!senderId_) return false;
    if (!gestureClient_) gestureClient_ = IOHIDEventSystemClientCreate(kCFAllocatorDefault);
    if (!gestureClient_) {
        if (!gestureFailed_) os_log_error(OS_LOG_DEFAULT, "UKE PEN system gesture client unavailable");
        gestureFailed_ = true;
        return false;
    }
    CFTypeRef event = IOHIDEventCreateGenericGestureEvent(kCFAllocatorDefault, mach_absolute_time(), kind, 0);
    if (!event) return false;
    // UIKit uses type 0x27, kind 5 for squeeze and kind 0/flavor 2 for double tap.
    IOHIDEventSetSenderID(event, senderId_);
    IOHIDEventSetIntegerValue(event, 0x270001, flavor);
    IOHIDEventSetPhase(event, phase);
    IOHIDEventSystemClientDispatchEvent(gestureClient_, event);
    CFRelease(event);
    return true;
}

void UkePenReports::acceptBodyGesture(uint8_t value) {
    if (value == 0x6e && !squeezeDown_) {
        squeezeDown_ = dispatchGesture(5, 0, 1);
        squeezeDeadlineNs_ = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) + 5000000000ull;
        return;
    }
    if (squeezeDown_ && value != 0x6e) {
        dispatchGesture(5, 0, value == 0 ? 4 : 8);
        squeezeDown_ = false;
    }
    if (value == 0x58) {
        const bool submitted = dispatchGesture(0, 2, 4);
        os_log(OS_LOG_DEFAULT, "UKE PEN native double tap submitted=%d", submitted);
        if (submitted) {
            // Some apps never issue Haptics Output for a submitted tap. A
            // real P81c body gesture may request the pen's short tick too.
            uint32_t pulse = 1;
            if (!sysctlbyname("kern.uke_pen_haptic", nullptr, nullptr, &pulse, sizeof(pulse)))
                os_log(OS_LOG_DEFAULT, "UKE PEN requested double tap feedback");
        }
    }
}

bool UkePenReports::poll() {
    const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    if (squeezeDown_ && now >= squeezeDeadlineNs_) {
        dispatchGesture(5, 0, 8);
        squeezeDown_ = false;
    }
    if (rawDevice_) {
        for (unsigned i = 0; i < 4; ++i)
            if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) != kCFRunLoopRunHandledSource) break;
        if (now < nextSnapshotNs_) return false;
    }
    // Raw HID is the primary path. Limit synchronous reads on the BLE
    // work loop while still checking disconnect and fallback reports.
    nextSnapshotNs_ = now + 8000000;
    UkePenBleSnapshot snapshot{};
    size_t size = sizeof snapshot;
    if (sysctlbyname(UKE_PEN_BLE_SYSCTL, &snapshot, &size, nullptr, 0)) {
        if (errno != EINTR && !failed_) {
            os_log_error(OS_LOG_DEFAULT, "UKE PEN driver BLE read failed errno=%d", errno);
            failed_ = true;
        }
        return false;
    }
    if (size != sizeof snapshot || snapshot.version != UKE_PEN_BLE_ABI) return false;

    const bool changed = generation_ != snapshot.generation || connected_ != !!snapshot.connected;
    if (changed || senderId_ != snapshot.sender_id) {
        if (squeezeDown_) dispatchGesture(5, 0, 8);
        squeezeDown_ = false;
    }
    if (changed) {
        closeRawDevice();
        generation_ = snapshot.generation;
        connected_ = snapshot.connected != 0;
        lastPressureNs_ = 0;
        lastPressure_ = 0;
        pressureHistory_ = {};
        for (uint64_t &ticks : seenTicks_) ticks = 0;
        for (unsigned &count : loggedReports_) count = 0;
        piano_pen_core_reset(decoder_);
        os_log(OS_LOG_DEFAULT, "UKE PEN driver BLE connected=%d gen=%u", connected_, generation_);
        if (connected_) openRawDevice(snapshot.device_id);
    }
    senderId_ = snapshot.sender_id;
    if (!connected_) return changed;
    for (const auto &report : snapshot.reports) {
        if (!report.length || report.length > sizeof(report.bytes)) continue;
        acceptReport(report.bytes[0], report.bytes, report.length, report.absolute_ticks, report.continuous_ns);
    }
    return changed;
}

bool UkePenReports::confirmedLift(uint64_t nowNs, uint64_t *sampleNs) const {
    if (lastPressure_ || !lastPressureNs_ || nowNs < lastPressureNs_ ||
        nowNs - lastPressureNs_ < uke_pen::liftConfirmNs) return false;
    *sampleNs = lastPressureNs_;
    return true;
}

