// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "UkePenBle.h"
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOLib.h>
#include <IOKit/hid/IOHIDElement.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>

extern "C" uint64_t native_hid_pen_sender_id() __attribute__((weak_import));

namespace {
// 24A446 IORegistry capture 11B9797C-6AA5-4D9F-A684-CF17E34D9CB8.
constexpr uint8_t reportMap[] = {
    0x05,0x01,0x09,0x02,0xa1,0x01,0x09,0x01,0xa1,0x00,0x85,0x01,0x05,0x09,0x19,0x01,
    0x29,0x08,0x15,0x00,0x25,0x01,0x95,0x08,0x75,0x01,0x81,0x02,0x05,0x01,0x09,0x30,
    0x09,0x31,0x15,0x80,0x25,0x7f,0x75,0x08,0x95,0x02,0x81,0x06,0x09,0x38,0x15,0x80,
    0x25,0x7f,0x75,0x08,0x95,0x01,0x81,0x06,0xc0,0xc0,0x05,0x20,0x09,0x20,0x09,0x03,
    0xa1,0x01,0x85,0x06,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x14,0x81,0x02,0xc0,
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x02,0x05,0x07,0x19,0x4b,0x29,0x6e,0x15,0x4b,
    0x25,0x6e,0x75,0x08,0x95,0x01,0x81,0x00,0xc0,0x09,0x57,0xa1,0x01,0x85,0x05,0x15,
    0x00,0x26,0x00,0x40,0x75,0x10,0x95,0x04,0x09,0x57,0x81,0x06,0x15,0x00,0x25,0xff,
    0x75,0x08,0x95,0x06,0x09,0x57,0x81,0x06,0xc0,
};
constexpr unsigned reportIds[UKE_PEN_BLE_REPORT_COUNT] = {2, 5, 6};
constexpr uint64_t searchIntervalNs = 250000000;
constexpr uint64_t reportMaxAgeNs = 100000000;

struct Bridge {
    IOLock *lock;
    IOService *device;
    unsigned generation;
    unsigned matchFailures;
    uint64_t nextSearchNs;
    uint64_t lastReported[UKE_PEN_BLE_REPORT_COUNT];
};
Bridge *gBridge;

uint64_t continuousNs(uint64_t ticks) {
    uint64_t ns;
    absolutetime_to_nanoseconds(ticks, &ns);
    return ns;
}

IOService *findPen() {
    auto match = OSSharedPtr<OSDictionary>(IOService::serviceMatching("IOHIDDevice"), OSNoRetain);
    auto properties = OSSharedPtr<OSDictionary>(OSDictionary::withCapacity(3), OSNoRetain);
    auto vendor = OSSharedPtr<OSNumber>(OSNumber::withNumber(0x0022, 32), OSNoRetain);
    auto product = OSSharedPtr<OSNumber>(OSNumber::withNumber(0x5081, 32), OSNoRetain);
    auto transport = OSSharedPtr<OSString>(OSString::withCString("BluetoothLowEnergy"), OSNoRetain);
    if (!match || !properties || !vendor || !product || !transport ||
        !properties->setObject("VendorID", vendor.get()) || !properties->setObject("ProductID", product.get()) ||
        !properties->setObject("Transport", transport.get()) || !match->setObject("IOPropertyMatch", properties.get()))
        return nullptr;
    auto devices = IOService::getMatchingServices(match.get());
    if (!devices) return nullptr;
    IOService *selected = nullptr;
    while (auto *candidate = OSDynamicCast(IOService, devices->getNextObject())) {
        auto raw = OSSharedPtr<OSObject>(candidate->copyProperty("ReportDescriptor"), OSNoRetain);
        auto *descriptor = OSDynamicCast(OSData, raw.get());
        if (candidate->isInactive() || !descriptor || descriptor->getLength() != sizeof reportMap ||
            memcmp(descriptor->getBytesNoCopy(), reportMap, sizeof reportMap)) continue;
        if (selected) {
            selected->release();
            return nullptr;
        }
        candidate->retain();
        selected = candidate;
    }
    return selected;
}

IOReturn sampleReports(OSObject *owner, void *snapshot, void *observation, void *, void *) {
    auto *device = static_cast<IOService *>(owner);
    if (device->isInactive()) return kIOReturnOffline;
    auto &result = *static_cast<UkePenBleSnapshot *>(snapshot);
    auto &bridge = *static_cast<Bridge *>(observation);
    auto raw = OSSharedPtr<OSObject>(device->copyProperty("InputReportElements"), OSNoRetain);
    auto *elements = OSDynamicCast(OSArray, raw.get());
    if (!elements) return kIOReturnNotReady;
    const uint64_t now = continuousNs(mach_continuous_time());
    for (unsigned i = 0, n = elements->getCount(); i < n; ++i) {
        auto *object = elements->getObject(i);
        if (!object || !object->metaCast("IOHIDElement")) continue;
        auto *element = static_cast<IOHIDElement *>(object);
        const unsigned id = element->getReportID();
        for (unsigned slot = 0; slot < UKE_PEN_BLE_REPORT_COUNT; ++slot) {
            if (reportIds[slot] != id) continue;
            const uint64_t stamp = __OSAbsoluteTime(element->getTimeStamp());
            if (!stamp || stamp == bridge.lastReported[slot]) break;
            const uint64_t ns = continuousNs(absolutetime_to_continuoustime(stamp));
            if (ns > now || now - ns >= reportMaxAgeNs) break;
            const OSData *data = element->getDataValue();
            if (!data) break;
            const unsigned expected = uke_pen_ble_body_size(id);
            const auto *bytes = static_cast<const uint8_t *>(data->getBytesNoCopy());
            if (!bytes) break;
            if (data->getLength() == expected + 1 && bytes[0] == id) ++bytes;
            else if (data->getLength() != expected) break;
            if (!bridge.lastReported[slot])
                IOLog("UKE PEN kernel BLE first report id=%u bytes=%u\n", id, expected + 1);
            bridge.lastReported[slot] = stamp;
            auto &report = result.reports[slot];
            report.length = expected + 1;
            report.bytes[0] = static_cast<uint8_t>(id);
            memcpy(report.bytes + 1, bytes, expected);
            report.absolute_ticks = stamp;
            report.continuous_ns = ns;
            break;
        }
    }
    return kIOReturnSuccess;
}
}

extern "C" int uke_pen_ble_snapshot(UkePenBleSnapshot *result) {
    auto *bridge = __atomic_load_n(&gBridge, __ATOMIC_ACQUIRE);
    if (!bridge) {
        unsigned mode = 0;
        PE_parse_boot_argn("uke-pencil", &mode, sizeof mode);
        if (mode != 4) return ENODEV;
        auto *created = static_cast<Bridge *>(IOMallocZero(sizeof(Bridge)));
        if (!created) return ENOMEM;
        created->lock = IOLockAlloc();
        if (!created->lock) {
            IOFree(created, sizeof(Bridge));
            return ENOMEM;
        }
        if (__atomic_compare_exchange_n(&gBridge, &bridge, created, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            bridge = created;
        } else {
            IOLockFree(created->lock);
            IOFree(created, sizeof(Bridge));
        }
    }
    IOLockLock(bridge->lock);
    if (bridge->device && bridge->device->isInactive()) {
        bridge->device->release();
        bridge->device = nullptr;
        bridge->nextSearchNs = 0;
        ++bridge->generation;
        memset(bridge->lastReported, 0, sizeof bridge->lastReported);
        IOLog("UKE PEN kernel BLE disconnected gen=%u\n", bridge->generation);
    }
    const uint64_t now = continuousNs(mach_continuous_time());
    if (!bridge->device && now >= bridge->nextSearchNs) {
        bridge->nextSearchNs = now + searchIntervalNs;
        bridge->device = findPen();
        if (bridge->device)
            IOLog("UKE PEN kernel BLE P81c matched gen=%u\n", ++bridge->generation);
    }
    result->version = UKE_PEN_BLE_ABI;
    result->generation = bridge->generation;
    result->connected = bridge->device && !bridge->device->isInactive();
    if (&native_hid_pen_sender_id) result->sender_id = native_hid_pen_sender_id();
    if (result->connected) {
        result->device_id = bridge->device->getRegistryEntryID();
        auto *loop = bridge->device->getWorkLoop();
        if (loop && loop->runAction(sampleReports, bridge->device, result, bridge) && bridge->matchFailures++ < 3)
            IOLog("UKE PEN kernel BLE report elements not available\n");
    }
    IOLockUnlock(bridge->lock);
    return 0;
}
